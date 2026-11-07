#define _POSIX_C_SOURCE 200809L
#include "vars.h"
#include <stdlib.h>
#include <string.h>

const char *obtener_variable(const pipeline_t *plan, const char *nombre)
{
    if (!plan || !nombre) return NULL;
    for (size_t i = 0; i < plan->cantidad_variables; ++i) {
        if (plan->variables[i].nombre &&
            strcmp(plan->variables[i].nombre, nombre) == 0) {
            return plan->variables[i].valor;
        }
    }

    return NULL;
}

char *resolver_variables_texto(const pipeline_t *plan, const char *texto)
{
if (!texto) return NULL;
if (!strstr(texto, "$(")) return strdup(texto);

    size_t capacidad_salida = strlen(texto) * 2 + 64;
    char *resultado = malloc(capacidad_salida);
    if (!resultado) return NULL;
    resultado[0] = '\0';

    const char *p = texto;
    size_t len_res = 0;

    while (*p) {
        
        if (*p == '$' && p[1] == '(') {
        const char *inicio_nombre = p + 2;
        const char *fin_nombre = strchr(inicio_nombre, ')');

            if (fin_nombre) {
            size_t len_nombre = (size_t)(fin_nombre - inicio_nombre);
            char nombre_var[128];

                if (len_nombre < sizeof(nombre_var)) {
                memcpy(nombre_var, inicio_nombre, len_nombre);
                nombre_var[len_nombre] = '\0';
                const char *val = obtener_variable(plan, nombre_var);

                    if (val) {
                    size_t len_val = strlen(val);
                    
                        if (len_res + len_val >= capacidad_salida) {
                        capacidad_salida = len_res + len_val + 128;
                        char *temp = realloc(resultado, capacidad_salida);
                        
                        if (!temp) { free(resultado); return NULL; }
                        resultado = temp;
                        }
                        strcpy(resultado + len_res, val);
                        len_res += len_val;
                    }
                    p = fin_nombre + 1;
                    continue;
                }
            }
        }
if (len_res + 1 >= capacidad_salida) {
    capacidad_salida += 128;
    char *temp = realloc(resultado, capacidad_salida);
    if (!temp) { free(resultado); return NULL; }
    resultado = temp;
}
    resultado[len_res++] = *p++;
    resultado[len_res] = '\0';
}
return resultado;
}