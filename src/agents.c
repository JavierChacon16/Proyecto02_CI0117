#define _POSIX_C_SOURCE 200809L
#include "motor.h"

#include <limits.h>
#include <omp.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Orden de locks: mu_id se adquiere SOLO (en scheduler.c). El motor toma
 * exclusivamente s->mu. Nunca hace I/O, callbacks, espera de hijos, join,
 * ni locks de logs/vars/artifacts mientras lo sostiene. pthread_cond_wait
 * libera s->mu durante la espera. Los agentes nunca adquieren otro lock
 * dentro de una seccion con s->mu. */

static struct timespec reloj(void)
{
    struct timespec t = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static int tiempo_mayor_igual(struct timespec a, struct timespec b)
{
    return a.tv_sec > b.tv_sec ||
           (a.tv_sec == b.tv_sec && a.tv_nsec >= b.tv_nsec);
}

static int terminal(estado_t st)
{
    return st == SUCCEEDED || st == PARTIALLY_SUCCEEDED ||
           st == FAILED || st == CANCELED || st == SKIPPED;
}

static int aceptable(estado_t st)
{
    return st == SUCCEEDED || st == PARTIALLY_SUCCEEDED;
}

static void fin_queued(estado_t *estado, struct timespec *fin,
                       estado_t nuevo)
{
    /* Las unicas rutas invocadas aqui son QUEUED -> terminal valida. */
    if (*estado == QUEUED) {
        *estado = nuevo;
        *fin = reloj();
    }
}

static void paso_pendiente_terminar(trabajo_t *t, estado_t nuevo)
{
    for (size_t i = 0; i < t->cantidad_pasos; ++i) {
        paso_t *p = &t->pasos[i];
        if (p->estado == QUEUED) {
            fin_queued(&p->estado, &p->fin, nuevo);
            ++t->pasos_terminales;
        }
    }
}

static void trabajo_pendiente_terminar(ejecucion_t *e, trabajo_t *t,
                                       estado_t nuevo)
{
    if (t->estado != QUEUED)
        return;
    fin_queued(&t->estado, &t->fin, nuevo);
    paso_pendiente_terminar(t, nuevo);
    ++e->trabajos_terminales;
    /* Los indices de un trabajo omitido/cancelado se invalidan logicamente.
     * La FIFO completa se descarta al cancelar; en ejecucion normal un trabajo
     * en FIFO no puede pasar a skipped (condiciones terminales e inmutables). */
}

static int condicion_etapa(const pipeline_t *p, const etapa_t *et)
{
    /* No se construye un vector temporal bajo el mutex global. */
    int todos = 1, fallo = 0;
    for (size_t i = 0; i < et->cantidad_dependencias; ++i) {
        estado_t st = p->etapas[et->dependencias[i]].estado;
        if (!aceptable(st)) todos = 0;
        if (st == FAILED) fallo = 1;
    }
    return et->condicion == COND_ALWAYS ||
           (et->condicion == COND_SUCCEEDED && todos) ||
           (et->condicion == COND_FAILED && fallo);
}

static int condicion_trabajo(const pipeline_t *p, const trabajo_t *t)
{
    int todos = 1, fallo = 0;
    for (size_t i = 0; i < t->cantidad_dependencias_condicion; ++i) {
        estado_t st = p->trabajos[t->dependencias_condicion[i]].estado;
        if (!aceptable(st)) todos = 0;
        if (st == FAILED) fallo = 1;
    }
    return t->condicion == COND_ALWAYS ||
           (t->condicion == COND_SUCCEEDED && todos) ||
           (t->condicion == COND_FAILED && fallo);
}

static int deps_etapa_terminales(const pipeline_t *p, const etapa_t *et)
{
    for (size_t i = 0; i < et->cantidad_dependencias; ++i)
        if (!terminal(p->etapas[et->dependencias[i]].estado)) return 0;
    return 1;
}

static int deps_job_terminales(const pipeline_t *p, const trabajo_t *t)
{
    for (size_t i = 0; i < t->cantidad_dependencias; ++i)
        if (!terminal(p->trabajos[t->dependencias[i]].estado)) return 0;
    return 1;
}

static void rr_quitar(servidor_t *s, ejecucion_t *e)
{
    if (!e->en_rr) return;
    ejecucion_t *prev = NULL;
    for (ejecucion_t *it = s->rr_primero; it; it = it->rr_siguiente) {
        if (it == e) {
            if (prev) prev->rr_siguiente = it->rr_siguiente;
            else s->rr_primero = it->rr_siguiente;
            if (s->rr_ultimo == it) s->rr_ultimo = prev;
            it->rr_siguiente = NULL;
            it->en_rr = 0;
            return;
        }
        prev = it;
    }
}

static void rr_agregar(servidor_t *s, ejecucion_t *e)
{
    if (e->en_rr || e->cantidad_listos == 0 || s->apagar ||
        atomic_load(&e->cancelar_solicitado)) return;
    e->en_rr = 1;
    e->rr_siguiente = NULL;
    if (s->rr_ultimo) s->rr_ultimo->rr_siguiente = e;
    else s->rr_primero = e;
    s->rr_ultimo = e;
}

static void fifo_agregar(servidor_t *s, ejecucion_t *e, size_t id)
{
    trabajo_t *t = &e->plan->trabajos[id];
    if (t->en_cola_listos || t->estado != QUEUED) return;
    t->en_cola_listos = true;
    t->siguiente_listo = SIZE_MAX;
    if (e->cantidad_listos)
        e->plan->trabajos[e->ultimo_listo].siguiente_listo = id;
    else e->primero_listo = id;
    e->ultimo_listo = id;
    ++e->cantidad_listos;
    ++s->trabajos_listos;
    rr_agregar(s, e);
}

static void fifo_quitar_trabajo(servidor_t *s, ejecucion_t *e, size_t id)
{
    trabajo_t *t = &e->plan->trabajos[id];
    if (!t->en_cola_listos) return;
    size_t anterior = SIZE_MAX;
    size_t actual = e->primero_listo;
    while (actual != SIZE_MAX && actual != id) {
        anterior = actual;
        actual = e->plan->trabajos[actual].siguiente_listo;
    }
    if (actual == SIZE_MAX) return;
    size_t siguiente = t->siguiente_listo;
    if (anterior == SIZE_MAX) e->primero_listo = siguiente;
    else e->plan->trabajos[anterior].siguiente_listo = siguiente;
    if (e->ultimo_listo == id) e->ultimo_listo = anterior;
    t->siguiente_listo = SIZE_MAX;
    t->en_cola_listos = false;
    --e->cantidad_listos;
    --s->trabajos_listos;
    if (!e->cantidad_listos) rr_quitar(s, e);
}

static void fifo_limpiar(servidor_t *s, ejecucion_t *e)
{
    rr_quitar(s, e);
    size_t id = e->primero_listo;
    while (id != SIZE_MAX) {
        trabajo_t *t = &e->plan->trabajos[id];
        size_t siguiente = t->siguiente_listo;
        t->en_cola_listos = false;
        t->siguiente_listo = SIZE_MAX;
        id = siguiente;
    }
    if (e->cantidad_listos <= s->trabajos_listos)
        s->trabajos_listos -= e->cantidad_listos;
    e->cantidad_listos = 0;
    e->primero_listo = e->ultimo_listo = SIZE_MAX;
}

static void liberar_reserva_ejecucion(servidor_t *s, ejecucion_t *e)
{
    /* Se llama una sola vez al abandonar QUEUED. Los tests inicializan
     * bytes=0. La reserva de SUBMIT se contabilizara al integrar parser. */
    if (s->n_cola) --s->n_cola;
    if (s->bytes_cola >= e->bytes) s->bytes_cola -= e->bytes;
}

static void ejecucion_a_running(servidor_t *s, ejecucion_t *e)
{
    if (e->estado == QUEUED) {
        e->estado = RUNNING;
        e->inicio = reloj();
        liberar_reserva_ejecucion(s, e);
    }
}

static estado_t agregar_etapa(const pipeline_t *p, const etapa_t *et)
{
    size_t saltados = 0;
    int fallo = 0, parcial = 0, cancelado = 0;
    for (size_t j = 0; j < et->cantidad_trabajos; ++j) {
        estado_t st = p->trabajos[et->primer_trabajo + j].estado;
        if (st == SKIPPED) ++saltados;
        if (st == FAILED) fallo = 1;
        if (st == PARTIALLY_SUCCEEDED) parcial = 1;
        if (st == CANCELED) cancelado = 1;
    }
    if (cancelado) return CANCELED;
    if (fallo) return FAILED;
    if (parcial) return PARTIALLY_SUCCEEDED;
    if (saltados == et->cantidad_trabajos) return SKIPPED;
    return SUCCEEDED;
}

static int actualizar_agregados(servidor_t *s, ejecucion_t *e)
{
    pipeline_t *p = e->plan;
    int cambios = 0;
    for (size_t i = 0; i < p->cantidad_etapas; ++i) {
        etapa_t *et = &p->etapas[i];
        if (terminal(et->estado)) continue;
        size_t finales = 0;
        for (size_t j = 0; j < et->cantidad_trabajos; ++j)
            finales += terminal(p->trabajos[et->primer_trabajo + j].estado) != 0;
        et->trabajos_terminales = finales;
        if (finales != et->cantidad_trabajos) continue;
        estado_t nuevo = agregar_etapa(p, et);
        if (et->estado == QUEUED && nuevo == SUCCEEDED) {
            /* Etapa vacia o plan invalido: no fingir QUEUED->SUCCEEDED. */
            nuevo = FAILED;
        }
        if (et->estado == QUEUED && nuevo == PARTIALLY_SUCCEEDED)
            nuevo = FAILED;
        if (et->estado == QUEUED && nuevo == CANCELED)
            fin_queued(&et->estado, &et->fin, CANCELED);
        else if (et->estado == QUEUED && nuevo == SKIPPED)
            fin_queued(&et->estado, &et->fin, SKIPPED);
        else if (et->estado == QUEUED && nuevo == FAILED)
            fin_queued(&et->estado, &et->fin, FAILED);
        else if (et->estado == RUNNING) {
            et->estado = nuevo;
            et->fin = reloj();
        }
        ++cambios;
    }
    size_t completas = 0;
    for (size_t i = 0; i < p->cantidad_etapas; ++i)
        completas += terminal(p->etapas[i].estado) != 0;
    if (completas == p->cantidad_etapas && e->trabajos_activos == 0 &&
        !terminal(e->estado)) {
        int fallo = 0, parcial = 0, cancelado = 0;
        size_t saltados = 0;
        for (size_t i = 0; i < p->cantidad_etapas; ++i) {
            estado_t st = p->etapas[i].estado;
            if (st == FAILED) fallo = 1;
            if (st == PARTIALLY_SUCCEEDED) parcial = 1;
            if (st == CANCELED) cancelado = 1;
            if (st == SKIPPED) ++saltados;
        }
        estado_t nuevo = cancelado ? CANCELED : fallo ? FAILED :
                          parcial ? PARTIALLY_SUCCEEDED :
                          saltados == p->cantidad_etapas ? SKIPPED : SUCCEEDED;
        if (atomic_load(&e->cancelar_solicitado)) nuevo = CANCELED;
        if (e->estado == QUEUED && nuevo != SKIPPED &&
            nuevo != FAILED && nuevo != CANCELED)
            nuevo = FAILED; /* imposibilidad de QUEUED -> SUCCEEDED */
        if (e->estado == QUEUED) liberar_reserva_ejecucion(s, e);
        e->estado = nuevo;
        e->fin = reloj();
        ++cambios;
    }
    return cambios;
}

static void recalcular(servidor_t *s, ejecucion_t *e)
{
    if (terminal(e->estado)) return;
    pipeline_t *p = e->plan;
    if (s->apagar || atomic_load(&e->cancelar_solicitado)) {
        fifo_limpiar(s, e);
        for (size_t j = 0; j < p->cantidad_trabajos; ++j)
            trabajo_pendiente_terminar(e, &p->trabajos[j], CANCELED);
        (void)actualizar_agregados(s, e);
        return;
    }

    /* Punto fijo: saltos y agregaciones no consumen agente. Un mismo barrido
     * puede habilitar varias etapas encadenadas y propagar skipped en cascada. */
    int cambio;
    do {
        cambio = 0;
        for (size_t i = 0; i < p->cantidad_etapas; ++i) {
            etapa_t *et = &p->etapas[i];
            if (terminal(et->estado) || !deps_etapa_terminales(p, et)) continue;
            if (!condicion_etapa(p, et)) {
                for (size_t j = 0; j < et->cantidad_trabajos; ++j) {
                    trabajo_t *t = &p->trabajos[et->primer_trabajo + j];
                    if (t->estado == QUEUED) {
                        trabajo_pendiente_terminar(e, t, SKIPPED);
                        ++cambio;
                    }
                }
                if (et->estado == QUEUED) {
                    fin_queued(&et->estado, &et->fin, SKIPPED);
                    ++cambio;
                }
                continue;
            }
            for (size_t j = 0; j < et->cantidad_trabajos; ++j) {
                size_t id = et->primer_trabajo + j;
                trabajo_t *t = &p->trabajos[id];
                if (t->estado != QUEUED || t->en_cola_listos ||
                    !deps_job_terminales(p, t)) continue;
                if (!condicion_trabajo(p, t)) {
                    trabajo_pendiente_terminar(e, t, SKIPPED);
                    ++cambio;
                } else {
                    fifo_agregar(s, e, id);
                    ++cambio;
                }
            }
        }
        cambio += actualizar_agregados(s, e);
    } while (cambio && !terminal(e->estado));
    pthread_cond_broadcast(&s->cambio);
}

static int validar_plan_minimo(const pipeline_t *p)
{
    if (!p || !p->etapas || !p->trabajos || !p->cantidad_etapas ||
        !p->cantidad_trabajos) return 0;
    size_t siguiente = 0;
    for (size_t i = 0; i < p->cantidad_etapas; ++i) {
        const etapa_t *et = &p->etapas[i];
        if (et->primer_trabajo != siguiente || et->cantidad_trabajos == 0 ||
            et->condicion > COND_ALWAYS ||
            (et->cantidad_dependencias && !et->dependencias)) return 0;
        if (et->cantidad_trabajos > p->cantidad_trabajos - siguiente) return 0;
        siguiente += et->cantidad_trabajos;
        for (size_t j = 0; j < et->cantidad_dependencias; ++j)
            if (et->dependencias[j] >= p->cantidad_etapas ||
                et->dependencias[j] == i) return 0;
    }
    if (siguiente != p->cantidad_trabajos) return 0;
    for (size_t j = 0; j < p->cantidad_trabajos; ++j) {
        const trabajo_t *t = &p->trabajos[j];
        if (t->indice_etapa >= p->cantidad_etapas ||
            j < p->etapas[t->indice_etapa].primer_trabajo ||
            j >= p->etapas[t->indice_etapa].primer_trabajo +
                 p->etapas[t->indice_etapa].cantidad_trabajos ||
            t->condicion > COND_ALWAYS ||
            (t->cantidad_dependencias && !t->dependencias) ||
            (t->cantidad_dependencias_condicion && !t->dependencias_condicion) ||
            (t->cantidad_pasos && !t->pasos)) return 0;
        for (size_t d = 0; d < t->cantidad_dependencias; ++d)
            if (t->dependencias[d] >= p->cantidad_trabajos ||
                t->dependencias[d] == j) return 0;
        for (size_t d = 0; d < t->cantidad_dependencias_condicion; ++d)
            if (t->dependencias_condicion[d] >= p->cantidad_trabajos ||
                t->dependencias_condicion[d] == j) return 0;
    }
    return 1;
}

int motor_encolar_ejecucion(servidor_t *s, ejecucion_t *e)
{
    if (!s || !e || !validar_plan_minimo(e->plan)) return -1;
    pthread_mutex_lock(&s->mu);
    if (s->apagar || s->n_cola >= MAX_COLA ||
        e->bytes > MAX_BYTES_COLA - s->bytes_cola) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    for (ejecucion_t *it = s->ejecuciones; it; it = it->siguiente)
        if (it == e || it->id == e->id) {
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
    atomic_init(&e->cancelar_solicitado, 0);
    e->nombre = e->plan->nombre;
    e->estado = QUEUED;
    e->aceptada_en = reloj();
    e->inicio = e->fin = (struct timespec){0, 0};
    e->trabajos_activos = e->trabajos_terminales = 0;
    e->primero_listo = e->ultimo_listo = SIZE_MAX;
    e->cantidad_listos = 0;
    e->en_rr = 0;
    e->rr_siguiente = NULL;
    for (size_t i = 0; i < e->plan->cantidad_etapas; ++i) {
        etapa_t *et = &e->plan->etapas[i];
        et->estado = QUEUED;
        et->inicio = et->fin = (struct timespec){0, 0};
        et->trabajos_terminales = 0;
    }
    for (size_t i = 0; i < e->plan->cantidad_trabajos; ++i) {
        trabajo_t *t = &e->plan->trabajos[i];
        t->indice_global = i;
        t->estado = QUEUED;
        t->inicio = t->fin = (struct timespec){0, 0};
        t->pasos_terminales = 0;
        t->hubo_fallo_continuable = false;
        t->siguiente_listo = SIZE_MAX;
        t->en_cola_listos = false;
        for (size_t k = 0; k < t->cantidad_pasos; ++k) {
            paso_t *paso = &t->pasos[k];
            paso->estado = QUEUED;
            paso->inicio = paso->fin = (struct timespec){0, 0};
            paso->codigo_salida = -1;
        }
    }
    e->siguiente = s->ejecuciones;
    s->ejecuciones = e;
    ++s->n_cola;
    s->bytes_cola += e->bytes;
    recalcular(s, e);
    pthread_mutex_unlock(&s->mu);
    return 0;
}

int motor_fallar_trabajo_preparacion(servidor_t *s, ejecucion_t *e,
                                     size_t indice_trabajo)
{
    if (!s || !e || !e->plan || indice_trabajo >= e->plan->cantidad_trabajos)
        return -1;
    pthread_mutex_lock(&s->mu);
    trabajo_t *t = &e->plan->trabajos[indice_trabajo];
    int publicado = 0;
    for (ejecucion_t *it = s->ejecuciones; it; it = it->siguiente)
        if (it == e) { publicado = 1; break; }
    if (!publicado || t->estado != QUEUED ||
        atomic_load(&e->cancelar_solicitado) || s->apagar) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    fifo_quitar_trabajo(s, e, indice_trabajo);
    trabajo_pendiente_terminar(e, t, FAILED);
    /* Antes de empezar, ninguno de los pasos produjo salida. */
    recalcular(s, e);
    pthread_mutex_unlock(&s->mu);
    return 0;
}

static int rr_extraer(servidor_t *s, ejecucion_t **out_e, size_t *out_j)
{
    ejecucion_t *e = s->rr_primero;
    if (!e) return 0;
    rr_quitar(s, e);
    size_t j = e->primero_listo;
    trabajo_t *t = &e->plan->trabajos[j];
    e->primero_listo = t->siguiente_listo;
    if (e->primero_listo == SIZE_MAX) e->ultimo_listo = SIZE_MAX;
    --e->cantidad_listos;
    --s->trabajos_listos;
    t->en_cola_listos = false;
    t->siguiente_listo = SIZE_MAX;
    rr_agregar(s, e);
    *out_e = e;
    *out_j = j;
    return 1;
}

static void *trabajador(servidor_t *s, motor_ejecutor_trabajo_t ejecutar,
                        void *contexto)
{
    for (;;) {
        ejecucion_t *e = NULL;
        size_t indice = 0;
        pthread_mutex_lock(&s->mu);
        while (!s->apagar && !s->rr_primero)
            pthread_cond_wait(&s->cambio, &s->mu);
        if (s->apagar) {
            pthread_mutex_unlock(&s->mu);
            break;
        }
        if (!rr_extraer(s, &e, &indice)) {
            pthread_mutex_unlock(&s->mu);
            continue;
        }
        trabajo_t *t = &e->plan->trabajos[indice];
        etapa_t *et = &e->plan->etapas[t->indice_etapa];
        struct timespec ahora = reloj();
        ejecucion_a_running(s, e);
        if (et->estado == QUEUED) {
            et->estado = RUNNING;
            et->inicio = ahora;
        }
        t->estado = RUNNING;
        t->inicio = ahora;
        ++s->activos;
        ++e->trabajos_activos;
        if (s->activos > s->max_activos) s->max_activos = s->activos;
        pthread_cond_broadcast(&s->cambio);
        pthread_mutex_unlock(&s->mu);

        /* Sin mutex: el ejecutor es responsable del timeout y de atender
         * atomic cancelar_solicitado. Esta tarea A1 no ejecuta pasos reales. */
        estado_t resultado = ejecutar(s, e, t, contexto);
        if (resultado != SUCCEEDED && resultado != PARTIALLY_SUCCEEDED &&
            resultado != FAILED && resultado != CANCELED)
            resultado = FAILED;

        pthread_mutex_lock(&s->mu);
        if (atomic_load(&e->cancelar_solicitado)) resultado = CANCELED;
        /* Un ejecutor de prueba puede simular los pasos mediante la API.
         * Un callback que retorna sin cerrar un paso RUNNING no puede dejar
         * estados a medias. Se registra fallo interno, salvo cancelacion. */
        for (size_t paso = 0; paso < t->cantidad_pasos; ++paso) {
            paso_t *p = &t->pasos[paso];
            if (p->estado == RUNNING) {
                p->estado = resultado == CANCELED ? CANCELED : FAILED;
                p->fin = reloj();
                ++t->pasos_terminales;
                if (resultado != CANCELED) resultado = FAILED;
            } else if (p->estado == QUEUED &&
                       (resultado == SUCCEEDED ||
                        resultado == PARTIALLY_SUCCEEDED)) {
                /* No se puede declarar exito omitiendo pasos sin justificacion. */
                resultado = FAILED;
            }
        }
        paso_pendiente_terminar(t, resultado == CANCELED ? CANCELED : SKIPPED);
        t->estado = resultado;
        t->fin = reloj();
        ++e->trabajos_terminales;
        --s->activos;
        --e->trabajos_activos;
        recalcular(s, e);
        pthread_cond_broadcast(&s->cambio);
        pthread_mutex_unlock(&s->mu);
    }
    return NULL;
}

int motor_paso_iniciar(servidor_t *s, ejecucion_t *e,
                      size_t id_trabajo, size_t id_paso)
{
    if (!s || !e || !e->plan || id_trabajo >= e->plan->cantidad_trabajos)
        return -1;
    trabajo_t *t = &e->plan->trabajos[id_trabajo];
    if (id_paso >= t->cantidad_pasos) return -1;
    pthread_mutex_lock(&s->mu);
    paso_t *paso = &t->pasos[id_paso];
    int bien = t->estado == RUNNING && paso->estado == QUEUED &&
               !atomic_load(&e->cancelar_solicitado);
    for (size_t k = 0; bien && k < id_paso; ++k)
        if (!terminal(t->pasos[k].estado)) bien = 0;
    if (bien) {
        paso->estado = RUNNING;
        paso->inicio = reloj();
        pthread_cond_broadcast(&s->cambio);
    }
    pthread_mutex_unlock(&s->mu);
    return bien ? 0 : -1;
}

int motor_paso_finalizar(servidor_t *s, ejecucion_t *e,
                        size_t id_trabajo, size_t id_paso,
                        estado_t resultado, int codigo_salida)
{
    if (!s || !e || !e->plan || id_trabajo >= e->plan->cantidad_trabajos ||
        (resultado != SUCCEEDED && resultado != FAILED && resultado != CANCELED))
        return -1;
    trabajo_t *t = &e->plan->trabajos[id_trabajo];
    if (id_paso >= t->cantidad_pasos) return -1;
    pthread_mutex_lock(&s->mu);
    paso_t *paso = &t->pasos[id_paso];
    int bien = t->estado == RUNNING && paso->estado == RUNNING;
    if (bien) {
        if (atomic_load(&e->cancelar_solicitado)) resultado = CANCELED;
        paso->estado = resultado;
        paso->codigo_salida = codigo_salida;
        paso->fin = reloj();
        ++t->pasos_terminales;
        if (resultado == FAILED && paso->continuar_si_falla)
            t->hubo_fallo_continuable = true;
        pthread_cond_broadcast(&s->cambio);
    }
    pthread_mutex_unlock(&s->mu);
    return bien ? 0 : -1;
}

int cancelar_ejecucion(servidor_t *s, int id)
{
    if (!s) return CANCELAR_ERROR;
    pthread_mutex_lock(&s->mu);
    ejecucion_t *e = s->ejecuciones;
    while (e && e->id != id) e = e->siguiente;
    if (!e) { pthread_mutex_unlock(&s->mu); return CANCELAR_NOT_FOUND; }
    if (terminal(e->estado)) {
        pthread_mutex_unlock(&s->mu);
        return CANCELAR_ESTADO_TERMINAL;
    }
    atomic_store(&e->cancelar_solicitado, 1);
    recalcular(s, e);
    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);
    return CANCELAR_OK;
}

void motor_solicitar_apagado(servidor_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    if (!s->apagar) {
        s->apagar = 1;
        s->apagado_iniciado = reloj();
        s->plazo_gracia = s->apagado_iniciado;
        s->plazo_gracia.tv_sec += APAGADO_GRACIA_SEGUNDOS;
        for (ejecucion_t *e = s->ejecuciones; e; e = e->siguiente)
            recalcular(s, e); /* no nuevas tareas; pending -> CANCELED */
    }
    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);
}

static void cancelar_activos(servidor_t *s)
{
    s->cancelar_activos_apagado = 1;
    for (ejecucion_t *e = s->ejecuciones; e; e = e->siguiente)
        if (e->trabajos_activos)
            atomic_store(&e->cancelar_solicitado, 1);
    pthread_cond_broadcast(&s->cambio);
}

int motor_esperar_apagado(servidor_t *s)
{
    if (!s) return -1;
    motor_solicitar_apagado(s);
    pthread_mutex_lock(&s->mu);
    while (s->activos > 0) {
        struct timespec ahora = reloj();
        if (!s->cancelar_activos_apagado &&
            tiempo_mayor_igual(ahora, s->plazo_gracia))
            cancelar_activos(s);
        if (s->cancelar_activos_apagado) {
            pthread_cond_wait(&s->cambio, &s->mu);
        } else {
            /* Condvar historica usa CLOCK_REALTIME: espera corta relativa
             * para evaluar la gracia MONOTONIC sin alterar state.h. */
            struct timespec hasta = {0, 0};
            (void)clock_gettime(CLOCK_REALTIME, &hasta);
            hasta.tv_nsec += 20000000L; /* 20ms */
            if (hasta.tv_nsec >= 1000000000L) {
                ++hasta.tv_sec;
                hasta.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&s->cambio, &s->mu, &hasta);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return 0;
}

int motor_reservar_extras(servidor_t *s, int hilos_totales)
{
    if (!s || hilos_totales < 1) return -1;
    pthread_mutex_lock(&s->mu);
    int extras = hilos_totales - 1;
    if (extras > s->extras_libres) extras = s->extras_libres;
    if (extras < 0) extras = 0;
    s->extras_libres -= extras;
    pthread_mutex_unlock(&s->mu);
    return extras;
}

int motor_devolver_extras(servidor_t *s, int extras)
{
    if (!s || extras < 0) return -1;
    pthread_mutex_lock(&s->mu);
    int capacidad = s->limite_hilos_total - s->agentes;
    if (capacidad < 0) capacidad = 0;
    if (extras > capacidad - s->extras_libres) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    s->extras_libres += extras;
    pthread_mutex_unlock(&s->mu);
    return 0;
}

static estado_t ejecutor_no_integrado(servidor_t *s, ejecucion_t *e,
                                     trabajo_t *t, void *contexto)
{
    (void)s; (void)e; (void)t; (void)contexto;
    /* TODO A2: reemplazar por ejecutor real de scripts/tasks; nunca simular
     * exito ni ejecutar un pipeline sin implementacion. */
    return FAILED;
}

static void *planificador(servidor_t *s, motor_ejecutor_trabajo_t ejecutar,
                         void *contexto)
{
    int N = s->agentes_configurados > 0 ? s->agentes_configurados : s->agentes;
    if (N < 1) N = 1;
    omp_set_dynamic(0);
    omp_set_max_active_levels(2);
    int L = omp_get_thread_limit();
    int P = omp_get_num_procs();
    if (L < 1) L = 1;
    int B = s->limite_hilos_total;
    if (B < 1) B = N > P ? N : P;
    if (B > L) B = L;
    if (B < 1) B = 1;
    int solicitar = N < B ? N : B;
    pthread_mutex_lock(&s->mu);
    s->limite_hilos_total = B;
    pthread_mutex_unlock(&s->mu);

    #pragma omp parallel num_threads(solicitar) default(none) shared(s, ejecutar, contexto, B)
    {
        #pragma omp single
        {
            pthread_mutex_lock(&s->mu);
            s->agentes = omp_get_num_threads();
            s->extras_libres = B > s->agentes ? B - s->agentes : 0;
            s->listo = 1;
            pthread_cond_broadcast(&s->cambio);
            pthread_mutex_unlock(&s->mu);
        }
        (void)trabajador(s, ejecutar, contexto);
    }
    return NULL;
}

void *motor_planificar_inyectado(void *argumento)
{
    motor_planificador_t *a = argumento;
    if (!a || !a->servidor || !a->ejecutar) return NULL;
    return planificador(a->servidor, a->ejecutar, a->contexto);
}

void *planificar(void *argumento)
{
    if (!argumento) return NULL;
    return planificador(argumento, ejecutor_no_integrado, NULL);
}
