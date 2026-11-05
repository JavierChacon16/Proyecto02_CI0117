
#include "yaml_parser.h"
#include <yaml.h>
#include <stdio.h>
#include <string.h>

static int fallo(char *error, size_t cap, const char *mensaje)
{
    snprintf(error, cap, "%s", mensaje);
    return -1;
}

static yaml_node_t *obtener(yaml_document_t *doc, int id)
{
    return yaml_document_get_node(doc, id);
}

static const char *texto(yaml_node_t *n)
{
    if (!n || n->type != YAML_SCALAR_NODE)
        return NULL;
    return (const char *)n->data.scalar.value;
}

static yaml_node_t *campo(yaml_document_t *doc,
                          yaml_node_t *mapa, const char *nombre)
{
    if (!mapa || mapa->type != YAML_MAPPING_NODE)
        return NULL;

    for (yaml_node_pair_t *p = mapa->data.mapping.pairs.start;
         p < mapa->data.mapping.pairs.top; p++) {
        const char *clave = texto(obtener(doc, p->key));
        if (clave && strcmp(clave, nombre) == 0)
            return obtener(doc, p->value);
    }

    return NULL;
}

static int claves(yaml_document_t *doc, yaml_node_t *mapa,
                  const char *permitidas, char *error, size_t cap)
{
    if (!mapa || mapa->type != YAML_MAPPING_NODE)
        return fallo(error, cap, "Se esperaba un mapa YAML");

    for (yaml_node_pair_t *p = mapa->data.mapping.pairs.start;
         p < mapa->data.mapping.pairs.top; p++) {
        const char *clave = texto(obtener(doc, p->key));

        if (!clave)
            return fallo(error, cap, "Clave YAML invalida");

        char patron[100];
        snprintf(patron, sizeof patron, "|%s|", clave);

        if (!strstr(permitidas, patron)) {
            snprintf(error, cap, "Clave desconocida: %s", clave);
            return -1;
        }

        for (yaml_node_pair_t *q = mapa->data.mapping.pairs.start;
             q < p; q++) {
            const char *anterior = texto(obtener(doc, q->key));
            if (anterior && strcmp(anterior, clave) == 0) {
                snprintf(error, cap, "Clave duplicada: %s", clave);
                return -1;
            }
        }
    }

    return 0;
}

static int pasos(yaml_document_t *doc, yaml_node_t *lista,
                 char *error, size_t cap)
{
    if (!lista || lista->type != YAML_SEQUENCE_NODE ||
        lista->data.sequence.items.start == lista->data.sequence.items.top)
        return fallo(error, cap, "steps debe ser una lista no vacia");

    for (yaml_node_item_t *i = lista->data.sequence.items.start;
         i < lista->data.sequence.items.top; i++) {
        yaml_node_t *paso = obtener(doc, *i);

        if (claves(doc, paso,
                   "|script||task||inputs||publish||artifact|"
                   "|download||path||continueOnError|",
                   error, cap) != 0)
            return -1;

        int tipos = (campo(doc, paso, "script") != NULL) +
                    (campo(doc, paso, "task") != NULL) +
                    (campo(doc, paso, "publish") != NULL) +
                    (campo(doc, paso, "download") != NULL);

        if (tipos != 1)
            return fallo(error, cap,
                         "Cada paso debe tener un solo tipo");

        if (campo(doc, paso, "task") &&
            !campo(doc, paso, "inputs"))
            return fallo(error, cap, "task requiere inputs");

        if (campo(doc, paso, "publish") &&
            !campo(doc, paso, "artifact"))
            return fallo(error, cap, "publish requiere artifact");

        if (campo(doc, paso, "download") &&
            !campo(doc, paso, "path"))
            return fallo(error, cap, "download requiere path");
    }

    return 0;
}

static int trabajos(yaml_document_t *doc, yaml_node_t *lista,
                    char *error, size_t cap)
{
    if (!lista || lista->type != YAML_SEQUENCE_NODE ||
        lista->data.sequence.items.start == lista->data.sequence.items.top)
        return fallo(error, cap, "jobs debe ser una lista no vacia");

    for (yaml_node_item_t *i = lista->data.sequence.items.start;
         i < lista->data.sequence.items.top; i++) {
        yaml_node_t *job = obtener(doc, *i);

        if (claves(doc, job,
                   "|job||dependsOn||condition||timeoutInSeconds||steps|",
                   error, cap) != 0)
            return -1;

        const char *nombre = texto(campo(doc, job, "job"));

        if (!nombre || !*nombre)
            return fallo(error, cap, "Falta el nombre job");

        if (pasos(doc, campo(doc, job, "steps"),
                  error, cap) != 0)
            return -1;

        for (yaml_node_item_t *j = lista->data.sequence.items.start;
             j < i; j++) {
            const char *anterior =
                texto(campo(doc, obtener(doc, *j), "job"));

            if (anterior && strcmp(nombre, anterior) == 0)
                return fallo(error, cap, "Nombre de trabajo duplicado");
        }
    }

    return 0;
}

static int etapas(yaml_document_t *doc, yaml_node_t *lista,
                  char *error, size_t cap)
{
    if (!lista || lista->type != YAML_SEQUENCE_NODE ||
        lista->data.sequence.items.start == lista->data.sequence.items.top)
        return fallo(error, cap, "stages debe ser una lista no vacia");

    for (yaml_node_item_t *i = lista->data.sequence.items.start;
         i < lista->data.sequence.items.top; i++) {
        yaml_node_t *stage = obtener(doc, *i);

        if (claves(doc, stage,
                   "|stage||dependsOn||condition||jobs|",
                   error, cap) != 0)
            return -1;

        const char *nombre = texto(campo(doc, stage, "stage"));

        if (!nombre || !*nombre)
            return fallo(error, cap, "Falta el nombre stage");

        if (trabajos(doc, campo(doc, stage, "jobs"),
                     error, cap) != 0)
            return -1;

        for (yaml_node_item_t *j = lista->data.sequence.items.start;
             j < i; j++) {
            const char *anterior =
                texto(campo(doc, obtener(doc, *j), "stage"));

            if (anterior && strcmp(nombre, anterior) == 0)
                return fallo(error, cap, "Nombre de etapa duplicado");
        }
    }

    return 0;
}

int validar_yaml(const char *contenido, size_t longitud,
                 char *error, size_t capacidad)
{
    if (!contenido || !error || capacidad == 0)
        return -1;

    error[0] = '\0';

    yaml_parser_t parser;
    yaml_document_t documento;

    if (!yaml_parser_initialize(&parser))
        return fallo(error, capacidad, "Error al iniciar libyaml");

    yaml_parser_set_input_string(
        &parser, (const unsigned char *)contenido, longitud);

    if (!yaml_parser_load(&parser, &documento)) {
        snprintf(error, capacidad, "YAML invalido en linea %zu",
                 parser.problem_mark.line + 1);
        yaml_parser_delete(&parser);
        return -1;
    }

    yaml_node_t *raiz = yaml_document_get_root_node(&documento);

    int resultado = 0;

    if (claves(&documento, raiz, "|name||variables||stages|",
               error, capacidad) != 0) {
        resultado = -1;
    } else {
        const char *nombre =
            texto(campo(&documento, raiz, "name"));

        if (!nombre || !*nombre)
            resultado = fallo(error, capacidad,
                              "Falta el nombre del pipeline");
        else
            resultado = etapas(&documento,
                               campo(&documento, raiz, "stages"),
                               error, capacidad);
    }

    yaml_document_delete(&documento);
    yaml_parser_delete(&parser);

    return resultado;
}
