#define _POSIX_C_SOURCE 200809L
#include "script.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Verifica flujo en caliente, canales separados y fragmentos limitados. */
typedef struct {
    int stdout_lineas, stderr_lineas, fragmentos, final_sin_nl;
    int variable_stdout, variable_stderr;
    int fallar_callback;
    int tuvo_nul;
    pid_t hijo;
    char ultima[1024];
} captura_t;

static int al_linea(void *ctx, const script_linea_t *l)
{
    captura_t *c = ctx;
    assert(l->longitud <= SCRIPT_MAX_LINEA_BYTES);
    if (c->fallar_callback)
        return -1;
    if (l->canal == SCRIPT_CANAL_STDOUT) c->stdout_lineas++;
    else c->stderr_lineas++;
    if (l->fragmentada) c->fragmentos++;
    if (!l->fin_de_linea) c->final_sin_nl++;
    if (memchr(l->datos, '\0', l->longitud)) c->tuvo_nul = 1;
    if (l->longitud >= 14 &&
        memcmp(l->datos, "##setvariable ", 14) == 0 && !l->fragmentada) {
        if (l->canal == SCRIPT_CANAL_STDOUT) c->variable_stdout++;
        else c->variable_stderr++;
    }
    if (l->longitud < sizeof c->ultima) {
        memcpy(c->ultima, l->datos, l->longitud);
        c->ultima[l->longitud] = '\0';
        if (sscanf(c->ultima, "CHILD_PID=%d", (int *)&c->hijo) != 1) {
            /* no es una linea con identificador de hijo */
        }
    }
    return 0;
}

static struct timespec mas_ms(long ms)
{
    struct timespec r;
    assert(clock_gettime(CLOCK_MONOTONIC, &r) == 0);
    r.tv_sec += ms / 1000;
    r.tv_nsec += (ms % 1000) * 1000000L;
    if (r.tv_nsec >= 1000000000L) {r.tv_sec++; r.tv_nsec -= 1000000000L;}
    return r;
}

static double transcurridos(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec-a.tv_sec) +
           (double)(b.tv_nsec-a.tv_nsec) / 1000000000.0;
}

typedef struct { atomic_int *cancelar; long ms; } cancelador_t;
static void *cancelar_tarde(void *arg)
{
    cancelador_t *c = arg;
    struct timespec t = {c->ms / 1000, (c->ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
    atomic_store(c->cancelar, 1);
    return NULL;
}

/* Comprueba si el descendiente sigue EJECUTANDOSE. Un zombie de PID 1 en
 * ciertos contenedores ya esta terminado y no se puede recolectar aqui. */
static int hijo_vivo(pid_t pid)
{
    if (pid <= 0) return 1;
    char ruta[64], linea[512];
    snprintf(ruta, sizeof ruta, "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(ruta, "r");
    if (!f) return 0;
    int vivo = 1;
    if (fgets(linea, sizeof linea, f)) {
        char *p = strrchr(linea, ')');
        if (p && p[1] == ' ' && (p[2] == 'Z' || p[2] == 'X')) vivo = 0;
    }
    fclose(f);
    return vivo;
}

int main(void)
{
    char dir[] = "/tmp/ci0117_script_XXXXXX";
    assert(mkdtemp(dir));
    script_resultado_t r;
    captura_t c = {0};
    script_opciones_t op = {
        .comando = "pwd; printf '##setvariable UNO=1\\n'; "
                   "printf '##setvariable NO=2\\n' >&2; "
                   "printf '\\000x\\n'; printf 'sin_nueva_linea' >&2",
        .directorio_trabajo = dir, .al_linea = al_linea, .contexto = &c
    };
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_OK);
    assert(r.salio_normalmente && r.codigo_salida == 0);
    assert(c.stdout_lineas == 3 && c.stderr_lineas == 2);
    assert(c.variable_stdout == 1 && c.variable_stderr == 1);
    assert(c.tuvo_nul && c.final_sin_nl == 1);
    puts("OK: captura stdout/stderr, NUL, cwd, variables solo por stdout");

    c = (captura_t){0};
    op.comando = "awk 'BEGIN{for(i=0;i<70000;i++)printf \"x\"; printf \"\\n\"}'";
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_OK);
    assert(c.fragmentos >= 2 && c.stdout_lineas >= 2);
    puts("OK: lineas fragmentadas >64 KiB sin desbordar buffer");

    c = (captura_t){0};
    op.comando = "exit 7";
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_FALLO);
    assert(r.salio_normalmente && r.codigo_salida == 7);
    op.comando = "false";
    c.fallar_callback = 1;
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_FALLO);
    op.comando = "printf 'x\\n'; sleep 60";
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_ERROR_CAPTURA);
    c.fallar_callback = 0;
    puts("OK: codigos de salida y fallo del callback sin espera indefinida");

    /* El padre bloquea SIGUSR1: el hijo NO debe heredar esa mascara. */
    sigset_t bloqueada, anterior;
    sigemptyset(&bloqueada); sigaddset(&bloqueada, SIGUSR1);
    assert(pthread_sigmask(SIG_BLOCK, &bloqueada, &anterior) == 0);
    op.comando = "kill -USR1 $$; echo INCORRECTO";
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_FALLO);
    assert(!r.salio_normalmente);
    assert(pthread_sigmask(SIG_SETMASK, &anterior, NULL) == 0);
    puts("OK: mascara de senales vacia en hijo");

    op.comando = "sleep 60";
    op.plazo = NULL;
    atomic_int cancelado;
    atomic_init(&cancelado, 0);
    op.cancelar = &cancelado;
    cancelador_t canc = {&cancelado, 55};
    pthread_t hilo;
    assert(pthread_create(&hilo, NULL, cancelar_tarde, &canc) == 0);
    struct timespec ini, fin;
    clock_gettime(CLOCK_MONOTONIC, &ini);
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_CANCELADO);
    clock_gettime(CLOCK_MONOTONIC, &fin);
    assert(pthread_join(hilo, NULL) == 0);
    assert(transcurridos(ini, fin) < 0.2);
    puts("OK: cancelar script sleep 60 en menos de 200 ms");

    atomic_store(&cancelado, 0);
    c = (captura_t){0};
    op.comando = "sleep 60 & echo CHILD_PID=$!; wait";
    assert(pthread_create(&hilo, NULL, cancelar_tarde, &canc) == 0);
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_CANCELADO);
    assert(pthread_join(hilo, NULL) == 0);
    assert(c.hijo > 0);
    struct timespec pausa = {0, 30000000L};
    nanosleep(&pausa, NULL);
    assert(!hijo_vivo(c.hijo));
    puts("OK: cancelar grupo de procesos; descendiente sleep no queda vivo");

    /* Un flujo continuo de stdout tampoco puede impedir cancelar. */
    atomic_store(&cancelado, 0);
    c = (captura_t){0};
    op.comando = "while :; do printf 'linea\\n'; done";
    assert(pthread_create(&hilo, NULL, cancelar_tarde, &canc) == 0);
    clock_gettime(CLOCK_MONOTONIC, &ini);
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_CANCELADO);
    clock_gettime(CLOCK_MONOTONIC, &fin);
    assert(pthread_join(hilo, NULL) == 0);
    assert(transcurridos(ini, fin) < 0.5);
    assert(c.stdout_lineas > 10);
    puts("OK: cancelar script con stdout continuo");

    op.cancelar = NULL;
    struct timespec plazo = mas_ms(110);
    op.plazo = &plazo;
    op.comando = "sleep 60";
    clock_gettime(CLOCK_MONOTONIC, &ini);
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &fin);
    assert(transcurridos(ini, fin) < 0.4);
    puts("OK: deadline CLOCK_MONOTONIC detiene el hijo");

    r = (script_resultado_t){0};
    op.comando = "echo x";
    op.directorio_trabajo = "/directorio/que/no/existe-ci0117";
    op.plazo = NULL;
    assert(ejecutar_script_capturado(&op, &r) == SCRIPT_ERROR_CAPTURA);
    assert(!r.salio_normalmente);
    assert(rmdir(dir) == 0);
    puts("OK: cwd invalido y limpieza de descriptores");
    puts("OK: test_script_capturado Fase 2 completo");
    return 0;
}
