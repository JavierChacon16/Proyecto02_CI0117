//
// Created by Sleyter Angulo on 9/14/26.
//


#define _GNU_SOURCE

#include "util.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

int nu_listen(unsigned short port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &yes, sizeof yes) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int nu_accept(int listen_fd)
{
    int fd;

    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 &&
             (errno == EINTR || errno == ECONNABORTED));

    return fd;
}

int nu_connect(const char *host, unsigned short port)
{
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = -1;

    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family,
                    p->ai_socktype | SOCK_CLOEXEC,
                    p->ai_protocol);

        if (fd < 0)
            continue;

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int nu_write_all(int fd, const void *buffer, size_t size)
{
    const char *p = buffer;
    size_t sent = 0;

    while (sent < size) {
        ssize_t n = send(fd, p + sent, size - sent,
                         MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

ssize_t nu_read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;
    int larga = 0;
    int invalida = 0;
    char c;

    if (size < 2)
        return NU_ERROR;

    for (;;) {
        ssize_t n;

        do {
            n = recv(fd, &c, 1, 0);
        } while (n < 0 && errno == EINTR);

        if (n < 0)
            return NU_ERROR;

        if (n == 0) {
            if (used == 0 && !larga && !invalida)
                return NU_CLOSED;
            return NU_ERROR;
        }

        if (c == '\n') {
            buffer[used] = '\0';

            if (invalida)
                return NU_BAD_LINE;
            if (larga)
                return NU_TOO_LONG;

            return (ssize_t)used;
        }

        if (c == '\0') {
            invalida = 1;
            continue;
        }

        if (used + 1 >= size) {
            larga = 1;
            continue;
        }

        if (!larga && !invalida)
            buffer[used++] = c;
    }
}

int nu_read_all(int fd, void *buffer, size_t size)
{
    char *p = buffer;
    size_t received = 0;

    while (received < size) {
        ssize_t n = recv(fd, p + received,
                         size - received, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return NU_ERROR;
        }

        if (n == 0)
            return NU_CLOSED;

        received += (size_t)n;
    }

    return 0;
}

int nu_send_response(int fd, const char *response)
{
    return nu_write_all(fd, response, strlen(response));
}
