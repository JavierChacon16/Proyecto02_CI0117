
#include "graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int buscar(const nodo_grafo_t *nodos, size_t cantidad,
                  const char *nombre)
{
    for (size_t i = 0; i < cantidad; i++) {
        if (strcmp(nodos[i].nombre, nombre) == 0)
            return (int)i;
    }

    return -1;
}

static int visitar(const nodo_grafo_t *nodos, size_t cantidad,
                   size_t actual, unsigned char *color,
                   char *error, size_t capacidad)
{
    color[actual] = 1;

    for (size_t i = 0;
         i < nodos[actual].cantidad_dependencias; i++) {

        int anterior = buscar(nodos, cantidad,
                              nodos[actual].depende[i]);

        if (anterior < 0)
            return -1;

        if (color[anterior] == 1) {
            snprintf(error, capacidad,
                     "Dependencia ciclica en '%s'",
                     nodos[actual].nombre);
            return -1;
        }

        if (color[anterior] == 0 &&
            visitar(nodos, cantidad, (size_t)anterior,
                    color, error, capacidad) != 0)
            return -1;
    }

    color[actual] = 2;
    return 0;
}

int validar_grafo(const nodo_grafo_t *nodos, size_t cantidad,
                  char *error, size_t capacidad)
{
    if (!error || capacidad == 0)
        return -1;

    error[0] = '\0';

    if (!nodos || cantidad == 0) {
        snprintf(error, capacidad, "El grafo esta vacio");
        return -1;
    }

    for (size_t i = 0; i < cantidad; i++) {
        if (!nodos[i].nombre || nodos[i].nombre[0] == '\0') {
            snprintf(error, capacidad,
                     "Nombre vacio en nodo %zu", i);
            return -1;
        }

        for (size_t j = 0; j < i; j++) {
            if (strcmp(nodos[i].nombre, nodos[j].nombre) == 0) {
                snprintf(error, capacidad,
                         "Nombre duplicado: %s",
                         nodos[i].nombre);
                return -1;
            }
        }

        if (nodos[i].cantidad_dependencias &&
            !nodos[i].depende) {
            snprintf(error, capacidad,
                     "Dependencias invalidas en %s",
                     nodos[i].nombre);
            return -1;
        }

        for (size_t j = 0;
             j < nodos[i].cantidad_dependencias; j++) {

            const char *nombre = nodos[i].depende[j];

            if (!nombre ||
                buscar(nodos, cantidad, nombre) < 0) {
                snprintf(error, capacidad,
                         "Dependencia inexistente en %s",
                         nodos[i].nombre);
                return -1;
            }

            for (size_t k = 0; k < j; k++) {
                if (strcmp(nombre, nodos[i].depende[k]) == 0) {
                    snprintf(error, capacidad,
                             "Dependencia repetida en %s",
                             nodos[i].nombre);
                    return -1;
                }
            }
        }
    }

    unsigned char *color = calloc(cantidad, sizeof *color);

    if (!color) {
        snprintf(error, capacidad,
                 "Sin memoria para validar el grafo");
        return -1;
    }

    int resultado = 0;

    for (size_t i = 0; i < cantidad; i++) {
        if (color[i] == 0 &&
            visitar(nodos, cantidad, i, color,
                    error, capacidad) != 0) {
            resultado = -1;
            break;
        }
    }

    free(color);
    return resultado;
}
