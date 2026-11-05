
#include "model.h"

#include <stdio.h>

int main(void)
{
    paso_t a[] = {
        {
            .tipo = PASO_SLEEP,
            .segundos = 0.10
        }
    };

    paso_t b[] = {
        {
            .tipo = PASO_CPU,
            .iteraciones = 1000,
            .semilla = 12,
            .hilos = 1
        }
    };

    paso_t c[] = {
        {
            .tipo = PASO_SCRIPT,
            .comando = "true"
        }
    };

    size_t dependencias[] = {0, 1};

    trabajo_t trabajos[] = {
        {
            .nombre = "A",
            .pasos = a,
            .cantidad_pasos = 1
        },
        {
            .nombre = "B",
            .pasos = b,
            .cantidad_pasos = 1
        },
        {
            .nombre = "C",
            .pasos = c,
            .cantidad_pasos = 1,
            .dependencias = dependencias,
            .cantidad_dependencias = 2
        }
    };

    plan_trabajos_t plan = {
        .trabajos = trabajos,
        .cantidad = 3
    };

    if (ejecutar_plan(&plan, 2) != 0)
        return 1;

    for (size_t i = 0; i < 3; i++) {
        if (trabajos[i].estado != TRABAJO_CORRECTO)
            return 1;
    }

    paso_t error[] = {
        {
            .tipo = PASO_SCRIPT,
            .comando = "exit 7"
        }
    };

    paso_t omitido[] = {
        {
            .tipo = PASO_SCRIPT,
            .comando = "true"
        }
    };

    size_t dependencia[] = {0};

    trabajo_t trabajos_fallidos[] = {
        {
            .nombre = "error",
            .pasos = error,
            .cantidad_pasos = 1
        },
        {
            .nombre = "dependiente",
            .pasos = omitido,
            .cantidad_pasos = 1,
            .dependencias = dependencia,
            .cantidad_dependencias = 1
        }
    };

    plan = (plan_trabajos_t){
        .trabajos = trabajos_fallidos,
        .cantidad = 2
    };

    if (ejecutar_plan(&plan, 2) == 0 ||
        trabajos_fallidos[0].estado != TRABAJO_FALLIDO ||
        trabajos_fallidos[1].estado != TRABAJO_OMITIDO)
        return 1;

    puts("Pruebas del motor OpenMP: OK");
    return 0;
}

