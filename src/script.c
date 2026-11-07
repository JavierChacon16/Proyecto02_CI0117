
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "script.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void terminar_proceso(pid_t pid)
{
    /* El hijo es lider de un grupo de procesos nuevo. */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    int estado;
    while (waitpid(pid, &estado, 0) < 0 && errno == EINTR) {}
}

int ejecutar_script_controlado(const char *comando, unsigned timeout_seg,
                              int *salida, const char *log_ruta,
                              const atomic_int *cancelar)
{
    if (salida)
        *salida = -1;
    if (!comando || !*comando ||
        (cancelar && atomic_load(cancelar)))
        return cancelar && atomic_load(cancelar) ? -3 : -1;

    posix_spawnattr_t atributos;
    posix_spawn_file_actions_t acciones;
    if (posix_spawnattr_init(&atributos) != 0)
        return -1;
    if (posix_spawn_file_actions_init(&acciones) != 0) {
        posix_spawnattr_destroy(&atributos);
        return -1;
    }

    int rc = posix_spawnattr_setpgroup(&atributos, 0);
    if (rc == 0)
        rc = posix_spawnattr_setflags(&atributos, POSIX_SPAWN_SETPGROUP);
    if (rc == 0 && log_ruta) {
        rc = posix_spawn_file_actions_addopen(
            &acciones, STDOUT_FILENO, log_ruta,
            O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (rc == 0)
            rc = posix_spawn_file_actions_adddup2(
                &acciones, STDOUT_FILENO, STDERR_FILENO);
    }

    pid_t pid = -1;
    if (rc == 0) {
        char *const argv[] = {"sh", "-c", (char *)comando, NULL};
        rc = posix_spawn(&pid, "/bin/sh", &acciones,
                         &atributos, argv, environ);
    }
    posix_spawn_file_actions_destroy(&acciones);
    posix_spawnattr_destroy(&atributos);
    if (rc != 0)
        return -1;

    struct timespec inicio;
    if (clock_gettime(CLOCK_MONOTONIC, &inicio) != 0) {
        terminar_proceso(pid);
        return -1;
    }

    for (;;) {
        int estado;
        pid_t listo = waitpid(pid, &estado, WNOHANG);
        if (listo == pid) {
            if (WIFEXITED(estado)) {
                if (salida)
                    *salida = WEXITSTATUS(estado);
                return WEXITSTATUS(estado) == 0 ? 0 : -1;
            }
            return -1;
        }
        if (listo < 0 && errno != EINTR)
            return -1;
        if (cancelar && atomic_load(cancelar)) {
            terminar_proceso(pid);
            return -3;
        }
        if (timeout_seg) {
            struct timespec ahora;
            if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) {
                terminar_proceso(pid);
                return -1;
            }
            time_t delta = ahora.tv_sec - inicio.tv_sec;
            long ns = ahora.tv_nsec - inicio.tv_nsec;
            if (ns < 0)
                --delta;
            if (delta >= (time_t)timeout_seg) {
                terminar_proceso(pid);
                return -2;
            }
        }
        struct timespec pausa = {0, 10000000L};
        while (nanosleep(&pausa, &pausa) == -1 && errno == EINTR) {}
    }
}

int ejecutar_script(const char *comando, unsigned timeout_seg, int *salida)
{
    return ejecutar_script_controlado(comando, timeout_seg,
                                      salida, NULL, NULL);
}

/* API v2: captura los dos canales en el agente, sin hilos lectores.
 * Los descriptores son privados del proceso; nunca se cambia el cwd global. */
#include <poll.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    script_canal_t canal;
    unsigned char linea[SCRIPT_MAX_LINEA_BYTES];
    size_t longitud;
    bool dividida;
} lector_script_t;

static void cerrar_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static int poner_no_bloqueante(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0
        ? 0 : -1;
}

static int emitir_linea(lector_script_t *l, bool fin, bool fragmentada,
                        const script_opciones_t *op)
{
    if (op->al_linea) {
        script_linea_t evento = {
            .canal = l->canal, .datos = (const char *)l->linea,
            .longitud = l->longitud, .fin_de_linea = fin,
            .fragmentada = fragmentada
        };
        if (op->al_linea(op->contexto, &evento) != 0)
            return -1;
    }
    l->longitud = 0;
    if (fin)
        l->dividida = false;
    return 0;
}

/* Lee bytes; no confunde NUL con fin y nunca mezcla stdout con stderr.
 * Una linea mayor a 64 KiB se informa por fragmentos. */
static int consumir_bytes(lector_script_t *l, const unsigned char *datos,
                         size_t n, const script_opciones_t *op)
{
    for (size_t i = 0; i < n; i++) {
        if (datos[i] == '\n') {
            if (emitir_linea(l, true, l->dividida, op) != 0)
                return -1;
        } else {
            if (l->longitud == SCRIPT_MAX_LINEA_BYTES) {
                l->dividida = true;
                if (emitir_linea(l, false, true, op) != 0)
                    return -1;
            }
            l->linea[l->longitud++] = datos[i];
        }
    }
    return 0;
}

static int motivo_interrupcion(const script_opciones_t *op);

static int drenar_canal(lector_script_t *l, const script_opciones_t *op)
{
    unsigned char b[8192];
    for (;;) {
        int motivo = motivo_interrupcion(op);
        if (motivo != SCRIPT_OK)
            return motivo;
        ssize_t n = read(l->fd, b, sizeof b);
        if (n > 0) {
            if (consumir_bytes(l, b, (size_t)n, op) != 0)
                return SCRIPT_ERROR_CAPTURA;
        } else if (n == 0) {
            if (l->longitud != 0 &&
                emitir_linea(l, false, l->dividida, op) != 0)
                return SCRIPT_ERROR_CAPTURA;
            cerrar_fd(&l->fd);
            return SCRIPT_OK;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SCRIPT_OK;
        } else {
            return SCRIPT_ERROR_CAPTURA;
        }
    }
}

static int comparar_instantes(struct timespec a, struct timespec b)
{
    if (a.tv_sec != b.tv_sec)
        return a.tv_sec > b.tv_sec ? 1 : -1;
    return (a.tv_nsec > b.tv_nsec) - (a.tv_nsec < b.tv_nsec);
}

static int motivo_interrupcion(const script_opciones_t *op)
{
    if (op->cancelar && atomic_load(op->cancelar))
        return SCRIPT_CANCELADO;
    if (op->plazo) {
        struct timespec ahora;
        if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0)
            return SCRIPT_ERROR_CAPTURA;
        if (comparar_instantes(ahora, *op->plazo) >= 0)
            return SCRIPT_TIMEOUT;
    }
    return SCRIPT_OK;
}

static int preparar_canal(int p[2])
{
    /* pipe2 hace O_CLOEXEC atomico: ningun spawn concurrente puede
     * heredar estos descriptores entre pipe() y fcntl(). */
    return pipe2(p, O_CLOEXEC) == 0 ? 0 : -1;
}

/* Tras posix_spawn siempre se espera al hijo. Al abortar se mata el grupo
 * entero (shell y descendientes); nunca se deja al hijo directo zombie. */
static int esperar_hijo(pid_t pid, int *estado)
{
    for (;;) {
        pid_t r = waitpid(pid, estado, 0);
        if (r == pid)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
}

int ejecutar_script_capturado(const script_opciones_t *op,
                             script_resultado_t *resultado)
{
    if (resultado) {
        resultado->codigo_salida = -1;
        resultado->salio_normalmente = false;
    }
    if (!op || !op->comando || !*op->comando || !resultado)
        return SCRIPT_FALLO;

    int motivo = motivo_interrupcion(op);
    if (motivo != SCRIPT_OK)
        return motivo;

    int out[2] = {-1, -1}, err[2] = {-1, -1};
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    bool attr_ok = false, act_ok = false;
    pid_t pid = -1;
    int rc = SCRIPT_ERROR_CAPTURA;
    int estado_hijo = 0;
    bool recogido = false;

    if (preparar_canal(out) != 0 || preparar_canal(err) != 0)
        goto limpiar;
    if (posix_spawnattr_init(&attr) != 0)
        goto limpiar;
    attr_ok = true;
    if (posix_spawn_file_actions_init(&actions) != 0)
        goto limpiar;
    act_ok = true;
    sigset_t vacia;
    sigemptyset(&vacia);
    int e = posix_spawnattr_setsigmask(&attr, &vacia);
    if (!e)
        e = posix_spawnattr_setpgroup(&attr, 0);
    if (!e)
        e = posix_spawnattr_setflags(&attr,
                     POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    if (!e)
        e = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    if (!e)
        e = posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    if (!e)
        e = posix_spawn_file_actions_addclose(&actions, out[0]);
    if (!e)
        e = posix_spawn_file_actions_addclose(&actions, err[0]);
    if (!e)
        e = posix_spawn_file_actions_addclose(&actions, out[1]);
    if (!e)
        e = posix_spawn_file_actions_addclose(&actions, err[1]);
    if (!e && op->directorio_trabajo)
        e = posix_spawn_file_actions_addchdir_np(&actions,
                                                 op->directorio_trabajo);
    if (e)
        goto limpiar;

    /* La mascara de senales del padre NO se propaga al script. */
    char *const argv[] = {"sh", "-c", (char *)op->comando, NULL};
    e = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ);
    if (e)
        goto limpiar;
    cerrar_fd(&out[1]);
    cerrar_fd(&err[1]);
    if (poner_no_bloqueante(out[0]) != 0 ||
        poner_no_bloqueante(err[0]) != 0)
        goto abortar;

    lector_script_t canales[2] = {
        {.fd = out[0], .canal = SCRIPT_CANAL_STDOUT},
        {.fd = err[0], .canal = SCRIPT_CANAL_STDERR}
    };
    /* Los lectores ahora son propietarios de esos extremos. */
    out[0] = -1;
    err[0] = -1;
    for (;;) {
        motivo = motivo_interrupcion(op);
        if (motivo != SCRIPT_OK) {
            rc = motivo;
            break;
        }
        if (!recogido) {
            pid_t r = waitpid(pid, &estado_hijo, WNOHANG);
            if (r == pid)
                recogido = true;
            else if (r < 0 && errno != EINTR) {
                rc = SCRIPT_ERROR_CAPTURA;
                break;
            }
        }
        if (canales[0].fd < 0 && canales[1].fd < 0 && recogido) {
            resultado->salio_normalmente = WIFEXITED(estado_hijo);
            if (resultado->salio_normalmente)
                resultado->codigo_salida = WEXITSTATUS(estado_hijo);
            rc = resultado->salio_normalmente &&
                 resultado->codigo_salida == 0 ? SCRIPT_OK : SCRIPT_FALLO;
            break;
        }
        struct pollfd pfds[2];
        nfds_t cuantos = 0;
        for (int i = 0; i < 2; i++) {
            if (canales[i].fd >= 0) {
                pfds[cuantos].fd = canales[i].fd;
                pfds[cuantos].events = POLLIN | POLLHUP;
                pfds[cuantos].revents = 0;
                cuantos++;
            }
        }
        int listo = poll(pfds, cuantos, 20);
        if (listo < 0 && errno != EINTR) {
            rc = SCRIPT_ERROR_CAPTURA;
            break;
        }
        if (listo <= 0)
            continue;
        for (int i = 0; i < 2; i++) {
            if (canales[i].fd >= 0) {
                int dr = drenar_canal(&canales[i], op);
                if (dr != SCRIPT_OK) {
                    rc = dr;
                    goto cerrar_lectores;
                }
            }
        }
    }
cerrar_lectores:
    cerrar_fd(&canales[0].fd);
    cerrar_fd(&canales[1].fd);
    if (rc != SCRIPT_OK && rc != SCRIPT_FALLO) {
        /* Aun si la shell salio, sus descendientes pueden seguir vivos. */
        (void)kill(-pid, SIGKILL);
        if (!recogido)
            (void)kill(pid, SIGKILL);
    }
    if (!recogido && esperar_hijo(pid, &estado_hijo) != 0)
        rc = SCRIPT_ERROR_CAPTURA;
    pid = -1;
    goto limpiar;

abortar:
    if (pid > 0) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)esperar_hijo(pid, &estado_hijo);
        pid = -1;
    }
limpiar:
    if (act_ok)
        posix_spawn_file_actions_destroy(&actions);
    if (attr_ok)
        posix_spawnattr_destroy(&attr);
    cerrar_fd(&out[0]);
    cerrar_fd(&out[1]);
    cerrar_fd(&err[0]);
    cerrar_fd(&err[1]);
    return rc;
}
