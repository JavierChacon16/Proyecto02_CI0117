
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "Uso: %s <archivo> <MB> <texto|binario>\n",
                argv[0]);
        return 1;
    }

    char *fin;
    errno = 0;
    long mb = strtol(argv[2], &fin, 10);

    if (errno || *argv[2] == '\0' ||
        *fin != '\0' || mb < 1 || mb > 512) {
        fprintf(stderr, "MB debe estar entre 1 y 512\n");
        return 1;
    }

    int texto = strcmp(argv[3], "texto") == 0;

    if (!texto && strcmp(argv[3], "binario") != 0) {
        fprintf(stderr, "Modo invalido\n");
        return 1;
    }

    int fd = open(argv[1],
                  O_WRONLY | O_CREAT | O_EXCL, 0644);

    if (fd < 0) {
        perror("No se pudo crear archivo (puede existir)");
        return 1;
    }

    FILE *f = fdopen(fd, "wb");

    if (!f) {
        perror("fdopen");
        close(fd);
        unlink(argv[1]);
        return 1;
    }

    unsigned char bloque[65536];

    const char frase[] =
        "concurrencia paralelismo datos pipeline "
        "prueba determinismo\n";

    unsigned int semilla = 12345u;
    size_t offset = 0;
    size_t total = (size_t)mb * 1024u * 1024u;
    size_t pendiente = total;
    int error = 0;

    while (pendiente > 0) {
        size_t n = pendiente < sizeof bloque
                   ? pendiente : sizeof bloque;

        for (size_t i = 0; i < n; i++) {
            if (texto) {
                bloque[i] = (unsigned char)
                    frase[(offset + i) % (sizeof frase - 1)];
            } else {
                semilla = semilla * 1664525u + 1013904223u;
                bloque[i] = (unsigned char)(semilla >> 24);
            }
        }

        if (fwrite(bloque, 1, n, f) != n) {
            perror("fwrite");
            error = 1;
            break;
        }

        offset += n;
        pendiente -= n;
    }

    if (fclose(f) != 0) {
        perror("fclose");
        error = 1;
    }

    if (error) {
        unlink(argv[1]);
        return 1;
    }

    printf("Archivo generado: %s (%ld MiB, modo %s)\n",
           argv[1], mb, argv[3]);

    return 0;
}

