#define _POSIX_C_SOURCE 200809L
#include "logs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

struct entrada {
    char *etapa;
    char *trabajo;
    unsigned char *datos;
    size_t longitud;
};

struct bitacoras_ejecucion {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct entrada *entradas;
    size_t cantidad_entradas;
    size_t capacidad_entradas;
    int finalizado;
};

bitacoras_ejecucion_t *bitacoras_crear(void)
{
    bitacoras_ejecucion_t *b = malloc(sizeof(*b));
    if (!b) return NULL;
    if (pthread_mutex_init(&b->mutex, NULL) != 0) {
        free(b);
        return NULL;
    }

    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex);
        free(b);
        return NULL;
    }

    b->entradas = NULL;
    b->cantidad_entradas = 0;
    b->capacidad_entradas = 0;
    b->finalizado = 0;
    return b;
}

void bitacoras_destruir(bitacoras_ejecucion_t *b)
{
    if (!b) 
    return;

    pthread_mutex_lock(&b->mutex);
    b->finalizado = 1;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);

    for (size_t i = 0; i < b->cantidad_entradas; i++) {
        free(b->entradas[i].etapa);
        free(b->entradas[i].trabajo);
        free(b->entradas[i].datos);
    }

    free(b->entradas);
    pthread_cond_destroy(&b->cond);
    pthread_mutex_destroy(&b->mutex);
    free(b);
}

int bitacoras_agregar(bitacoras_ejecucion_t *b, const char *etapa,const char *trabajo, const void *datos, size_t longitud)
{
    if (!b || (!datos && longitud > 0)) 
    return -1;

    pthread_mutex_lock(&b->mutex);
    if (b->finalizado) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    if (b->cantidad_entradas >= b->capacidad_entradas) {
        size_t nueva_cap = b->capacidad_entradas ? b->capacidad_entradas * 2 : 16;
        struct entrada *tmp = realloc(b->entradas, nueva_cap * sizeof(*b->entradas));
        if (!tmp) {
            pthread_mutex_unlock(&b->mutex);
            return -1;
        }
        b->entradas = tmp;
        b->capacidad_entradas = nueva_cap;
    }

    struct entrada *e = &b->entradas[b->cantidad_entradas];
    e->etapa = etapa ? strdup(etapa) : NULL;
    e->trabajo = trabajo ? strdup(trabajo) : NULL;
    e->longitud = longitud;
    e->datos = malloc(longitud > 0 ? longitud : 1);

    if (!e->datos && longitud > 0) {
        free(e->etapa);
        free(e->trabajo);
        pthread_mutex_unlock(&b->mutex);
    return -1;
    }
    if (longitud > 0 && datos) {
        memcpy(e->datos, datos, longitud);
    }

    b->cantidad_entradas++;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);

    return 0;
}

void bitacoras_finalizar(bitacoras_ejecucion_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->mutex);
    b->finalizado = 1;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);
}

int bitacoras_leer(bitacoras_ejecucion_t *b, const char *referencia,size_t *posicion, void *destino, size_t capacidad,unsigned esperar_ms, size_t *leidos, int *finalizado)
{
if (!b || !posicion || !destino || !leidos || !finalizado) 
return -1;

*leidos = 0;

const char *ref_etapa = NULL;
const char *ref_trabajo = NULL;
char ref_copia[256];

if (referencia && referencia[0] != '\0') {
    if (strlen(referencia) >= sizeof(ref_copia)) return -1;
    snprintf(ref_copia, sizeof(ref_copia), "%s", referencia);
    char *slash = strchr(ref_copia, '/');

if (slash) {
        *slash = '\0';
        ref_etapa = ref_copia;
        ref_trabajo = slash + 1;
    } else {
        ref_trabajo = ref_copia;
    }
}

pthread_mutex_lock(&b->mutex);

struct timespec timeout;
int usar_timeout = 0;
if (esperar_ms > 0) {
    clock_gettime(CLOCK_REALTIME, &timeout);
    unsigned long long ns = timeout.tv_nsec + (unsigned long long)esperar_ms * 1000000ULL;
    timeout.tv_sec += ns / 1000000000ULL;
    timeout.tv_nsec = ns % 1000000000ULL;
    usar_timeout = 1;
}

for (;;) {
if (ref_trabajo && !ref_etapa) {
    const char *primera_etapa = NULL;
    for (size_t i = 0; i < b->cantidad_entradas; i++) {
    struct entrada *e = &b->entradas[i];
    if (e->trabajo && strcmp(e->trabajo, ref_trabajo) == 0) {
        if (!primera_etapa) {
            primera_etapa = e->etapa;
        } else if (e->etapa && primera_etapa && strcmp(e->etapa, primera_etapa) != 0) {
            pthread_mutex_unlock(&b->mutex);
        return -2;
        }
    }
    }
}

size_t stream_offset = 0;
size_t bytes_copiados = 0;
size_t pos_actual = *posicion;

for (size_t i = 0; i < b->cantidad_entradas; i++) {
    struct entrada *e = &b->entradas[i];
    
    int coincide = 0;
    if (!referencia || referencia[0] == '\0') {
        coincide = 1;
    } else if (ref_etapa && ref_trabajo) {
        if (e->etapa && e->trabajo &&
            strcmp(e->etapa, ref_etapa) == 0 &&
            strcmp(e->trabajo, ref_trabajo) == 0) {
            coincide = 1;
        }
    } else if (ref_trabajo) {
        if (e->trabajo && strcmp(e->trabajo, ref_trabajo) == 0) {
            coincide = 1;
        }
    }

if (coincide) {
size_t len = e->longitud;
if (stream_offset + len <= pos_actual) {
    stream_offset += len;
continue;
}

size_t inicio_en_entrada = 0;
if (stream_offset < pos_actual) {
    inicio_en_entrada = pos_actual - stream_offset;
}

size_t bytes_a_copiar = len - inicio_en_entrada;
if (bytes_copiados + bytes_a_copiar > capacidad) {
    bytes_a_copiar = capacidad - bytes_copiados;
}

if (bytes_a_copiar > 0) {
    memcpy((char *)destino + bytes_copiados, e->datos + inicio_en_entrada, bytes_a_copiar);
    bytes_copiados += bytes_a_copiar;
}

stream_offset += len;
if (bytes_copiados >= capacidad) {
break;
}
}
}

*leidos = bytes_copiados;
*finalizado = b->finalizado ? 1 : 0;

if (bytes_copiados > 0 || b->finalizado || esperar_ms == 0) {
    *posicion += bytes_copiados;
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int rc = 0;
if (usar_timeout) {
    rc = pthread_cond_timedwait(&b->cond, &b->mutex, &timeout);
    if (rc == ETIMEDOUT) {
        *finalizado = b->finalizado ? 1 : 0;
        pthread_mutex_unlock(&b->mutex);
    return 0;
    }
} else {
    pthread_cond_wait(&b->cond, &b->mutex);
}

if (b->finalizado && bytes_copiados == 0) {
    *finalizado = 1;
    pthread_mutex_unlock(&b->mutex);
return 0;
}
}
}