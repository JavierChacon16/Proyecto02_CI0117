
#include <stdio.h>
#include <string.h>
#include "yaml_parser.h"

static int probar(const char *nombre, const char *yaml, int valido)
{
    char error[256];
    int resultado = validar_yaml(yaml, strlen(yaml),
                                 error, sizeof error);

    if ((resultado == 0) != valido) {
        printf("Error en prueba %s: %s\n", nombre, error);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *correcto =
        "name: Ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        steps:\n"
        "          - script: echo hola\n";

    const char *duplicado =
        "name: Ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        steps:\n"
        "          - script: echo hola\n"
        "      - job: compilar\n"
        "        steps:\n"
        "          - script: echo mundo\n";

    const char *desconocida =
        "name: Ejemplo\n"
        "incorrecto: valor\n"
        "stages: []\n";

    const char *sin_pasos =
        "name: Ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        steps: []\n";

    int errores = 0;

    errores += probar("YAML valido", correcto, 1);
    errores += probar("Nombre duplicado", duplicado, 0);
    errores += probar("Clave desconocida", desconocida, 0);
    errores += probar("Trabajo sin pasos", sin_pasos, 0);
    errores += probar("YAML vacio", "", 0);

    if (errores != 0) {
        printf("Fallaron %d pruebas YAML\n", errores);
        return 1;
    }

    puts("Pruebas YAML: OK");
    return 0;
}
