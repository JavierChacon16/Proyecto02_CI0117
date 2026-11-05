
#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PASO_SCRIPT,
    PASO_SLEEP,
    PASO_CPU,
    PASO_WORDCOUNT,
    PASO_HISTOGRAM
} tipo_paso_t;

typedef enum {
    TRABAJO_PENDIENTE,
    TRABAJO_EN_CURSO,
    TRABAJO_CORRECTO,
    TRABAJO_PARCIAL,
    TRABAJO_FALLIDO,
    TRABAJO_OMITIDO
} estado_trabajo_t;

typedef enum {
    COND_SUCCEEDED,
    COND_FAILED,
    COND_ALWAYS
} condicion_t;

typedef struct {
    tipo_paso_t tipo;

    const char *comando;
    const char *archivo;
    const char *salida;

    double segundos;
    uint64_t iteraciones;
    uint64_t semilla;

    unsigned bins;
    int hilos;
    size_t top;

    int continuar_si_falla;
} paso_t;

typedef struct {
    const char *nombre;

    const size_t *dependencias;
    size_t cantidad_dependencias;

    const paso_t *pasos;
    size_t cantidad_pasos;

    unsigned timeout_segundos;
    condicion_t condicion;

    estado_trabajo_t estado;
    uint64_t resultado_cpu;
} trabajo_t;

typedef struct {
    trabajo_t *trabajos;
    size_t cantidad;
} plan_trabajos_t;

/* Ejecuta los trabajos recibidos utilizando agentes OpenMP.
 * Devuelve 0 si todos terminaron correctamente.
 * Devuelve -1 ante fallos o dependencias inconsistentes.
 */
int ejecutar_plan(plan_trabajos_t *plan, int agentes);

#endif
