#define _POSIX_C_SOURCE 200809L
#include "motor.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPETICIONES 200
#define MAX_E 5
#define MAX_J 32

typedef struct {
    pthread_mutex_t mu;
    int activos, pico, cuenta[MAX_E][MAX_J];
    int comienzo[MAX_E][MAX_J], termino[MAX_E][MAX_J];
    int orden[MAX_E * MAX_J], n_orden, tick;
    estado_t resultados[MAX_E][MAX_J];
    int us, esperar_cancelacion;
} falso_t;

typedef struct {
    servidor_t s;
    pthread_t hilo;
    motor_planificador_t args;
} entorno_t;

typedef struct {
    ejecucion_t *e;
    pipeline_t *p;
} caso_t;

static struct timespec ahora_realtime(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_REALTIME, &t) == 0);
    return t;
}

static void pausa_us(int us)
{
    if (us < 1) return;
    struct timespec t = {.tv_sec = us / 1000000,
                         .tv_nsec = (long)(us % 1000000) * 1000L};
    while (nanosleep(&t, &t) != 0) {}
}

static void falso_iniciar(falso_t *f, int duracion)
{
    memset(f, 0, sizeof *f);
    assert(pthread_mutex_init(&f->mu, NULL) == 0);
    f->us = duracion;
    for (int e = 0; e < MAX_E; ++e)
        for (int j = 0; j < MAX_J; ++j) f->resultados[e][j] = SUCCEEDED;
}

static void falso_destruir(falso_t *f)
{
    assert(pthread_mutex_destroy(&f->mu) == 0);
}

static estado_t ejecutar_falso(servidor_t *s, ejecucion_t *e,
                              trabajo_t *t, void *ctx)
{
    falso_t *f = ctx;
    assert(e->id > 0 && e->id < MAX_E && t->indice_global < MAX_J);
    int id = e->id;
    size_t j = t->indice_global;
    pthread_mutex_lock(&f->mu);
    ++f->cuenta[id][j];
    assert(f->cuenta[id][j] == 1);
    ++f->activos;
    if (f->activos > f->pico) f->pico = f->activos;
    f->comienzo[id][j] = ++f->tick;
    assert(f->n_orden < MAX_E * MAX_J);
    f->orden[f->n_orden++] = id * MAX_J + (int)j;
    pthread_mutex_unlock(&f->mu);

    int paso_iniciado = 0;
    if (t->cantidad_pasos) {
        if (motor_paso_iniciar(s, e, j, 0) == 0) paso_iniciado = 1;
        else assert(atomic_load(&e->cancelar_solicitado));
    }
    if (f->esperar_cancelacion) {
        for (int n = 0; n < 800; ++n) {
            if (atomic_load(&e->cancelar_solicitado)) break;
            pausa_us(5000);
        }
    } else {
        /* Fraccionar para permitir cancelacion en medio del trabajo. */
        int pasos = f->us > 1000 ? f->us / 1000 : 1;
        for (int k = 0; k < pasos; ++k) {
            if (atomic_load(&e->cancelar_solicitado)) break;
            pausa_us(f->us / pasos);
        }
    }
    estado_t r = f->resultados[id][j];
    if (atomic_load(&e->cancelar_solicitado)) r = CANCELED;
    if (paso_iniciado) {
        estado_t paso = r == PARTIALLY_SUCCEEDED ? FAILED : r;
        assert(motor_paso_finalizar(s, e, j, 0, paso,
                                   paso == FAILED ? 1 : 0) == 0);
    }
    pthread_mutex_lock(&f->mu);
    f->termino[id][j] = ++f->tick;
    --f->activos;
    pthread_mutex_unlock(&f->mu);
    return r;
}

static entorno_t *entorno_crear(int agentes, int B, falso_t *f)
{
    entorno_t *ent = calloc(1, sizeof *ent);
    assert(ent);
    assert(pthread_mutex_init(&ent->s.mu, NULL) == 0);
    assert(pthread_mutex_init(&ent->s.mu_id, NULL) == 0);
    assert(pthread_cond_init(&ent->s.cambio, NULL) == 0);
    ent->s.agentes_configurados = agentes;
    ent->s.agentes = agentes;
    ent->s.limite_hilos_total = B;
    ent->s.proximo_id = 1;
    ent->s.timeout_defecto_segundos = 10;
    ent->args.servidor = &ent->s;
    ent->args.ejecutar = ejecutar_falso;
    ent->args.contexto = f;
    assert(pthread_create(&ent->hilo, NULL, motor_planificar_inyectado,
                          &ent->args) == 0);
    pthread_mutex_lock(&ent->s.mu);
    while (!ent->s.listo)
        pthread_cond_wait(&ent->s.cambio, &ent->s.mu);
    assert(ent->s.agentes >= 1 && ent->s.agentes <= agentes);
    assert(ent->s.extras_libres == ent->s.limite_hilos_total - ent->s.agentes);
    pthread_mutex_unlock(&ent->s.mu);
    return ent;
}

static void entorno_apagar(entorno_t *ent)
{
    motor_solicitar_apagado(&ent->s);
    assert(motor_esperar_apagado(&ent->s) == 0);
    assert(pthread_join(ent->hilo, NULL) == 0);
    assert(ent->s.activos == 0 && ent->s.trabajos_listos == 0);
    assert(pthread_cond_destroy(&ent->s.cambio) == 0);
    assert(pthread_mutex_destroy(&ent->s.mu) == 0);
    assert(pthread_mutex_destroy(&ent->s.mu_id) == 0);
    free(ent);
}

static caso_t caso_crear(int id, size_t num_etapas, const size_t *cantidades)
{
    caso_t c = {0};
    c.e = calloc(1, sizeof *c.e);
    c.p = calloc(1, sizeof *c.p);
    assert(c.e && c.p);
    c.e->id = id;
    c.e->plan = c.p;
    c.p->nombre = (char *)"prueba";
    c.p->cantidad_etapas = num_etapas;
    c.p->etapas = calloc(num_etapas, sizeof *c.p->etapas);
    for (size_t i = 0; i < num_etapas; ++i)
        c.p->cantidad_trabajos += cantidades[i];
    assert(c.p->cantidad_trabajos <= MAX_J);
    c.p->trabajos = calloc(c.p->cantidad_trabajos, sizeof *c.p->trabajos);
    assert(c.p->etapas && c.p->trabajos);
    size_t id_j = 0;
    for (size_t i = 0; i < num_etapas; ++i) {
        etapa_t *et = &c.p->etapas[i];
        et->indice = i;
        et->primer_trabajo = id_j;
        et->cantidad_trabajos = cantidades[i];
        for (size_t j = 0; j < cantidades[i]; ++j) {
            trabajo_t *t = &c.p->trabajos[id_j];
            t->indice_global = id_j;
            t->indice_etapa = i;
            t->cantidad_pasos = 1;
            t->pasos = calloc(1, sizeof *t->pasos);
            assert(t->pasos);
            ++id_j;
        }
    }
    return c;
}

static void caso_destruir(caso_t *c)
{
    for (size_t i = 0; i < c->p->cantidad_etapas; ++i)
        free(c->p->etapas[i].dependencias);
    for (size_t i = 0; i < c->p->cantidad_trabajos; ++i) {
        trabajo_t *t = &c->p->trabajos[i];
        free(t->dependencias);
        free(t->dependencias_condicion);
        free(t->pasos);
    }
    free(c->p->etapas);
    free(c->p->trabajos);
    free(c->p);
    free(c->e);
}

static void dep_trabajo(caso_t *c, size_t job, size_t dep,
                         condicion_t condicion_job)
{
    trabajo_t *t = &c->p->trabajos[job];
    t->dependencias = malloc(sizeof *t->dependencias);
    t->dependencias_condicion = malloc(sizeof *t->dependencias_condicion);
    assert(t->dependencias && t->dependencias_condicion);
    t->dependencias[0] = t->dependencias_condicion[0] = dep;
    t->cantidad_dependencias = t->cantidad_dependencias_condicion = 1;
    t->condicion = condicion_job;
}

static void dep_etapa(caso_t *c, size_t etapa, size_t anterior,
                       condicion_t cond)
{
    etapa_t *et = &c->p->etapas[etapa];
    et->cantidad_dependencias = 1;
    et->dependencias = malloc(sizeof *et->dependencias);
    assert(et->dependencias);
    et->dependencias[0] = anterior;
    et->condicion = cond;
    /* El parser convierte el arco de etapa en una barrera de espera.
     * Para este fixture cada etapa de recuperacion contiene un solo job. */
    trabajo_t *t = &c->p->trabajos[et->primer_trabajo];
    t->cantidad_dependencias = 1;
    t->dependencias = malloc(sizeof *t->dependencias);
    assert(t->dependencias);
    t->dependencias[0] = c->p->etapas[anterior].primer_trabajo;
}

static void esperar_terminal(servidor_t *s, ejecucion_t *e)
{
    struct timespec limite = ahora_realtime();
    limite.tv_sec += 8;
    pthread_mutex_lock(&s->mu);
    while (e->estado == QUEUED || e->estado == RUNNING) {
        int rc = pthread_cond_timedwait(&s->cambio, &s->mu, &limite);
        if (rc) {
            fprintf(stderr, "Timeout al esperar ejecucion %d estado %d\n",
                    e->id, e->estado);
            abort();
        }
    }
    pthread_mutex_unlock(&s->mu);
}

static void esperar_inicio(servidor_t *s, ejecucion_t *e)
{
    struct timespec limite = ahora_realtime();
    limite.tv_sec += 8;
    pthread_mutex_lock(&s->mu);
    while (e->trabajos_activos == 0) {
        int rc = pthread_cond_timedwait(&s->cambio, &s->mu, &limite);
        if (rc) abort();
    }
    pthread_mutex_unlock(&s->mu);
}

static void test_limite_y_justicia(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 2000);
        entorno_t *ent = entorno_crear(2, 2, &f);
        size_t cantidad_a[] = {10}, cantidad_b[] = {3};
        caso_t a = caso_crear(1, 1, cantidad_a);
        caso_t b = caso_crear(2, 1, cantidad_b);
        assert(motor_encolar_ejecucion(&ent->s, a.e) == 0);
        assert(motor_encolar_ejecucion(&ent->s, b.e) == 0);
        esperar_terminal(&ent->s, a.e);
        esperar_terminal(&ent->s, b.e);
        assert(a.e->estado == SUCCEEDED && b.e->estado == SUCCEEDED);
        assert(ent->s.max_activos <= ent->s.agentes && f.pico <= ent->s.agentes);
        for (int j = 0; j < 10; ++j) assert(f.cuenta[1][j] == 1);
        for (int j = 0; j < 3; ++j) assert(f.cuenta[2][j] == 1);
        /* No se exige precedencia sobre A1 si ya estaba en curso al admitir B.
         * Con 10 jobs A, al menos un B debe despacharse antes que A5. */
        int pos_b = INT_MAX, pos_a5 = INT_MAX;
        for (int k = 0; k < f.n_orden; ++k) {
            if (f.orden[k] / MAX_J == 2 && pos_b == INT_MAX) pos_b = k;
            if (f.orden[k] == MAX_J + 5) pos_a5 = k;
        }
        assert(pos_b < pos_a5);
        entorno_apagar(ent);
        caso_destruir(&a); caso_destruir(&b); falso_destruir(&f);
    }
    puts("OK: limite global, round-robin y no doble despacho (200 repeticiones)");
}

static void test_justicia_un_agente(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 2000);
        entorno_t *ent = entorno_crear(1, 1, &f);
        size_t ancha[] = {10}, corta[] = {2};
        caso_t a = caso_crear(1, 1, ancha), b = caso_crear(2, 1, corta);
        assert(motor_encolar_ejecucion(&ent->s, a.e) == 0);
        assert(motor_encolar_ejecucion(&ent->s, b.e) == 0);
        esperar_terminal(&ent->s, a.e);
        esperar_terminal(&ent->s, b.e);
        assert(f.pico == 1 && ent->s.max_activos <= 1);
        assert(b.e->estado == SUCCEEDED && a.e->estado == SUCCEEDED);
        int pos_b = INT_MAX, pos_a3 = INT_MAX;
        for (int i = 0; i < f.n_orden; ++i) {
            if (f.orden[i] / MAX_J == 2 && pos_b == INT_MAX) pos_b = i;
            if (f.orden[i] == MAX_J + 3) pos_a3 = i;
        }
        assert(pos_b < pos_a3);
        entorno_apagar(ent);
        caso_destruir(&a); caso_destruir(&b); falso_destruir(&f);
    }
    puts("OK: justicia round-robin con 1 agente (200 repeticiones)");
}

static void test_dependencias_condiciones(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 300);
        entorno_t *ent = entorno_crear(3, 4, &f);
        size_t n[] = {5};
        caso_t c = caso_crear(1, 1, n);
        dep_trabajo(&c, 1, 0, COND_SUCCEEDED);
        dep_trabajo(&c, 2, 1, COND_FAILED);
        dep_trabajo(&c, 3, 2, COND_ALWAYS);
        dep_trabajo(&c, 4, 0, COND_FAILED);
        f.resultados[1][0] = FAILED;
        assert(motor_encolar_ejecucion(&ent->s, c.e) == 0);
        esperar_terminal(&ent->s, c.e);
        assert(c.p->trabajos[0].estado == FAILED);
        assert(c.p->trabajos[1].estado == SKIPPED);
        assert(c.p->trabajos[2].estado == SKIPPED);
        assert(c.p->trabajos[3].estado == SUCCEEDED);
        assert(c.p->trabajos[4].estado == SUCCEEDED);
        assert(c.p->etapas[0].estado == FAILED);
        assert(c.e->estado == FAILED);
        assert(f.cuenta[1][0] == 1 && f.cuenta[1][1] == 0 &&
               f.cuenta[1][2] == 0 && f.cuenta[1][3] == 1 &&
               f.cuenta[1][4] == 1);
        assert(f.comienzo[1][3] > f.termino[1][0]);
        assert(f.comienzo[1][4] > f.termino[1][0]);
        entorno_apagar(ent); caso_destruir(&c); falso_destruir(&f);
    }
    puts("OK: DAG, failed/always y skipped en cascada (200 repeticiones)");
}

static void test_etapas_y_parcial(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 150);
        entorno_t *ent = entorno_crear(2, 4, &f);
        size_t n[] = {1, 1, 1};
        caso_t c = caso_crear(1, 3, n);
        dep_etapa(&c, 1, 0, COND_FAILED);
        dep_etapa(&c, 2, 1, COND_ALWAYS);
        f.resultados[1][0] = FAILED;
        f.resultados[1][1] = PARTIALLY_SUCCEEDED;
        c.p->trabajos[1].pasos[0].continuar_si_falla = true;
        assert(motor_encolar_ejecucion(&ent->s, c.e) == 0);
        esperar_terminal(&ent->s, c.e);
        assert(c.p->etapas[0].estado == FAILED);
        assert(c.p->etapas[1].estado == PARTIALLY_SUCCEEDED);
        assert(c.p->etapas[2].estado == SUCCEEDED);
        assert(c.p->trabajos[1].estado == PARTIALLY_SUCCEEDED);
        assert(c.p->trabajos[1].pasos[0].estado == FAILED);
        assert(c.p->trabajos[1].hubo_fallo_continuable);
        assert(c.e->estado == FAILED);
        assert(f.comienzo[1][1] > f.termino[1][0]);
        assert(f.comienzo[1][2] > f.termino[1][1]);
        entorno_apagar(ent); caso_destruir(&c); falso_destruir(&f);
    }
    puts("OK: condiciones de etapas, precedencia y agregacion (200 repeticiones)");
}

static void test_cancelacion(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 5000);
        f.esperar_cancelacion = 1;
        entorno_t *ent = entorno_crear(1, 2, &f);
        size_t n[] = {6};
        caso_t a = caso_crear(1, 1, n), b = caso_crear(2, 1, n);
        assert(motor_encolar_ejecucion(&ent->s, a.e) == 0);
        esperar_inicio(&ent->s, a.e);
        assert(motor_encolar_ejecucion(&ent->s, b.e) == 0);
        assert(cancelar_ejecucion(&ent->s, 9911) == CANCELAR_NOT_FOUND);
        assert(cancelar_ejecucion(&ent->s, b.e->id) == CANCELAR_OK);
        assert(cancelar_ejecucion(&ent->s, a.e->id) == CANCELAR_OK);
        esperar_terminal(&ent->s, a.e);
        esperar_terminal(&ent->s, b.e);
        assert(a.e->estado == CANCELED && b.e->estado == CANCELED);
        assert(cancelar_ejecucion(&ent->s, a.e->id) == CANCELAR_ESTADO_TERMINAL);
        assert(f.cuenta[1][0] == 1 && f.cuenta[2][0] == 0);
        for (int j = 1; j < 6; ++j) {
            assert(f.cuenta[1][j] == 0 && f.cuenta[2][j] == 0);
            assert(a.p->trabajos[j].estado == CANCELED);
        }
        entorno_apagar(ent); caso_destruir(&a); caso_destruir(&b);
        falso_destruir(&f);
    }
    puts("OK: cancelacion activa y en cola, sin doble trabajo (200 repeticiones)");
}

static void test_fallo_interno(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 1000);
        f.esperar_cancelacion = 1;
        entorno_t *ent = entorno_crear(1, 1, &f);
        size_t n[] = {3};
        caso_t c = caso_crear(1, 1, n);
        dep_trabajo(&c, 2, 1, COND_FAILED);
        assert(motor_encolar_ejecucion(&ent->s, c.e) == 0);
        esperar_inicio(&ent->s, c.e);
        assert(motor_fallar_trabajo_preparacion(&ent->s, c.e, 1) == 0);
        assert(motor_fallar_trabajo_preparacion(&ent->s, c.e, 1) == -1);
        assert(c.p->trabajos[1].estado == FAILED);
        /* Para liberar el job activo despues de la prueba del fallo interno. */
        assert(cancelar_ejecucion(&ent->s, c.e->id) == CANCELAR_OK);
        esperar_terminal(&ent->s, c.e);
        assert(c.e->estado == CANCELED);
        assert(f.cuenta[1][1] == 0 && f.cuenta[1][2] == 0);
        entorno_apagar(ent); caso_destruir(&c); falso_destruir(&f);
    }
    puts("OK: queued -> failed por error interno (200 repeticiones)");
}

static void test_extras_y_shutdown(void)
{
    for (int rep = 0; rep < REPETICIONES; ++rep) {
        falso_t f; falso_iniciar(&f, 500);
        entorno_t *ent = entorno_crear(2, 4, &f);
        assert(motor_reservar_extras(&ent->s, 4) == 2);
        assert(motor_reservar_extras(&ent->s, 100) == 0);
        assert(motor_devolver_extras(&ent->s, 1) == 0);
        assert(motor_reservar_extras(&ent->s, 3) == 1);
        assert(motor_devolver_extras(&ent->s, 2) == 0);
        assert(motor_devolver_extras(&ent->s, 1) == -1);
        size_t n[] = {7};
        caso_t a = caso_crear(1, 1, n);
        assert(motor_encolar_ejecucion(&ent->s, a.e) == 0);
        /* Apagado inmediato: no exige empezar algun trabajo. */
        motor_solicitar_apagado(&ent->s);
        entorno_apagar(ent);
        assert(a.e->estado == CANCELED || a.e->estado == SUCCEEDED);
        caso_destruir(&a); falso_destruir(&f);
    }
    puts("OK: presupuesto de extras y shutdown rapido (200 repeticiones)");

    /* Esta prueba es deliberadamente UNA ejecucion lenta: verifica t0+2s
     * sin sumar 400 segundos innecesarios al bucle de stress. */
    falso_t f; falso_iniciar(&f, 10000);
    f.esperar_cancelacion = 1;
    entorno_t *ent = entorno_crear(1, 1, &f);
    size_t n[] = {2};
    caso_t a = caso_crear(1, 1, n);
    assert(motor_encolar_ejecucion(&ent->s, a.e) == 0);
    esperar_inicio(&ent->s, a.e);
    struct timespec inicio;
    assert(clock_gettime(CLOCK_MONOTONIC, &inicio) == 0);
    motor_solicitar_apagado(&ent->s);
    entorno_apagar(ent);
    struct timespec fin;
    assert(clock_gettime(CLOCK_MONOTONIC, &fin) == 0);
    double elapsed = (fin.tv_sec - inicio.tv_sec) +
                     (fin.tv_nsec - inicio.tv_nsec) * 1e-9;
    assert(elapsed >= 1.95 && elapsed < 3.5);
    assert(a.e->estado == CANCELED);
    assert(f.cuenta[1][0] == 1 && f.cuenta[1][1] == 0);
    caso_destruir(&a); falso_destruir(&f);
    printf("OK: shutdown con gracia 2s, demora observada %.3f s\n", elapsed);
}

int main(void)
{
    test_limite_y_justicia();
    test_justicia_un_agente();
    test_dependencias_condiciones();
    test_etapas_y_parcial();
    test_cancelacion();
    test_fallo_interno();
    test_extras_y_shutdown();
    puts("OK: test_motor A1 completo");
    return 0;
}
