#include "model.h"
#include <stdlib.h>

/* El pipeline es propiedad exclusiva de una ejecucion.
 * Se destruye solo despues de que hayan terminado todos sus agentes y lectores. */
void liberar_pipeline(pipeline_t *plan)
{
    if (!plan) return;
    free(plan->nombre);
    for (size_t i = 0; i < plan->cantidad_variables; ++i) {
        free(plan->variables[i].nombre);
        free(plan->variables[i].valor);
    }
    free(plan->variables);
    for (size_t i = 0; i < plan->cantidad_etapas; ++i) {
        free(plan->etapas[i].nombre);
        free(plan->etapas[i].dependencias);
    }
    free(plan->etapas);
    for (size_t i = 0; i < plan->cantidad_trabajos; ++i) {
        trabajo_t *t = &plan->trabajos[i];
        free(t->nombre);
        free(t->dependencias);
        free(t->dependencias_condicion);
        free(t->directorio_trabajo);
        free(t->ruta_bitacora);
        for (size_t j = 0; j < t->cantidad_pasos; ++j) {
            paso_t *p = &t->pasos[j];
            free(p->comando);
            free(p->tarea);
            free(p->ruta_publicar);
            free(p->nombre_artefacto);
            free(p->ruta_descargar);
            for (size_t k = 0; k < p->cantidad_inputs; ++k) {
                free(p->inputs[k].nombre);
                free(p->inputs[k].valor);
            }
            free(p->inputs);
        }
        free(t->pasos);
    }
    free(plan->trabajos);
    free(plan);
}
