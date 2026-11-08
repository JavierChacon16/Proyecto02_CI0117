

#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double ahora(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pausa(void) {
    struct timespec ts = {0, 20000000L};
    nanosleep(&ts, NULL);
}

static char *cargar(const char *ruta, size_t *tam) {
    FILE *f = fopen(ruta, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > 1048576 || fseek(f, 0, SEEK_SET)) {
        fclose(f); return NULL;
    }
    char *b = malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    fclose(f); *tam = (size_t)n; return b;
}

static int peticion(int puerto, const char *comando, const void *datos,
                    size_t tam, char *respuesta, size_t capacidad) {
    int fd = nu_connect("127.0.0.1", (unsigned short)puerto);
    if (fd < 0) return -1;
    int rc = nu_write_all(fd, comando, strlen(comando));
    if (rc == 0 && datos) rc = nu_write_all(fd, datos, tam);
    if (rc == 0 && nu_read_line(fd, respuesta, capacidad) < 0) rc = -1;
    close(fd); return rc;
}

static pid_t iniciar(int puerto, int agentes, int omp, const char *directorio) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    char p[32], a[32], o[32];
    snprintf(p, sizeof p, "%d", puerto);
    snprintf(a, sizeof a, "%d", agentes);
    snprintf(o, sizeof o, "%d", omp);
    setenv("OMP_NUM_THREADS", o, 1);
    execl("./pipelined", "pipelined", p, a, directorio, (char *)NULL);
    _exit(127);
}

static int disponible(int puerto, pid_t pid) {
    for (int i = 0; i < 150; ++i) {
        if (kill(pid, 0) != 0) return -1;
        int fd = nu_connect("127.0.0.1", (unsigned short)puerto);
        if (fd >= 0) { close(fd); return 0; }
        pausa();
    }
    return -1;
}

static void cerrar_servidor(pid_t pid, int puerto) {
    char r[256];
    (void)peticion(puerto, "SHUTDOWN\n", NULL, 0, r, sizeof r);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static double una_ejecucion(int puerto, const char *yaml, size_t tam) {
    char comando[64], respuesta[1024], estado[64];
    snprintf(comando, sizeof comando, "SUBMIT %zu\n", tam);
    double inicio = ahora();
    if (peticion(puerto, comando, yaml, tam, respuesta, sizeof respuesta)) return -1;
    int id = 0;
    if (sscanf(respuesta, "OK %d", &id) != 1 || id <= 0) {
        fprintf(stderr, "SUBMIT: %s\n", respuesta); return -1;
    }
    for (int i = 0; i < 9000; ++i) {
        snprintf(comando, sizeof comando, "STATUS %d\n", id);
        if (peticion(puerto, comando, NULL, 0, respuesta, sizeof respuesta)) return -1;
        if (sscanf(respuesta, "STATUS %*d %63s", estado) != 1) return -1;
        if (strcmp(estado, "succeeded") == 0) return ahora() - inicio;
        if (strcmp(estado, "failed") == 0 || strcmp(estado, "canceled") == 0 ||
            strcmp(estado, "skipped") == 0 || strcmp(estado, "partiallySucceeded") == 0)
            return -1;
        pausa();
    }
    return -1;
}

static int ordenar(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Uso: %s <yaml> <OMP_NUM_THREADS> <salida.csv> <salida.tex>\n", argv[0]);
        return 2;
    }
    char *fin = NULL;
    long omp = strtol(argv[2], &fin, 10);
    if (!*argv[2] || *fin || omp < 1 || omp > 1024) return 2;
    size_t tam = 0;
    char *yaml = cargar(argv[1], &tam);
    if (!yaml) { perror(argv[1]); return 2; }
    FILE *csv = fopen(argv[3], "w");
    FILE *tex = fopen(argv[4], "w");
    if (!csv || !tex) {
        perror("resultados");
        free(yaml);
        if (csv) fclose(csv);
        if (tex) fclose(tex);
        return 1;
    }
    fprintf(csv, "escenario,agentes,omp,repeticion,segundos,mediana,speedup,eficiencia,fraccion_serial\n");
    fprintf(tex, "%% Medidas reales: %s; OMP=%ld\n", argv[1], omp);
    fprintf(tex, "\\begin{tabular}{rrrrrr}\\hline\nAgentes & OMP & $T(p)$ & $S(p)$ & $E(p)$ & $f$ \\\\\n\\hline\n");
    const int configuraciones[] = {1, 2, 4, 8};
    double base = 0;
    int fallo = 0;
    for (size_t k = 0; k < 4 && !fallo; ++k) {
        int agentes = configuraciones[k];
        char directorio[] = "/tmp/p2benchXXXXXX";
        if (!mkdtemp(directorio)) { fallo = 1; break; }
        int puerto = 30000 + (getpid() + (int)k * 127) % 20000;
        pid_t pid = iniciar(puerto, agentes, (int)omp, directorio);
        if (pid < 0 || disponible(puerto, pid) != 0) {
            fallo = 1;
            if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
            rmdir(directorio); break;
        }
        double muestras[5];
        for (int j = 0; j < 5; ++j) {
            muestras[j] = una_ejecucion(puerto, yaml, tam);
            if (muestras[j] < 0) { fallo = 1; break; }
        }
        cerrar_servidor(pid, puerto);
        rmdir(directorio);
        if (fallo) break;
        double ordenados[5];
        memcpy(ordenados, muestras, sizeof ordenados);
        qsort(ordenados, 5, sizeof(double), ordenar);
        double mediana = ordenados[2];
        if (k == 0) base = mediana;
        double s = base / mediana, e = s / agentes;
        double f = agentes > 1 ? (agentes / s - 1) / (agentes - 1) : 0;
        for (int j = 0; j < 5; ++j)
            fprintf(csv, "%s,%d,%ld,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                    argv[1], agentes, omp, j + 1, muestras[j], mediana, s, e, f);
        fprintf(tex, "%d & %ld & %.4f & %.3f & %.3f & %.3f \\\\\n",
                agentes, omp, mediana, s, e, f);
        printf("Agentes=%d OMP=%ld: mediana %.4fs, S=%.3f, E=%.3f, f=%.3f\n",
               agentes, omp, mediana, s, e, f);
    }
    fprintf(tex, "\\hline\\end{tabular}\n");
    fclose(csv); fclose(tex); free(yaml);
    if (fallo) fprintf(stderr, "Error: medicion incompleta; revisar SUBMIT y STATUS\n");
    return fallo ? 1 : 0;
}
