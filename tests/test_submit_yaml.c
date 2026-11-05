
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include "util.h"

static int enviar(unsigned short puerto, const char *yaml,
                  const char *esperado)
{
    int fd = nu_connect("127.0.0.1", puerto);

    if (fd < 0)
        return -1;

    char cabecera[64];
    char respuesta[4096];
    size_t n = strlen(yaml);

    snprintf(cabecera, sizeof cabecera, "SUBMIT %zu\n", n);

    int ok = nu_send_response(fd, cabecera) == 0 &&
             nu_write_all(fd, yaml, n) == 0 &&
             nu_read_line(fd, respuesta,
                          sizeof respuesta) >= 0 &&
             strncmp(respuesta, esperado,
                     strlen(esperado)) == 0;

    close(fd);
    return ok ? 0 : -1;
}

int main(void)
{
    alarm(20);

    unsigned short puerto =
        (unsigned short)(30000 + getpid() % 20000);

    char numero[16];

    snprintf(numero, sizeof numero, "%u", (unsigned)puerto);

    pid_t pid = fork();

    if (pid < 0)
        return 1;

    if (pid == 0) {
        execl("./pipelined", "pipelined", numero,
              "2", ".", (char *)NULL);
        _exit(127);
    }

    int listo = 0;

    for (int i = 0; i < 40; i++) {
        int fd = nu_connect("127.0.0.1", puerto);

        if (fd >= 0) {
            close(fd);
            listo = 1;
            break;
        }

        struct timespec pausa = {0, 50000000};
        nanosleep(&pausa, NULL);
    }

    int fallos = !listo;

    if (listo) {
        const char *valido =
            "name: prueba\n"
            "stages:\n"
            "  - stage: construir\n"
            "    jobs:\n"
            "      - job: compilar\n"
            "        steps:\n"
            "          - script: echo hola\n";

        const char *invalido =
            "name: prueba\n"
            "stages: []\n";

        if (enviar(puerto, invalido, "INVALID 1") != 0) {
            puts("Error: no rechazo el YAML invalido");
            fallos++;
        }

        if (enviar(puerto, valido, "OK ") != 0) {
            puts("Error: no acepto el YAML valido");
            fallos++;
        }

        int fd = nu_connect("127.0.0.1", puerto);

        if (fd < 0 ||
            nu_send_response(fd, "SHUTDOWN\n") != 0)
            fallos++;

        if (fd >= 0)
            close(fd);
    }

    if (fallos)
        kill(pid, SIGTERM);

    int estado;

    if (waitpid(pid, &estado, 0) < 0 ||
        !WIFEXITED(estado) ||
        WEXITSTATUS(estado) != 0)
        fallos++;

    if (fallos) {
        printf("Fallaron %d pruebas SUBMIT YAML\n", fallos);
        return 1;
    }

    puts("Pruebas SUBMIT YAML: OK");
    return 0;
}

