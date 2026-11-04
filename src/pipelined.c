#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int valores[2];
    const long maximos[2] = {65535, 1024};
    int valido = argc == 4;
    for (int i = 0; valido && i < 2; ++i) {
        char *fin;
        errno = 0;
        long n = strtol(argv[i + 1], &fin, 10);
        if (!*argv[i + 1] || errno || *fin || n < 1 || n > maximos[i])
            valido = 0;
        else
            valores[i] = (int)n;
    }
    if (!valido) {
        fprintf(stderr, "Uso: %s <puerto 1..65535> <agentes 1..1024> <directorio>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    return iniciar_servidor(valores[0], valores[1], argv[3]);
}

