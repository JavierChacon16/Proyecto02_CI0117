
#define _POSIX_C_SOURCE 200809L

#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int igual(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb");
    FILE *y = fopen(b, "rb");

    if (!x || !y) {
        if (x)
            fclose(x);

        if (y)
            fclose(y);

        return 0;
    }

    int cx;
    int cy;
    int bien = 1;

    do {
        cx = fgetc(x);
        cy = fgetc(y);

        if (cx != cy) {
            bien = 0;
            break;
        }
    } while (cx != EOF);

    fclose(x);
    fclose(y);

    return bien;
}

int main(void)
{
    char entrada[] = "/tmp/p2_datos_XXXXXX";
    char hist1[] = "/tmp/p2_hist1_XXXXXX";
    char hist4[] = "/tmp/p2_hist4_XXXXXX";
    char wc1[] = "/tmp/p2_wc1_XXXXXX";
    char wc4[] = "/tmp/p2_wc4_XXXXXX";

    char *rutas[] = {
        entrada, hist1, hist4, wc1, wc4
    };

    for (int i = 0; i < 5; i++) {
        int fd = mkstemp(rutas[i]);

        if (fd < 0) {
            perror("mkstemp");
            return 1;
        }

        close(fd);
    }

    FILE *f = fopen(entrada, "wb");

    if (!f)
        return 1;

    const char *contenido =
        "Hola mundo hola\nMUNDO prueba\n";

    fwrite(contenido, 1, strlen(contenido), f);
    fclose(f);

    int errores = 0;

    if (tarea_histogram(entrada, 16, 1, hist1) != 0 ||
        tarea_histogram(entrada, 16, 4, hist4) != 0 ||
        !igual(hist1, hist4)) {
        puts("Error Histogram: resultados distintos");
        errores++;
    }

    if (tarea_wordcount(entrada, 1, wc1, 10) != 0 ||
        tarea_wordcount(entrada, 4, wc4, 10) != 0 ||
        !igual(wc1, wc4)) {
        puts("Error WordCount: resultados distintos");
        errores++;
    }

    for (int i = 0; i < 5; i++) {
        unlink(rutas[i]);
    }

    if (errores)
        return 1;

    puts("Pruebas Histogram y WordCount: OK");
    return 0;
}

