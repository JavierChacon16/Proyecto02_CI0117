#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "protocol.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static void rechazar_ocupado(int fd)
{
    /* No bloquear el hilo accept por un cliente que no lee su rechazo.
     * La entrega del mensaje es de mejor esfuerzo si el peer se desconecta.
     */
    static const char mensaje[] = "ERR BUSY limite de clientes o recursos\n";
    (void)send(fd, mensaje, sizeof mensaje - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}

int error_cliente(int fd, const char *codigo, const char *mensaje)
{
    char linea[PROTO_MAX_RESPONSE_LINE];
    if (protocol_format_error(linea, sizeof linea, codigo, mensaje) < 0) return -1;
    return nu_send_response(fd, linea);
}

static void solicitar_apagado(servidor_t *s)
{
    pthread_mutex_lock(&s->mu);
    s->apagar = 1;
    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);
}

static void atender(cliente_t *c)
{
    char linea[PROTO_MAX_REQUEST_LINE];
    request_t req;
    const char *motivo = NULL;
    ssize_t n = nu_read_line_until(c->fd, linea, sizeof linea, &c->vencimiento);
    if (n == NU_CLOSED) return;
    if (n == NU_TIMEOUT) {
        error_cliente(c->fd, "TIMEOUT", "plazo total de lectura agotado");
        return;
    }
    if (n < 0 || protocol_parse_request(linea, &req, &motivo) != 0) {
        error_cliente(c->fd, ERR_BAD_REQUEST, motivo ? motivo : (n == NU_TOO_LONG ? "linea demasiado larga" : "linea invalida"));
        return;
    }
    switch (req.type) {
    case CMD_SUBMIT:
        submit(c, req.data.submit.yaml_len);
        break;
    case CMD_LIST:
        consultar(c, 0, 1);
        break;
    case CMD_STATUS:
        consultar(c, req.data.status.id, 0);
        break;
    case CMD_CANCEL:
        cancelar_ejecucion(c, req.data.cancel.id);
        break;
    case CMD_SHUTDOWN:
        if (protocol_format_bye(linea, sizeof linea) >= 0)
            nu_send_response(c->fd, linea);
        solicitar_apagado(c->servidor);
        break;
    default:
        error_cliente(c->fd, ERR_INVALID_STATE, "comando aun no implementado");
        break;
    }
}

static void *cliente_hilo(void *arg)
{
    cliente_t *c = arg;
    servidor_t *s = c->servidor;
    atender(c);
    pthread_mutex_lock(&s->mu);
    cliente_t **p = &s->clientes;
    while (*p != c) p = &(*p)->siguiente;
    *p = c->siguiente;
    --s->n_clientes;
    close(c->fd); /* Bajo mutex: evita shutdown sobre un descriptor reutilizado. */
    free(c);
    pthread_cond_broadcast(&s->cambio);
    pthread_mutex_unlock(&s->mu);
    return NULL; /* No vuelve a acceder a s. */
}

int iniciar_servidor(int puerto, int agentes, const char *directorio)
{
    struct stat st;
    if ((mkdir(directorio, 0700) < 0 && errno != EEXIST) ||
        stat(directorio, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Directorio de trabajo invalido\n"); return EXIT_FAILURE;
    }
    /* SIGINT/SIGTERM bloqueadas ANTES de crear hilos. main las consume
     * con sigtimedwait: no hay mutex/free/close desde un signal handler. */
    /* Integracion futura de scripts, SIN fork:
     * posix_spawnattr_init(&attr_spawn);
     * sigemptyset(&mascara_hijo);
     * posix_spawnattr_setsigmask(&attr_spawn, &mascara_hijo);
     * posix_spawnattr_setpgroup(&attr_spawn, 0); // grupo propio, PGID = PID
     * posix_spawnattr_setflags(&attr_spawn,
     *     POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
     * posix_spawn(...); comprobar retorno, destruir atributos y recolectar hijo.
     * La mascara vacia evita heredar SIGINT/SIGTERM bloqueadas del servidor.
     */
    sigset_t senales;
    sigemptyset(&senales); sigaddset(&senales, SIGINT); sigaddset(&senales, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &senales, NULL) != 0) return EXIT_FAILURE;
    int fd = nu_listen((unsigned short)puerto, 64);
    if (fd < 0) return EXIT_FAILURE;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd); return EXIT_FAILURE;
    }
    servidor_t s = { .mu = PTHREAD_MUTEX_INITIALIZER,
        .mu_id = PTHREAD_MUTEX_INITIALIZER,
        .cambio = PTHREAD_COND_INITIALIZER, .proximo_id = 1,
        .agentes = agentes, .directorio = directorio };
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        close(fd); pthread_cond_destroy(&s.cambio);
        pthread_mutex_destroy(&s.mu); pthread_mutex_destroy(&s.mu_id);
        return EXIT_FAILURE;
    }
    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t planificador;
    if (rc == 0) rc = pthread_create(&planificador, NULL, planificar, &s);
    if (rc != 0) {
        fprintf(stderr, "pthread: %s\n", strerror(rc));
        pthread_attr_destroy(&attr); close(fd);
        pthread_cond_destroy(&s.cambio); pthread_mutex_destroy(&s.mu);
        pthread_mutex_destroy(&s.mu_id);
        return EXIT_FAILURE;
    }
    pthread_mutex_lock(&s.mu);
    while (!s.listo) pthread_cond_wait(&s.cambio, &s.mu);
    pthread_mutex_unlock(&s.mu);
    printf("Servidor escuchando en el puerto %d (simulacion)\n", puerto);
    fflush(stdout);
    int fallo = 0;
    for (;;) {
        struct timespec cero = {0};
        int signo = sigtimedwait(&senales, NULL, &cero);
        if (signo == SIGINT || signo == SIGTERM) solicitar_apagado(&s);
        pthread_mutex_lock(&s.mu);
        int apagar = s.apagar;
        pthread_mutex_unlock(&s.mu);
        if (apagar) break;
        struct pollfd evento = { .fd = fd, .events = POLLIN };
        rc = poll(&evento, 1, 100);
        if (rc < 0) { if (errno == EINTR) continue; fallo = 1; break; }
        if (!rc) continue;
        if (!(evento.revents & POLLIN)) { fallo = 1; break; }
        int conexion = nu_accept(fd);
        if (conexion < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                struct timespec pausa = { .tv_nsec = 50000000L };
                while (nanosleep(&pausa, &pausa) < 0 && errno == EINTR) {}
                continue;
            }
            perror("accept"); fallo = 1; break;
        }
        struct timespec vencimiento;
        if (clock_gettime(CLOCK_MONOTONIC, &vencimiento) < 0) {
            close(conexion); continue;
        }
        vencimiento.tv_sec += PLAZO_LECTURA;
        pthread_mutex_lock(&s.mu);
        int lleno = s.n_clientes >= MAX_CLIENTES;
        pthread_mutex_unlock(&s.mu);
        if (lleno) { rechazar_ocupado(conexion); continue; }
        struct timeval limite = { .tv_sec = 10 };
        if (setsockopt(conexion, SOL_SOCKET, SO_RCVTIMEO, &limite, sizeof limite) < 0 ||
            setsockopt(conexion, SOL_SOCKET, SO_SNDTIMEO, &limite, sizeof limite) < 0) {
            close(conexion); continue;
        }
        cliente_t *c = malloc(sizeof *c);
        if (!c) { rechazar_ocupado(conexion); continue; }
        c->fd = conexion; c->servidor = &s; c->vencimiento = vencimiento;
        pthread_mutex_lock(&s.mu);
        c->siguiente = s.clientes; s.clientes = c;
        ++s.n_clientes; /* Solo main admite; las bajas usan el mismo mutex. */
        pthread_t hilo;
        rc = pthread_create(&hilo, &attr, cliente_hilo, c);
        if (rc != 0) { s.clientes = c->siguiente; --s.n_clientes; free(c); }
        pthread_mutex_unlock(&s.mu);
        if (rc != 0) rechazar_ocupado(conexion);
    }
    close(fd);
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&s.mu);
    s.apagar = 1;
    /* Politica: cancelar pendientes, terminar los que ya estan running. */
    for (ejecucion_t *e = s.primero; e; e = e->cola) {
        e->estado = CANCELED; free(e->yaml); e->yaml = NULL;
        --s.n_cola; s.bytes_cola -= e->bytes;
    }
    s.primero = s.ultimo = NULL;
    for (cliente_t *c = s.clientes; c; c = c->siguiente)
        shutdown(c->fd, SHUT_RDWR); /* Desbloquea lecturas . */
    pthread_cond_broadcast(&s.cambio);
    while (s.clientes) pthread_cond_wait(&s.cambio, &s.mu);
    pthread_mutex_unlock(&s.mu);
    pthread_join(planificador, NULL);
    /* Sincronizacion explicita adicional con los ultimos accesos bajo mu.
     * join y la barrera de salida OpenMP ya esperan la terminacion.
     * */
    pthread_mutex_lock(&s.mu);
    pthread_mutex_unlock(&s.mu);
    fprintf(stderr, "Maximo de agentes simultaneos: %d/%d\n", s.max_activos, s.agentes);
    ejecucion_t *e = s.ejecuciones;
    while (e) { ejecucion_t *sig = e->siguiente; free(e->yaml); free(e); e = sig; }
    pthread_cond_destroy(&s.cambio); pthread_mutex_destroy(&s.mu);
    pthread_mutex_destroy(&s.mu_id);
    return fallo ? EXIT_FAILURE : EXIT_SUCCESS;
}
