#ifndef PROTOCOL_H
#define PROTOCOL_H


#include <stdbool.h>
#include <stddef.h>

#define PROTO_MAX_REQUEST_LINE   256
#define PROTO_MAX_RESPONSE_LINE  4096
#define PROTO_MAX_YAML           (1024 * 1024)
#define PROTO_JOBREF_MAX         128
#define ERR_BAD_REQUEST    "BAD_REQUEST"
#define ERR_NOT_FOUND      "NOT_FOUND"
#define ERR_INVALID_STATE  "INVALID_STATE"
#define ERR_SHUTTING_DOWN  "SHUTTING_DOWN"
#define ERR_TOO_LARGE      "TOO_LARGE"
#define ERR_INTERNAL       "INTERNAL"

typedef enum {
    CMD_UNKNOWN = 0,CMD_SUBMIT,CMD_STATUS,CMD_LOGS,CMD_CANCEL,CMD_LIST,CMD_SHUTDOWN
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    union {
        struct { size_t yaml_len; } submit;
        struct { int id; } status;
        struct { int id; char job[PROTO_JOBREF_MAX]; bool follow; } logs; /* job "" = toda la ejecucion */
        struct { int id; } cancel;
    } data;
} request_t;

int protocol_parse_request(const char *line, request_t *out_req, const char **motivo);
int protocol_format_ok(char *buf, int max_len, int id);
int protocol_format_error(char *buf, int max_len, const char *codigo, const char *motivo);
int protocol_format_invalid(char *buf, int max_len, int n_errores);
int protocol_format_invalid_item(char *buf, int max_len, const char *ubicacion, const char *mensaje);
int protocol_format_status(char *buf, int max_len, int id, const char *estado, long long dur_ms);
int protocol_format_stage(char *buf, int max_len, const char *etapa, const char *estado, long long dur_ms);
int protocol_format_job(char *buf, int max_len, const char *etapa, const char *trabajo,  const char *estado, long long dur_ms);
int protocol_format_step(char *buf, int max_len, const char *etapa, const char *trabajo, int indice, const char *estado, long long dur_ms);
int protocol_format_list(char *buf, int max_len, int n);
int protocol_format_run(char *buf, int max_len, int id, const char *estado, const char *nombre);
int protocol_format_logs(char *buf, int max_len, int id);
int protocol_format_log_line(char *buf, int max_len, const char *texto);
int protocol_format_end(char *buf, int max_len);
int protocol_format_bye(char *buf, int max_len);

#endif