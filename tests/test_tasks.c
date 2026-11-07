
#include "tasks.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    int errores = 0;
    uint64_t serial = 0;
    uint64_t paralelo = 0;

    if (tarea_sleep(0.01) != 0 ||
        tarea_sleep(-1.0) == 0) {
        puts("Error Sleep");
        errores++;
    }

    if (tarea_cpu(100000, 27, 1, &serial) != 0 ||
        tarea_cpu(100000, 27, 4, &paralelo) != 0 ||
        serial != paralelo) {
        puts("Error Cpu: resultado distinto entre hilos");
        errores++;
    }

    if (tarea_cpu(100, 27, 0, &paralelo) == 0) {
        puts("Error Cpu: hilos invalidos");
        errores++;
    }

    if (errores)
        return 1;

    puts("Pruebas Sleep y Cpu: OK");
    return 0;
}
