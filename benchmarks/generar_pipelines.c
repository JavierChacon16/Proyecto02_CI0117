
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int generar(const char *ruta, int n, int modo)
{
    FILE *f = fopen(ruta, "w");
    if (!f) {
        perror(ruta);
        return -1;
    }

    fprintf(f,
            "name: rendimiento\n"
            "stages:\n"
            "  - stage: prueba\n"
            "    jobs:\n");

    for (int i = 0; i < n; i++) {
        fprintf(f, "      - job: j%d\n", i);

        if (modo == 1 && i > 0)
            fprintf(f,
                    "        dependsOn: j%d\n",
                    i - 1);

        if (modo == 2 && i > 1)
            fprintf(f,
                    "        dependsOn: j%d\n",
                    i - 2);

        fprintf(f,
                "        steps:\n"
                "          - script: sleep 1\n");
    }

    if (fclose(f) != 0)
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
                "Uso: %s <cantidad_trabajos> "
                "<prefijo_salida>\n", argv[0]);
        return 2;
    }

    char *fin;
    long n = strtol(argv[1], &fin, 10);

    if (*fin || n < 2 || n > 100)
        return 2;

    const char *nombres[] = {
        "ancho", "profundo", "mixto"
    };

    for (int modo = 0; modo < 3; modo++) {
        size_t capacidad = strlen(argv[2]) + 32;
        char *ruta = malloc(capacidad);

        if (!ruta)
            return 1;

        snprintf(ruta, capacidad, "%s_%s.yaml",
                 argv[2], nombres[modo]);

        int rc = generar(ruta, (int)n, modo);

        if (rc == 0)
            puts(ruta);

        free(ruta);

        if (rc != 0)
            return 1;
    }

    return 0;
}

