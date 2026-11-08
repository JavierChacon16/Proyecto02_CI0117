
#ifndef LOGS_H
#define LOGS_H

#include <stddef.h>

/* Bitacoras por ejecucion. Las entradas mantienen el orden de llegada.
 * El propietario debe detener los agentes antes de destruir el registro. */
typedef struct bitacoras_ejecucion bitacoras_ejecucion_t;

bitacoras_ejecucion_t *bitacoras_crear(void);
void bitacoras_destruir(bitacoras_ejecucion_t *b);

/* Agrega datos binarios, sin exigir fin de linea. */
int bitacoras_agregar(bitacoras_ejecucion_t *b, const char *etapa,
                     const char *trabajo, const void *datos, size_t longitud);

/* Despierta a quienes siguen una bitacora al terminar la ejecucion. */
void bitacoras_finalizar(bitacoras_ejecucion_t *b);

/* Lee desde 'posicion' en el flujo filtrado.
 * referencia: NULL/"" = toda la ejecucion; "job" = trabajo unico;
 * "etapa/job" = trabajo especifico.
 * esperar_ms = 0: lectura inmediata; >0: espera hasta que haya datos
 * o finalice la ejecucion. El cursor cuenta bytes del flujo filtrado.
 * Devuelve 0 correcto, -1 error, -2 nombre de trabajo ambiguo.
 * 'finalizado' se activa cuando no habra mas entradas. */
int bitacoras_leer(bitacoras_ejecucion_t *b, const char *referencia,
                   size_t *posicion, void *destino, size_t capacidad,
                   unsigned esperar_ms, size_t *leidos, int *finalizado);

#endif
