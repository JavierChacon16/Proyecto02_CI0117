
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "util.h"

static int numero(const char *texto, int minimo, int maximo, int *valor)
{
    char *fin;
    errno = 0;
    long n = strtol(texto, &fin, 10);

    if (texto[0] == '\0' || errno != 0 || *fin != '\0' ||
        n < minimo || n > maximo)
        return -1;

    *valor = (int)n;
    return 0;
}

static int responder_error(int fd, const char *codigo, const char *mensaje)
{
    char linea[PROTO_MAX_RESPONSE_LINE];

    if (protocol_format_error(linea, sizeof linea,
                              codigo, mensaje) < 0)
        return -1;

    return nu_send_response(fd, linea);
}

static int atender(int fd)
{
    char linea[PROTO_MAX_REQUEST_LINE];
    request_t solicitud;
    const char *motivo = NULL;

    ssize_t leidos = nu_read_line(fd, linea, sizeof linea);

    if (leidos == NU_CLOSED)
        return 0;

    if (leidos < 0)
        return responder_error(fd, ERR_BAD_REQUEST, "linea invalida");

    if (protocol_parse_request(linea, &solicitud, &motivo) != 0)
        return responder_error(fd, ERR_BAD_REQUEST,
                               motivo ? motivo : "solicitud invalida");

    if (solicitud.type == CMD_LIST) {
        char respuesta[PROTO_MAX_RESPONSE_LINE];

        if (protocol_format_list(respuesta, sizeof respuesta, 0) < 0 ||
            nu_send_response(fd, respuesta) != 0)
            return -1;

        if (protocol_format_end(respuesta, sizeof respuesta) < 0)
            return -1;

        return nu_send_response(fd, respuesta);
    }

    if (solicitud.type == CMD_SHUTDOWN) {
        char respuesta[PROTO_MAX_RESPONSE_LINE];

        if (protocol_format_bye(respuesta, sizeof respuesta) < 0 ||
            nu_send_response(fd, respuesta) != 0)
            return -1;

        return 1;
    }

    return responder_error(fd, ERR_INVALID_STATE,
                           "comando aun no implementado");
}

int main(int argc, char *argv[])
{
    int puerto, agentes;

    if (argc != 4 ||
        numero(argv[1], 1, 65535, &puerto) != 0 ||
        numero(argv[2], 1, INT_MAX, &agentes) != 0) {
        fprintf(stderr, "Uso: %s <puerto> <agentes> <directorio>\n",
                argv[0]);
        return 1;
    }

    (void)agentes;

    int servidor = nu_listen((unsigned short)puerto, 16);

    if (servidor < 0)
        return 1;

    printf("Servidor escuchando en el puerto %d\n", puerto);
    fflush(stdout);

    int apagar = 0;

    while (!apagar) {
        int cliente = nu_accept(servidor);

        if (cliente < 0) {
            perror("accept");
            break;
        }

        int resultado = atender(cliente);
        close(cliente);

        if (resultado == 1)
            apagar = 1;
    }

    close(servidor);
    return 0;
}
