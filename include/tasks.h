#ifndef TASKS_H
#define TASKS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/* NULL = tarea sin cancelacion y sin deadline (compatibilidad anterior).
 * deadline={0,0} = sin plazo. Si se especifica es absoluto CLOCK_MONOTONIC. */
typedef struct {
    const atomic_int *cancelar;
    struct timespec deadline;
} tarea_contexto_t;

typedef enum {
    TAREA_OK = 0,
    TAREA_FALLO = -1,
    TAREA_TIMEOUT = -2,
    TAREA_CANCELADO = -3
} tarea_estado_t;

/* API para el ejecutor real (Fase 4). Comprueba cancelacion y plazo en
 * tramos acotados. No conserva reservas de hilos: son propiedad del motor. */
int tarea_sleep_controlada(double segundos, const tarea_contexto_t *ctx);
int tarea_cpu_controlada(uint64_t iteraciones, uint64_t semilla,
                         int hilos, uint64_t *resultado,
                         const tarea_contexto_t *ctx);
int tarea_histogram_controlada(const char *archivo, unsigned bins,
                               int hilos, const char *salida,
                               const tarea_contexto_t *ctx);
int tarea_wordcount_controlada(const char *archivo, int hilos,
                               const char *salida, size_t top,
                               const tarea_contexto_t *ctx);

/* API historica: mantiene las pruebas y clientes anteriores. */
int tarea_sleep(double segundos);
int tarea_cpu(uint64_t iteraciones, uint64_t semilla,
              int hilos, uint64_t *resultado);
int tarea_histogram(const char *archivo, unsigned bins,
                    int hilos, const char *salida);
int tarea_wordcount(const char *archivo, int hilos,
                    const char *salida, size_t top);

#endif
