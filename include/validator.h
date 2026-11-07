
#ifndef VALIDATOR_H
#define VALIDATOR_H

#include <stddef.h>

/* Valida condiciones y dependencias entre etapas y trabajos. */
int validar_dependencias_yaml(const char *contenido, size_t longitud,
                              char *error, size_t capacidad);

#endif
