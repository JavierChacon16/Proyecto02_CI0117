#ifndef VARS_H
#define VARS_H

#include "state.h"

/* Cada ejecucion conserva variables por trabajo bajo un mutex propio.
 * Solo los ancestros ya finalizados publican variables a sus dependientes. */
typedef struct variables_ejecucion variables_ejecucion_t;
variables_ejecucion_t *vars_crear(const pipeline_t *plan);
void vars_destruir(variables_ejecucion_t *variables);
int vars_definir(variables_ejecucion_t *vars, size_t trabajo,
                 const char *nombre, const char *valor);
void vars_publicar(variables_ejecucion_t *vars, size_t trabajo);
char *vars_resolver(const pipeline_t *plan, variables_ejecucion_t *vars,
                    size_t trabajo, const char *texto);

/* API historica: solo variables declaradas en YAML. */
const char *obtener_variable(const pipeline_t *plan, const char *nombre);
char *resolver_variables_texto(const pipeline_t *plan, const char *texto);
#endif
