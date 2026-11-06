
#ifndef TASKS_H
#define TASKS_H

#include <stddef.h>
#include <stdint.h>

/* Devuelven 0 si terminan correctamente y -1 si hay error. */

int tarea_sleep(double segundos);

int tarea_cpu(uint64_t iteraciones, uint64_t semilla,
              int hilos, uint64_t *resultado);

int tarea_histogram(const char *archivo, unsigned bins,
                    int hilos, const char *salida);

int tarea_wordcount(const char *archivo, int hilos,
                    const char *salida, size_t top);

#endif
