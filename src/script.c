
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
