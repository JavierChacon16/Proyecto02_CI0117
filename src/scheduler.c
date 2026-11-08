#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "protocol.h"
#include "yaml_parser.h" 
#include "artifacts.h"
#include "logs.h"
#include "motor.h"
#include "util.h"
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        // Drenaje best-effort del YAML enviado por el cliente cuando está ocupado
        char buffer[8192];
        size_t restante = bytes;
        while (restante) {
            size_t n = restante < sizeof buffer ? restante : sizeof buffer;
            if (nu_read_all_until(c->fd, buffer, n, &c->vencimiento) != 0)
                break;
            restante -= n;
        }
        return 0;
    }

    // Reservar en la cola
    pthread_mutex_lock(&s->mu);
    s->n_cola++;
    s->bytes_cola += bytes;
    pthread_mutex_unlock(&s->mu);

    char *yaml_buf = malloc(bytes + 1);
    if (!yaml_buf) {
        pthread_mutex_lock(&s->mu);
        s->n_cola--;
        s->bytes_cola -= bytes;
        pthread_mutex_unlock(&s->mu);
        return -1;
    }

    size_t restante = bytes;
    size_t leidos = 0;
    while (restante) {
        size_t n = restante < 8192 ? restante : 8192;
        if (nu_read_all_until(c->fd, yaml_buf + leidos, n, &c->vencimiento) != 0) {
            free(yaml_buf);
            pthread_mutex_lock(&s->mu);
            s->n_cola--;
            s->bytes_cola -= bytes;
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
        leidos += n;
        restante -= n;
    }
    yaml_buf[leidos] = '\0';

    pipeline_t *plan = NULL;
    errores_yaml_t errores = {0};
    int parse_rc = parsear_pipeline_yaml(yaml_buf, bytes, &plan, &errores);

    if (parse_rc != 0 || errores.cantidad > 0) {
        pthread_mutex_lock(&s->mu);
        s->n_cola--;
        s->bytes_cola -= bytes;
        pthread_mutex_unlock(&s->mu);

        int rc_inv = error_cliente(c->fd, ERR_BAD_REQUEST, "error al parsear YAML");
        liberar_errores_yaml(&errores);
        free(yaml_buf);
        if (plan) liberar_pipeline(plan);
        return rc_inv;
    }

    int id = siguiente_id(s);
    artifacts_inicializar_directorio(s->directorio, id);

    ejecucion_t *ej = calloc(1, sizeof(ejecucion_t));
    if (!ej) {
        pthread_mutex_lock(&s->mu);
        s->n_cola--;
        s->bytes_cola -= bytes;
        pthread_mutex_unlock(&s->mu);
        free(yaml_buf);
        liberar_pipeline(plan);
        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }

    ej->id = id;
    ej->plan = plan;
    ej->nombre = plan->nombre;
    ej->yaml = yaml_buf;
    ej->bytes = bytes;
    ej->estado = QUEUED;
    clock_gettime(CLOCK_MONOTONIC, &ej->aceptada_en);
    ej->bitacoras = bitacoras_crear();
    atomic_init(&ej->cancelar_solicitado, 0);

    motor_encolar_ejecucion(s, ej);

    char respuesta[64];
    snprintf(respuesta, sizeof(respuesta), "OK %d\n", id);
    return nu_write_all(c->fd, respuesta, strlen(respuesta));
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
    pipeline_t *plan;
} fila_t;

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
        filas[k].plan = e->plan;
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
        if (lista) {
            rc = protocol_format_run(linea, sizeof linea, filas[i].id,
                      estado_texto(filas[i].estado), filas[i].nombre);
            if (rc >= 0) rc = nu_send_response(c->fd, linea);
        } else {
            rc = protocol_format_status(linea, sizeof linea, filas[i].id,
                      estado_texto(filas[i].estado), filas[i].ms);
            if (rc >= 0) rc = nu_send_response(c->fd, linea);

            if (filas[i].plan) {
                pipeline_t *plan = filas[i].plan;
                for (size_t st = 0; st < plan->cantidad_etapas && rc >= 0; ++st) {
                    etapa_t *etapa = &plan->etapas[st];
                    long long dur_stage = duracion(etapa->estado, &etapa->inicio, &etapa->fin);
                    if (protocol_format_stage(linea, sizeof linea, etapa->nombre, estado_texto(etapa->estado), dur_stage) >= 0) {
                        rc = nu_send_response(c->fd, linea);
                    }

                    for (size_t j = 0; j < etapa->cantidad_trabajos && rc >= 0; ++j) {
                        trabajo_t *trabajo = &plan->trabajos[etapa->primer_trabajo + j];
                        long long dur_job = duracion(trabajo->estado, &trabajo->inicio, &trabajo->fin);
                        if (protocol_format_job(linea, sizeof linea, etapa->nombre, trabajo->nombre, estado_texto(trabajo->estado), dur_job) >= 0) {
                            rc = nu_send_response(c->fd, linea);
                        }

                        for (size_t p = 0; p < trabajo->cantidad_pasos && rc >= 0; ++p) {
                            paso_t *paso = &trabajo->pasos[p];
                            long long dur_step = duracion(paso->estado, &paso->inicio, &paso->fin);
                            if (protocol_format_step(linea, sizeof linea, etapa->nombre, trabajo->nombre, (int)p, estado_texto(paso->estado), dur_step) >= 0) {
                                rc = nu_send_response(c->fd, linea);
                            }
                        }
                    }
                }
            }
        }
    }

    free(filas);

    if (rc < 0 || protocol_format_end(linea, sizeof linea) < 0) return -1;
    return nu_send_response(c->fd, linea);
}

int consultar_logs(cliente_t *c, int id, const char *referencia, int follow)
{
    if (!c || !c->servidor) return -1;
    servidor_t *s = c->servidor;

    pthread_mutex_lock(&s->mu);
    ejecucion_t *ej = NULL;
    for (ejecucion_t *e = s->ejecuciones; e; e = e->siguiente) {
        if (e->id == id) {
            ej = e;
            break;
        }
    }
    if (!ej || !ej->bitacoras) {
        pthread_mutex_unlock(&s->mu);
        return error_cliente(c->fd, "NOT_FOUND", "bitacora no encontrada");
    }
    bitacoras_ejecucion_t *b = ej->bitacoras;
    pthread_mutex_unlock(&s->mu);

    size_t posicion = 0;
    char destino[4096];
    size_t leidos = 0;
    int finalizado = 0;
    unsigned esperar_ms = follow ? 2000 : 0;

    while (1) {
        int rc = bitacoras_leer(b, referencia, &posicion, destino, sizeof(destino), esperar_ms, &leidos, &finalizado);
        if (rc == -2) return error_cliente(c->fd, "AMBIGUOUS", "nombre de trabajo ambiguo");
        if (rc < 0) return -1;

        if (leidos > 0) {
            if (nu_write_all(c->fd, destino, leidos) != 0) {
                return -1;
            }
        }

        if (finalizado && leidos == 0) break;
        if (!follow) break;
    }
    return 0;
}