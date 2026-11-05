
#define _POSIX_C_SOURCE 200809L

#include "model.h"
#include "script.h"
#include "tasks.h"

#include <omp.h>
#include <sched.h>
#include <stddef.h>
#include <time.h>

static double reloj_actual(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return -1.0;

    return (double)t.tv_sec +
           (double)t.tv_nsec / 1000000000.0;
}

static int terminado(estado_trabajo_t estado)
{
    return estado == TRABAJO_CORRECTO ||
           estado == TRABAJO_PARCIAL ||
           estado == TRABAJO_FALLIDO ||
           estado == TRABAJO_OMITIDO;
}

/* Esta funcion se utiliza dentro de la region critica. */
static int listo(const plan_trabajos_t *plan,
                 size_t indice, int *omitir)
{
    const trabajo_t *trabajo = &plan->trabajos[indice];
    int hubo_fallo = 0;

    for (size_t i = 0;
         i < trabajo->cantidad_dependencias; i++) {

        size_t dependencia = trabajo->dependencias[i];

        if (dependencia >= plan->cantidad ||
            dependencia == indice)
            return -1;

        estado_trabajo_t estado =
            plan->trabajos[dependencia].estado;

        if (!terminado(estado))
            return 0;

        if (estado == TRABAJO_FALLIDO ||
            estado == TRABAJO_OMITIDO)
            hubo_fallo = 1;
    }

    if (trabajo->condicion == COND_SUCCEEDED &&
        hubo_fallo)
        *omitir = 1;

    if (trabajo->condicion == COND_FAILED &&
        !hubo_fallo)
        *omitir = 1;

    return 1;
}

static int ejecutar_paso(const paso_t *paso,
                         unsigned limite,
                         uint64_t *resultado_cpu)
{
    switch (paso->tipo) {
    case PASO_SCRIPT: {
        int salida;

        return ejecutar_script(
            paso->comando, limite, &salida);
    }

    case PASO_SLEEP:
        return tarea_sleep(paso->segundos);

    case PASO_CPU:
        return tarea_cpu(
            paso->iteraciones,
            paso->semilla,
            paso->hilos,
            resultado_cpu);

    case PASO_WORDCOUNT:
        return tarea_wordcount(
            paso->archivo,
            paso->hilos,
            paso->salida,
            paso->top ? paso->top : 10);

    case PASO_HISTOGRAM:
        return tarea_histogram(
            paso->archivo,
            paso->bins,
            paso->hilos,
            paso->salida);
    }

    return -1;
}

static estado_trabajo_t ejecutar_trabajo(trabajo_t *trabajo)
{
    if (!trabajo->pasos ||
        trabajo->cantidad_pasos == 0)
        return TRABAJO_FALLIDO;

    double inicio = reloj_actual();
    int parcial = 0;

    if (inicio < 0)
        return TRABAJO_FALLIDO;

    for (size_t i = 0;
         i < trabajo->cantidad_pasos; i++) {

        unsigned restante = 0;

        if (trabajo->timeout_segundos != 0) {
            double transcurrido =
                reloj_actual() - inicio;

            if (transcurrido < 0 ||
                transcurrido >=
                    (double)trabajo->timeout_segundos)
                return TRABAJO_FALLIDO;

            restante = trabajo->timeout_segundos -
                       (unsigned)transcurrido;
        }

        int resultado = ejecutar_paso(
            &trabajo->pasos[i],
            restante,
            &trabajo->resultado_cpu);

        if (trabajo->timeout_segundos &&
            reloj_actual() - inicio >=
                (double)trabajo->timeout_segundos)
            resultado = -1;

        if (resultado != 0) {
            if (!trabajo->pasos[i].continuar_si_falla)
                return TRABAJO_FALLIDO;

            parcial = 1;
        }
    }

    return parcial
        ? TRABAJO_PARCIAL
        : TRABAJO_CORRECTO;
}

int ejecutar_plan(plan_trabajos_t *plan, int agentes)
{
    if (!plan || !plan->trabajos ||
        !plan->cantidad || agentes < 1)
        return -1;

    /* Comprobar indices antes de ejecutar. */
    for (size_t i = 0; i < plan->cantidad; i++) {
        trabajo_t *trabajo = &plan->trabajos[i];

        if ((trabajo->cantidad_dependencias &&
             !trabajo->dependencias) ||
            (trabajo->cantidad_pasos &&
             !trabajo->pasos))
            return -1;

        for (size_t j = 0;
             j < trabajo->cantidad_dependencias; j++) {

            if (trabajo->dependencias[j] >=
                    plan->cantidad ||
                trabajo->dependencias[j] == i)
                return -1;
        }

        trabajo->estado = TRABAJO_PENDIENTE;
        trabajo->resultado_cpu = 0;
    }

    size_t restantes = plan->cantidad;
    int fallo = 0;
    int bloqueado = 0;
    int activos = 0;

    int niveles_previos = omp_get_max_active_levels();

    if (niveles_previos < 2)
        omp_set_max_active_levels(2);

    #pragma omp parallel num_threads(agentes) \
        default(none) \
        shared(plan, restantes, fallo, bloqueado, activos)
    {
        for (;;) {
            size_t elegido = plan->cantidad;
            int salir = 0;

            #pragma omp critical(planificador_trabajos)
            {
                if (restantes == 0 || bloqueado) {
                    salir = 1;
                } else {
                    for (size_t i = 0;
                         i < plan->cantidad; i++) {

                        trabajo_t *trabajo =
                            &plan->trabajos[i];

                        if (trabajo->estado !=
                            TRABAJO_PENDIENTE)
                            continue;

                        int omitir = 0;

                        int preparado = listo(
                            plan, i, &omitir);

                        if (preparado < 0) {
                            bloqueado = 1;
                            salir = 1;
                            break;
                        }

                        if (preparado == 0)
                            continue;

                        if (omitir) {
                            trabajo->estado =
                                TRABAJO_OMITIDO;
                            restantes--;
                            fallo = 1;
                            continue;
                        }

                        trabajo->estado =
                            TRABAJO_EN_CURSO;

                        activos++;
                        elegido = i;
                        break;
                    }

                    if (elegido == plan->cantidad &&
                        activos == 0 && restantes)
                        bloqueado = 1;
                }
            }

            if (salir)
                break;

            if (elegido == plan->cantidad) {
                sched_yield();
                continue;
            }

            estado_trabajo_t resultado =
                ejecutar_trabajo(
                    &plan->trabajos[elegido]);

            #pragma omp critical(planificador_trabajos)
            {
                plan->trabajos[elegido].estado =
                    resultado;

                restantes--;
                activos--;

                if (resultado == TRABAJO_FALLIDO ||
                    resultado == TRABAJO_PARCIAL)
                    fallo = 1;
            }
        }
    }

    if (niveles_previos < 2)
        omp_set_max_active_levels(niveles_previos);

    return fallo || bloqueado || restantes ? -1 : 0;
}
