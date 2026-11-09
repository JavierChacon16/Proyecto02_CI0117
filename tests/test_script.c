
#include <stdio.h>
#include "script.h"

int main(void)
{
    int errores = 0;
    int salida = -1;

    if (ejecutar_script("true", 2, &salida) != 0 ||
        salida != 0) {
        puts("Error: comando correcto");
        errores++;
    }

    if (ejecutar_script("exit 7", 2, &salida) != -1 ||
        salida != 7) {
        puts("Error: codigo de salida");
        errores++;
    }

    if (ejecutar_script("sleep 2", 1, &salida) != -2) {
        puts("Error: tiempo limite");
        errores++;
    }

    if (ejecutar_script("", 1, &salida) != -1) {
        puts("Error: comando vacio");
        errores++;
    }

    if (errores != 0) {
        printf("Fallaron %d pruebas de script\n", errores);
        return 1;
    }

    puts("Pruebas de script: OK");
    return 0;
}

