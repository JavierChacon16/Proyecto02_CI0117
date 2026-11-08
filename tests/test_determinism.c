
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char *leer_yaml(const char *ruta, size_t *n)
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

static int solicitud(const char *host,
                     unsigned short puerto,
                     const char *cabecera,
                     const char *datos,
                     size_t n,
                     char *respuesta,
                     size_t capacidad)
{
    int fd = nu_connect(host, puerto);
    if (fd < 0) return -1;

    int rc = nu_write_all(fd, cabecera,
                          strlen(cabecera));

    if (rc == 0 && datos)
        rc = nu_write_all(fd, datos, n);

    if (rc == 0)
        rc = nu_read_line(fd, respuesta,
                          capacidad) < 0 ? -1 : 0;

    close(fd);
    return rc;
}

static int estado_final(const char *estado)
{
    return strcmp(estado, "succeeded") == 0 ||
           strcmp(estado, "partiallySucceeded") == 0 ||
           strcmp(estado, "failed") == 0 ||
           strcmp(estado, "canceled") == 0 ||
           strcmp(estado, "skipped") == 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "Uso: %s <host> <puerto> <yaml>\n",
                argv[0]);
        return 2;
    }

    int puerto = atoi(argv[2]);
    if (puerto <= 0 || puerto > 65535)
        return 2;

    size_t n = 0;
    char *yaml = leer_yaml(argv[3], &n);
    if (!yaml) return 2;

    char referencia[64] = "";
    int errores = 0;

    for (int i = 0; i < 20; i++) {
        char cabecera[64];
        char respuesta[4096];

        snprintf(cabecera, sizeof cabecera,
                 "SUBMIT %zu\n", n);

        if (solicitud(argv[1],
                      (unsigned short)puerto,
                      cabecera, yaml, n,
                      respuesta,
                      sizeof respuesta) != 0) {
            errores++;
            break;
        }

        int id = 0;

        if (sscanf(respuesta, "OK %d", &id) != 1 ||
            id <= 0) {
            fprintf(stderr,
                    "SUBMIT: %s\n", respuesta);
            errores++;
            break;
        }

        char estado[64] = "";
        int terminado = 0;

        for (int intento = 0;
             intento < 1500; intento++) {

            snprintf(cabecera, sizeof cabecera,
                     "STATUS %d\n", id);

            if (solicitud(argv[1],
                          (unsigned short)puerto,
                          cabecera, NULL, 0,
                          respuesta,
                          sizeof respuesta) != 0 ||
                sscanf(respuesta,
                       "STATUS %*d %63s",
                       estado) != 1) {
                errores++;
                break;
            }

            if (estado_final(estado)) {
                terminado = 1;
                break;
            }

            struct timespec pausa = {
                0, 20000000L
            };
            nanosleep(&pausa, NULL);
        }

        if (errores || !terminado) {
            errores++;
            break;
        }

        if (!*referencia)
            strcpy(referencia, estado);
        else if (strcmp(referencia, estado) != 0) {
            fprintf(stderr,
                    "Diferencia: %s frente a %s\n",
                    referencia, estado);
            errores++;
            break;
        }
    }

    free(yaml);

    if (errores) return 1;

    printf("OK: 20 ejecuciones "
           "con estado final %s\n", referencia);

    return 0;
}
