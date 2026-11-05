
#define _POSIX_C_SOURCE 200809L

#include "tasks.h"

#include <errno.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *texto;
} palabra_t;

typedef struct {
    palabra_t *datos;
    size_t n;
    size_t cap;
    int error;
} lista_t;

typedef struct {
    const char *texto;
    size_t veces;
} frecuencia_t;

static int cargar(const char *ruta, unsigned char **datos,
                  size_t *tam)
{
    FILE *f = fopen(ruta, "rb");

    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long n = ftell(f);

    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    unsigned char *p = malloc((size_t)n + 1);

    if (!p) {
        fclose(f);
        return -1;
    }

    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return -1;
    }

    fclose(f);

    p[n] = 0;
    *datos = p;
    *tam = (size_t)n;

    return 0;
}

/* Sleep@1 */

int tarea_sleep(double segundos)
{
    if (!(segundos >= 0.0) || segundos > 86400.0)
        return -1;

    time_t s = (time_t)segundos;
    long ns = (long)((segundos - (double)s) * 1000000000.0);

    struct timespec t = {s, ns};

    while (nanosleep(&t, &t) == -1) {
        if (errno != EINTR)
            return -1;
    }

    return 0;
}

/* Cpu@1 */

static uint64_t mezclar(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);

    return x ^ (x >> 31);
}

int tarea_cpu(uint64_t iteraciones, uint64_t semilla,
              int hilos, uint64_t *resultado)
{
    if (!resultado || hilos < 1)
        return -1;

    uint64_t acumulado = 0;

    #pragma omp parallel for num_threads(hilos) reduction(^:acumulado) schedule(static)
    for (uint64_t i = 0; i < iteraciones; i++) {
        acumulado ^= mezclar(semilla ^ i);
    }

    *resultado = acumulado;

    return 0;
}

/* Histogram@1 */

int tarea_histogram(const char *archivo, unsigned bins,
                    int hilos, const char *salida)
{
    if (!archivo || !salida || bins < 1 ||
        bins > 256 || hilos < 1)
        return -1;

    unsigned char *datos;
    size_t n;

    if (cargar(archivo, &datos, &n) != 0)
        return -1;

    uint64_t *parciales = calloc(
        (size_t)hilos * bins, sizeof *parciales);

    if (!parciales) {
        free(datos);
        return -1;
    }

    #pragma omp parallel num_threads(hilos)
    {
        int id = omp_get_thread_num();

        uint64_t *local = parciales + (size_t)id * bins;

        #pragma omp for schedule(static)
        for (size_t i = 0; i < n; i++) {
            unsigned b = ((unsigned)datos[i] * bins) / 256;
            local[b]++;
        }
    }

    FILE *f = fopen(salida, "w");

    if (!f) {
        free(parciales);
        free(datos);
        return -1;
    }

    int error = fprintf(f, "bin,desde,hasta,cantidad\n") < 0;

    for (unsigned b = 0; b < bins; b++) {
        uint64_t total = 0;

        for (int t = 0; t < hilos; t++) {
            total += parciales[(size_t)t * bins + b];
        }

        unsigned desde =
            (b * 256u + bins - 1) / bins;

        unsigned hasta =
            ((b + 1u) * 256u + bins - 1) / bins - 1u;

        if (fprintf(f, "%u,%u,%u,%llu\n",
                    b, desde, hasta,
                    (unsigned long long)total) < 0) {
            error = 1;
        }
    }

    if (fclose(f) != 0)
        error = 1;

    free(parciales);
    free(datos);

    return error ? -1 : 0;
}

/* WordCount@1 */

static int letra(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static void agregar(lista_t *lista,
                    const unsigned char *datos,
                    size_t inicio, size_t fin)
{
    if (lista->error)
        return;

    if (lista->n == lista->cap) {
        size_t nueva = lista->cap ? lista->cap * 2 : 128;

        if (nueva < lista->cap ||
            nueva > SIZE_MAX / sizeof(palabra_t)) {
            lista->error = 1;
            return;
        }

        palabra_t *p = realloc(
            lista->datos, nueva * sizeof *p);

        if (!p) {
            lista->error = 1;
            return;
        }

        lista->datos = p;
        lista->cap = nueva;
    }

    char *p = malloc(fin - inicio + 1);

    if (!p) {
        lista->error = 1;
        return;
    }

    for (size_t i = inicio; i < fin; i++) {
        unsigned char c = datos[i];

        p[i - inicio] = (char)(
            (c >= 'A' && c <= 'Z') ? c + 32 : c);
    }

    p[fin - inicio] = '\0';

    lista->datos[lista->n++].texto = p;
}

static int comparar_palabras(const void *a, const void *b)
{
    const palabra_t *x = a;
    const palabra_t *y = b;

    return strcmp(x->texto, y->texto);
}

static int comparar_frecuencia(const void *a, const void *b)
{
    const frecuencia_t *x = a;
    const frecuencia_t *y = b;

    if (x->veces != y->veces)
        return x->veces < y->veces ? 1 : -1;

    return strcmp(x->texto, y->texto);
}

int tarea_wordcount(const char *archivo, int hilos,
                    const char *salida, size_t top)
{
    if (!archivo || !salida || hilos < 1)
        return -1;

    unsigned char *datos;
    size_t n;

    if (cargar(archivo, &datos, &n) != 0)
        return -1;

    lista_t *listas = calloc(
        (size_t)hilos, sizeof *listas);

    if (!listas) {
        free(datos);
        return -1;
    }

    #pragma omp parallel num_threads(hilos)
    {
        int id = omp_get_thread_num();

        size_t inicio =
            (n / (size_t)hilos) * (size_t)id;

        size_t fin = id == hilos - 1
            ? n
            : (n / (size_t)hilos) * (size_t)(id + 1);

        size_t i = inicio;

        while (i < fin) {
            if (!letra(datos[i]) ||
                (i > 0 && letra(datos[i - 1]))) {
                i++;
                continue;
            }

            size_t j = i + 1;

            while (j < n && letra(datos[j]))
                j++;

            agregar(&listas[id], datos, i, j);
            i = j;
        }
    }

    size_t total = 0;
    int error = 0;

    for (int t = 0; t < hilos; t++) {
        if (listas[t].error ||
            SIZE_MAX - total < listas[t].n) {
            error = 1;
        } else {
            total += listas[t].n;
        }
    }

    palabra_t *todas = NULL;
    frecuencia_t *frecuencias = NULL;

    if (!error && total) {
        todas = malloc(total * sizeof *todas);
        frecuencias = malloc(total * sizeof *frecuencias);

        if (!todas || !frecuencias)
            error = 1;
    }

    if (!error) {
        size_t pos = 0;

        for (int t = 0; t < hilos; t++) {
            if (listas[t].n > 0) {
                memcpy(todas + pos, listas[t].datos,
                       listas[t].n * sizeof *todas);
            }

            pos += listas[t].n;
        }

        qsort(todas, total, sizeof *todas,
              comparar_palabras);

        size_t distintas = 0;

        for (size_t i = 0; i < total;) {
            size_t j = i + 1;

            while (j < total &&
                   strcmp(todas[i].texto,
                          todas[j].texto) == 0) {
                j++;
            }

            frecuencias[distintas++] =
                (frecuencia_t){todas[i].texto, j - i};

            i = j;
        }

        qsort(frecuencias, distintas,
              sizeof *frecuencias, comparar_frecuencia);

        FILE *f = fopen(salida, "w");

        if (!f) {
            error = 1;
        } else {
            if (fprintf(f,
                        "total,%zu\ndistintas,%zu\n"
                        "palabra,cantidad\n",
                        total, distintas) < 0) {
                error = 1;
            }

            if (top > distintas)
                top = distintas;

            for (size_t i = 0; i < top; i++) {
                if (fprintf(f, "%s,%zu\n",
                            frecuencias[i].texto,
                            frecuencias[i].veces) < 0) {
                    error = 1;
                }
            }

            if (fclose(f) != 0)
                error = 1;
        }
    }

    for (int t = 0; t < hilos; t++) {
        for (size_t i = 0; i < listas[t].n; i++) {
            free(listas[t].datos[i].texto);
        }

        free(listas[t].datos);
    }

    free(listas);
    free(todas);
    free(frecuencias);
    free(datos);

    return error ? -1 : 0;
}
