#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { cliente_t c; size_t bytes; } prueba_t;
static void *rechazar(void *arg)
{
    prueba_t *p = arg;
    assert(submit(&p->c, p->bytes) == 0);
    close(p->c.fd);
    return NULL;
}
int main(void)
{
    for (int parcial = 0; parcial < 2; ++parcial) {
        servidor_t s = { .mu = PTHREAD_MUTEX_INITIALIZER, .n_cola = MAX_COLA };
        int fd[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        prueba_t p = { .c = {.fd = fd[0], .servidor = &s}, .bytes = 1024 * 1024 };
        assert(clock_gettime(CLOCK_MONOTONIC, &p.c.vencimiento) == 0);
        /* Plazo corto en este fixture; produccion conserva los diez segundos. */
        p.c.vencimiento.tv_sec += 1;
        pthread_t hilo;
        assert(pthread_create(&hilo, NULL, rechazar, &p) == 0);
        size_t n = parcial ? 1 : p.bytes;
        char *yaml = malloc(n);
        assert(yaml);
        memset(yaml, 'x', n);
        assert(nu_write_all(fd[1], yaml, n) == 0);
        free(yaml);
        char linea[256];
        assert(nu_read_line(fd[1], linea, sizeof linea) > 0);
        assert(strncmp(linea, "ERR BUSY ", 9) == 0);
        assert(nu_read_line(fd[1], linea, sizeof linea) == NU_CLOSED);
        pthread_join(hilo, NULL);
        assert(s.n_cola == MAX_COLA && s.bytes_cola == 0);
        close(fd[1]);
        pthread_mutex_destroy(&s.mu);
    }
    puts("OK: BUSY con payload de 1 MiB sin Broken Pipe; drenaje parcial vence");
    return 0;
}
