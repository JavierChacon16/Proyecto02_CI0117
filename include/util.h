//
// Created by Sleyter Angulo on 9/14/26.
//

#ifndef MINI_SERVER_NET_UTIL_H
#define MINI_SERVER_NET_UTIL_H

#include <stddef.h>
#include <sys/types.h>

// Codigos de error 
#define NU_ERROR    (-1)   // error de red, o cierre a mitad de un mensaje
#define NU_CLOSED   (-2)   // el otro extremo cerro la conexion sin enviar nada mas
#define NU_TOO_LONG (-3)   // la linea no cabe en el buffer del llamador
#define NU_BAD_LINE (-4)   // la linea contiene un byte nulo

int nu_listen(unsigned short port, int backlog);
int nu_accept(int listen_fd);
int nu_connect(const char *host, unsigned short port);
int nu_write_all(int file_descriptor, const void *buffer, size_t size);
ssize_t nu_read_line(int file_descriptor, char *buffer, size_t size);
int nu_read_all(int file_descriptor, void *buffer, size_t size);
int nu_send_response(int file_descriptor, const char *response);

#endif //MINI_SERVER_NET_UTIL_H