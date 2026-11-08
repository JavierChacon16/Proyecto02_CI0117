
#ifndef GRAPH_H
#define GRAPH_H

#include <stddef.h>

/* Cada nodo representa una etapa o un trabajo. */
typedef struct {
    const char *nombre;
    const char *const *depende;
    size_t cantidad_dependencias;
} nodo_grafo_t;

/* Devuelve 0 si es valido y -1 si hay errores. */
int validar_grafo(const nodo_grafo_t *nodos, size_t cantidad,
                  char *error, size_t capacidad);

#endif
