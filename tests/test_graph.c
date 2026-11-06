
#include <stdio.h>
#include "graph.h"

static int comprobar(const char *titulo, const nodo_grafo_t *nodos,
                     size_t cantidad, int valido)
{
    char error[128];

    int resultado = validar_grafo(nodos, cantidad,
                                  error, sizeof error);

    if ((resultado == 0) != valido) {
        printf("Error en %s: %s\n", titulo, error);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *dep_build[] = {"preparar"};
    const char *dep_test[] = {"compilar"};

    const nodo_grafo_t correcto[] = {
        {"preparar", NULL, 0},
        {"compilar", dep_build, 1},
        {"probar", dep_test, 1}
    };

    const nodo_grafo_t duplicado[] = {
        {"compilar", NULL, 0},
        {"compilar", NULL, 0}
    };

    const char *no_existe[] = {"inexistente"};

    const nodo_grafo_t faltante[] = {
        {"compilar", no_existe, 1}
    };

    const char *dep_a[] = {"b"};
    const char *dep_b[] = {"a"};

    const nodo_grafo_t ciclico[] = {
        {"a", dep_a, 1},
        {"b", dep_b, 1}
    };

    const char *dep_si_mismo[] = {"a"};

    const nodo_grafo_t propio[] = {
        {"a", dep_si_mismo, 1}
    };

    int errores = 0;

    errores += comprobar("grafo valido", correcto, 3, 1);
    errores += comprobar("nombre duplicado", duplicado, 2, 0);
    errores += comprobar("dependencia inexistente", faltante, 1, 0);
    errores += comprobar("ciclo", ciclico, 2, 0);
    errores += comprobar("ciclo propio", propio, 1, 0);
    errores += comprobar("grafo vacio", NULL, 0, 0);

    if (errores) {
        printf("Fallaron %d pruebas del grafo\n", errores);
        return 1;
    }

    puts("Pruebas del grafo: OK");
    return 0;
}
