
#include "validator.h"
#include "graph.h"

#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yaml_node_t *nodo(yaml_document_t *doc, int id)
{
    return yaml_document_get_node(doc, id);
}

static const char *cadena(yaml_node_t *n)
{
    if (!n || n->type != YAML_SCALAR_NODE)
        return NULL;

    return (const char *)n->data.scalar.value;
}

static yaml_node_t *campo(yaml_document_t *doc, yaml_node_t *mapa,
                          const char *nombre)
{
    if (!mapa || mapa->type != YAML_MAPPING_NODE)
        return NULL;

    for (yaml_node_pair_t *p = mapa->data.mapping.pairs.start;
         p < mapa->data.mapping.pairs.top; p++) {
        const char *clave = cadena(nodo(doc, p->key));

        if (clave && strcmp(clave, nombre) == 0)
            return nodo(doc, p->value);
    }

    return NULL;
}

static int error_simple(char *error, size_t cap, const char *mensaje)
{
    snprintf(error, cap, "%s", mensaje);
    return -1;
}

static int condicion_valida(yaml_document_t *doc, yaml_node_t *mapa,
                            char *error, size_t cap)
{
    yaml_node_t *valor = campo(doc, mapa, "condition");

    if (!valor)
        return 0;

    const char *c = cadena(valor);

    if (c && (strcmp(c, "succeeded()") == 0 ||
              strcmp(c, "failed()") == 0 ||
              strcmp(c, "always()") == 0))
        return 0;

    return error_simple(error, cap, "Condicion de ejecucion invalida");
}

static int buscar(const char *const *nombres, size_t cantidad,
                  const char *nombre)
{
    for (size_t i = 0; i < cantidad; i++) {
        if (strcmp(nombres[i], nombre) == 0)
            return (int)i;
    }

    return -1;
}

static void liberar_dependencias(nodo_grafo_t *grafo, size_t cantidad)
{
    for (size_t i = 0; i < cantidad; i++)
        free((void *)grafo[i].depende);

    free(grafo);
}

static int validar_lista(yaml_document_t *doc, yaml_node_t *lista,
                         const char *clave, int es_etapa,
                         char *error, size_t cap)
{
    if (!lista || lista->type != YAML_SEQUENCE_NODE)
        return error_simple(error, cap, "Se esperaba una lista");

    size_t cantidad = (size_t)(
        lista->data.sequence.items.top -
        lista->data.sequence.items.start
    );

    if (cantidad == 0)
        return error_simple(error, cap, "La lista esta vacia");

    const char **nombres = calloc(cantidad, sizeof *nombres);
    nodo_grafo_t *grafo = calloc(cantidad, sizeof *grafo);

    if (!nombres || !grafo) {
        free(nombres);
        free(grafo);
        return error_simple(error, cap, "Sin memoria");
    }

    int resultado = -1;

    /* Obtener los nombres de todas las etapas o trabajos. */
    for (size_t i = 0; i < cantidad; i++) {
        yaml_node_t *elemento = nodo(
            doc, lista->data.sequence.items.start[i]);

        nombres[i] = cadena(campo(doc, elemento, clave));

        if (!nombres[i] || !nombres[i][0]) {
            snprintf(error, cap, "Falta el nombre %s", clave);
            goto terminar;
        }

        if (condicion_valida(doc, elemento, error, cap) != 0)
            goto terminar;

        for (size_t j = 0; j < i; j++) {
            if (strcmp(nombres[i], nombres[j]) == 0) {
                snprintf(error, cap, "Nombre duplicado: %s",
                         nombres[i]);
                goto terminar;
            }
        }

        grafo[i].nombre = nombres[i];
    }

    /* Leer y verificar las dependencias. */
    for (size_t i = 0; i < cantidad; i++) {
        yaml_node_t *elemento = nodo(
            doc, lista->data.sequence.items.start[i]);

        yaml_node_t *deps = campo(doc, elemento, "dependsOn");
        size_t total = 0;

        if (!deps) {
            /* Solo las etapas dependen de la anterior por defecto. */
            if (es_etapa && i > 0)
                total = 1;
        } else if (deps->type == YAML_SCALAR_NODE) {
            total = 1;
        } else if (deps->type == YAML_SEQUENCE_NODE) {
            total = (size_t)(deps->data.sequence.items.top -
                             deps->data.sequence.items.start);
        } else {
            error_simple(error, cap, "dependsOn invalido");
            goto terminar;
        }

        const char **referencias = NULL;

        if (total) {
            referencias = calloc(total, sizeof *referencias);

            if (!referencias) {
                error_simple(error, cap, "Sin memoria");
                goto terminar;
            }
        }

        grafo[i].depende = referencias;
        grafo[i].cantidad_dependencias = total;

        for (size_t j = 0; j < total; j++) {
            const char *nombre = NULL;

            if (!deps) {
                nombre = nombres[i - 1];
            } else if (deps->type == YAML_SCALAR_NODE) {
                nombre = cadena(deps);
            } else {
                nombre = cadena(nodo(
                    doc, deps->data.sequence.items.start[j]));
            }

            if (!nombre || !nombre[0] ||
                buscar(nombres, cantidad, nombre) < 0) {
                snprintf(error, cap,
                         "Dependencia inexistente en %s",
                         nombres[i]);
                goto terminar;
            }

            for (size_t k = 0; k < j; k++) {
                if (strcmp(referencias[k], nombre) == 0) {
                    snprintf(error, cap,
                             "Dependencia repetida en %s",
                             nombres[i]);
                    goto terminar;
                }
            }

            referencias[j] = nombre;
        }

        /* Revisar tambien los trabajos de cada etapa. */
        if (es_etapa) {
            yaml_node_t *jobs = campo(doc, elemento, "jobs");

            if (validar_lista(doc, jobs, "job", 0,
                              error, cap) != 0)
                goto terminar;
        }
    }

    resultado = validar_grafo(grafo, cantidad, error, cap);

terminar:
    liberar_dependencias(grafo, cantidad);
    free(nombres);
    return resultado;
}

int validar_dependencias_yaml(const char *contenido, size_t longitud,
                              char *error, size_t capacidad)
{
    if (!contenido || !error || capacidad == 0)
        return -1;

    error[0] = '\0';

    yaml_parser_t parser;
    yaml_document_t doc;

    if (!yaml_parser_initialize(&parser))
        return error_simple(error, capacidad,
                            "No se pudo iniciar libyaml");

    yaml_parser_set_input_string(
        &parser, (const unsigned char *)contenido, longitud);

    if (!yaml_parser_load(&parser, &doc)) {
        snprintf(error, capacidad, "YAML invalido en linea %zu",
                 parser.problem_mark.line + 1);
        yaml_parser_delete(&parser);
        return -1;
    }

    yaml_node_t *raiz = yaml_document_get_root_node(&doc);

    if (!raiz || raiz->type != YAML_MAPPING_NODE) {
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        return error_simple(error, capacidad,
                            "El pipeline debe ser un mapa");
    }

    int resultado = validar_lista(
        &doc, campo(&doc, raiz, "stages"),
        "stage", 1, error, capacidad);

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);

    return resultado;
}
