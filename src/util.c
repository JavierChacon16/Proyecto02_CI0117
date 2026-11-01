//
// Created by Sleyter Angulo on 9/14/26.
//

#define _GNU_SOURCE   

#include "../include/util.h"
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
    int file_descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); // CLOEXEC: los procesos hijos no heredan el socket
    if (file_descriptor < 0)
    {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(file_descriptor, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    {
        perror("setsockopt");
        close(file_descriptor); //cerrar socket si hay un error
        return -1;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // solo local: el servidor ejecuta scripts
    server_address.sin_port = htons(port);

    if (bind(file_descriptor, (struct sockaddr*)&server_address, sizeof(server_address)) < 0)
    {
        perror("bind");
        close(file_descriptor);
        return -1;
    }

    if (listen(file_descriptor, backlog) < 0)
    {
        perror("listen");
        close(file_descriptor);
        return -1;
    }

    return file_descriptor;
}

int nu_accept(int listen_fd)
{
    int file_descriptor;

    do {
        file_descriptor = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (file_descriptor < 0 && (errno == EINTR || errno == ECONNABORTED));

    
    return file_descriptor;
}

int nu_connect(const char *host, unsigned short port)
{
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int file_descriptor = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next)
    {
        file_descriptor = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (file_descriptor < 0)
            continue;

        if (connect(file_descriptor, p->ai_addr, p->ai_addrlen) == 0)
            break;

        close(file_descriptor);
        file_descriptor = -1;
    }

    freeaddrinfo(res);
    return file_descriptor;
}

int nu_write_all(int file_descriptor, const void* buffer, size_t size)
{
    const char *p = buffer;
    size_t sent = 0;

    while (sent < size) {
        
        ssize_t n = send(file_descriptor, p + sent, size - sent, MSG_NOSIGNAL);
            if (n < 0) {
            if (errno == EINTR)
                continue;          
            perror("send");
            return -1;
        }
     
        if (n == 0)
        {
            fprintf(stderr, "send: no se enviaron datos\n");
            return -1;
        }

        
        sent += (size_t)n;
    }

    
    return 0;
}

ssize_t nu_read_line(int file_descriptor, char *buffer, size_t size)
{
    size_t used = 0;
    char c;
    ssize_t n;

    if (size < 2)
        return NU_ERROR;

    for (;;) {
        do {
            n = recv(file_descriptor, &c, 1, 0);
        } while (n < 0 && errno == EINTR);

        if (n < 0)
        {
            perror("recv");
            return NU_ERROR;
        }
        if (n == 0)
            return used == 0 ? NU_CLOSED : NU_ERROR;

        if (c == '\n')
        {
            buffer[used] = '\0';
            return (ssize_t)used;
        }
        if (c == '\0')
            return NU_BAD_LINE;

      
        if (used + 1 >= size)
            return NU_TOO_LONG;

        buffer[used++] = c;
    }
}

int nu_read_all(int file_descriptor, void *buffer, size_t size)
{
    char *p = buffer;
    size_t received = 0;

    while (received < size) {
        ssize_t n = recv(file_descriptor, p + received, size - received, 0);
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

int nu_send_response(int file_descriptor, const char *response)
{
    return nu_write_all(file_descriptor, response, strlen(response));
}