
#include <stdio.h>
#include <string.h>

#include "validator.h"

static int probar(const char *nombre, const char *yaml,
                  int esperado)
{
    char error[256];

    int resultado = validar_dependencias_yaml(
        yaml, strlen(yaml), error, sizeof error);

    int correcto = (resultado == 0);

    if (correcto != esperado) {
        printf("Error en %s: %s\n", nombre, error);
        return 1;
    }

    return 0;
}

int main(void)
{
    int errores = 0;

    const char *valido =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        steps:\n"
        "          - script: echo compilar\n"
        "      - job: probar\n"
        "        dependsOn: compilar\n"
        "        condition: succeeded()\n"
        "        steps:\n"
        "          - script: echo probar\n"
        "  - stage: publicar\n"
        "    condition: always()\n"
        "    jobs:\n"
        "      - job: guardar\n"
        "        steps:\n"
        "          - script: echo guardar\n";

    const char *etapa_inexistente =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    dependsOn: inexistente\n"
        "    jobs:\n"
        "      - job: compilar\n";

    const char *trabajo_inexistente =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: compilar\n"
        "        dependsOn: otro\n";

    const char *ciclo_etapas =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: a\n"
        "    dependsOn: b\n"
        "    jobs:\n"
        "      - job: uno\n"
        "  - stage: b\n"
        "    dependsOn: a\n"
        "    jobs:\n"
        "      - job: dos\n";

    const char *ciclo_trabajos =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: a\n"
        "        dependsOn: b\n"
        "      - job: b\n"
        "        dependsOn: a\n";

    const char *condicion_invalida =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    condition: incorrecta()\n"
        "    jobs:\n"
        "      - job: uno\n";

    const char *dependencia_repetida =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: a\n"
        "      - job: b\n"
        "        dependsOn: [a, a]\n";

    const char *dependencia_propia =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    jobs:\n"
        "      - job: a\n"
        "        dependsOn: a\n";

    const char *sin_dependencias =
        "name: ejemplo\n"
        "stages:\n"
        "  - stage: construir\n"
        "    dependsOn: []\n"
        "    jobs:\n"
        "      - job: a\n"
        "        dependsOn: []\n"
        "      - job: b\n"
        "        condition: failed()\n";

    errores += probar("Pipeline valido", valido, 1);
    errores += probar("Etapa inexistente",
                     etapa_inexistente, 0);
    errores += probar("Trabajo inexistente",
                     trabajo_inexistente, 0);
    errores += probar("Ciclo entre etapas",
                     ciclo_etapas, 0);
    errores += probar("Ciclo entre trabajos",
                     ciclo_trabajos, 0);
    errores += probar("Condicion invalida",
                     condicion_invalida, 0);
    errores += probar("Dependencia repetida",
                     dependencia_repetida, 0);
    errores += probar("Dependencia propia",
                     dependencia_propia, 0);
    errores += probar("Dependencias vacias",
                     sin_dependencias, 1);

    if (errores != 0) {
        printf("Fallaron %d pruebas\n", errores);
        return 1;
    }

    puts("Pruebas de validacion: OK");
    return 0;
}

