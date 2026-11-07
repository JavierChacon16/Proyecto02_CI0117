
#ifndef STATE_H
#define STATE_H

/* Diseño v2: contratos del servidor, sin implementaciones .c. */

#include "model.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

#define MAX_CLIENTES 256
#define MAX_COLA 1000
#define MAX_BYTES_COLA ((size_t)256 * 1024 * 1024)
#define PLAZO_LECTURA 10
#define APAGADO_GRACIA_SEGUNDOS 2
#define TIMEOUT_TRABAJO_DEFECTO_SEGUNDOS 300U
#define TIMEOUT_TRABAJO_MAX_SEGUNDOS 86400U

/* Valores de retorno de cancelar_ejecucion().
 * El cero significa peticion aceptada, NO que el agente ya haya terminado. */
typedef enum {
    CANCELAR_ERROR = -1,
    CANCELAR_OK = 0,
    CANCELAR_NOT_FOUND = 1,
    CANCELAR_ESTADO_TERMINAL = 2
} cancelacion_resultado_t;

/* No se presupone el contenido ni los locks concretos de estos modulos. */
struct bitacoras_ejecucion;
struct variables_ejecucion;
struct artefactos_ejecucion;

typedef struct ejecucion {
    int id;
    const char *nombre;          /* alias a plan->nombre; no liberar aparte */
    pipeline_t *plan;            /* propietario unico */
    char *yaml;                  /* temporal durante admision, puede ser NULL */
    size_t bytes;

    /* Estados, tiempos, FIFO y enlaces bajo servidor_t.mu. */
    estado_t estado;
    struct timespec aceptada_en;
    struct timespec inicio;
    struct timespec fin;
    size_t trabajos_terminales;
    size_t trabajos_activos;
    size_t primero_listo;        /* indice job o SIZE_MAX */
    size_t ultimo_listo;
    size_t cantidad_listos;
    int en_rr;
    struct ejecucion *siguiente;
    struct ejecucion *rr_siguiente;

    /* Escritura por CANCEL/SHUTDOWN; lectura sin mu por agentes y tareas. */
    atomic_int cancelar_solicitado;

    struct bitacoras_ejecucion *bitacoras;
    struct variables_ejecucion *variables;
    struct artefactos_ejecucion *artefactos;
} ejecucion_t;

typedef struct cliente cliente_t;

typedef struct servidor {
    pthread_mutex_t mu;      /* estados, contadores, colas, presupuesto */
    pthread_mutex_t mu_id;   /* solo proximo_id; carrera SIN_PROTECCION */
    pthread_cond_t cambio;   /* cambios del planificador y finalizacion */

    ejecucion_t *ejecuciones;
    ejecucion_t *rr_primero;
    ejecucion_t *rr_ultimo;
    cliente_t *clientes;
    unsigned int proximo_id;

    /* Bajo mu. El apagado comienza al recibir SHUTDOWN/SIGINT/SIGTERM. */
    int apagar;
    struct timespec apagado_iniciado;
    struct timespec plazo_gracia; /* CLOCK_MONOTONIC; +2 s exactos */
    int cancelar_activos_apagado; /* se activa al vencer la gracia */

    int agentes_configurados;
    int agentes;              /* A = equipo externo OpenMP REAL */
    int activos;              /* trabajos ejecutandose, 0 <= activos <= A */
    int max_activos;
    int limite_hilos_total;   /* B efectivo, <= OMP_THREAD_LIMIT; no <= CPUs */
    int extras_libres;        /* al inicio: max(0, B - A) */
    unsigned timeout_defecto_segundos; /* 1..86400; configurable */
    size_t trabajos_listos;
    size_t n_clientes;
    size_t n_cola;            /* reservas SUBMIT + ejecuciones sin comenzar */
    size_t bytes_cola;
    int listo;

    const char *directorio;   /* inmutable por vida del servidor */
} servidor_t;

struct cliente {
    int fd;
    struct timespec vencimiento;
    servidor_t *servidor;
    cliente_t *siguiente;
};

/* Configuracion alternativa explicita para futuro CLI; 0 en B significa
 * presupuesto automatico, NO ilimitado. Timeout siempre >0.
 * La funcion historica iniciar_servidor() lee la variable de entorno
 * PIPELINED_JOB_TIMEOUT_SECONDS; su ausencia utiliza 300 segundos. */
typedef struct {
    int puerto;
    int agentes;
    const char *directorio;
    int limite_hilos_total;      /* 0=auto, >0=presupuesto solicitado */
    unsigned timeout_defecto_segundos;
} servidor_config_t;

/* Interfaz red/motor: propuestas para la migracion posterior. */
int error_cliente(int fd, const char *codigo, const char *mensaje);
int submit(cliente_t *c, size_t bytes);
int consultar(cliente_t *c, int id, int lista);

/* Referencia vacia = logs de la ejecucion; trabajo o etapa/trabajo.
 * Si nombre simple identifica mas de un trabajo: ERR AMBIGUOUS. */
int consultar_logs(cliente_t *c, int id, const char *referencia, int follow);

/* Devuelve solo los valores declarados por cancelacion_resultado_t. */
int cancelar_ejecucion(servidor_t *s, int id);

void *planificar(void *arg);
int iniciar_servidor(int puerto, int agentes, const char *directorio);
int iniciar_servidor_configurado(const servidor_config_t *config);

#endif


