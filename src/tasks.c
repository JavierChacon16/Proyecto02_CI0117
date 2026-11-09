
#define _POSIX_C_SOURCE 200809L

#include "tasks.h"

#include <errno.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* La memoria del buffer sigue siendo del llamador, incluso si se interrumpe. */
static int estado_interrupcion(const tarea_contexto_t *ctx)
{
    if (!ctx)
        return TAREA_OK;
    if (ctx->cancelar && atomic_load(ctx->cancelar))
        return TAREA_CANCELADO;
    if (ctx->deadline.tv_sec || ctx->deadline.tv_nsec) {
        struct timespec ahora;
        if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0)
            return TAREA_FALLO;
        if (ahora.tv_sec > ctx->deadline.tv_sec ||
            (ahora.tv_sec == ctx->deadline.tv_sec &&
             ahora.tv_nsec >= ctx->deadline.tv_nsec))
            return TAREA_TIMEOUT;
    }
    return TAREA_OK;
}

static int cargar(const char *ruta, unsigned char **datos,
                  size_t *tam, const tarea_contexto_t *ctx)
{
    int estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK)
        return estado;
    FILE *f = fopen(ruta, "rb");

    if (!f)
        return TAREA_FALLO;

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

    /* No leer 200 MiB de golpe: comprobar el plazo cada 64 KiB. */
    size_t recibidos = 0;
    while (recibidos < (size_t)n) {
        estado = estado_interrupcion(ctx);
        if (estado != TAREA_OK) {
            free(p);
            fclose(f);
            return estado;
        }
        size_t bloque = (size_t)n - recibidos;
        if (bloque > 65536)
            bloque = 65536;
        size_t leidos = fread(p + recibidos, 1, bloque, f);
        if (leidos != bloque) {
            free(p);
            fclose(f);
            return TAREA_FALLO;
        }
        recibidos += leidos;
    }
    if (fclose(f) != 0) {
        free(p);
        return TAREA_FALLO;
    }
    estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK) {
        free(p);
        return estado;
    }

    p[n] = 0;
    *datos = p;
    *tam = (size_t)n;

    return 0;
}

/* Sleep@1 */

int tarea_sleep_controlada(double segundos, const tarea_contexto_t *ctx)
{
    if (!(segundos >= 0.0) || segundos > 86400.0)
        return TAREA_FALLO;
    int estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK)
        return estado;

    /* Siempre dormir en rebanadas <= 50 ms y comprobar antes/despues. */
    struct timespec total = {(time_t)segundos,
        (long)((segundos - (double)(time_t)segundos) * 1000000000.0)};
    while (total.tv_sec || total.tv_nsec) {
        estado = estado_interrupcion(ctx);
        if (estado != TAREA_OK)
            return estado;
        struct timespec tramo = total;
        if (tramo.tv_sec || tramo.tv_nsec > 50000000L) {
            tramo.tv_sec = 0;
            tramo.tv_nsec = 50000000L;
        }
        /* El nanosleep puede interrumpirse con EINTR. */
        struct timespec resto = tramo;
        while (nanosleep(&resto, &resto) == -1) {
            if (errno != EINTR)
                return TAREA_FALLO;
            estado = estado_interrupcion(ctx);
            if (estado != TAREA_OK)
                return estado;
        }
        if (total.tv_nsec < tramo.tv_nsec) {
            total.tv_sec--;
            total.tv_nsec += 1000000000L;
        }
        total.tv_nsec -= tramo.tv_nsec;
    }
    return estado_interrupcion(ctx);
}

int tarea_sleep(double segundos)
{
    return tarea_sleep_controlada(segundos, NULL);
}

/* Cpu@1 */

static uint64_t mezclar(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);

    return x ^ (x >> 31);
}

int tarea_cpu_controlada(uint64_t iteraciones, uint64_t semilla,
                         int hilos, uint64_t *resultado,
                         const tarea_contexto_t *ctx)
{
    if (!resultado || hilos < 1)
        return TAREA_FALLO;
    int estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK)
        return estado;
    uint64_t acumulado = 0;
    atomic_int interrupcion;
    atomic_init(&interrupcion, TAREA_OK);
    atomic_uint_fast64_t siguiente;
    atomic_init(&siguiente, 0);

    /* Despacho de bloques: al cancelar, los agentes salen sin recorrer
     * el resto de los millones de iteraciones. XOR es determinista. */
    #pragma omp parallel num_threads(hilos) reduction(^:acumulado)
    {
        uint64_t parcial = 0;
        while (atomic_load(&interrupcion) == TAREA_OK) {
            int r = estado_interrupcion(ctx);
            if (r != TAREA_OK) {
                atomic_store(&interrupcion, r);
                break;
            }
            uint64_t inicio = atomic_fetch_add(&siguiente, UINT64_C(4096));
            if (inicio >= iteraciones)
                break;
            uint64_t cantidad = iteraciones - inicio;
            if (cantidad > 4096)
                cantidad = 4096;
            for (uint64_t i = 0; i < cantidad; i++)
                parcial ^= mezclar(semilla ^ (inicio + i));
        }
        acumulado ^= parcial;
    }
    estado = atomic_load(&interrupcion);
    if (estado == TAREA_OK)
        estado = estado_interrupcion(ctx);
    if (estado == TAREA_OK)
        *resultado = acumulado;
    return estado;
}

int tarea_cpu(uint64_t iteraciones, uint64_t semilla,
              int hilos, uint64_t *resultado)
{
    return tarea_cpu_controlada(iteraciones, semilla, hilos, resultado, NULL);
}

/* Histogram@1 */

int tarea_histogram_controlada(const char *archivo, unsigned bins,
                               int hilos, const char *salida,
                               const tarea_contexto_t *ctx)
{
    if (!archivo || !salida || bins < 1 ||
        bins > 256 || hilos < 1)
        return -1;

    unsigned char *datos;
    size_t n;

    int estado = cargar(archivo, &datos, &n, ctx);
    if (estado != TAREA_OK)
        return estado;

    uint64_t *parciales = calloc(
        (size_t)hilos * bins, sizeof *parciales);

    if (!parciales) {
        free(datos);
        return -1;
    }

    atomic_int interrupcion;
    atomic_init(&interrupcion, TAREA_OK);
    atomic_size_t siguiente;
    atomic_init(&siguiente, 0);
    #pragma omp parallel num_threads(hilos)
    {
        int id = omp_get_thread_num();
        uint64_t *local = parciales + (size_t)id * bins;
        while (atomic_load(&interrupcion) == TAREA_OK) {
            int r = estado_interrupcion(ctx);
            if (r != TAREA_OK) {
                atomic_store(&interrupcion, r);
                break;
            }
            size_t inicio = atomic_fetch_add(&siguiente, (size_t)8192);
            if (inicio >= n)
                break;
            size_t cantidad = n - inicio;
            if (cantidad > 8192)
                cantidad = 8192;
            for (size_t i = 0; i < cantidad; i++) {
                unsigned b = ((unsigned)datos[inicio + i] * bins) / 256;
                local[b]++;
            }
        }
    }
    estado = atomic_load(&interrupcion);
    if (estado == TAREA_OK)
        estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK) {
        free(parciales);
        free(datos);
        return estado;
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
    estado = estado_interrupcion(ctx);
    if (estado != TAREA_OK || error)
        (void)unlink(salida);
    free(parciales);
    free(datos);
    return error ? TAREA_FALLO : estado;
}

int tarea_histogram(const char *archivo, unsigned bins,
                    int hilos, const char *salida)
{
    return tarea_histogram_controlada(archivo, bins, hilos, salida, NULL);
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

/* Con plazo/cancelacion, ordenar por bloques de 4096 y fusionar.
 * Evita que un unico qsort enorme ignore la cancelacion durante segundos.
 * Sin contexto preserva el qsort historico (rapido en pruebas antiguas). */
static int ordenar_controlado(void *base, size_t n, size_t ancho,
                              int (*comparar)(const void *, const void *),
                              const tarea_contexto_t *ctx)
{
    if (n < 2)
        return estado_interrupcion(ctx);
    if (!ctx) {
        qsort(base, n, ancho, comparar);
        return TAREA_OK;
    }
    if (ancho == 0 || n > SIZE_MAX / ancho)
        return TAREA_FALLO;
    unsigned char *datos = base;
    const size_t bloque = 4096;
    for (size_t i = 0; i < n;) {
        int r = estado_interrupcion(ctx);
        if (r != TAREA_OK)
            return r;
        size_t cuantos = n - i;
        if (cuantos > bloque)
            cuantos = bloque;
        qsort(datos + i * ancho, cuantos, ancho, comparar);
        i += cuantos;
    }
    if (n <= bloque)
        return estado_interrupcion(ctx);
    unsigned char *aux = malloc(n * ancho);
    if (!aux)
        return TAREA_FALLO;
    for (size_t tramo = bloque; tramo < n;) {
        for (size_t ini = 0; ini < n;) {
            int r = estado_interrupcion(ctx);
            if (r != TAREA_OK) {free(aux); return r;}
            size_t izq_fin = n - ini < tramo ? n : ini + tramo;
            size_t der_fin = n - izq_fin < tramo ? n : izq_fin + tramo;
            size_t a = ini, b = izq_fin, dst = ini;
            while (a < izq_fin && b < der_fin) {
                if (((dst - ini) & (size_t)4095) == 0) {
                    r = estado_interrupcion(ctx);
                    if (r != TAREA_OK) {free(aux); return r;}
                }
                size_t elegido = comparar(datos + a * ancho,
                                         datos + b * ancho) <= 0 ? a++ : b++;
                memcpy(aux + dst * ancho, datos + elegido * ancho, ancho);
                dst++;
            }
            if (a < izq_fin)
                memcpy(aux + dst * ancho, datos + a * ancho,
                       (izq_fin - a) * ancho);
            else if (b < der_fin)
                memcpy(aux + dst * ancho, datos + b * ancho,
                       (der_fin - b) * ancho);
            ini = der_fin;
        }
        memcpy(datos, aux, n * ancho);
        if (tramo > n / 2)
            break;
        tramo *= 2;
    }
    free(aux);
    return estado_interrupcion(ctx);
}

int tarea_wordcount_controlada(const char *archivo, int hilos,
                               const char *salida, size_t top,
                               const tarea_contexto_t *ctx)
{
    if (!archivo || !salida || hilos < 1)
        return -1;

    unsigned char *datos;
    size_t n;

    int estado = cargar(archivo, &datos, &n, ctx);
    if (estado != TAREA_OK)
        return estado;

    lista_t *listas = calloc(
        (size_t)hilos, sizeof *listas);

    if (!listas) {
        free(datos);
        return -1;
    }

    atomic_int interrupcion;
    atomic_init(&interrupcion, TAREA_OK);
    #pragma omp parallel num_threads(hilos)
    {
        int id = omp_get_thread_num();
        /* La cantidad real puede ser menor que la solicitada. */
        int reales = omp_get_num_threads();
        size_t inicio = (n / (size_t)reales) * (size_t)id;
        size_t fin = id == reales - 1 ? n :
                     (n / (size_t)reales) * (size_t)(id + 1);
        size_t i = inicio;
        while (i < fin && atomic_load(&interrupcion) == TAREA_OK) {
            if (((i - inicio) & (size_t)4095) == 0) {
                int r = estado_interrupcion(ctx);
                if (r != TAREA_OK) {
                    atomic_store(&interrupcion, r);
                    break;
                }
            }
            if (!letra(datos[i]) ||
                (i > 0 && letra(datos[i - 1]))) {
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < n && letra(datos[j])) {
                if ((j & (size_t)4095) == 0) {
                    int r = estado_interrupcion(ctx);
                    if (r != TAREA_OK) {
                        atomic_store(&interrupcion, r);
                        break;
                    }
                }
                j++;
            }
            if (atomic_load(&interrupcion) != TAREA_OK)
                break;
            agregar(&listas[id], datos, i, j);
            i = j;
        }
    }
    estado = atomic_load(&interrupcion);
    if (estado == TAREA_OK)
        estado = estado_interrupcion(ctx);
    size_t total = 0;
    int error = estado != TAREA_OK;

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

    int archivo_creado = 0;
    if (!error) {
        size_t pos = 0;

        for (int t = 0; t < hilos; t++) {
            if (listas[t].n > 0) {
                memcpy(todas + pos, listas[t].datos,
                       listas[t].n * sizeof *todas);
            }

            pos += listas[t].n;
        }

        estado = ordenar_controlado(todas, total, sizeof *todas,
                                    comparar_palabras, ctx);
        if (estado != TAREA_OK)
            error = 1;
        size_t distintas = 0;

        for (size_t i = 0; i < total && !error;) {
            if ((i & (size_t)4095) == 0) {
                estado = estado_interrupcion(ctx);
                if (estado != TAREA_OK) { error = 1; break; }
            }
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

        if (!error) {
            estado = ordenar_controlado(frecuencias, distintas,
                                         sizeof *frecuencias,
                                         comparar_frecuencia, ctx);
            if (estado != TAREA_OK)
                error = 1;
        }
        FILE *f = error ? NULL : fopen(salida, "w");

        if (!f) {
            error = 1;
        } else {
            archivo_creado = 1;
            if (fprintf(f,
                        "total,%zu\ndistintas,%zu\n"
                        "palabra,cantidad\n",
                        total, distintas) < 0) {
                error = 1;
            }

            if (top > distintas)
                top = distintas;

            for (size_t i = 0; i < top; i++) {
                if ((i & (size_t)4095) == 0) {
                    estado = estado_interrupcion(ctx);
                    if (estado != TAREA_OK) {
                        error = 1;
                        break;
                    }
                }
                if (fprintf(f, "%s,%zu\n",
                            frecuencias[i].texto,
                            frecuencias[i].veces) < 0) {
                    error = 1;
                }
            }

            if (fclose(f) != 0)
                error = 1;
            if (error)
                (void)unlink(salida);
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
    if (!error)
        estado = estado_interrupcion(ctx);
    if ((error || estado != TAREA_OK) && archivo_creado)
        (void)unlink(salida);
    return error ? (estado != TAREA_OK ? estado : TAREA_FALLO) : estado;
}

int tarea_wordcount(const char *archivo, int hilos,
                    const char *salida, size_t top)
{
    return tarea_wordcount_controlada(archivo, hilos, salida, top, NULL);
}
