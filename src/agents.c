#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "protocol.h"
#include "util.h"
#include <errno.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

/* Equipo fijo OpenMP: cada miembro retira un trabajo de la cola global. */

/*
Ejecutar en paralelo en varios hilos OpenMP. 
Cada hilo retira un trabajo de la cola global y lo ejecuta. 
La función no retorna hasta que se le indique que apague el servidor.
*/
static void *ejecutar_pipeline(void *arg)
{
    servidor_t *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mu);// bloquear para acceder a la cola de trabajos
        while (!s->apagar && !s->primero)
            pthread_cond_wait(&s->cambio, &s->mu);// bloquear hasta que haya un trabajo disponible o se indique apagar
        if (s->apagar) { pthread_mutex_unlock(&s->mu); return NULL; }
        ejecucion_t *e = s->primero;
        s->primero = e->cola;// sacar el trabajo de la cola
        if (!s->primero) s->ultimo = NULL;
        e->cola = NULL;
        --s->n_cola;
        s->bytes_cola -= e->bytes;
        e->estado = RUNNING;
        clock_gettime(CLOCK_MONOTONIC, &e->inicio);// marcar el inicio de la ejecución
        ++s->activos;
        if (s->activos > s->max_activos) s->max_activos = s->activos;
        pthread_mutex_unlock(&s->mu);

        struct timespec demora = { .tv_sec = 2 };
        while (nanosleep(&demora, &demora) < 0 && errno == EINTR) {}

        pthread_mutex_lock(&s->mu);
        e->estado = SUCCEEDED;// marcar el trabajo como completado
        clock_gettime(CLOCK_MONOTONIC, &e->fin);
        free(e->yaml);
        e->yaml = NULL;
        --s->activos;
        pthread_cond_broadcast(&s->cambio);// notificar a otros hilos que un trabajo ha terminado
        pthread_mutex_unlock(&s->mu);
    }
}
/* Inicia los hilos OpenMP y los configura para ejecutar el pipeline. */
void *planificar(void *arg)
{
    servidor_t *s = arg;
    omp_set_dynamic(0);
    #pragma omp parallel num_threads(s->agentes) default(none) shared(s, stderr)
    {
        #pragma omp single
        {
            pthread_mutex_lock(&s->mu);
            int reales = omp_get_num_threads();
            if (reales < s->agentes)
                fprintf(stderr, "Aviso: OpenMP creo %d de %d agentes; limite efectivo %d\n",
                        reales, s->agentes, reales);
            s->agentes = reales;
            s->listo = 1;
            fprintf(stderr, "Agentes OpenMP disponibles: %d\n", reales);
            pthread_cond_broadcast(&s->cambio);
            pthread_mutex_unlock(&s->mu);
        }
        ejecutar_pipeline(s);
    }
    return NULL;
}
