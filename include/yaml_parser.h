#ifndef YAML_PARSER_H
#define YAML_PARSER_H

#include "model.h"
#include <stdbool.h>
#include <stddef.h>

/* Limites obligatorios de la propuesta v2, aplicados al YAML y a los
 * valores de task.inputs despues de expandir $(X). */
#define YAML_MAX_ERRORES 50U
#define YAML_MAX_PROFUNDIDAD 32U
#define YAML_MAX_THREADS 1024U
#define YAML_MAX_BINS 256U
#define YAML_MAX_SECONDS 86400U

/* Cada ubicacion es segura como token del protocolo (sin espacios):
 * L12:C5:stages[1].jobs[0].steps[2].inputs.seconds
 * Los contadores linea/columna empiezan en 1; si no hay ubicacion,
 * usar L0:C0:yaml. La ruta solo utiliza [A-Za-z0-9_.\[\]-]. */
typedef struct {
    size_t linea;
    size_t columna;
    char *ruta;             /* p.ej. stages[1].jobs[0] */
    char *ubicacion;        /* p.ej. L12:C5:stages[1].jobs[0] */
    char *mensaje;
} error_yaml_t;

typedef struct {
    error_yaml_t *items;
    size_t cantidad;         /* 0..YAML_MAX_ERRORES */
    bool truncados;          /* true si hubo mas de 50 errores */
} errores_yaml_t;

/* Contrato del parser/validador (implementacion del equipo YAML):
 * - texto contiene longitud bytes sin exigir NUL final.
 * - validar estructura, nombres, claves, DAG y ciclos; tareas permitidas
 *   solamente Sleep@1/Cpu@1/WordCount@1/Histogram@1.
 * - threads entero en 1..1024, bins entero en 1..256, seconds real
 *   finito en 0..86400; timeoutInSeconds entero en 1..86400 cuando exista.
 * - los valores con $(X) se guardan CRUDOS hasta ejecutar el paso; en
 *   runtime se repiten las validaciones de tipo y rango tras sustituir.
 * - rechazar cualquier ancla/alias YAML, claves de merge, profundidad >32,
 *   y rutas absolutas o con segmento '..' (tambien tras interpolacion).
 * - diagnosticos independientes hasta 50, cada uno con ubicacion sin espacios.
 * - comprobar publish duplicado y download sin productor/dependencia.
 * - construir pipeline_t con asignacion y propiedad independientes del input.
 * - no publicar plan parcial. En error, *salida queda NULL.
 * Retorno: 0 exito (errores vacios), -1 YAML invalido, -2 error interno. */
int parsear_pipeline_yaml(const char *texto, size_t longitud,
                         pipeline_t **salida, errores_yaml_t *errores);

void liberar_errores_yaml(errores_yaml_t *errores);

/* Durante migracion: la implementacion antigua solo valida y comunica
 * un error; no sustituye parsear_pipeline_yaml ni satisface el contrato v2. */
int validar_yaml(const char *texto, size_t longitud,
                 char *error, size_t capacidad);

#endif
