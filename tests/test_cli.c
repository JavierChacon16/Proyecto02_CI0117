
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include "util.h"

static int ejecutar(const char *puerto, const char *comando,
                    const char *argumento, const char *esperado,
                    int salida_esperada)
{
    int tuberia[2];

    if (pipe(tuberia) != 0)
        return 1;

    pid_t pid = fork();

    if (pid < 0) {
        close(tuberia[0]);
        close(tuberia[1]);
        return 1;
    }

    if (pid == 0) {
        close(tuberia[0]);
        dup2(tuberia[1], STDOUT_FILENO);
        close(tuberia[1]);

        if (argumento) {
            execl("./pipectl", "pipectl", "127.0.0.1",
                  puerto, comando, argumento, (char *)NULL);
        } else {
            execl("./pipectl", "pipectl", "127.0.0.1",
                  puerto, comando, (char *)NULL);
        }

        _exit(127);
    }

    close(tuberia[1]);

    char respuesta[4096];
    size_t usado = 0;
    ssize_t n;

    while (usado < sizeof respuesta - 1 &&
           (n = read(tuberia[0], respuesta + usado,
                     sizeof respuesta - 1 - usado)) > 0) {
        usado += (size_t)n;
    }

    respuesta[usado] = '\0';
    close(tuberia[0]);

    int estado = 0;

    if (waitpid(pid, &estado, 0) < 0)
        return 1;

    int codigo = WIFEXITED(estado) ? WEXITSTATUS(estado) : -1;

    if (codigo != salida_esperada ||
        (esperado && !strstr(respuesta, esperado))) {
        printf("Error en %s: codigo %d, respuesta: %s\n",
               comando, codigo, respuesta);
        return 1;
    }

    return 0;
}

int main(void)
{
    alarm(20);

    char puerto[16];
    unsigned short numero =
        (unsigned short)(30000 + getpid() % 20000);

    snprintf(puerto, sizeof puerto, "%u", (unsigned)numero);

    pid_t servidor = fork();

    if (servidor < 0) {
        perror("fork");
        return 1;
    }

    if (servidor == 0) {
        execl("./pipelined", "pipelined", puerto,
              "2", ".", (char *)NULL);
        _exit(127);
    }

    int listo = 0;

    for (int i = 0; i < 40; i++) {
        int fd = nu_connect("127.0.0.1", numero);

        if (fd >= 0) {
            close(fd);
            listo = 1;
            break;
        }

        struct timespec pausa = {0, 50000000};
        nanosleep(&pausa, NULL);
    }

    int errores = 0;

    if (!listo) {
        puts("Error: el servidor no inicio");
        errores++;
    } else {
        errores += ejecutar(puerto, "list", NULL, "LIST 0", 0);
        errores += ejecutar(puerto, "status", "abc", NULL, 1);
        errores += ejecutar(puerto, "shutdown", NULL, "BYE", 0);
    }

    if (errores)
        kill(servidor, SIGTERM);

    int estado = 0;

    if (waitpid(servidor, &estado, 0) < 0 ||
        !WIFEXITED(estado) ||
        WEXITSTATUS(estado) != 0) {
        errores++;
    }

    if (errores) {
        printf("Fallaron %d pruebas del cliente\n", errores);
        return 1;
    }

    puts("Pruebas del cliente: OK");
    return 0;
}

