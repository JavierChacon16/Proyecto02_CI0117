
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "util.h"

static void uso(const char *programa)
{
    fprintf(stderr, "Uso: %s <host> <puerto> <comando> [argumentos]\n", programa);
    fprintf(stderr, "Comandos: submit <archivo.yaml>, status <id>, logs <id> [trabajo] [--follow], cancel <id>, list, shutdown\n");
}

static int entero(const char *texto, int minimo, int maximo, int *valor)
{
    char *fin;
    errno = 0;
    long numero = strtol(texto, &fin, 10);

    if (texto[0] == '\0' || errno != 0 || *fin != '\0' ||
        numero < minimo || numero > maximo)
        return -1;

    *valor = (int)numero;
    return 0;
}

static int enviar_archivo(int fd, const char *ruta)
{
    FILE *archivo = fopen(ruta, "rb");
    if (!archivo) {
        perror("fopen");
        return -1;
    }

    if (fseek(archivo, 0, SEEK_END) != 0) {
        fclose(archivo);
        return -1;
    }

    long largo = ftell(archivo);
    if (largo <= 0 || largo > PROTO_MAX_YAML ||
        fseek(archivo, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Tamano de YAML invalido\n");
        fclose(archivo);
        return -1;
    }

    char *datos = malloc((size_t)largo);
    if (!datos) {
        fclose(archivo);
        return -1;
    }

    int correcto = fread(datos, 1, (size_t)largo, archivo)
                   == (size_t)largo;
    fclose(archivo);

    char cabecera[64];
    snprintf(cabecera, sizeof cabecera, "SUBMIT %ld\n", largo);

    int resultado = -1;

    if (correcto &&
        nu_send_response(fd, cabecera) == 0 &&
        nu_write_all(fd, datos, (size_t)largo) == 0)
        resultado = 0;

    free(datos);
    return resultado;
}

int main(int argc, char *argv[])
{
    int puerto;

    if (argc < 4 || entero(argv[2], 1, 65535, &puerto) != 0) {
        uso(argv[0]);
        return 1;
    }

    const char *comando = argv[3];
    char solicitud[PROTO_MAX_REQUEST_LINE];
    int es_submit = 0;

    if (strcmp(comando, "submit") == 0 && argc == 5) {
        es_submit = 1;
    } else if (strcmp(comando, "list") == 0 && argc == 4) {
        strcpy(solicitud, "LIST\n");
    } else if (strcmp(comando, "shutdown") == 0 && argc == 4) {
        strcpy(solicitud, "SHUTDOWN\n");
    } else if ((strcmp(comando, "status") == 0 ||
                strcmp(comando, "cancel") == 0) && argc == 5) {
        int id;

        if (entero(argv[4], 0, INT_MAX, &id) != 0) {
            uso(argv[0]);
            return 1;
        }

        snprintf(solicitud, sizeof solicitud, "%s %d\n",
                 strcmp(comando, "status") == 0 ? "STATUS" : "CANCEL",
                 id);
    } else if (strcmp(comando, "logs") == 0 &&
               argc >= 5 && argc <= 7) {
        int id;
        const char *trabajo = "-";
        int seguir = 0;

        if (entero(argv[4], 0, INT_MAX, &id) != 0) {
            uso(argv[0]);
            return 1;
        }

        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--follow") == 0) {
                if (seguir) {
                    uso(argv[0]);
                    return 1;
                }
                seguir = 1;
            } else if (strcmp(trabajo, "-") == 0) {
                trabajo = argv[i];
            } else {
                uso(argv[0]);
                return 1;
            }
        }

        int n = snprintf(solicitud, sizeof solicitud,
                         "LOGS %d %s %d\n", id, trabajo, seguir);

        if (n < 0 || (size_t)n >= sizeof solicitud) {
            fprintf(stderr, "Nombre de trabajo demasiado largo\n");
            return 1;
        }
    } else {
        uso(argv[0]);
        return 1;
    }

    if (!es_submit) {
        request_t req;
        const char *motivo = NULL;

        if (protocol_parse_request(solicitud, &req, &motivo) != 0) {
            fprintf(stderr, "Solicitud invalida: %s\n",
                    motivo ? motivo : "error");
            return 1;
        }
    }

    int fd = nu_connect(argv[1], (unsigned short)puerto);
    if (fd < 0) {
        fprintf(stderr, "No se pudo conectar con el servidor\n");
        return 1;
    }

    int resultado;

    if (es_submit)
        resultado = enviar_archivo(fd, argv[4]);
    else
        resultado = nu_send_response(fd, solicitud);

    if (resultado != 0) {
        fprintf(stderr, "No se pudo enviar la solicitud\n");
        close(fd);
        return 1;
    }

    char linea[PROTO_MAX_RESPONSE_LINE];
    int error = 0;

    while (1) {
        ssize_t n = nu_read_line(fd, linea, sizeof linea);

        if (n == NU_CLOSED)
            break;

        if (n < 0) {
            fprintf(stderr, "Error al recibir la respuesta\n");
            error = 1;
            break;
        }

        puts(linea);

        if (strncmp(linea, "ERR ", 4) == 0 ||
            strncmp(linea, "INVALID ", 8) == 0)
            error = 1;

        if (strcmp(linea, "END") == 0 ||
            strcmp(linea, "BYE") == 0)
            break;
    }

    close(fd);
    return error;
}
