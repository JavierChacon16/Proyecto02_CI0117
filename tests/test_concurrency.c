
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *host;
    unsigned short puerto;
    const char *yaml;
    size_t longitud;
    pthread_barrier_t *barrera;
    int id;
} intento_t;

static char *leer_archivo(const char *ruta, size_t *longitud)
{
    FILE *f = fopen(ruta, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || n > 1048576 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *datos = malloc((size_t)n);
    if (!datos) {
        fclose(f);
        return NULL;
    }
    if (fread(datos, 1, (size_t)n, f) != (size_t)n) {
        free(datos);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *longitud = (size_t)n;
    return datos;
}

static void *enviar(void *arg)
{
    intento_t *t = arg;
    char cabecera[64], respuesta[256];

    snprintf(cabecera, sizeof cabecera,
             "SUBMIT %zu\n", t->longitud);

    t->id = -1;

    pthread_barrier_wait(t->barrera);

    int fd = nu_connect(t->host, t->puerto);
    if (fd < 0) return NULL;

    if (nu_write_all(fd, cabecera,
                     strlen(cabecera)) == 0 &&
        nu_write_all(fd, t->yaml,
                     t->longitud) == 0 &&
        nu_read_line(fd, respuesta,
                     sizeof respuesta) >= 0) {
        int id = 0;
        if (sscanf(respuesta, "OK %d", &id) == 1 &&
            id > 0)
            t->id = id;
    }

    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr,
                "Uso: %s <host> <puerto> <yaml> [race]\n",
                argv[0]);
        return 2;
    }

    int puerto = atoi(argv[2]);
    if (puerto < 1 || puerto > 65535) return 2;

    size_t longitud = 0;
    char *yaml = leer_archivo(argv[3], &longitud);
    if (!yaml) {
        perror("YAML");
        return 2;
    }

    enum { N = 100 };
    pthread_t hilos[N];
    intento_t intentos[N];
    pthread_barrier_t barrera;

    if (pthread_barrier_init(&barrera, NULL, N) != 0) {
        free(yaml);
        return 2;
    }

    for (int i = 0; i < N; i++) {
        intentos[i] = (intento_t){
            argv[1], (unsigned short)puerto,
            yaml, longitud, &barrera, -1
        };

        if (pthread_create(&hilos[i], NULL,
                           enviar, &intentos[i]) != 0) {
            fprintf(stderr,
                    "No se pudo crear el hilo %d\n", i);
            _exit(2);
        }
    }

    for (int i = 0; i < N; i++)
        pthread_join(hilos[i], NULL);

    pthread_barrier_destroy(&barrera);

    int errores = 0;
    int duplicados = 0;

    for (int i = 0; i < N; i++) {
        if (intentos[i].id <= 0)
            errores++;

        for (int j = 0; j < i; j++) {
            if (intentos[i].id > 0 &&
                intentos[i].id == intentos[j].id) {
                duplicados++;
                break;
            }
        }
    }

    printf("SUBMIT aceptados: %d/%d; "
           "IDs duplicados: %d\n",
           N - errores, N, duplicados);

    free(yaml);

    if (errores) return 1;

    if (argc == 5 && strcmp(argv[4], "race") == 0) {
        /* La carrera puede no manifestarse en cada intento. */
        return duplicados ? 0 : 1;
    }

    return duplicados == 0 ? 0 : 1;
}
