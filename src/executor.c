#define _POSIX_C_SOURCE 200809L
#include "motor.h"
#include "script.h"
#include "tasks.h"
#include "vars.h"
#include "artifacts.h"
#include "logs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

estado_t ejecutor_real(servidor_t *s, ejecucion_t *e, trabajo_t *t, void *contexto)
{
    (void)contexto;
    pipeline_t *plan = e->plan;
    etapa_t *et = &plan->etapas[t->indice_etapa];

    for (size_t i = 0; i < t->cantidad_pasos; ++i) {
        paso_t *p = &t->pasos[i];
        if (atomic_load(&e->cancelar_solicitado) || s->apagar) {
            motor_paso_finalizar(s, e, t->indice_global, i, CANCELED, -1);
            return CANCELED;
        }

        motor_paso_iniciar(s, e, t->indice_global, i);
        int codigo_salida = 0;
        estado_t resultado = SUCCEEDED;

        if (p->tipo == PASO_SCRIPT) {
            char *cmd_interpolado = resolver_variables_texto(plan, p->comando);
            int rc = ejecutar_script_controlado(cmd_interpolado, t->timeout_segundos, &codigo_salida, NULL, &e->cancelar_solicitado);
            free(cmd_interpolado);
            if (rc == -3) resultado = CANCELED;
            else if (rc != 0) resultado = FAILED;
        } else if (p->tipo == PASO_SLEEP) {
            // Lógica para tarea sleep
        } else if (p->tipo == PASO_PUBLICAR) {
            if (artifacts_publicar(s->directorio, e->id, p->ruta_publicar, p->nombre_artefacto) != 0)
                resultado = FAILED;
        } else if (p->tipo == PASO_DESCARGAR) {
            if (artifacts_descargar(s->directorio, e->id, p->nombre_artefacto, p->ruta_descargar) != 0)
                resultado = FAILED;
        }
        
        motor_paso_finalizar(s, e, t->indice_global, i, resultado, codigo_salida);
        if (resultado == FAILED && !p->continuar_si_falla) {
            return FAILED;
        }
    }
    return SUCCEEDED;
}