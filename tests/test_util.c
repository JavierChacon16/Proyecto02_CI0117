
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "util.h"

int main(void)
{
    int fd[2];
    char linea[32];
    char bloque[5];
    int errores = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) {
        perror("socketpair");
        return 1;
    }

    if (nu_write_all(fd[0], "HOLA\n", 5) != 0 ||
        nu_read_line(fd[1], linea, sizeof linea) != 4 ||
        strcmp(linea, "HOLA") != 0) {
        puts("Error en lectura de linea");
        errores++;
    }

    if (nu_write_all(fd[0], "abcde", 5) != 0 ||
        nu_read_all(fd[1], bloque, sizeof bloque) != 0 ||
        memcmp(bloque, "abcde", 5) != 0) {
        puts("Error en lectura de bloque");
        errores++;
    }

    if (nu_write_all(fd[0], "12345678\nOK\n", 12) != 0 ||
        nu_read_line(fd[1], linea, 4) != NU_TOO_LONG ||
        nu_read_line(fd[1], linea, sizeof linea) != 2 ||
        strcmp(linea, "OK") != 0) {
        puts("Error en recuperacion tras linea larga");
        errores++;
    }

    close(fd[0]);

    if (nu_read_line(fd[1], linea, sizeof linea) != NU_CLOSED) {
        puts("Error al detectar cierre");
        errores++;
    }

    close(fd[1]);

    if (errores != 0) {
        printf("Fallaron %d pruebas\n", errores);
        return 1;
    }

    puts("Pruebas de utilidades: OK");
    return 0;
}


