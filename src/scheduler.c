
#define _POSIX_C_SOURCE 200809L

#include "state.h"
#include "protocol.h"
#include "util.h"
#include "yaml_parser.h"
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

static int siguiente_id(servidor_t *s)
{
#ifndef SIN_PROTECCION
    pthread_mutex_lock(&s->mu_id);
#endif

    unsigned int id = s->proximo_id;

#ifdef SIN_PROTECCION
    /* Carrera intencional para la demostracion. */
    sched_yield();
#endif

    if (id <= INT_MAX)
        s->proximo_id = id + 1U;

#ifndef SIN_PROTECCION
    pthread_mutex_unlock(&s->mu_id);
#endif

    return id <= INT_MAX ? (int)id : 0;
}

static void liberar_reserva(servidor_t *s, size_t bytes)
{
    pthread_mutex_lock(&s->mu);
    --s->n_cola;
    s->bytes_cola -= bytes;
    pthread_mutex_unlock(&s->mu);
}

static const char *estado_texto(estado_t estado)
{
    switch (estado) {
    case QUEUED: return "queued";
    case RUNNING: return "running";
    case SUCCEEDED: return "succeeded";
    case CANCELED: return "canceled";
    }

    return "failed";
}

static void nombre_provisional(ejecucion_t *e)
{
    strcpy(e->nombre, "sin_nombre");

    const char *p = e->yaml;

    while (*p) {
        const char *fin = strchr(p, '\n');

        if (!fin)
            fin = p + strlen(p);

        if (fin - p >= 5 && strncmp(p, "name:", 5) == 0) {
            const char *v = p + 5;

            while (v < fin && (*v == ' ' || *v == '\t'))
                ++v;

            while (fin > v &&
                   (fin[-1] == ' ' || fin[-1] == '\r'))
                --fin;

            if (fin - v >= 2 &&
                ((*v == '"' && fin[-1] == '"') ||
                 (*v == '\'' && fin[-1] == '\''))) {
                ++v;
                --fin;
            }

            size_t n = (size_t)(fin - v);

            if (n && n < sizeof e->nombre) {
                memcpy(e->nombre, v, n);
                e->nombre[n] = '\0';
            }

            return;
        }

        p = *fin ? fin + 1 : fin;
    }
}

static void drenar_payload(cliente_t *c, size_t bytes)
{
    char descarte[8192];

    while (bytes > 0) {
        size_t n = bytes < sizeof descarte
                   ? bytes : sizeof descarte;

        if (nu_read_all_until(c->fd, descarte, n,
                              &c->vencimiento) != 0)
            break;

        bytes -= n;
    }
}

int submit(cliente_t *c, size_t bytes)
{
    servidor_t *s = c->servidor;

    pthread_mutex_lock(&s->mu);

    int cerrado = s->apagar;
    int lleno = s->n_cola >= MAX_COLA ||
                bytes > MAX_BYTES_COLA - s->bytes_cola;

    if (!cerrado && !lleno) {
        ++s->n_cola;
        s->bytes_cola += bytes;
    }

    pthread_mutex_unlock(&s->mu);

    if (cerrado)
        return error_cliente(c->fd, ERR_INVALID_STATE,
                             "servidor cerrado");

    if (lleno) {
        int rc = error_cliente(c->fd, "BUSY",
                               "limite de cola o bytes YAML");

        if (rc == 0)
            drenar_payload(c, bytes);

        return rc;
    }

    ejecucion_t *e = calloc(1, sizeof *e);

    if (e)
        e->yaml = malloc(bytes + 1);

    if (!e || !e->yaml) {
        free(e);
        liberar_reserva(s, bytes);

        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }

    int lectura = nu_read_all_until(c->fd, e->yaml, bytes,
                                   &c->vencimiento);

    if (lectura != 0 || memchr(e->yaml, '\0', bytes)) {
        free(e->yaml);
        free(e);
        liberar_reserva(s, bytes);

        return error_cliente(
            c->fd,
            lectura == NU_TIMEOUT ? "TIMEOUT" : ERR_BAD_REQUEST,
            "payload incompleto, vencido o con NUL"
        );
    }

    e->yaml[bytes] = '\0';
    e->bytes = bytes;

    /* Validar antes de asignar el ID. */
    char error_yaml[256];

    if (validar_yaml(e->yaml, bytes,
                     error_yaml, sizeof error_yaml) != 0) {
        free(e->yaml);
        free(e);
        liberar_reserva(s, bytes);

        char respuesta[PROTO_MAX_RESPONSE_LINE];

        if (protocol_format_invalid(respuesta,
                                    sizeof respuesta, 1) < 0 ||
            nu_send_response(c->fd, respuesta) != 0)
            return -1;

        if (protocol_format_invalid_item(
                respuesta, sizeof respuesta,
                "yaml", error_yaml) < 0 ||
            nu_send_response(c->fd, respuesta) != 0)
            return -1;

        if (protocol_format_end(respuesta,
                                sizeof respuesta) < 0)
            return -1;

        return nu_send_response(c->fd, respuesta);
    }

    nombre_provisional(e);
    e->id = siguiente_id(s);

    pthread_mutex_lock(&s->mu);

    if (s->apagar || e->id == 0) {
        --s->n_cola;
        s->bytes_cola -= bytes;

        pthread_mutex_unlock(&s->mu);

        free(e->yaml);
        free(e);

        return error_cliente(
            c->fd, ERR_INVALID_STATE,
            "servidor cerrado o IDs agotados"
        );
    }

    e->estado = QUEUED;
    e->siguiente = s->ejecuciones;
    s->ejecuciones = e;

    if (s->ultimo)
        s->ultimo->cola = e;
    else
        s->primero = e;

    s->ultimo = e;

    int id = e->id;

    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);

    char respuesta[PROTO_MAX_RESPONSE_LINE];

    if (protocol_format_ok(respuesta,
                           sizeof respuesta, id) < 0)
        return -1;

    return nu_send_response(c->fd, respuesta);
}

static long long duracion(const ejecucion_t *e)
{
    if (e->estado == QUEUED || e->estado == CANCELED)
        return 0;

    struct timespec fin = e->fin;

    if (e->estado == RUNNING)
        clock_gettime(CLOCK_MONOTONIC, &fin);

    return (long long)(fin.tv_sec - e->inicio.tv_sec) * 1000 +
           (fin.tv_nsec - e->inicio.tv_nsec) / 1000000;
}

int consultar(cliente_t *c, int id, int lista)
{
    servidor_t *s = c->servidor;
    char linea[PROTO_MAX_RESPONSE_LINE];

    typedef struct {
        int id;
        estado_t estado;
        char nombre[128];
        long long ms;
    } fila_t;

    pthread_mutex_lock(&s->mu);

    size_t n = 0;

    for (ejecucion_t *e = s->ejecuciones;
         e; e = e->siguiente) {
        if (lista || e->id == id)
            ++n;
    }

    fila_t *filas = n ? calloc(n, sizeof *filas) : NULL;

    if (n && !filas) {
        pthread_mutex_unlock(&s->mu);
        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }

    size_t i = 0;

    for (ejecucion_t *e = s->ejecuciones;
         e; e = e->siguiente) {
        if (!lista && e->id != id)
            continue;

        filas[i].id = e->id;
        filas[i].estado = e->estado;

        memcpy(filas[i].nombre, e->nombre,
               sizeof e->nombre);

        filas[i++].ms = duracion(e);
    }

    pthread_mutex_unlock(&s->mu);

    if (!lista && !n)
        return error_cliente(c->fd, "NOT_FOUND",
                             "ID inexistente");

    int rc = 0;

    if (lista) {
        rc = protocol_format_list(linea, sizeof linea, (int)n);

        if (rc >= 0)
            rc = nu_send_response(c->fd, linea);
    }

    for (i = 0; i < n && rc >= 0; ++i) {
        if (lista) {
            rc = protocol_format_run(
                linea, sizeof linea, filas[i].id,
                estado_texto(filas[i].estado),
                filas[i].nombre
            );
        } else {
            rc = protocol_format_status(
                linea, sizeof linea, filas[i].id,
                estado_texto(filas[i].estado),
                filas[i].ms
            );
        }

        if (rc >= 0)
            rc = nu_send_response(c->fd, linea);
    }

    free(filas);

    if (rc < 0 ||
        protocol_format_end(linea, sizeof linea) < 0)
        return -1;

    return nu_send_response(c->fd, linea);
}
