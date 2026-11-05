

#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "script.h"
#include "tasks.h"

#include <math.h>
#include <omp.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

static double ahora(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return -1.0;
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static int completo(estado_trabajo_t e)
{
    return e == TRABAJO_CORRECTO || e == TRABAJO_PARCIAL ||
           e == TRABAJO_FALLIDO || e == TRABAJO_OMITIDO;
}

/* Valida indices y ciclos antes de iniciar cualquier paso. */
static int validar_plan(const plan_trabajos_t *plan)
{
    size_t n = plan->cantidad;
    unsigned char *visitados = calloc(n, sizeof *visitados);
    if (!visitados)
        return -1;

    for (size_t i = 0; i < n; ++i) {
        const trabajo_t *t = &plan->trabajos[i];
        if (!t->nombre || !t->pasos || t->cantidad_pasos == 0 ||
            (t->cantidad_dependencias && !t->dependencias) ||
            t->condicion > COND_ALWAYS) {
            free(visitados);
            return -1;
        }
        for (size_t d = 0; d < t->cantidad_dependencias; ++d) {
            if (t->dependencias[d] >= n || t->dependencias[d] == i) {
                free(visitados);
                return -1;
            }
        }
    }

    /* Eliminar virtualmente vertices cuyos predecesores ya se eliminaron. */
    size_t completados = 0;
    while (completados < n) {
        int avance = 0;
        for (size_t i = 0; i < n; ++i) {
            if (visitados[i])
                continue;
            const trabajo_t *t = &plan->trabajos[i];
            int disponible = 1;
            for (size_t j = 0; j < t->cantidad_dependencias; ++j) {
                if (!visitados[t->dependencias[j]]) {
                    disponible = 0;
                    break;
                }
            }
            if (disponible) {
                visitados[i] = 1;
                ++completados;
                avance = 1;
            }
        }
        if (!avance) {
            free(visitados);
            return -1;
        }
    }
    free(visitados);
    return 0;
}

/* Usar solo dentro de la region critica OpenMP. */
static int disponible(const plan_trabajos_t *plan, size_t id,
                      int *omitir)
{
    const trabajo_t *t = &plan->trabajos[id];
    int fallaron = 0;
    for (size_t i = 0; i < t->cantidad_dependencias; ++i) {
        estado_trabajo_t e = plan->trabajos[t->dependencias[i]].estado;
        if (!completo(e))
            return 0;
        if (e == TRABAJO_FALLIDO || e == TRABAJO_OMITIDO)
            fallaron = 1;
    }
    *omitir = (t->condicion == COND_SUCCEEDED && fallaron) ||
              (t->condicion == COND_FAILED && !fallaron);
    return 1;
}

static int ejecutar_paso(const paso_t *p, unsigned limite,
                         uint64_t *cpu, const atomic_int *cancelar)
{
    switch (p->tipo) {
    case PASO_SCRIPT: {
        int salida = -1;
        return ejecutar_script_controlado(p->comando, limite, &salida,
                                          p->log_ruta, cancelar);
    }
    case PASO_SLEEP:
        return tarea_sleep(p->segundos);
    case PASO_CPU:
        return tarea_cpu(p->iteraciones, p->semilla, p->hilos, cpu);
    case PASO_WORDCOUNT:
        return tarea_wordcount(p->archivo, p->hilos, p->salida,
                               p->top ? p->top : 10);
    case PASO_HISTOGRAM:
        return tarea_histogram(p->archivo, p->bins, p->hilos, p->salida);
    }
    return -1;
}

static estado_trabajo_t ejecutar_trabajo(trabajo_t *t)
{
    double inicio = ahora();
    if (inicio < 0)
        return TRABAJO_FALLIDO;

    int parcial = 0;
    for (size_t i = 0; i < t->cantidad_pasos; ++i) {
        if (t->cancelar && atomic_load(t->cancelar))
            return TRABAJO_FALLIDO;
        unsigned limite = 0;
        if (t->timeout_segundos) {
            double tiempo = ahora() - inicio;
            if (tiempo < 0 || tiempo >= (double)t->timeout_segundos)
                return TRABAJO_FALLIDO;
            /* Redondear hacia arriba para que el subproceso no reciba 0,
               que en ejecutar_script significa sin limite. */
            limite = (unsigned)ceil((double)t->timeout_segundos - tiempo);
            if (limite == 0)
                limite = 1;
        }

        int resultado = ejecutar_paso(&t->pasos[i], limite,
                                      &t->resultado_cpu, t->cancelar);
        if (t->timeout_segundos &&
            ahora() - inicio >= (double)t->timeout_segundos)
            return TRABAJO_FALLIDO;

        if (resultado != 0) {
            if (!t->pasos[i].continuar_si_falla)
                return TRABAJO_FALLIDO;
            parcial = 1;
        }
    }
    return parcial ? TRABAJO_PARCIAL : TRABAJO_CORRECTO;
}

int ejecutar_plan(plan_trabajos_t *plan, int agentes)
{
    if (!plan || !plan->trabajos || !plan->cantidad || agentes < 1 ||
        validar_plan(plan) != 0)
        return -1;

    for (size_t i = 0; i < plan->cantidad; ++i) {
        plan->trabajos[i].estado = TRABAJO_PENDIENTE;
        plan->trabajos[i].resultado_cpu = 0;
    }

    size_t restantes = plan->cantidad;
    int activos = 0;
    int fallo = 0;

    #pragma omp parallel num_threads(agentes) default(none) \
        shared(plan, restantes, activos, fallo)
    {
        for (;;) {
            size_t elegido = plan->cantidad;
            int salir = 0;

            #pragma omp critical(planificador_trabajos)
            {
                if (!restantes) {
                    salir = 1;
                } else {
                    for (size_t i = 0; i < plan->cantidad; ++i) {
                        trabajo_t *t = &plan->trabajos[i];
                        if (t->estado != TRABAJO_PENDIENTE)
                            continue;
                        int omitir = 0;
                        if (!disponible(plan, i, &omitir))
                            continue;
                        if (omitir) {
                            t->estado = TRABAJO_OMITIDO;
                            --restantes;
                            fallo = 1;
                            continue;
                        }
                        t->estado = TRABAJO_EN_CURSO;
                        ++activos;
                        elegido = i;
                        break;
                    }
                }
            }

            if (salir)
                break;
            if (elegido == plan->cantidad) {
                sched_yield();
                continue;
            }

            estado_trabajo_t resultado = ejecutar_trabajo(
                &plan->trabajos[elegido]);
            #pragma omp critical(planificador_trabajos)
            {
                plan->trabajos[elegido].estado = resultado;
                --restantes;
                --activos;
                if (resultado == TRABAJO_FALLIDO ||
                    resultado == TRABAJO_PARCIAL)
                    fallo = 1;
            }
        }
    }
    return fallo ? -1 : 0;
}

