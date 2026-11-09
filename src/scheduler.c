#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "protocol.h"
#include "yaml_parser.h" 
#include "artifacts.h"
#include "logs.h"
#include "vars.h"
#include <errno.h>
#include <sys/stat.h>
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
    /* Amplia la ventana de carrera solo en el binario demostrativo. */
    sched_yield();
    struct timespec ventana = { .tv_sec = 0, .tv_nsec = 2000000L };
    (void)nanosleep(&ventana, NULL);
    fprintf(stderr, "SIN_PROTECCION ID_PROPUESTO %u\n", id);
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

/* Los nombres de etapas y trabajos ya fueron validados por el parser. */
static int crear_carpeta(const char *ruta)
{
    if (mkdir(ruta,0700)==0) return 0;
    return errno==EEXIST?0:-1;
}
static int crear_workspaces(servidor_t *s,ejecucion_t *e)
{
    size_t n=strlen(s->directorio)+64;
    char *base=malloc(n);
    if (!base) return -1;
    snprintf(base,n,"%s/run_%d",s->directorio,e->id);
    if (crear_carpeta(base)) {free(base);return -1;}
    for(size_t i=0;i<e->plan->cantidad_etapas;++i) {
        etapa_t *et=&e->plan->etapas[i];
        size_t l=strlen(base)+strlen(et->nombre)+2;
        char *dir=malloc(l);
        if(!dir) {free(base);return -1;}
        snprintf(dir,l,"%s/%s",base,et->nombre);
        if(crear_carpeta(dir)) {free(dir);free(base);return -1;}
        for(size_t j=0;j<et->cantidad_trabajos;++j) {
            trabajo_t *t=&e->plan->trabajos[et->primer_trabajo+j];
            size_t z=strlen(dir)+strlen(t->nombre)+2;
            char *ruta=malloc(z);
            if(!ruta) {free(dir);free(base);return -1;}
            snprintf(ruta,z,"%s/%s",dir,t->nombre);
            if(crear_carpeta(ruta)) {free(ruta);free(dir);free(base);return -1;}
            t->directorio_trabajo=ruta; /* liberar_pipeline() es propietario */
        }
        free(dir);
    }
    free(base);return 0;
}

/* Reserva atomica antes de leer el payload; la propiedad pasa al motor
 * exactamente una vez. No mantenemos s->mu durante I/O ni el parser. */
int submit(cliente_t *c, size_t bytes)
{
    if (!c || !c->servidor || bytes == 0 || bytes > PROTO_MAX_YAML) return -1;
    servidor_t *s = c->servidor;
    pthread_mutex_lock(&s->mu);
    int apagar = s->apagar;
    int ocupado = s->n_cola >= MAX_COLA || bytes > MAX_BYTES_COLA - s->bytes_cola;
    if (!apagar && !ocupado) {
        ++s->n_cola;
        s->bytes_cola += bytes;
    }
    pthread_mutex_unlock(&s->mu);
    if (apagar) return error_cliente(c->fd, ERR_INVALID_STATE, "servidor cerrado");
    if (ocupado) {
        int rc = error_cliente(c->fd, "BUSY", "limite de cola o bytes YAML");
        if (rc != 0) return rc;
        char buffer[8192];
        size_t restante = bytes;
        while (restante) {
            size_t n = restante < sizeof buffer ? restante : sizeof buffer;
            if (nu_read_all_until(c->fd, buffer, n, &c->vencimiento) != 0) break;
            restante -= n;
        }
        return 0;
    }

    char *yaml = malloc(bytes + 1);
    if (!yaml) {
        motor_liberar_admision(s, bytes);
        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }
    if (nu_read_all_until(c->fd, yaml, bytes, &c->vencimiento) != 0) {
        free(yaml);
        motor_liberar_admision(s, bytes);
        return error_cliente(c->fd, ERR_BAD_REQUEST, "payload incompleto");
    }
    yaml[bytes] = '\0';
    pipeline_t *plan = NULL;
    errores_yaml_t errores = {0};
    if (memchr(yaml, 0, bytes)) {
        motor_liberar_admision(s, bytes);
        free(yaml);
        return error_cliente(c->fd, ERR_BAD_REQUEST, "NUL en YAML");
    }
    int rc_parser = parsear_pipeline_yaml(yaml, bytes, &plan, &errores);
    if (rc_parser != 0 || errores.cantidad != 0) {
        motor_liberar_admision(s, bytes);
        char linea[PROTO_MAX_RESPONSE_LINE];
        int n = errores.cantidad ? (int)errores.cantidad : 1;
        if (protocol_format_invalid(linea, sizeof linea, n) >= 0)
            (void)nu_send_response(c->fd, linea);
        if (errores.cantidad == 0) {
            if (protocol_format_invalid_item(linea, sizeof linea,
                                             "L0:C0:yaml", "YAML invalido") >= 0)
                (void)nu_send_response(c->fd, linea);
        } else {
            for (size_t i = 0; i < errores.cantidad; ++i) {
                error_yaml_t *it = &errores.items[i];
                if (protocol_format_invalid_item(linea, sizeof linea,
                       it->ubicacion ? it->ubicacion : "L0:C0:yaml",
                       it->mensaje ? it->mensaje : "YAML invalido") >= 0)
                    (void)nu_send_response(c->fd, linea);
            }
        }
        if (protocol_format_end(linea, sizeof linea) >= 0)
            (void)nu_send_response(c->fd, linea);
        liberar_errores_yaml(&errores);
        liberar_pipeline(plan);
        free(yaml);
        return 0;
    }
    liberar_errores_yaml(&errores);

    ejecucion_t *e = calloc(1, sizeof *e);
    if (!e) {
        liberar_pipeline(plan); free(yaml);
        motor_liberar_admision(s, bytes);
        return error_cliente(c->fd, "INTERNAL", "sin memoria");
    }
    e->id = siguiente_id(s);
    e->plan = plan;
    e->nombre = plan->nombre;
    e->yaml = yaml;
    e->bytes = bytes;
    e->reserva_admision = 1;
    e->bitacoras = bitacoras_crear();
    e->variables = vars_crear(plan);
    atomic_init(&e->cancelar_solicitado, 0);
    if (!e->id || !e->bitacoras || !e->variables ||
        crear_workspaces(s,e) != 0 ||
        artifacts_inicializar_directorio(s->directorio, e->id) != 0 ||
        motor_encolar_ejecucion(s, e) != 0) {
        motor_liberar_admision(s, bytes);
        if (e->bitacoras) bitacoras_destruir(e->bitacoras);
        vars_destruir(e->variables);
        liberar_pipeline(plan); free(yaml); free(e);
        return error_cliente(c->fd, "INTERNAL", "no se pudo encolar");
    }
    char respuesta[64];
    snprintf(respuesta, sizeof respuesta, "OK %d\n", e->id);
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
    servidor_t *s=c->servidor;
    pthread_mutex_lock(&s->mu);
    ejecucion_t *e=s->ejecuciones;
    while(e && e->id!=id) e=e->siguiente;
    bitacoras_ejecucion_t *b=e?e->bitacoras:NULL;
    pthread_mutex_unlock(&s->mu);
    if(!b) return error_cliente(c->fd,"NOT_FOUND","bitacora no encontrada");
    char linea[PROTO_MAX_RESPONSE_LINE];
    if(protocol_format_logs(linea,sizeof linea,id)<0 ||
       nu_send_response(c->fd,linea)!=0) return -1;
    size_t posicion=0;
    for(;;) {
        char datos[3001];size_t leidos=0;int finalizado=0;
        int rc=bitacoras_leer(b,referencia,&posicion,datos,sizeof datos-1,
                              follow?100:0,&leidos,&finalizado);
        if(rc==-2) return error_cliente(c->fd,"AMBIGUOUS","nombre de trabajo ambiguo");
        if(rc<0) return -1;
        if(leidos) {
            datos[leidos]=0;
            if(protocol_send_log_lines(c->fd,datos)) return -1;
        }
        pthread_mutex_lock(&s->mu);
        int termino=(e->estado!=QUEUED && e->estado!=RUNNING) || s->apagar;
        pthread_mutex_unlock(&s->mu);
        if(!follow || (termino && !leidos) || (finalizado && !leidos)) break;
    }
    if(protocol_format_end(linea,sizeof linea)<0) return -1;
    return nu_send_response(c->fd,linea);
}
