
#ifndef YAML_PARSER_H
#define YAML_PARSER_H

#include <stddef.h>

/* Devuelve 0 si la estructura es valida, -1 si hay errores. */
int validar_yaml(const char *texto, size_t longitud,
                 char *error, size_t capacidad);

#endif
