
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double reloj(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec +
           (double)t.tv_nsec / 1e9;
}

static char *leer_archivo(const char *ruta, size_t *n)
{
    FILE *f = fopen(ruta, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }

    long largo = ftell(f);

    if (largo <= 0 || largo > 1048576 ||
        fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }

    char *p = malloc((size_t)largo);

    if (!p) {
        fclose(f);
        return NULL;
    }

    if (fread(p, 1, (size_t)largo, f) !=
        (size_t)largo) {
        free(p);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *n = (size_t)largo;
    return p;
}

static int primera_linea(const char *host,
                         unsigned short puerto,
                         const char *comando,
                         const char *datos,
                         size_t n,
                         char *respuesta,
                         size_t capacidad)
{
    int fd = nu_connect(host, puerto);
    if (fd < 0)
        return -1;

    int rc = nu_write_all(fd, comando,
                          strlen(comando));

    if (rc == 0 && datos)
        rc = nu_write_all(fd, datos, n);

    if (rc == 0)
        rc = nu_read_line(fd, respuesta,
                          capacidad) < 0 ? -1 : 0;

    close(fd);
    return rc;
}

static int comparar(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static double medir(const char *host,
                    unsigned short puerto,
                    const char *yaml,
                    size_t n)
{
    char cabecera[64];
    char respuesta[4096];
    char estado[64];

    snprintf(cabecera, sizeof cabecera,
             "SUBMIT %zu\n", n);

    double inicio = reloj();

    if (primera_linea(host, puerto, cabecera,
                      yaml, n, respuesta,
                      sizeof respuesta) != 0)
        return -1;

    int id = 0;

    if (sscanf(respuesta, "OK %d", &id) != 1 ||
        id <= 0)
        return -1;

    for (int i = 0; i < 15000; i++) {
        snprintf(cabecera, sizeof cabecera,
                 "STATUS %d\n", id);

        if (primera_linea(host, puerto, cabecera,
                          NULL, 0, respuesta,
                          sizeof respuesta) != 0 ||
            sscanf(respuesta,
                   "STATUS %*d %63s",
                   estado) != 1)
            return -1;

        if (!strcmp(estado, "succeeded") ||
            !strcmp(estado, "partiallySucceeded"))
            return reloj() - inicio;

        if (!strcmp(estado, "failed") ||
            !strcmp(estado, "canceled") ||
            !strcmp(estado, "skipped"))
            return -1;

        struct timespec pausa = {
            0, 20000000L
        };
        nanosleep(&pausa, NULL);
    }

    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr,
                "Uso: %s <host> <puerto> "
                "<yaml> <agentes> <csv>\n",
                argv[0]);
        return 2;
    }

    int puerto = atoi(argv[2]);
    int agentes = atoi(argv[4]);

    if (puerto < 1 || puerto > 65535 ||
        agentes < 1)
        return 2;

    size_t n = 0;
    char *yaml = leer_archivo(argv[3], &n);

    if (!yaml)
        return 2;

    double tiempos[5];

    for (int i = 0; i < 5; i++) {
        tiempos[i] = medir(
            argv[1], (unsigned short)puerto,
            yaml, n);

        if (tiempos[i] < 0) {
            fprintf(stderr,
                    "Fallo la repeticion %d\n",
                    i + 1);
            free(yaml);
            return 1;
        }

        printf("Repeticion %d: %.6f s\n",
               i + 1, tiempos[i]);
    }

    free(yaml);

    qsort(tiempos, 5,
          sizeof tiempos[0], comparar);

    double mediana = tiempos[2];

    FILE *f = fopen(argv[5], "a");

    if (!f) {
        perror(argv[5]);
        return 1;
    }

    fprintf(f, "%s,%d,%.6f\n",
            argv[3], agentes, mediana);

    if (fclose(f) != 0)
        return 1;

    printf("Mediana (%d agentes): %.6f s\n",
           agentes, mediana);

    return 0;
}

