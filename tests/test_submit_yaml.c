
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
        return 1;

    char cabecera[64];
    char respuesta[4096];
    size_t largo = strlen(yaml);

    snprintf(cabecera, sizeof cabecera,
             "SUBMIT %zu\n", largo);

    int error = 0;

    if (nu_send_response(fd, cabecera) != 0 ||
        nu_write_all(fd, yaml, largo) != 0) {
        error = 1;
    } else {
        ssize_t n = nu_read_line(
            fd, respuesta, sizeof respuesta);

        if (n < 0 || strcmp(respuesta, esperado) != 0) {
            printf("Se esperaba %s y se recibio %s\n",
                   esperado, n >= 0 ? respuesta : "error");
            error = 1;
        }
    }

    close(fd);
    return error;
}

static int esperar_servidor(unsigned short puerto)
{
    for (int i = 0; i < 40; i++) {
        int fd = nu_connect("127.0.0.1", puerto);

        if (fd >= 0) {
            close(fd);
            return 0;
        }

        struct timespec pausa = {0, 50000000};
        nanosleep(&pausa, NULL);
    }

    return 1;
}

int main(void)
{
    alarm(30);

    unsigned short puerto =
        (unsigned short)(30000 + getpid() % 20000);

    char numero[16];

    snprintf(numero, sizeof numero,
             "%u", (unsigned)puerto);

    pid_t servidor = fork();

    if (servidor < 0) {
        perror("fork");
        return 1;
    }

    if (servidor == 0) {
        execl("./pipelined", "pipelined",
              numero, "2", ".", (char *)NULL);
        _exit(127);
    }

    int errores = 0;

    if (esperar_servidor(puerto) != 0) {
        puts("Error al iniciar el servidor");
        errores++;
    } else {
        const char *estructura_invalida =
            "name: prueba\n"
            "stages: []\n";

        const char *etapa_inexistente =
            "name: prueba\n"
            "stages:\n"
            "  - stage: construir\n"
            "    dependsOn: otra\n"
            "    jobs:\n"
            "      - job: compilar\n"
            "        steps:\n"
            "          - script: echo hola\n";

        const char *ciclo =
            "name: prueba\n"
            "stages:\n"
            "  - stage: a\n"
            "    dependsOn: b\n"
            "    jobs:\n"
            "      - job: uno\n"
            "        steps:\n"
            "          - script: echo uno\n"
            "  - stage: b\n"
            "    dependsOn: a\n"
            "    jobs:\n"
            "      - job: dos\n"
            "        steps:\n"
            "          - script: echo dos\n";

        const char *condicion_invalida =
            "name: prueba\n"
            "stages:\n"
            "  - stage: construir\n"
            "    condition: cualquiera()\n"
            "    jobs:\n"
            "      - job: compilar\n"
            "        steps:\n"
            "          - script: echo hola\n";

        const char *trabajo_inexistente =
            "name: prueba\n"
            "stages:\n"
            "  - stage: construir\n"
            "    jobs:\n"
            "      - job: compilar\n"
            "        dependsOn: otro\n"
            "        steps:\n"
            "          - script: echo hola\n";

        const char *valido =
            "name: prueba\n"
            "stages:\n"
            "  - stage: construir\n"
            "    jobs:\n"
            "      - job: compilar\n"
            "        steps:\n"
            "          - script: echo hola\n"
            "  - stage: probar\n"
            "    jobs:\n"
            "      - job: pruebas\n"
            "        steps:\n"
            "          - script: echo pruebas\n";

        errores += enviar(puerto, estructura_invalida,
                          "INVALID 1");

        errores += enviar(puerto, etapa_inexistente,
                          "INVALID 1");

        errores += enviar(puerto, ciclo,
                          "INVALID 1");

        errores += enviar(puerto, condicion_invalida,
                          "INVALID 1");

        errores += enviar(puerto, trabajo_inexistente,
                          "INVALID 1");

        errores += enviar(puerto, valido, "OK 1");

        int fd = nu_connect("127.0.0.1", puerto);

        if (fd < 0) {
            errores++;
        } else {
            char respuesta[128];

            if (nu_send_response(fd, "SHUTDOWN\n") != 0 ||
                nu_read_line(fd, respuesta,
                             sizeof respuesta) < 0 ||
                strcmp(respuesta, "BYE") != 0) {
                puts("Error al apagar el servidor");
                errores++;
            }

            close(fd);
        }
    }

    if (errores)
        kill(servidor, SIGTERM);

    int estado = 0;

    if (waitpid(servidor, &estado, 0) < 0 ||
        !WIFEXITED(estado) ||
        WEXITSTATUS(estado) != 0)
        errores++;

    if (errores) {
        printf("Fallaron %d pruebas SUBMIT YAML\n",
               errores);
        return 1;
    }

    puts("Pruebas SUBMIT YAML: OK");
    return 0;
}


