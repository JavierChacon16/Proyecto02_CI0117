
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "util.h"

int main(void)
{
    int fd[2];
    char texto[32];
    char bloque[5];
    int errores = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) {
        perror("socketpair");
        return 1;
    }

    if (nu_write_all(fd[0], "HOLA\n", 5) != 0 ||
        nu_read_line(fd[1], texto, sizeof texto) != 4 ||
        strcmp(texto, "HOLA") != 0) {
        printf("Error: envio y lectura de linea\n");
        errores++;
    }

    if (nu_write_all(fd[0], "abcde", 5) != 0 ||
        nu_read_all(fd[1], bloque, sizeof bloque) != 0 ||
        memcmp(bloque, "abcde", 5) != 0) {
        printf("Error: lectura de bloque\n");
        errores++;
    }

    if (nu_send_response(fd[0], "OK\n") != 0 ||
        nu_read_line(fd[1], texto, sizeof texto) != 2 ||
        strcmp(texto, "OK") != 0) {
        printf("Error: envio de respuesta\n");
        errores++;
    }

    if (nu_write_all(fd[0], "12345678\n", 9) != 0 ||
        nu_read_line(fd[1], texto, 4) != NU_TOO_LONG) {
        printf("Error: linea demasiado larga\n");
        errores++;
    }

    /* nu_read_line deja pendientes los bytes restantes de una linea larga. */
    if (nu_read_line(fd[1], texto, sizeof texto) != 4 ||
        strcmp(texto, "5678") != 0) {
        printf("Error: lectura despues de linea larga\n");
        errores++;
    }

    close(fd[0]);
    if (nu_read_line(fd[1], texto, sizeof texto) != NU_CLOSED) {
        printf("Error: deteccion del cierre\n");
        errores++;
    }
    close(fd[1]);

    if (errores != 0) {
        printf("Fallaron %d pruebas de red\n", errores);
        return 1;
    }

    printf("Pruebas de utilidades: OK\n");
    return 0;
}

