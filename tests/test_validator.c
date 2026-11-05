
#include <stdio.h>
#include <string.h>
#include "validator.h"

static int prueba(const char *nombre, const char *yaml, int valido)
{
    char error[256];

    int resultado = validar_dependencias_yaml(
        yaml, strlen(yaml), error, sizeof error);

    if ((resultado == 0) != valido) {
        printf("Fallo: %s (%s)\n", nombre, error);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *correcto =
        "name: prueba\n"
        "stages:\n"
        "  - stage: preparar\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        steps:\n"
        "          - script: echo hola\n"
        "  - stage: probar\n"
        "    dependsOn: preparar\n"
        "    condition: succeeded()\n"
        "    jobs:\n"
        "      - job: unitarias\n"
        "        steps:\n"
        "          - script: echo test\n";

    const char *inexistente =
        "name: prueba\n"
        "stages:\n"
        "  - stage: compilar\n"
        "    dependsOn: otra\n"
        "    jobs:\n"
        "      - job: uno\n";

    const char *ciclo =
        "name: prueba\n"
        "stages:\n"
        "  - stage: a\n"
        "    dependsOn: b\n"
        "    jobs:\n"
        "      - job: uno\n"
        "  - stage: b\n"
        "    dependsOn: a\n"
        "    jobs:\n"
        "      - job: dos\n";

    const char *condicion =
        "name: prueba\n"
        "stages:\n"
        "  - stage: compilar\n"
        "    condition: cualquier()\n"
        "    jobs:\n"
        "      - job: uno\n";

    const char *trabajo_inexistente =
        "name: prueba\n"
        "stages:\n"
        "  - stage: compilar\n"
        "    jobs:\n"
        "      - job: probar\n"
        "        dependsOn: otro\n";

    const char *trabajos_ciclicos =
        "name: prueba\n"
        "stages:\n"
        "  - stage: compilar\n"
        "    jobs:\n"
        "      - job: a\n"
        "        dependsOn: b\n"
        "      - job: b\n"
        "        dependsOn: a\n";

    const char *independientes =
        "name: prueba\n"
        "stages:\n"
        "  - stage: compilar\n"
        "    jobs:\n"
        "      - job: a\n"
        "      - job: b\n"
        "        dependsOn: []\n"
        "        condition: always()\n";

    int errores = 0;

    errores += prueba("Dependencias correctas",
                      correcto, 1);
    errores += prueba("Etapa inexistente",
                      inexistente, 0);
    errores += prueba("Ciclo entre etapas",
                      ciclo, 0);
    errores += prueba("Condicion invalida",
                      condicion, 0);
    errores += prueba("Trabajo inexistente",
                      trabajo_inexistente, 0);
    errores += prueba("Ciclo entre trabajos",
                      trabajos_ciclicos, 0);
    errores += prueba("Trabajos independientes",
                      independientes, 1);

    if (errores) {
        printf("Fallaron %d pruebas de validacion\n", errores);
        return 1;
    }

    puts("Pruebas de validacion: OK");
    return 0;
}
