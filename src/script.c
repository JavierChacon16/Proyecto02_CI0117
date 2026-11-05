
#define _POSIX_C_SOURCE 200809L

#include "script.h"
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void finalizar_grupo(pid_t pid)
{
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);

    int estado;
    while (waitpid(pid, &estado, 0) < 0 && errno == EINTR) {}
}

int ejecutar_script(const char *comando, unsigned int timeout_seg,
                    int *salida)
{
    if (!comando || !*comando)
        return -1;

    if (salida)
        *salida = -1;

    posix_spawnattr_t atributos;

    if (posix_spawnattr_init(&atributos) != 0)
        return -1;

    sigset_t mascara;
    sigemptyset(&mascara);

    int rc = posix_spawnattr_setpgroup(&atributos, 0);

    if (rc == 0)
        rc = posix_spawnattr_setsigmask(&atributos, &mascara);

    if (rc == 0)
        rc = posix_spawnattr_setflags(
            &atributos,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK
        );

    if (rc != 0) {
        posix_spawnattr_destroy(&atributos);
        return -1;
    }

    char *const argumentos[] = {
        "sh", "-c", (char *)comando, NULL
    };

    pid_t pid;

    rc = posix_spawn(&pid, "/bin/sh", NULL,
                     &atributos, argumentos, environ);

    posix_spawnattr_destroy(&atributos);

    if (rc != 0)
        return -1;

    struct timespec inicio;

    if (clock_gettime(CLOCK_MONOTONIC, &inicio) != 0) {
        finalizar_grupo(pid);
        return -1;
    }

    for (;;) {
        int estado;
        pid_t terminado = waitpid(pid, &estado, WNOHANG);

        if (terminado == pid) {
            if (WIFEXITED(estado)) {
                if (salida)
                    *salida = WEXITSTATUS(estado);

                return WEXITSTATUS(estado) == 0 ? 0 : -1;
            }

            return -1;
        }

        if (terminado < 0 && errno != EINTR) {
            finalizar_grupo(pid);
            return -1;
        }

        if (timeout_seg > 0) {
            struct timespec ahora;

            if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) {
                finalizar_grupo(pid);
                return -1;
            }

            time_t segundos = ahora.tv_sec - inicio.tv_sec;
            long nanos = ahora.tv_nsec - inicio.tv_nsec;

            if (nanos < 0)
                segundos--;

            if (segundos >= (time_t)timeout_seg) {
                finalizar_grupo(pid);
                return -2;
            }
        }

        struct timespec pausa = {0, 10000000L};

        while (nanosleep(&pausa, &pausa) != 0 &&
               errno == EINTR) {}
    }
}
