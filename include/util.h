//
// Created by Sleyter Angulo on 9/14/26.
//

#ifndef UTIL_H
/* Utilidades para operaciones de red y manejo de errores. */
#define UTIL_H
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#define NU_ERROR (-1)
#define NU_CLOSED (-2)
#define NU_BAD_LINE (-3)
#define NU_TOO_LONG (-4)
#define NU_TIMEOUT (-5)
int nu_listen(unsigned short port, int backlog);
int nu_accept(int listen_fd);
int nu_connect(const char *host, unsigned short port);
int nu_write_all(int fd, const void *buffer, size_t size);
ssize_t nu_read_line(int fd, char *buffer, size_t size);
int nu_read_all(int fd, void *buffer, size_t size);
int nu_send_response(int fd, const char *response);
ssize_t nu_read_line_until(int fd, char *buffer, size_t size,
                           const struct timespec *until);
int nu_read_all_until(int fd, void *buffer, size_t size,
                      const struct timespec *until);
#endif
