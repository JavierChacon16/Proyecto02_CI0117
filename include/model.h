#ifndef MODEL_H
#define MODEL_H

/* Diseño v2: contrato para migracion; NO hay implementacion .c asociada. */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Serializacion exacta: queued, running, succeeded, partiallySucceeded,
 * failed, canceled, skipped. Las transiciones son responsabilidad del motor. */
typedef enum {
    QUEUED,
    RUNNING,
    SUCCEEDED,
    PARTIALLY_SUCCEEDED,
    FAILED,
    CANCELED,
    SKIPPED
} estado_t;

typedef enum {
    COND_SUCCEEDED,
    COND_FAILED,
    COND_ALWAYS
} condicion_t;

typedef enum {
    PASO_SCRIPT,
    PASO_SLEEP,
    PASO_CPU,
    PASO_WORDCOUNT,
    PASO_HISTOGRAM,
    PASO_PUBLICAR,
    PASO_DESCARGAR
} tipo_paso_t;

/* Definicion YAML inicial o input crudo de una tarea. El modulo vars es
 * propietario de las variables producidas en tiempo de ejecucion. */
typedef struct {
    char *nombre;
    char *valor;
} variable_definicion_t;

typedef struct paso {
    /* Definicion inmutable: no se expande $(X) durante el parseo. */
    tipo_paso_t tipo;
    char *comando;           /* script crudo, con posibles $(X) */
    char *tarea;             /* Sleep@1, Cpu@1, WordCount@1, Histogram@1 */
    variable_definicion_t *inputs; /* mapa de valores crudos; incluye numeros */
    size_t cantidad_inputs;
    bool continuar_si_falla;
    char *ruta_publicar;
    char *nombre_artefacto;
    char *ruta_descargar;

    /* Los inputs y comandos se sustituyen en el momento de ejecutar el paso.
     * Sus valores tipados (seconds, bins, threads, etc.) son LOCALES del agente,
     * no campos mutables de esta definicion compartida con status. */

    /* Estado de esta ejecucion; siempre protegido por servidor_t.mu. */
    estado_t estado;
    struct timespec inicio;
    struct timespec fin;
    int codigo_salida;      /* -1 si no existe codigo de proceso */
} paso_t;

typedef struct trabajo {
    /* Nombres, rangos y DAG inmutables despues de validar el YAML. */
    char *nombre;             /* unico dentro de su etapa */
    size_t indice_global;     /* posicion estable en pipeline_t.trabajos */
    size_t indice_etapa;
    size_t *dependencias;     /* DAG EFECTIVO: trabajos de etapa + job */
    size_t cantidad_dependencias;
    size_t *dependencias_condicion; /* solo dependsOn de job */
    size_t cantidad_dependencias_condicion;
    paso_t *pasos;            /* secuencia YAML */
    size_t cantidad_pasos;
    unsigned timeout_segundos; /* 0 = usar default configurable del servidor */
    condicion_t condicion;

    /* Bajo servidor_t.mu. CPU_RESULT NO se almacena aqui: va a vars. */
    estado_t estado;
    struct timespec inicio;
    struct timespec fin;
    size_t pasos_terminales;
    bool hubo_fallo_continuable;
    size_t siguiente_listo;   /* indice global o SIZE_MAX */
    bool en_cola_listos;

    /* Rutas preparadas antes de publicar el plan; inmutables en ejecucion. */
    char *directorio_trabajo;
    char *ruta_bitacora;
} trabajo_t;

typedef struct etapa {
    char *nombre;
    size_t indice;
    size_t *dependencias;       /* indices de etapas; condicion de etapa */
    size_t cantidad_dependencias;
    condicion_t condicion;
    size_t primer_trabajo;      /* rango contiguo en trabajos[] */
    size_t cantidad_trabajos;

    /* Bajo servidor_t.mu. */
    estado_t estado;
    struct timespec inicio;
    struct timespec fin;
    size_t trabajos_terminales;
} etapa_t;

typedef struct pipeline {
    /* Propiedad exclusiva de una ejecucion_t; nunca se comparte entre SUBMIT. */
    char *nombre;
    variable_definicion_t *variables; /* valores iniciales del YAML */
    size_t cantidad_variables;
    etapa_t *etapas;
    size_t cantidad_etapas;
    trabajo_t *trabajos;          /* indices globales estables para DAG/vars */
    size_t cantidad_trabajos;
} pipeline_t;

/* Contrato de propiedad; su implementacion se agregara durante la migracion. */
void liberar_pipeline(pipeline_t *plan);

#endif



