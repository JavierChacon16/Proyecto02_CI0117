
#include <stdio.h>
#include <string.h>
#include "protocol.h"

int main(void)
{
    request_t req;
    const char *motivo = NULL;
    char respuesta[128];
    int errores = 0;

    if (protocol_parse_request("LIST\n", &req, &motivo) != 0 ||
        req.type != CMD_LIST) {
        printf("Error: LIST\n");
        errores++;
    }

    if (protocol_parse_request("SHUTDOWN\n", &req, &motivo) != 0 ||
        req.type != CMD_SHUTDOWN) {
        printf("Error: SHUTDOWN\n");
        errores++;
    }

    if (protocol_parse_request("STATUS 12\n", &req, &motivo) != 0 ||
        req.type != CMD_STATUS || req.data.status.id != 12) {
        printf("Error: STATUS\n");
        errores++;
    }

    if (protocol_parse_request("CANCEL 12\n", &req, &motivo) != 0 ||
        req.type != CMD_CANCEL || req.data.cancel.id != 12) {
        printf("Error: CANCEL\n");
        errores++;
    }

    if (protocol_parse_request("SUBMIT 50\n", &req, &motivo) != 0 ||
        req.type != CMD_SUBMIT || req.data.submit.yaml_len != 50) {
        printf("Error: SUBMIT\n");
        errores++;
    }

    if (protocol_parse_request("LOGS 12 - 1\n", &req, &motivo) != 0 ||
        req.type != CMD_LOGS || req.data.logs.id != 12 ||
        req.data.logs.job[0] != '\0' || !req.data.logs.follow) {
        printf("Error: LOGS\n");
        errores++;
    }

    if (protocol_parse_request("LOGS 12 build/test 1\n",
                               &req, &motivo) != 0 ||
        req.type != CMD_LOGS ||
        strcmp(req.data.logs.job, "build/test") != 0) {
        printf("Error: nombre de trabajo en LOGS\n");
        errores++;
    }

    const char *invalidos[] = {
        "",
        "NOEXISTE\n",
        "STATUS abc\n",
        "SUBMIT 0\n",
        "LIST 3\n",
        "LOGS 1 trabajo 2\n"
    };

    int cantidad = (int)(sizeof invalidos / sizeof invalidos[0]);

    for (int i = 0; i < cantidad; i++) {
        motivo = NULL;

        if (protocol_parse_request(invalidos[i], &req,
                                   &motivo) == 0 || motivo == NULL) {
            printf("Error: solicitud invalida %d\n", i + 1);
            errores++;
        }
    }

    if (protocol_format_ok(respuesta, (int)sizeof respuesta, 7) <= 0 ||
        strcmp(respuesta, "OK 7\n") != 0) {
        printf("Error: respuesta OK\n");
        errores++;
    }

    if (protocol_format_end(respuesta, (int)sizeof respuesta) <= 0 ||
        strcmp(respuesta, "END\n") != 0) {
        printf("Error: respuesta END\n");
        errores++;
    }

    if (errores > 0) {
        printf("Fallaron %d pruebas\n", errores);
        return 1;
    }

    printf("Pruebas de protocolo: OK\n");
    return 0;
}

