#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stddef.h>
#include <stdbool.h>
#define PROTO_MAX_REQUEST_LINE 1024//longitud maxima de una linea de solicitud, incluyendo el terminador nulo
#define PROTO_MAX_RESPONSE_LINE 4096
#define PROTO_MAX_YAML 1048576
#define PROTO_JOBREF_MAX 256
#define PROTO_LOG_CHUNK 4000
#define ERR_BAD_REQUEST "BAD_REQUEST"
#define ERR_INVALID_STATE "INVALID_STATE"
typedef enum { CMD_UNKNOWN, CMD_SUBMIT, CMD_STATUS, CMD_CANCEL, CMD_LOGS,
               CMD_LIST, CMD_SHUTDOWN } command_t;
typedef struct {
    command_t type;
    union {
        struct { size_t yaml_len; } submit;
        struct { int id; } status;
        struct { int id; } cancel;
        struct { int id; char job[PROTO_JOBREF_MAX]; bool follow; } logs;
    } data;
} request_t;
int protocol_parse_request(const char *line, request_t *out_req, const char **motivo);
int protocol_format_ok(char *buf, int max_len, int id);
int protocol_format_error(char *buf, int max_len, const char *codigo, const char *motivo);
int protocol_format_invalid(char *buf, int max_len, int n_errores);
int protocol_format_invalid_item(char *buf, int max_len, const char *ubicacion, const char *mensaje);
int protocol_format_status(char *buf, int max_len, int id, const char *estado, long long dur_ms);
int protocol_format_stage(char *buf, int max_len, const char *etapa, const char *estado, long long dur_ms);
int protocol_format_job(char *buf, int max_len, const char *etapa, const char *trabajo, const char *estado, long long dur_ms);
int protocol_format_step(char *buf, int max_len, const char *etapa, const char *trabajo,int indice, const char *estado, long long dur_ms);
int protocol_format_list(char *buf, int max_len, int n);
int protocol_format_run(char *buf, int max_len, int id, const char *estado, const char *nombre);
int protocol_format_logs(char *buf, int max_len, int id);
int protocol_format_log_line(char *buf, int max_len, const char *texto);
int protocol_format_end(char *buf, int max_len);
int protocol_format_bye(char *buf, int max_len);
/* format_log_line admite varias lineas si caben; devuelve -1 si no caben.
 * send_log_lines transmite todas las partes sin un buffer proporcional al texto. */
int protocol_send_log_lines(int fd, const char *texto);
#endif
