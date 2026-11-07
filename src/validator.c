
#include "validator.h"
#include "yaml_parser.h"
#include <stdio.h>

/* Adaptador heredado: la validacion completa del plan ahora es unica y
 * ocurre en parsear_pipeline_yaml, sin depender de libyaml. */
int validar_dependencias_yaml(const char *contenido, size_t longitud,
                              char *error, size_t capacidad)
{
    pipeline_t *plan = NULL;
    errores_yaml_t errores = {0};
    int rc = parsear_pipeline_yaml(contenido, longitud, &plan, &errores);
    if (error && capacidad) {
        if (rc && errores.cantidad)
            snprintf(error, capacidad, "%s: %s", errores.items[0].ubicacion,
                     errores.items[0].mensaje);
        else if (rc) snprintf(error, capacidad, "Error interno de YAML");
        else error[0] = '\0';
    }
    liberar_pipeline(plan);
    liberar_errores_yaml(&errores);
    return rc == 0 ? 0 : -1;
}
