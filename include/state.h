#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <pthread.h>
#include <time.h>

/* Estado compartido: mu; IDs: mu_id, salvo SIN_PROTECCION.
 * El YAML esta simulado porque no se ha hecho aun. */
#define MAX_CLIENTES 256
#define MAX_COLA 1000
#define MAX_BYTES_COLA ((size_t)256 * 1024 * 1024)
#define PLAZO_LECTURA 10

typedef enum { QUEUED, RUNNING,SUCCEEDED, PARTIALLY_SUCCEEDED,FAILED,CANCELED,SKIPPED } estado_t;
typedef struct ejecucion {
    int id;
    estado_t estado;
    char nombre[128];
    char *yaml;
    size_t bytes;
    struct timespec inicio, fin;
    struct ejecucion *siguiente, *cola;
} ejecucion_t;


typedef struct cliente cliente_t;

typedef struct {
    pthread_mutex_t mu, mu_id;
    pthread_cond_t cambio;
    ejecucion_t *ejecuciones, *primero, *ultimo;
    cliente_t *clientes;
    unsigned int proximo_id;
    int apagar, agentes, activos, max_activos;
    size_t n_clientes, n_cola, bytes_cola;
    /* n_cola/bytes_cola incluyen reservas de SUBMIT en recepcion. */
    int listo;
    const char *directorio; /* sin cambios durante la vida de los hilos. */
} servidor_t;
struct cliente {
    int fd;
    struct timespec vencimiento; 
    servidor_t *servidor;
    cliente_t *siguiente;
};


/* Interfaces entre modulos. Los helpers privados permanecen static. */
int error_cliente(int fd, const char *codigo, const char *mensaje);
int submit(cliente_t *c, size_t bytes);
int consultar(cliente_t *c, int id, int lista);
void *planificar(void *arg);
int iniciar_servidor(int puerto, int agentes, const char *directorio);

#endif
