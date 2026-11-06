
#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    PASO_SCRIPT, PASO_SLEEP, PASO_CPU,
    PASO_WORDCOUNT, PASO_HISTOGRAM
} tipo_paso_t;

typedef enum {
    TRABAJO_PENDIENTE, TRABAJO_EN_CURSO,
    TRABAJO_CORRECTO, TRABAJO_PARCIAL,
    TRABAJO_FALLIDO, TRABAJO_OMITIDO
} estado_trabajo_t;

typedef enum { COND_SUCCEEDED, COND_FAILED, COND_ALWAYS } condicion_t;

typedef struct {
    tipo_paso_t tipo;
    const char *comando;
    const char *archivo;
    const char *salida;
    const char *log_ruta;
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
    const atomic_int *cancelar;
} trabajo_t;

typedef struct {
    trabajo_t *trabajos;
    size_t cantidad;
} plan_trabajos_t;

/* Los indices de dependencias son relativos al plan. El parser debe
 * proveer un plan completo y mantener sus cadenas durante la ejecucion.
 * Cada trabajo se ejecuta una vez y los pasos se recorren en orden.
 * Devuelve 0 si todos terminan correctamente, -1 en los demas casos. */
int ejecutar_plan(plan_trabajos_t *plan, int agentes);

#endif


