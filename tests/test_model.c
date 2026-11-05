
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int prueba_orden(void)
{
    paso_t a[] = {{.tipo = PASO_SLEEP, .segundos = 0.02}};
    paso_t b[] = {{.tipo = PASO_CPU, .iteraciones = 2000,
                   .semilla = 18, .hilos = 1}};
    paso_t c[] = {{.tipo = PASO_SCRIPT, .comando = "true"}};
    size_t deps[] = {0, 1};
    trabajo_t trabajos[] = {
        {.nombre="A", .pasos=a, .cantidad_pasos=1},
        {.nombre="B", .pasos=b, .cantidad_pasos=1},
        {.nombre="C", .pasos=c, .cantidad_pasos=1,
         .dependencias=deps, .cantidad_dependencias=2}
    };
    plan_trabajos_t plan = {trabajos, 3};
    if (ejecutar_plan(&plan, 2) != 0)
        return 1;
    for (size_t i = 0; i < 3; ++i)
        if (trabajos[i].estado != TRABAJO_CORRECTO)
            return 1;
    return 0;
}

static int prueba_fallo_y_condiciones(void)
{
    paso_t error[] = {{.tipo=PASO_SCRIPT, .comando="exit 7"}};
    paso_t correcto[] = {{.tipo=PASO_SCRIPT, .comando="true"}};
    size_t deps[] = {0};
    trabajo_t trabajos[] = {
        {.nombre="fallo", .pasos=error, .cantidad_pasos=1},
        {.nombre="omitido", .pasos=correcto, .cantidad_pasos=1,
         .dependencias=deps, .cantidad_dependencias=1},
        {.nombre="si_falla", .pasos=correcto, .cantidad_pasos=1,
         .dependencias=deps, .cantidad_dependencias=1,
         .condicion=COND_FAILED},
        {.nombre="siempre", .pasos=correcto, .cantidad_pasos=1,
         .dependencias=deps, .cantidad_dependencias=1,
         .condicion=COND_ALWAYS}
    };
    plan_trabajos_t plan = {trabajos, 4};
    if (ejecutar_plan(&plan, 3) == 0)
        return 1;
    return trabajos[0].estado != TRABAJO_FALLIDO ||
           trabajos[1].estado != TRABAJO_OMITIDO ||
           trabajos[2].estado != TRABAJO_CORRECTO ||
           trabajos[3].estado != TRABAJO_CORRECTO;
}

static int prueba_ciclo(void)
{
    paso_t paso[] = {{.tipo=PASO_SCRIPT, .comando="true"}};
    size_t da[] = {1}, db[] = {0};
    trabajo_t trabajos[] = {
        {.nombre="A", .pasos=paso, .cantidad_pasos=1,
         .dependencias=da, .cantidad_dependencias=1},
        {.nombre="B", .pasos=paso, .cantidad_pasos=1,
         .dependencias=db, .cantidad_dependencias=1}
    };
    plan_trabajos_t plan = {trabajos, 2};
    return ejecutar_plan(&plan, 2) == 0;
}

static int prueba_continuar(void)
{
    paso_t pasos[] = {
        {.tipo=PASO_SCRIPT, .comando="exit 1", .continuar_si_falla=1},
        {.tipo=PASO_SCRIPT, .comando="true"}
    };
    trabajo_t trabajos[] = {{.nombre="A", .pasos=pasos, .cantidad_pasos=2}};
    plan_trabajos_t plan = {trabajos, 1};
    return ejecutar_plan(&plan, 1) == 0 ||
           trabajos[0].estado != TRABAJO_PARCIAL;
}

static int prueba_log(void)
{
    char ruta[] = "/tmp/p2_model_log_XXXXXX";
    int fd = mkstemp(ruta);
    if (fd < 0) return 1;
    close(fd);
    paso_t pasos[] = {{.tipo=PASO_SCRIPT,
                        .comando="echo LOG_DE_PRUEBA",
                        .log_ruta=ruta}};
    trabajo_t trabajo[] = {{.nombre="log", .pasos=pasos,
                            .cantidad_pasos=1}};
    plan_trabajos_t plan = {trabajo, 1};
    int fallo = ejecutar_plan(&plan, 1) != 0;
    FILE *f = fopen(ruta, "r");
    char texto[128] = {0};
    if (!f || !fgets(texto, sizeof texto, f) ||
        !strstr(texto, "LOG_DE_PRUEBA")) fallo = 1;
    if (f) fclose(f);
    unlink(ruta);
    return fallo;
}

int main(void)
{
    int errores = 0;
    errores += prueba_orden();
    errores += prueba_fallo_y_condiciones();
    errores += prueba_ciclo();
    errores += prueba_continuar();
    errores += prueba_log();
    if (errores) {
        printf("Fallaron %d pruebas del motor\n", errores);
        return 1;
    }
    puts("Pruebas del motor OpenMP: OK");
    return 0;
}

