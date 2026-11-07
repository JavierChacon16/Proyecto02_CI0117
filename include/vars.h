#ifndef VARS_H
#define VARS_H

#include "state.h"


const char *obtener_variable(const pipeline_t *plan, const char *nombre);
char *resolver_variables_texto(const pipeline_t *plan, const char *texto);

#endif