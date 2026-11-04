#include "state.h"
#include "protocol.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 8// numero maximo de tokens en una linea de solicitud, incluyendo el comando y los argumentos



static int fail(const char **motivo, const char *msg)
{
    if (motivo != NULL) {
        *motivo = msg;
    }
    return -1;
}

/* dividir una linea en partes separadas por espacios o tabuladores*/
static int tokenize(const char *line, char *work, size_t work_size, char *tokv[], int max_tokens)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len >= work_size) {
        return -1;
    }
    memcpy(work, line, len);
    work[len] = '\0';

    int n = 0;
    char *p = work;
    while (true) {
        while (*p == ' ' || *p == '\t') {
            *p = '\0';
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (n >= max_tokens) {
            return -1;
        }
        tokv[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
}
return n;
}

static int parse_int(const char *s, int min, int max, int *out)
{
    if (!isdigit((unsigned char)s[0])) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v < min || v > max) {
        return -1;
    }
    *out = (int)v;
    return 0;
}


static bool valid_job_ref(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len >= PROTO_JOBREF_MAX) {
        return false;
    }
    int slashes = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/') {
            slashes++;
            if (i == 0 || i == len - 1) {
                return false;
            }
        } else if (!isalnum(c) && c != '_') {
            return false;
        }
    }
    return slashes <= 1;
}
/*parsea una linea de solicitud y llena la estructura request_t correspondiente */
int protocol_parse_request(const char *line, request_t *out_req, const char **motivo)
{
    if (line == NULL || out_req == NULL) {
        return fail(motivo, "solicitud nula");
    }
    memset(out_req, 0, sizeof *out_req); /* type = CMD_UNKNOWN */

    char work[PROTO_MAX_REQUEST_LINE];
    char *tok[MAX_TOKENS];
    int n = tokenize(line, work, sizeof work, tok, MAX_TOKENS);
    if (n < 0) {
        return fail(motivo, "linea demasiado larga");
    }
    if (n == 0) {
        return fail(motivo, "linea vacia");
    }

    int id = 0;

    if (strcmp(tok[0], "SUBMIT") == 0) {//verifica si el comando es SUBMIT
        int len = 0;
        if (n != 2 || parse_int(tok[1], 1, PROTO_MAX_YAML, &len) != 0) {
            return fail(motivo, "uso: SUBMIT <bytes> (1 a 1048576)");
        }
        out_req->type = CMD_SUBMIT;
        out_req->data.submit.yaml_len = (size_t)len;
        return 0;
    }

    if (strcmp(tok[0], "STATUS") == 0) {//verifica si el comando es STATUS, si es asi, verifica que tenga 2 tokens y
        //que el segundo token sea un entero valido, si no, devuelve un error con el mensaje de uso correcto
        if (n != 2 || parse_int(tok[1], 0, INT_MAX, &id) != 0) {
            return fail(motivo, "uso: STATUS <id>");
        }
        out_req->type = CMD_STATUS;
        out_req->data.status.id = id;
        return 0;
    }
    if (strcmp(tok[0], "CANCEL") == 0) {//verifica si el comando es CANCEL, si es asi, verifica que tenga 2 tokens y 
        //que el segundo token sea un entero valido, si no, devuelve un error con el mensaje de uso correcto
        if (n != 2 || parse_int(tok[1], 0, INT_MAX, &id) != 0) {
            return fail(motivo, "uso: CANCEL <id>");
        }
        out_req->type = CMD_CANCEL;
        out_req->data.cancel.id = id;
        return 0;
    }
    if (strcmp(tok[0], "LOGS") == 0) {//verifica si el comando es LOGS, si es asi, verifica que tenga 4 tokens 
        //y que el segundo token sea un entero valido, si no, devuelve un error con el mensaje de uso correcto
        int follow = 0;
        if (n != 4 || parse_int(tok[1], 0, INT_MAX, &id) != 0
            || parse_int(tok[3], 0, 1, &follow) != 0) {
            return fail(motivo, "uso: LOGS <id> <etapa/trabajo|trabajo|-> <0|1>");
        }
        if (strcmp(tok[2], "-") == 0) {
            out_req->data.logs.job[0] = '\0';
        } else if (valid_job_ref(tok[2])) {
            memcpy(out_req->data.logs.job, tok[2], strlen(tok[2]) + 1);
        } else {
            return fail(motivo, "nombre de trabajo invalido");
        }
        out_req->type = CMD_LOGS;
        out_req->data.logs.id = id;
        out_req->data.logs.follow = (follow == 1);
        return 0;
    }
    if (strcmp(tok[0], "LIST") == 0) {//verifica si el comando es LIST, si es asi, verifica que tenga 1 token, 
        //si no, devuelve un error con el mensaje de uso correcto
        if (n != 1) {
            return fail(motivo, "LIST no lleva parametros");
        }
        out_req->type = CMD_LIST;
        return 0;
    }
    if (strcmp(tok[0], "SHUTDOWN") == 0) {//verifica si el comando es SHUTDOWN, si es asi, 
        //verifica que tenga 1 token, si no, devuelve un error con el mensaje de uso correcto
        if (n != 1) {
            return fail(motivo, "SHUTDOWN no lleva parametros");
        }
        out_req->type = CMD_SHUTDOWN;
        return 0;
    }
    return fail(motivo, "comando desconocido");
}


static int format_line(char *buf, int max_len, const char *fmt, ...)
{
    if (buf == NULL || max_len < 2) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)max_len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[0] = '\0';
        return -1;
    }
    if (n > max_len - 2) {
        n = max_len - 2;
    }
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = ' ';
        }
    }
    buf[n] = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}
/* FUNCIONES PARA FORMATEAR LAS RESPUESTAS DEL PROTOCOLO*/
int protocol_format_ok(char *buf, int max_len, int id)
{
    return format_line(buf, max_len, "OK %d", id);
}
int protocol_format_error(char *buf, int max_len, const char *codigo, const char *motivo)
{
    return format_line(buf, max_len, "ERR %s %s", codigo, motivo);
}
int protocol_format_invalid(char *buf, int max_len, int n_errores)
{
    return format_line(buf, max_len, "INVALID %d", n_errores);
}
int protocol_format_invalid_item(char *buf, int max_len, const char *ubicacion, const char *mensaje)
{
    return format_line(buf, max_len, "E %s %s", ubicacion, mensaje);
}
int protocol_format_status(char *buf, int max_len, int id, const char *estado, long long dur_ms)
{
    return format_line(buf, max_len, "STATUS %d %s %lld", id, estado, dur_ms);
}
int protocol_format_stage(char *buf, int max_len, const char *etapa, const char *estado, long long dur_ms)
{
    return format_line(buf, max_len, "STAGE %s %s %lld", etapa, estado, dur_ms);
}
int protocol_format_job(char *buf, int max_len, const char *etapa, const char *trabajo, const char *estado, long long dur_ms)
{
    return format_line(buf, max_len, "JOB %s %s %s %lld", etapa, trabajo, estado, dur_ms);
}
int protocol_format_step(char *buf, int max_len, const char *etapa, const char *trabajo,int indice, const char *estado, long long dur_ms)
{
    return format_line(buf, max_len, "STEP %s %s %d %s %lld", etapa, trabajo, indice, estado, dur_ms);
}
int protocol_format_list(char *buf, int max_len, int n)
{
    return format_line(buf, max_len, "LIST %d", n);
}
int protocol_format_run(char *buf, int max_len, int id, const char *estado, const char *nombre)
{
    return format_line(buf, max_len, "RUN %d %s %s", id, estado, nombre);
}
int protocol_format_logs(char *buf, int max_len, int id)
{
    return format_line(buf, max_len, "LOGS %d", id);
}
int protocol_format_log_line(char *buf, int max_len, const char *texto)
{
    if (!buf || max_len < 4 || !texto) return -1;
    buf[0] = '\0';
    size_t len = strlen(texto);
    size_t partes = len ? 1 + (len - 1) / PROTO_LOG_CHUNK : 1;
    /* Comprobar capacidad sin overflow; nunca truncar silenciosamente. */
    size_t capacidad = (size_t)max_len - 1;
    if (len > capacidad || partes > (capacidad - len) / 3) return -1;
    size_t entrada = 0, salida = 0;
    do {
        size_t n = len - entrada;
        if (n > PROTO_LOG_CHUNK) n = PROTO_LOG_CHUNK;
        buf[salida++] = 'L'; buf[salida++] = ' ';
        for (size_t i = 0; i < n; ++i) {
            char c = texto[entrada++];
            buf[salida++] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        buf[salida++] = '\n';
    } while (entrada < len);
    buf[salida] = '\0';
    return (int)salida;
}
int protocol_format_end(char *buf, int max_len)
{
    return format_line(buf, max_len, "END");
}
int protocol_format_bye(char *buf, int max_len)
{
    return format_line(buf, max_len, "BYE");
}
/* Para LOGS: transmitir texto arbitrariamente largo con buffers acotados.
 * Mantener la serializacion por conexion si varios productores la comparten.
 */
int protocol_send_log_lines(int fd, const char *texto)
{
    if (!texto) return -1;
    size_t restante = strlen(texto);
    do {
        size_t n = restante > PROTO_LOG_CHUNK ? PROTO_LOG_CHUNK : restante;
        char trozo[PROTO_LOG_CHUNK + 1];
        char linea[PROTO_LOG_CHUNK + 4];
        memcpy(trozo, texto, n); trozo[n] = '\0';
        if (protocol_format_log_line(linea, sizeof linea, trozo) < 0 ||
            nu_send_response(fd, linea) != 0) return -1;
        texto += n; restante -= n;
    } while (restante);
    return 0;
}
