#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "protocol.h"
#include "util.h"

typedef struct { int fd; const char *texto; } envio_t;
static void *enviar(void *arg)
{
    envio_t *e = arg;
    assert(protocol_send_log_lines(e->fd, e->texto) == 0);
    shutdown(e->fd, SHUT_WR);
    return NULL;
}
int main(void)
{
    request_t req;
    const char *motivo = NULL;
    char enorme[PROTO_MAX_REQUEST_LINE + 10];
    memset(enorme, 'a', sizeof enorme - 1); enorme[sizeof enorme - 1] = '\0';
    assert(protocol_parse_request(enorme, &req, &motivo) < 0);
    assert(strcmp(motivo, "linea demasiado larga") == 0);
    assert(protocol_parse_request(" \tSUBMIT\t8\r\n", &req, &motivo) == 0);
    assert(req.type == CMD_SUBMIT && req.data.submit.yaml_len == 8);
    const size_t largos[] = {0, 1, 4000, 4001, 8000, 12037};
    for (size_t k = 0; k < sizeof largos / sizeof largos[0]; ++k) {
        size_t len = largos[k];
        char *texto = malloc(len + 1);
        char *salida = malloc(len + 100);
        assert(texto && salida);
        for (size_t i = 0; i < len; ++i) texto[i] = (char)('a' + i % 26);
        texto[len] = '\0';
        int n = protocol_format_log_line(salida, (int)len + 100, texto);
        assert(n >= 3);
        char chico[4];
        if (len > 0) assert(protocol_format_log_line(chico, sizeof chico, texto) == -1);
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        envio_t envio = { sockets[0], texto };
        pthread_t hilo;
        assert(pthread_create(&hilo, NULL, enviar, &envio) == 0);
        char linea[PROTO_MAX_RESPONSE_LINE];
        size_t leidos = 0, partes = 0;
        ssize_t bytes;
        while ((bytes = nu_read_line(sockets[1], linea, sizeof linea)) != NU_CLOSED) {
            assert(bytes >= 2 && bytes <= PROTO_LOG_CHUNK + 2);
            assert(strncmp(linea, "L ", 2) == 0);
            assert(memcmp(texto + leidos, linea + 2, (size_t)bytes - 2) == 0);
            leidos += (size_t)bytes - 2;
            ++partes;
        }
        assert(leidos == len);
        assert(partes == (len ? (len + 3999) / 4000 : 1));
        assert((size_t)n == len + partes * 3);
        pthread_join(hilo, NULL);
        close(sockets[0]); close(sockets[1]);
        free(texto); free(salida);
    }
    puts("OK: protocolo y logs fragmentados sin perdida");
    return 0;
}

