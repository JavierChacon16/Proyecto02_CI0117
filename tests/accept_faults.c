/* Solo pruebas Linux: inyectar una vez cada error recuperable de accept4. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

/* glibc declara el parametro con una union transparente en modo GNU. */
#ifdef __GLIBC__
int accept4(int fd, __SOCKADDR_ARG address, socklen_t *len, int flags)
#else
int accept4(int fd, struct sockaddr *address, socklen_t *len, int flags)
#endif
{
    static unsigned int siguiente;
    static const int errores[] = { EMFILE, ENFILE, ENOBUFS, ENOMEM };
    /* accept4 solo lo invoca main en este servidor. */
    if (getenv("TEST_ACCEPT_ERRORS") && siguiente < 4) {
        errno = errores[siguiente++];
        return -1;
    }
#ifdef __GLIBC__
    return (int)syscall(SYS_accept4, fd, address.__sockaddr__, len, flags);
#else
    return (int)syscall(SYS_accept4, fd, address, len, flags);
#endif
}
