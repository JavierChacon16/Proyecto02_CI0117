
#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "protocol.h"
#include "util.h"
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A1 conserva la demostracion -DSIN_PROTECCION del contador de IDs.
 * mu_id se toma siempre solo, nunca anidado en s->mu. */
static int siguiente_id(servidor_t *s)
{
#ifndef SIN_PROTECCION
    pthread_mutex_lock(&s->mu_id);
#endif
    unsigned int id = s->proximo_id;
#ifdef SIN_PROTECCION
    sched_yield();
#endif
    if (id <= INT_MAX) s->proximo_id = id + 1U;
#ifndef SIN_PROTECCION
    pthread_mutex_unlock(&s->mu_id);
#endif
    return id <= INT_MAX ? (int)id : 0;
}

static const char *estado_texto(estado_t st)
{
    switch (st) {
    case QUEUED: return "queued";
    case RUNNING: return "running";
    case SUCCEEDED: return "succeeded";
    case PARTIALLY_SUCCEEDED: return "partiallySucceeded";
    case FAILED: return "failed";
    case CANCELED: return "canceled";
    case SKIPPED: return "skipped";
    }
    return "failed";
}

static int terminal(estado_t st)
{
    return st != QUEUED && st != RUNNING;
}

/* TODO A2: leer YAML, llamar parsear_pipeline_yaml(), obtener ID,
 * preparar workspaces/logs/vars/artifacts y motor_encolar_ejecucion().
 * En A1 una solicitud bien formada siempre devuelve ERR INTERNAL.
 * Conservamos drenaje y comportamiento BUSY para evitar resets TCP y para
 * que los tests historicos de limite de recursos sigan siendo relevantes. */
int submit(cliente_t *c, size_t bytes)
{
    if (!c || !c->servidor || bytes == 0) return -1;
    servidor_t *s = c->servidor;
    pthread_mutex_lock(&s->mu);
    int apagar = s->apagar;
    int ocupado = s->n_cola >= MAX_COLA ||
                  bytes > MAX_BYTES_COLA - s->bytes_cola;
    pthread_mutex_unlock(&s->mu);
    if (apagar) return error_cliente(c->fd, ERR_INVALID_STATE, "servidor cerrado");
    if (ocupado) {
        int rc = error_cliente(c->fd, "BUSY", "limite de cola o bytes YAML");
        if (rc != 0) return rc;
    }
    char buffer[8192];
    size_t restante = bytes;
    while (restante) {
        size_t n = restante < sizeof buffer ? restante : sizeof buffer;
        if (nu_read_all_until(c->fd, buffer, n, &c->vencimiento) != 0)
            return ocupado ? 0 : -1; /* drenaje BUSY best-effort */
        restante -= n;
    }
    (void)siguiente_id;
    return ocupado ? 0 : error_cliente(c->fd, "INTERNAL", "no implementado");
}

static long long duracion(estado_t estado, const struct timespec *inicio,
                          const struct timespec *fin)
{
    if (inicio->tv_sec == 0 && inicio->tv_nsec == 0) return 0;
    struct timespec cierre = *fin;
    if (!terminal(estado)) (void)clock_gettime(CLOCK_MONOTONIC, &cierre);
    return (long long)(cierre.tv_sec - inicio->tv_sec) * 1000 +
           (cierre.tv_nsec - inicio->tv_nsec) / 1000000;
}

typedef struct {
    int id;
    estado_t estado;
    char nombre[128];
    long long ms;
} fila_t;

/* Snapshot bajo mu, IO fuera de mu. A1 devuelve resumen de ejecuciones;
 * TODO A2: snapshot de etapas, trabajos y pasos y LOGS --follow. */
int consultar(cliente_t *c, int id, int lista)
{
    if (!c || !c->servidor) return -1;
    servidor_t *s = c->servidor;
    char linea[PROTO_MAX_RESPONSE_LINE];
    pthread_mutex_lock(&s->mu);
    size_t n = 0;
    for (ejecucion_t *e = s->ejecuciones; e; e = e->siguiente)
        if (lista || e->id == id) ++n;
    fila_t *filas = n ? calloc(n, sizeof *filas) : NULL;
    if (n && !filas) {
        pthread_mutex_unlock(&s->mu);
        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }
    size_t k = 0;
    for (ejecucion_t *e = s->ejecuciones; e; e = e->siguiente) {
        if (!lista && e->id != id) continue;
        filas[k].id = e->id;
        filas[k].estado = e->estado;
        snprintf(filas[k].nombre, sizeof filas[k].nombre, "%s",
                 e->nombre ? e->nombre : "sin_nombre");
        filas[k].ms = duracion(e->estado, &e->inicio, &e->fin);
        ++k;
    }
    pthread_mutex_unlock(&s->mu);
    if (!lista && n == 0)
        return error_cliente(c->fd, "NOT_FOUND", "ID inexistente");
    int rc = 0;
    if (lista) {
        rc = protocol_format_list(linea, sizeof linea, (int)n);
        if (rc >= 0) rc = nu_send_response(c->fd, linea);
    }
    for (size_t i = 0; i < n && rc >= 0; ++i) {
        if (lista)
            rc = protocol_format_run(linea, sizeof linea, filas[i].id,
                      estado_texto(filas[i].estado), filas[i].nombre);
        else
            rc = protocol_format_status(linea, sizeof linea, filas[i].id,
                      estado_texto(filas[i].estado), filas[i].ms);
        if (rc >= 0) rc = nu_send_response(c->fd, linea);
    }
    free(filas);
    if (rc < 0 || protocol_format_end(linea, sizeof linea) < 0) return -1;
    return nu_send_response(c->fd, linea);
}

int cancelar_ejecucion(cliente_t *c, int id)
{
    servidor_t *s = c->servidor;
    pthread_mutex_lock(&s->mu);
    ejecucion_t *e = s->ejecuciones;
    ejecucion_t *encontrado = NULL;

    while (e) {
        if (e->id == id) {
            encontrado = e;
            break;
        }
        e = e->siguiente;
    }

    if (!encontrado) {
        pthread_mutex_unlock(&s->mu);
        return error_cliente(c->fd, "NOT_FOUND", "ID de ejecucion inexistente");
    }

    
    if (encontrado->estado == SUCCEEDED || encontrado->estado == PARTIALLY_SUCCEEDED ||
        encontrado->estado == FAILED || encontrado->estado == CANCELED || 
        encontrado->estado == SKIPPED) {
        pthread_mutex_unlock(&s->mu);
        return error_cliente(c->fd, ERR_INVALID_STATE, "La ejecucion ya ha finalizado o no esta activa");
    }

    // Si estaba en cola (QUEUED), lo removemos de la cola de pendientes y ajustamos contadores
    if (encontrado->estado == QUEUED) {
        // Buscar y remover de la lista enlazada de pendientes (primero/cola)
        if (s->primero == encontrado) {
            s->primero = encontrado->cola;
        } else {
            ejecucion_t *act = s->primero;
            while (act && act->cola != encontrado)
                act = act->cola;
            if (act)
                act->cola = encontrado->cola;
        }
        if (s->ultimo == encontrado) {
            // Recalcular el ultimo
            ejecucion_t *act = s->primero;
            while (act && act->cola)
                act = act->cola;
            s->ultimo = act;
        }

        --s->n_cola;
        s->bytes_cola -= encontrado->bytes;
        free(encontrado->yaml);
        encontrado->yaml = NULL;
    }

    // Cambiar estado a CANCELED
    encontrado->estado = CANCELED;
    clock_gettime(CLOCK_MONOTONIC, &encontrado->fin);

    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);

    char respuesta[PROTO_MAX_RESPONSE_LINE];
    if (protocol_format_ok(respuesta, sizeof respuesta, id) < 0)
        return -1;

    return nu_send_response(c->fd, respuesta);
}