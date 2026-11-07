
#ifndef SCRIPT_H
#define SCRIPT_H

/* Diseño v2: un AGENTE invoca y drena dos pipes con poll(), sin hilos
 * lectores adicionales. Aun no hay implementacion .c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>

#define SCRIPT_MAX_LINEA_BYTES 65536U

typedef enum {
    SCRIPT_CANAL_STDOUT,
    SCRIPT_CANAL_STDERR
} script_canal_t;

/* 'datos' solo vive durante el callback; longitud puede incluir NUL.
 * fin_de_linea indica que llego '\n'; un ultimo fragmento de EOF puede no
 * terminar en '\n'. fragmentada=1 indica linea > SCRIPT_MAX_LINEA_BYTES:
 * se conserva en logs por fragmentos pero NO se acepta ##setvariable.
 * Solo STDOUT puede producir ##setvariable; STDERR nunca. */
typedef struct {
    script_canal_t canal;
    const char *datos;
    size_t longitud;
    bool fin_de_linea;
    bool fragmentada;
} script_linea_t;

/* Invocado en el hilo del agente. Devuelve 0 si se acepto el registro;
 * !=0 es error interno de captura, que fuerza terminar/recolectar el hijo. */
typedef int (*script_linea_callback_t)(void *contexto,
                                       const script_linea_t *linea);

typedef struct {
    const char *comando;               /* ya sustituido por el agente */
    const char *directorio_trabajo;    /* cwd privado, no chdir() global */
    const struct timespec *plazo;      /* deadline absoluto CLOCK_MONOTONIC */
    const atomic_int *cancelar;        /* cancelar ejecucion */
    script_linea_callback_t al_linea; /* stdout/stderr en caliente */
    void *contexto;
} script_opciones_t;

typedef struct {
    int codigo_salida;      /* -1 si no hubo WIFEXITED */
    bool salio_normalmente;
} script_resultado_t;

typedef enum {
    SCRIPT_OK = 0,
    SCRIPT_FALLO = -1,
    SCRIPT_TIMEOUT = -2,
    SCRIPT_CANCELADO = -3,
    SCRIPT_ERROR_CAPTURA = -4
} script_estado_t;

/* posix_spawn en grupo de procesos propio. Captura stdout y stderr en pipes
 * separados y los drena en el agente con poll() no bloqueante; acumula
 * lineas completas o fragmentos acotados. Registra via callback y permite
 * detectar ##setvariable solo en STDOUT sin esperar al fin del script.
 * Comprueba cancelacion/deadline mientras drena, mata el grupo al abortar,
 * cierra FDs y hace waitpid antes de retornar. No sostiene servidor_t.mu.
 * El callback agrega logs y variables mediante locks separados. */
int ejecutar_script_capturado(const script_opciones_t *opciones,
                             script_resultado_t *resultado);

/* API original conservada solo para compilacion de tests existentes hasta
 * su migracion. NO ofrece eventos de stdout ni setvariable en caliente. */
int ejecutar_script_controlado(const char *comando, unsigned timeout_seg,
                               int *salida, const char *log_ruta,
                               const atomic_int *cancelar);
int ejecutar_script(const char *comando, unsigned timeout_seg, int *salida);

#endif
