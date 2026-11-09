#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "util.h"

static int enviar(unsigned short puerto, const char *pedido,
                  const char *primera, const char *segunda)
{
    int fd = nu_connect("127.0.0.1", puerto);

    if (fd < 0)
        return -1;

    char linea[4096];
    int bien = nu_send_response(fd, pedido) == 0;

    if (bien) {
        ssize_t n = nu_read_line(fd, linea, sizeof linea);
        bien = n >= 0 &&
               strncmp(linea, primera, strlen(primera)) == 0;
    }

    if (bien && segunda != NULL) {
        ssize_t n = nu_read_line(fd, linea, sizeof linea);
        bien = n >= 0 && strcmp(linea, segunda) == 0;
    }

    close(fd);
    return bien ? 0 : -1;
}

int main(void)
{
    unsigned short puerto =
        (unsigned short)(30000 + getpid() % 20000);

    char puerto_texto[16];
    snprintf(puerto_texto, sizeof puerto_texto,
             "%u", (unsigned)puerto);

    pid_t servidor = fork();

    if (servidor < 0) {
        perror("fork");
        return 1;
    }

    if (servidor == 0) {
        execl("./pipelined", "pipelined",
              puerto_texto, "2", ".", (char *)NULL);
        _exit(127);
    }

    int listo = 0;

    for (int i = 0; i < 30; i++) {
        int fd = nu_connect("127.0.0.1", puerto);

        if (fd >= 0) {
            close(fd);
            listo = 1;
            break;
        }

        struct timespec pausa = {0, 100000000};
        nanosleep(&pausa, NULL);
    }

    int errores = 0;

    if (!listo ||
        enviar(puerto, "LIST\n", "LIST ", "END") != 0) {
        puts("Error en LIST");
        errores++;
    }

    if (listo &&
        enviar(puerto, "SHUTDOWN\n", "BYE", NULL) != 0) {
        puts("Error en SHUTDOWN");
        errores++;
    }

    if (errores != 0)
        kill(servidor, SIGTERM);

    int estado;

    if (waitpid(servidor, &estado, 0) < 0 ||
        !WIFEXITED(estado) ||
        WEXITSTATUS(estado) != 0) {
        errores++;
    }

    if (errores != 0) {
        printf("Fallaron %d pruebas de integracion\n", errores);
        return 1;
    }

    puts("Pruebas de integracion TCP: OK");
    return 0;
}

