
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "script.h"
#include "tasks.h"
#include "state.h"

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
    return e == SUCCEEDED || e == PARTIALLY_SUCCEEDED ||
           e == FAILED || e == SKIPPED;
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
            int disponible_flag = 1;
            for (size_t j = 0; j < t->cantidad_dependencias; ++j) {
                if (!visitados[t->dependencias[j]]) {
                    disponible_flag = 0;
                    break;
                }
            }
            if (disponible_flag) {
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

/* Evalúa disponibilidad y condiciones (succeeded(), failed(), always()). */
static int disponible(const plan_trabajos_t *plan, size_t id,
                      int *omitir)
{
    const trabajo_t *t = &plan->trabajos[id];
    int fallaron = 0;
    for (size_t i = 0; i < t->cantidad_dependencias; ++i) {
        estado_t e = plan->trabajos[t->dependencias[i]].estado;
        if (!completo(e))
            return 0;
        if (e == FAILED || e == SKIPPED)
            fallaron = 1;
    }
    
    if (t->condicion == COND_ALWAYS) {
        *omitir = 0;
    } else if (t->condicion == COND_FAILED) {
        *omitir = !fallaron;
    } else { /* COND_SUCCEEDED por defecto */
        *omitir = fallaron;
    }
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

static estado_t ejecutar_trabajo(trabajo_t *t)
{
    double inicio = ahora();
    if (inicio < 0)
        return FAILED;

    int parcial = 0;
    for (size_t i = 0; i < t->cantidad_pasos; ++i) {
        if (t->cancelar && atomic_load(t->cancelar))
            return CANCELED;
        unsigned limite = 0;
        if (t->timeout_segundos) {
            double tiempo = ahora() - inicio;
            if (tiempo < 0 || tiempo >= (double)t->timeout_segundos)
                return FAILED;
            limite = (unsigned)ceil((double)t->timeout_segundos - tiempo);
            if (limite == 0)
                limite = 1;
        }

        int resultado = ejecutar_paso(&t->pasos[i], limite,
                                      &t->resultado_cpu, t->cancelar);
        if (t->timeout_segundos &&
            ahora() - inicio >= (double)t->timeout_segundos)
            return FAILED;

        if (resultado != 0) {
            if (!t->pasos[i].continuar_si_falla)
                return FAILED;
            parcial = 1;
        }
    }
    return parcial ? PARTIALLY_SUCCEEDED : SUCCEEDED;
}

int ejecutar_plan(plan_trabajos_t *plan, int agentes)
{
    if (!plan || !plan->trabajos || !plan->cantidad || agentes < 1 ||
        validar_plan(plan) != 0)
        return -1;

    for (size_t i = 0; i < plan->cantidad; ++i) {
        plan->trabajos[i].estado = QUEUED;
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
                        if (t->estado != QUEUED)
                            continue;
                        int omitir = 0;
                        if (!disponible(plan, i, &omitir))
                            continue;
                        if (omitir) {
                            t->estado = SKIPPED;
                            --restantes;
                            continue;
                        }
                        t->estado = RUNNING;
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

            estado_t resultado = ejecutar_trabajo(
                &plan->trabajos[elegido]);
            #pragma omp critical(planificador_trabajos)
            {
                plan->trabajos[elegido].estado = resultado;
                --restantes;
                --activos;
                if (resultado == FAILED ||
                    resultado == PARTIALLY_SUCCEEDED)
                    fallo = 1;
            }
        }
    }
    return fallo ? -1 : 0;
}