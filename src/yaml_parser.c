#define _POSIX_C_SOURCE 200809L
#include "yaml_parser.h"
#include "graph.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Analizador limitado al subconjunto indicado en el proyecto.
 * No interpreta YAML completo, tags, referencias, merge keys ni anclas.
 * Ninguna estructura mutable se comparte entre llamadas concurrentes. */
#define LIMITE_DOCUMENTO (1024U * 1024U)
#define LIMITE_NODOS 10000U
#define RUTA_CAP 512U

typedef enum { N_ESCALAR, N_MAPA, N_LISTA } clase_nodo_t;
typedef struct nodo nodo_t;
typedef struct {
    char *clave;
    nodo_t *valor;
    size_t linea, columna;
} par_t;
struct nodo {
    clase_nodo_t clase;
    char *valor;
    par_t *pares;
    size_t cantidad_pares;
    nodo_t **elementos;
    size_t cantidad_elementos;
    size_t linea, columna;
};
typedef struct {
    char *inicio;
    size_t indentacion, numero;
    int util;
} linea_t;
typedef struct {
    linea_t *lineas;
    size_t cantidad_lineas, posicion;
    char *buffer;
    errores_yaml_t *errores;
    int memoria_agotada;
    size_t nodos_creados;
} lector_t;

static char *duplicar_rango(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (r) { memcpy(r, s, n); r[n] = '\0'; }
    return r;
}
static char *duplicar(const char *s)
{
    return s ? duplicar_rango(s, strlen(s)) : NULL;
}
static const char *saltar_espacios(const char *s)
{
    while (*s == ' ') ++s;
    return s;
}
static size_t longitud_sin_espacios(const char *s)
{
    size_t n = strlen(s);
    while (n && s[n-1] == ' ') --n;
    return n;
}
static void copiar_limite(char *dst, size_t cap, const char *s)
{
    if (!cap) return;
    size_t n = strlen(s);
    if (n >= cap) n = cap-1;
    memcpy(dst,s,n); dst[n]='\0';
}
static void concatenar_limite(char *dst, size_t cap, const char *s)
{
    size_t usado = strlen(dst);
    if (usado < cap) copiar_limite(dst+usado,cap-usado,s);
}
static void ruta_hija(char *dst, size_t cap, const char *raiz, const char *clave)
{
    copiar_limite(dst,cap,raiz ? raiz : "");
    if (raiz && *raiz) concatenar_limite(dst,cap,".");
    concatenar_limite(dst,cap,clave);
}
static void ruta_indice(char *dst, size_t cap, const char *raiz, size_t i)
{
    char indice[40];
    snprintf(indice,sizeof indice,"[%zu]",i);
    copiar_limite(dst,cap,raiz);
    concatenar_limite(dst,cap,indice);
}
static void informar(lector_t *l, size_t linea, size_t col,
                     const char *ruta, const char *mensaje)
{
    if (!l->errores || l->memoria_agotada) return;
    if (l->errores->cantidad == YAML_MAX_ERRORES) {
        l->errores->truncados = true;
        return;
    }
    error_yaml_t *mas = realloc(l->errores->items,
        (l->errores->cantidad + 1) * sizeof *mas);
    if (!mas) { l->memoria_agotada = 1; return; }
    l->errores->items = mas;
    error_yaml_t *e = &mas[l->errores->cantidad];
    memset(e, 0, sizeof *e);
    e->linea = linea;
    e->columna = col;
    char seguro[RUTA_CAP];
    size_t p = 0;
    const char *base = ruta && *ruta ? ruta : "yaml";
    for (size_t i = 0; base[i] && p + 1 < sizeof seguro; ++i) {
        unsigned char c = (unsigned char)base[i];
        seguro[p++] = (char)(isalnum(c) || c == '_' || c == '.' ||
                            c == '[' || c == ']' || c == '-' ? c : '_');
    }
    seguro[p] = '\0';
    char ubicacion[RUTA_CAP + 64];
    snprintf(ubicacion, sizeof ubicacion, "L%zu:C%zu:%s", linea, col, seguro);
    e->ruta = duplicar(seguro);
    e->ubicacion = duplicar(ubicacion);
    e->mensaje = duplicar(mensaje);
    if (!e->ruta || !e->ubicacion || !e->mensaje) {
        free(e->ruta); free(e->ubicacion); free(e->mensaje);
        memset(e, 0, sizeof *e);
        l->memoria_agotada = 1;
        return;
    }
    ++l->errores->cantidad;
}
void liberar_errores_yaml(errores_yaml_t *errores)
{
    if (!errores) return;
    for (size_t i = 0; i < errores->cantidad; ++i) {
        free(errores->items[i].ruta);
        free(errores->items[i].ubicacion);
        free(errores->items[i].mensaje);
    }
    free(errores->items);
    memset(errores, 0, sizeof *errores);
}
static nodo_t *nuevo_nodo(lector_t *l, clase_nodo_t clase, size_t linea, size_t columna)
{
    if (++l->nodos_creados > LIMITE_NODOS) {
        informar(l, linea, columna, "yaml", "Demasiados nodos YAML");
        return NULL;
    }
    nodo_t *n = calloc(1, sizeof *n);
    if (!n) l->memoria_agotada = 1;
    else { n->clase = clase; n->linea = linea; n->columna = columna; }
    return n;
}
static void liberar_nodo(nodo_t *n)
{
    if (!n) return;
    free(n->valor);
    for (size_t i = 0; i < n->cantidad_pares; ++i) {
        free(n->pares[i].clave);
        liberar_nodo(n->pares[i].valor);
    }
    free(n->pares);
    for (size_t i = 0; i < n->cantidad_elementos; ++i)
        liberar_nodo(n->elementos[i]);
    free(n->elementos);
    free(n);
}
static int agregar_par(lector_t *l, nodo_t *n, char *clave, nodo_t *valor,
                       size_t linea, size_t columna)
{
    par_t *p = realloc(n->pares, (n->cantidad_pares + 1) * sizeof *p);
    if (!p) { l->memoria_agotada = 1; return -1; }
    n->pares = p;
    p[n->cantidad_pares++] = (par_t){ clave, valor, linea, columna };
    return 0;
}
static int agregar_elemento(lector_t *l, nodo_t *n, nodo_t *hijo)
{
    nodo_t **p = realloc(n->elementos,
                         (n->cantidad_elementos + 1) * sizeof *p);
    if (!p) { l->memoria_agotada = 1; return -1; }
    n->elementos = p;
    p[n->cantidad_elementos++] = hijo;
    return 0;
}
static size_t proxima_util(const lector_t *l, size_t pos)
{
    while (pos < l->cantidad_lineas && !l->lineas[pos].util) ++pos;
    return pos;
}
/* Encontrar ':' estructural. Un ':' en "echo x:y" es texto normal. */
static const char *dos_puntos(const char *s)
{
    int comillas = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '\'' && comillas != '"') comillas = comillas == '\'' ? 0 : '\'';
        else if (*p == '"' && comillas != '\'') comillas = comillas == '"' ? 0 : '"';
        else if (*p == ':' && !comillas && (!p[1] || p[1] == ' ')) return p;
    }
    return NULL;
}
/* Quota y comentarios basicos. No se admiten cadenas multilínea entre comillas. */
static char *valor_escalar(lector_t *l, const char *s, size_t linea, size_t col)
{
    s = saltar_espacios(s);
    size_t n = strlen(s);
    int comillas = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\'' && comillas != '"') {
            if (comillas == '\'' && i+1 < n && s[i+1] == '\'') { ++i; continue; }
            comillas = comillas == '\'' ? 0 : '\'';
        } else if (s[i] == '"' && comillas != '\'') {
            if (i && s[i-1] == '\\') continue;
            comillas = comillas == '"' ? 0 : '"';
        } else if (s[i] == '#' && !comillas &&
                   (i == 0 || s[i-1] == ' ')) { n = i; break; }
    }
    while (n && s[n-1] == ' ') --n;
    if (!n) return duplicar("");
    if (s[0] == '&' || s[0] == '*' || (n >= 2 && s[0] == '<' && s[1] == '<'))
        informar(l, linea, col, "yaml", "Anclas, alias y merges no permitidos");
    if (s[0] == '\'' || s[0] == '"') {
        char q = s[0];
        if (n < 2 || s[n-1] != q) {
            informar(l, linea, col, "yaml", "Comillas sin cerrar");
            return duplicar_rango(s, n);
        }
        char *r = malloc(n);
        if (!r) { l->memoria_agotada = 1; return NULL; }
        size_t j = 0;
        for (size_t i = 1; i + 1 < n; ++i) {
            if (q == '\'' && s[i] == '\'' && i+2 < n && s[i+1] == '\'') {
                r[j++] = '\''; ++i;
            } else if (q == '"' && s[i] == '\\' && i+2 < n) {
                ++i;
                r[j++] = s[i] == 'n' ? '\n' : (s[i] == 't' ? '\t' : s[i]);
            } else r[j++] = s[i];
        }
        r[j] = '\0';
        return r;
    }
    return duplicar_rango(s, n);
}
static nodo_t *interpretar_valor(lector_t *l, const char *s,
                                size_t linea, size_t col, size_t nivel);
static nodo_t *leer_bloque(lector_t *l, size_t indentacion, size_t nivel);

static nodo_t *leer_literal(lector_t *l, size_t key_indent, size_t linea, size_t col)
{
    nodo_t *n = nuevo_nodo(l, N_ESCALAR, linea, col);
    if (!n) return NULL;
    size_t j = l->posicion, minimo = SIZE_MAX;
    while (j < l->cantidad_lineas) {
        linea_t *ln = &l->lineas[j];
        if (ln->util && ln->indentacion <= key_indent) break;
        if (ln->util && ln->indentacion < minimo) minimo = ln->indentacion;
        ++j;
    }
    if (minimo == SIZE_MAX) minimo = key_indent+1;
    size_t total = 1;
    for (size_t i = l->posicion; i < j; ++i) {
        size_t len = strlen(l->lineas[i].inicio);
        if (len > minimo) total += len - minimo;
        ++total;
    }
    n->valor = malloc(total);
    if (!n->valor) { l->memoria_agotada = 1; liberar_nodo(n); return NULL; }
    size_t pos = 0;
    for (size_t i = l->posicion; i < j; ++i) {
        linea_t *ln = &l->lineas[i];
        const char *linea_raw = ln->inicio;
        size_t largo = strlen(linea_raw);
        size_t dedent = ln->util ? minimo : (largo < minimo ? largo : minimo);
        if (largo > dedent) {
            memcpy(n->valor + pos, linea_raw + dedent, largo-dedent);
            pos += largo-dedent;
        }
        n->valor[pos++] = '\n';
    }
    n->valor[pos] = '\0';
    l->posicion = j;
    return n;
}
static nodo_t *leer_lista_flujo(lector_t *l, const char *s,
                               size_t linea, size_t col, size_t nivel)
{
    size_t n = strlen(s);
    nodo_t *lista = nuevo_nodo(l, N_LISTA, linea, col);
    if (!lista) return NULL;
    if (n < 2 || s[n-1] != ']') {
        informar(l, linea, col, "yaml", "Lista de flujo sin cerrar");
        return lista;
    }
    size_t start = 1; int q = 0;
    for (size_t i = 1; i < n; ++i) {
        if (s[i] == '\'' && q != '"') q = q == '\'' ? 0 : '\'';
        else if (s[i] == '"' && q != '\'' && (i == 0 || s[i-1] != '\\'))
            q = q == '"' ? 0 : '"';
        if ((s[i] == ',' && !q) || i == n-1) {
            char *item = duplicar_rango(s + start, i-start);
            if (!item) { l->memoria_agotada = 1; break; }
            const char *t = saltar_espacios(item);
            if (*t || i != 1) {
                nodo_t *v = interpretar_valor(l, t, linea, col + start, nivel+1);
                if (v && agregar_elemento(l, lista, v) != 0) liberar_nodo(v);
                if (!v || l->memoria_agotada) { free(item); break; }
            }
            free(item);
            start = i+1;
        }
    }
    return lista;
}
static nodo_t *interpretar_valor(lector_t *l, const char *s,
                                size_t linea, size_t col, size_t nivel)
{
    if (nivel > YAML_MAX_PROFUNDIDAD) {
        informar(l, linea, col, "yaml", "Profundidad YAML excede 32");
        return NULL;
    }
    nodo_t *n = NULL;
    char *texto = valor_escalar(l, s, linea, col);
    if (!texto) { l->memoria_agotada = 1; return NULL; }
    if (texto[0] == '[') {
        n = leer_lista_flujo(l, texto, linea, col, nivel);
        free(texto);
        return n;
    }
    n = nuevo_nodo(l, N_ESCALAR, linea, col);
    if (!n) { free(texto); return NULL; }
    n->valor = texto;
    return n;
}

static void leer_entrada(lector_t *l, nodo_t *mapa, const char *texto,
                         size_t linea, size_t columna, size_t indentacion, size_t nivel)
{
    const char *separador = dos_puntos(texto);
    if (!separador) {
        informar(l, linea, columna, "yaml", "Se esperaba clave: valor");
        return;
    }
    char *clave = duplicar_rango(texto, (size_t)(separador-texto));
    if (!clave) { l->memoria_agotada = 1; return; }
    size_t longitud = longitud_sin_espacios(clave);
    clave[longitud] = '\0';
    if (!longitud || strchr(clave, ' ') || strchr(clave, '\t'))
        informar(l, linea, columna, "yaml", "Clave YAML invalida");
    if (strcmp(clave, "<<") == 0)
        informar(l, linea, columna, "yaml", "Clave merge YAML prohibida");
    const char *texto_valor = saltar_espacios(separador+1);
    nodo_t *valor = NULL;
    if (strcmp(texto_valor, "|") == 0)
        valor = leer_literal(l, indentacion, linea, columna);
    else if (!*texto_valor || *texto_valor == '#') {
        size_t i = proxima_util(l, l->posicion);
        if (i < l->cantidad_lineas && l->lineas[i].indentacion > indentacion) {
            l->posicion = i;
            valor = leer_bloque(l, l->lineas[i].indentacion, nivel+1);
        } else valor = interpretar_valor(l, "", linea, columna, nivel+1);
    } else valor = interpretar_valor(l, texto_valor, linea, columna, nivel+1);
    if (!valor || agregar_par(l, mapa, clave, valor, linea, columna) != 0) {
        free(clave); liberar_nodo(valor);
    }
}
static nodo_t *leer_bloque(lector_t *l, size_t indentacion, size_t nivel)
{
    l->posicion = proxima_util(l, l->posicion);
    if (l->posicion >= l->cantidad_lineas) return NULL;
    linea_t *primera = &l->lineas[l->posicion];
    if (nivel > YAML_MAX_PROFUNDIDAD) {
        informar(l, primera->numero, indentacion+1, "yaml", "Profundidad YAML excede 32");
        return NULL;
    }
    int es_lista = strncmp(primera->inicio + indentacion, "- ", 2) == 0 ||
                   strcmp(primera->inicio + indentacion, "-") == 0;
    nodo_t *raiz = nuevo_nodo(l, es_lista ? N_LISTA : N_MAPA,
                               primera->numero, indentacion+1);
    if (!raiz) return NULL;
    while ((l->posicion = proxima_util(l, l->posicion)) < l->cantidad_lineas) {
        linea_t *ln = &l->lineas[l->posicion];
        if (ln->indentacion < indentacion) break;
        if (ln->indentacion > indentacion) {
            informar(l, ln->numero, ln->indentacion+1, "yaml", "Indentacion inesperada");
            ++l->posicion; continue;
        }
        const char *contenido = ln->inicio + indentacion;
        int guion = strncmp(contenido, "- ", 2) == 0 || strcmp(contenido, "-") == 0;
        if (guion != es_lista) {
            informar(l, ln->numero, indentacion+1, "yaml", "Mapa y lista mezclados");
            ++l->posicion; continue;
        }
        size_t lineano = ln->numero;
        ++l->posicion;
        if (!es_lista) {
            leer_entrada(l, raiz, contenido, lineano, indentacion+1,
                          indentacion, nivel);
        } else {
            const char *parte = saltar_espacios(contenido+1);
            nodo_t *elemento = NULL;
            if (!*parte || *parte == '#') {
                size_t i = proxima_util(l, l->posicion);
                if (i < l->cantidad_lineas && l->lineas[i].indentacion > indentacion) {
                    l->posicion = i;
                    elemento = leer_bloque(l, l->lineas[i].indentacion, nivel+1);
                } else elemento = interpretar_valor(l, "", lineano, indentacion+1, nivel+1);
            } else if (dos_puntos(parte)) {
                elemento = nuevo_nodo(l, N_MAPA, lineano, indentacion+3);
                if (elemento) {
                    leer_entrada(l, elemento, parte, lineano, indentacion+3,
                                 indentacion+2, nivel+1);
                    size_t i = proxima_util(l, l->posicion);
                    while (i < l->cantidad_lineas &&
                           l->lineas[i].indentacion == indentacion+2) {
                        linea_t *campo = &l->lineas[i];
                        l->posicion = i+1;
                        leer_entrada(l, elemento, campo->inicio+indentacion+2,
                                     campo->numero, indentacion+3, indentacion+2,
                                     nivel+1);
                        i = proxima_util(l, l->posicion);
                    }
                }
            } else elemento = interpretar_valor(l, parte, lineano, indentacion+3, nivel+1);
            if (elemento && agregar_elemento(l, raiz, elemento) != 0)
                liberar_nodo(elemento);
        }
        if (l->memoria_agotada) break;
    }
    return raiz;
}
static int preparar_lineas(lector_t *l, const char *texto, size_t longitud)
{
    if (!texto || !longitud || longitud > LIMITE_DOCUMENTO ||
        memchr(texto, '\0', longitud)) return -1;
    l->buffer = duplicar_rango(texto, longitud);
    if (!l->buffer) return -2;
    size_t lineas = 1;
    for (size_t i = 0; i < longitud; ++i)
        if (texto[i] == '\n') ++lineas;
    l->lineas = calloc(lineas, sizeof *l->lineas);
    if (!l->lineas) return -2;
    char *actual = l->buffer;
    for (size_t i = 0; i < lineas; ++i) {
        char *fin = strchr(actual, '\n');
        if (fin) *fin = '\0';
        size_t n = strlen(actual);
        if (n && actual[n-1] == '\r') actual[n-1] = '\0';
        size_t ind = 0;
        while (actual[ind] == ' ') ++ind;
        const char *s = actual + ind;
        l->lineas[i] = (linea_t){actual, ind, i+1, *s && *s != '#'};
        if (strchr(actual, '\t'))
            informar(l, i+1, (size_t)(strchr(actual, '\t')-actual)+1,
                     "yaml", "Tabulador no permitido");
        actual = fin ? fin+1 : actual+strlen(actual);
    }
    l->cantidad_lineas = lineas;
    return 0;
}

static const nodo_t *obtener(const nodo_t *m, const char *clave)
{
    if (!m || m->clase != N_MAPA) return NULL;
    for (size_t i = 0; i < m->cantidad_pares; ++i)
        if (strcmp(m->pares[i].clave, clave) == 0) return m->pares[i].valor;
    return NULL;
}
static const char *escalar(const nodo_t *n)
{
    return n && n->clase == N_ESCALAR ? n->valor : NULL;
}
static void comprobar_claves(lector_t *l, const nodo_t *m,
                            const char *permitidas, const char *ruta)
{
    if (!m || m->clase != N_MAPA) {
        informar(l, m ? m->linea : 0, m ? m->columna : 0, ruta,
                 "Se esperaba un mapa YAML");
        return;
    }
    for (size_t i = 0; i < m->cantidad_pares; ++i) {
        const par_t *p = &m->pares[i];
        char clave_ruta[RUTA_CAP], patron[256];
        ruta_hija(clave_ruta, sizeof clave_ruta, ruta, p->clave);
        if (strlen(p->clave) > 160) {
            informar(l, p->linea, p->columna, ruta, "Clave demasiado larga");
            continue;
        }
        snprintf(patron, sizeof patron, "|%s|", p->clave);
        if (!strstr(permitidas, patron))
            informar(l, p->linea, p->columna, clave_ruta, "Clave desconocida");
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(p->clave, m->pares[j].clave) == 0) {
                informar(l, p->linea, p->columna, clave_ruta, "Clave duplicada");
                break;
            }
        }
    }
}
static const char *requerir_escalar(lector_t *l, const nodo_t *m,
                                   const char *clave, const char *ruta)
{
    const nodo_t *n = obtener(m, clave);
    char r[RUTA_CAP]; ruta_hija(r, sizeof r, ruta, clave);
    if (!n || !escalar(n) || !*escalar(n)) {
        informar(l, n ? n->linea : (m ? m->linea : 0),
                 n ? n->columna : (m ? m->columna : 0), r,
                 "Se requiere un escalar no vacio");
        return NULL;
    }
    return escalar(n);
}
static const nodo_t *requerir_lista(lector_t *l, const nodo_t *m,
                                    const char *clave, const char *ruta)
{
    const nodo_t *n = obtener(m, clave);
    char r[RUTA_CAP]; ruta_hija(r, sizeof r, ruta, clave);
    if (!n || n->clase != N_LISTA || !n->cantidad_elementos) {
        informar(l, n ? n->linea : (m ? m->linea : 0),
                 n ? n->columna : (m ? m->columna : 0), r,
                 "Se requiere una lista no vacia");
        return NULL;
    }
    return n;
}
static int letra_ascii(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int numero_ascii(unsigned char c)
{
    return c >= '0' && c <= '9';
}
static int nombre_valido(const char *s)
{
    if (!s || !(letra_ascii((unsigned char)*s) || *s == '_')) return 0;
    for (++s; *s; ++s)
        if (!(letra_ascii((unsigned char)*s) ||
              numero_ascii((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}
static int ruta_segura(const char *s)
{
    if (!s || !*s || s[0] == '/') return 0;
    const char *p = s;
    while (*p) {
        const char *fin = strchr(p, '/');
        size_t n = fin ? (size_t)(fin-p) : strlen(p);
        if (!n || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!fin) return 1;
        p = fin+1;
    }
    return 0;
}
static int tiene_variable(const char *s)
{
    return s && strstr(s, "$(") != NULL;
}
static void validar_numero(lector_t *l, const nodo_t *n, const char *ruta,
                          int real, long double minimo, long double maximo)
{
    const char *s = escalar(n);
    if (!s || !*s) {
        informar(l, n ? n->linea : 0, n ? n->columna : 0, ruta,
                 "Se requiere un numero");
        return;
    }
    if (tiene_variable(s)) return; /* Se revalida tras interpolacion. */
    errno = 0; char *fin = NULL;
    long double x;
    if (real) x = strtold(s, &fin);
    else {
        if (*s == '-' || *s == '+') { fin = (char *)s; x = 0; }
        else x = (long double)strtoumax(s, &fin, 10);
    }
    if (errno || !fin || fin == s || *fin || !isfinite(x) ||
        x < minimo || x > maximo)
        informar(l, n->linea, n->columna, ruta,
                 "Numero fuera de rango o invalido");
}
static void validar_condicion(lector_t *l, const nodo_t *m,
                             const char *ruta, condicion_t *destino)
{
    *destino = COND_SUCCEEDED;
    const nodo_t *n = obtener(m, "condition");
    if (!n) return;
    const char *s = escalar(n);
    if (s && strcmp(s, "succeeded()") == 0) return;
    if (s && strcmp(s, "failed()") == 0) { *destino = COND_FAILED; return; }
    if (s && strcmp(s, "always()") == 0) { *destino = COND_ALWAYS; return; }
    char r[RUTA_CAP]; ruta_hija(r, sizeof r, ruta, "condition");
    informar(l, n->linea, n->columna, r, "Condicion desconocida");
}

static int anadir_indice(lector_t *l, size_t **lista, size_t *total, size_t valor)
{
    for (size_t i = 0; i < *total; ++i)
        if ((*lista)[i] == valor) return 0;
    size_t *p = realloc(*lista, (*total + 1) * sizeof *p);
    if (!p) { l->memoria_agotada = 1; return -1; }
    *lista = p; (*lista)[(*total)++] = valor;
    return 0;
}
static size_t buscar_etapa(const pipeline_t *plan, const char *s)
{
    if (!s) return SIZE_MAX;
    for (size_t i = 0; i < plan->cantidad_etapas; ++i)
        if (plan->etapas[i].nombre && strcmp(plan->etapas[i].nombre, s) == 0)
            return i;
    return SIZE_MAX;
}
static size_t buscar_trabajo(const pipeline_t *plan, const etapa_t *etapa,
                            const char *s)
{
    if (!s) return SIZE_MAX;
    for (size_t i = etapa->primer_trabajo;
         i < etapa->primer_trabajo + etapa->cantidad_trabajos; ++i)
        if (plan->trabajos[i].nombre && strcmp(plan->trabajos[i].nombre, s) == 0)
            return i;
    return SIZE_MAX;
}
static void validar_nombre(lector_t *l, const nodo_t *m,
                          const char *campo, const char *ruta, char **destino)
{
    const char *nombre = requerir_escalar(l, m, campo, ruta);
    if (!nombre) return;
    char r[RUTA_CAP]; ruta_hija(r, sizeof r, ruta, campo);
    if (!nombre_valido(nombre))
        informar(l, m->linea, m->columna, r,
                 "Nombre debe cumplir [A-Za-z_][A-Za-z0-9_]*");
    *destino = duplicar(nombre);
    if (!*destino) l->memoria_agotada = 1;
}
static void validar_entrada_de_tarea(lector_t *l, const nodo_t *inputs,
                                     paso_t *p, const char *ruta)
{
    if (!inputs || inputs->clase != N_MAPA) {
        informar(l, inputs ? inputs->linea : 0,
                 inputs ? inputs->columna : 0, ruta, "inputs debe ser un mapa");
        return;
    }
    const char *permitidas = "";
    const char *requeridas[5] = {NULL};
    if (p->tipo == PASO_SLEEP) {
        permitidas = "|seconds|"; requeridas[0] = "seconds";
    } else if (p->tipo == PASO_CPU) {
        permitidas = "|iterations||seed||threads|";
        requeridas[0] = "iterations"; requeridas[1] = "seed";
    } else if (p->tipo == PASO_WORDCOUNT) {
        permitidas = "|file||threads||output||top|";
        requeridas[0] = "file"; requeridas[1] = "threads";
        requeridas[2] = "output";
    } else if (p->tipo == PASO_HISTOGRAM) {
        permitidas = "|file||bins||threads||output|";
        requeridas[0] = "file"; requeridas[1] = "bins";
        requeridas[2] = "threads"; requeridas[3] = "output";
    }
    comprobar_claves(l, inputs, permitidas, ruta);
    for (size_t k = 0; k < 5 && requeridas[k]; ++k)
        (void)requerir_escalar(l, inputs, requeridas[k], ruta);
    p->cantidad_inputs = inputs->cantidad_pares;
    if (p->cantidad_inputs) {
        p->inputs = calloc(p->cantidad_inputs, sizeof *p->inputs);
        if (!p->inputs) { l->memoria_agotada = 1; return; }
    }
    for (size_t k = 0; k < inputs->cantidad_pares; ++k) {
        const par_t *item = &inputs->pares[k];
        char path[RUTA_CAP]; ruta_hija(path, sizeof path, ruta, item->clave);
        const char *v = escalar(item->valor);
        if (!v || !*v)
            informar(l, item->linea, item->columna, path, "Input vacio o no escalar");
        p->inputs[k].nombre = duplicar(item->clave);
        p->inputs[k].valor = duplicar(v ? v : "");
        if (!p->inputs[k].nombre || !p->inputs[k].valor)
            l->memoria_agotada = 1;
        if (!v) continue;
        if (!strcmp(item->clave, "seconds"))
            validar_numero(l, item->valor, path, 1, 0, 86400);
        else if (!strcmp(item->clave, "threads"))
            validar_numero(l, item->valor, path, 0, 1, 1024);
        else if (!strcmp(item->clave, "bins"))
            validar_numero(l, item->valor, path, 0, 1, 256);
        else if (!strcmp(item->clave, "iterations") ||
                 !strcmp(item->clave, "seed"))
            validar_numero(l, item->valor, path, 0, 0, UINT64_MAX);
        else if (!strcmp(item->clave, "top"))
            validar_numero(l, item->valor, path, 0, 1, 1000000);
        else if (!strcmp(item->clave, "file") ||
                 !strcmp(item->clave, "output")) {
            if (!ruta_segura(v))
                informar(l, item->linea, item->columna, path, "Ruta insegura");
        }
    }
}
static void interpretar_paso(lector_t *l, const nodo_t *n,
                            paso_t *p, const char *ruta)
{
    p->estado = QUEUED;
    p->codigo_salida = -1;
    comprobar_claves(l, n,
        "|script||task||inputs||publish||artifact||download||path||continueOnError|", ruta);
    if (!n || n->clase != N_MAPA) return;
    const nodo_t *cont = obtener(n, "continueOnError");
    if (cont) {
        const char *v = escalar(cont);
        if (v && strcmp(v, "true") == 0) p->continuar_si_falla = true;
        else if (v && strcmp(v, "false") == 0) p->continuar_si_falla = false;
        else {
            char r[RUTA_CAP]; ruta_hija(r, sizeof r, ruta, "continueOnError");
            informar(l, cont->linea, cont->columna, r, "Valor booleano invalido");
        }
    }
    int cantidad_acciones = !!obtener(n,"script") + !!obtener(n,"task") +
                           !!obtener(n,"publish") + !!obtener(n,"download");
    if (cantidad_acciones != 1) {
        informar(l, n->linea, n->columna, ruta,
                 "El paso requiere exactamente una accion");
        return;
    }
    if (obtener(n, "script")) {
        p->tipo = PASO_SCRIPT;
        const char *s = requerir_escalar(l, n, "script", ruta);
        if (s) { p->comando = duplicar(s); if (!p->comando) l->memoria_agotada=1; }
        if (obtener(n, "inputs") || obtener(n, "artifact") ||
            obtener(n, "path"))
            informar(l, n->linea, n->columna, ruta, "Claves incompatibles con script");
    } else if (obtener(n, "task")) {
        const char *s = requerir_escalar(l, n, "task", ruta);
        if (s) { p->tarea = duplicar(s); if (!p->tarea) l->memoria_agotada=1; }
        if (s && strcmp(s, "Sleep@1") == 0) p->tipo = PASO_SLEEP;
        else if (s && strcmp(s, "Cpu@1") == 0) p->tipo = PASO_CPU;
        else if (s && strcmp(s, "WordCount@1") == 0) p->tipo = PASO_WORDCOUNT;
        else if (s && strcmp(s, "Histogram@1") == 0) p->tipo = PASO_HISTOGRAM;
        else {
            char r[RUTA_CAP]; ruta_hija(r,sizeof r,ruta,"task");
            informar(l,n->linea,n->columna,r,"Tarea integrada desconocida");
            return;
        }
        char r[RUTA_CAP]; ruta_hija(r,sizeof r,ruta,"inputs");
        validar_entrada_de_tarea(l, obtener(n,"inputs"), p, r);
        if (obtener(n, "artifact") || obtener(n, "path"))
            informar(l, n->linea, n->columna, ruta, "Claves incompatibles con task");
    } else if (obtener(n, "publish")) {
        p->tipo = PASO_PUBLICAR;
        const char *s = requerir_escalar(l,n,"publish",ruta);
        const char *a = requerir_escalar(l,n,"artifact",ruta);
        if (s) { p->ruta_publicar=duplicar(s); if (!p->ruta_publicar) l->memoria_agotada=1; }
        if (a) { p->nombre_artefacto=duplicar(a); if (!p->nombre_artefacto) l->memoria_agotada=1; }
        if (a && !nombre_valido(a))
            informar(l,n->linea,n->columna,ruta,"Nombre de artefacto invalido");
        if (s && !ruta_segura(s))
            informar(l,n->linea,n->columna,ruta,"Ruta publish insegura");
        if (obtener(n,"path") || obtener(n,"inputs"))
            informar(l,n->linea,n->columna,ruta,"Claves incompatibles con publish");
    } else {
        p->tipo = PASO_DESCARGAR;
        const char *a = requerir_escalar(l,n,"download",ruta);
        const char *s = requerir_escalar(l,n,"path",ruta);
        if (a) { p->nombre_artefacto=duplicar(a); if (!p->nombre_artefacto) l->memoria_agotada=1; }
        if (s) { p->ruta_descargar=duplicar(s); if (!p->ruta_descargar) l->memoria_agotada=1; }
        if (a && !nombre_valido(a))
            informar(l,n->linea,n->columna,ruta,"Nombre de artefacto invalido");
        if (s && !ruta_segura(s))
            informar(l,n->linea,n->columna,ruta,"Ruta download insegura");
        if (obtener(n,"inputs") || obtener(n,"artifact"))
            informar(l,n->linea,n->columna,ruta,"Claves incompatibles con download");
    }
}
static void interpretar_trabajo(lector_t *l, const nodo_t *n,
                                trabajo_t *t, const char *ruta)
{
    t->estado = QUEUED;
    t->siguiente_listo = SIZE_MAX;
    comprobar_claves(l,n,
        "|job||dependsOn||condition||timeoutInSeconds||steps|",ruta);
    if (!n || n->clase != N_MAPA) return;
    validar_nombre(l,n,"job",ruta,&t->nombre);
    validar_condicion(l,n,ruta,&t->condicion);
    const nodo_t *timeout = obtener(n,"timeoutInSeconds");
    if (timeout) {
        char r[RUTA_CAP]; ruta_hija(r,sizeof r,ruta,"timeoutInSeconds");
        validar_numero(l,timeout,r,0,1,86400);
        const char *s = escalar(timeout);
        if (s && !tiene_variable(s)) {
            errno = 0; char *fin = NULL;
            unsigned long v = strtoul(s,&fin,10);
            if (!errno && fin != s && !*fin && v >= 1 && v <= 86400)
                t->timeout_segundos = (unsigned)v;
        } else if (s && tiene_variable(s)) {
            /* El campo tipado del header no admite interpolacion aqui. */
            informar(l,timeout->linea,timeout->columna,r,
                     "timeoutInSeconds debe ser literal, no admite variables");
        }
    }
    const nodo_t *pasos = requerir_lista(l,n,"steps",ruta);
    if (!pasos) return;
    t->cantidad_pasos = pasos->cantidad_elementos;
    t->pasos = calloc(t->cantidad_pasos, sizeof *t->pasos);
    if (!t->pasos) { l->memoria_agotada=1; return; }
    for (size_t i = 0; i < t->cantidad_pasos; ++i) {
        char base[RUTA_CAP], r[RUTA_CAP]; ruta_hija(base,sizeof base,ruta,"steps");
        ruta_indice(r,sizeof r,base,i);
        interpretar_paso(l,pasos->elementos[i],&t->pasos[i],r);
    }
}
static void interpretar_etapa(lector_t *l, const nodo_t *n,
                              etapa_t *e, const char *ruta)
{
    e->estado = QUEUED;
    comprobar_claves(l,n,"|stage||dependsOn||condition||jobs|",ruta);
    if (!n || n->clase != N_MAPA) return;
    validar_nombre(l,n,"stage",ruta,&e->nombre);
    validar_condicion(l,n,ruta,&e->condicion);
}
static void interpretar_variables(lector_t *l, const nodo_t *n,
                                 pipeline_t *plan)
{
    if (!n) return;
    if (n->clase != N_MAPA) {
        informar(l,n->linea,n->columna,"variables","variables debe ser un mapa");
        return;
    }
    plan->cantidad_variables = n->cantidad_pares;
    plan->variables = calloc(plan->cantidad_variables ? plan->cantidad_variables:1,
                            sizeof *plan->variables);
    if (!plan->variables) { l->memoria_agotada=1; return; }
    for (size_t i = 0; i < n->cantidad_pares; ++i) {
        const par_t *p = &n->pares[i];
        char r[RUTA_CAP]; ruta_hija(r,sizeof r,"variables",p->clave);
        if (!nombre_valido(p->clave))
            informar(l,p->linea,p->columna,r,"Nombre de variable invalido");
        for (size_t j = 0; j < i; ++j)
            if (strcmp(p->clave,n->pares[j].clave) == 0) {
                informar(l,p->linea,p->columna,r,"Variable duplicada"); break;
            }
        const char *valor = escalar(p->valor);
        if (!valor)
            informar(l,p->linea,p->columna,r,"Valor de variable debe ser escalar");
        plan->variables[i].nombre=duplicar(p->clave);
        plan->variables[i].valor=duplicar(valor ? valor : "");
        if (!plan->variables[i].nombre || !plan->variables[i].valor)
            l->memoria_agotada=1;
    }
}
static void extraer_dependencias(lector_t *l, const nodo_t *n,
                                 const char *ruta, const pipeline_t *plan,
                                 const etapa_t *etapa, int es_etapa,
                                 size_t **indices, size_t *cantidad)
{
    const nodo_t *de = obtener(n,"dependsOn");
    if (!de && es_etapa) {
        if (etapa->indice > 0)
            (void)anadir_indice(l,indices,cantidad,etapa->indice-1);
        return;
    }
    if (!de) return;
    char r[RUTA_CAP]; ruta_hija(r,sizeof r,ruta,"dependsOn");
    if (de->clase != N_ESCALAR && de->clase != N_LISTA) {
        informar(l,de->linea,de->columna,r,"dependsOn debe ser nombre o lista");
        return;
    }
    size_t total = de->clase == N_LISTA ? de->cantidad_elementos : 1;
    for (size_t k = 0; k < total; ++k) {
        const nodo_t *v = de->clase == N_LISTA ? de->elementos[k] : de;
        char camino[RUTA_CAP];
        if (de->clase == N_LISTA) ruta_indice(camino,sizeof camino,r,k);
        else snprintf(camino,sizeof camino,"%s",r);
        const char *s = escalar(v);
        if (!s || !*s) {
            informar(l,v->linea,v->columna,camino,"Dependencia no es nombre");
            continue;
        }
        size_t id = es_etapa ? buscar_etapa(plan,s) : buscar_trabajo(plan,etapa,s);
        if (id == SIZE_MAX) {
            informar(l,v->linea,v->columna,camino,"Dependencia inexistente");
            continue;
        }
        int duplicada = 0;
        for (size_t x = 0; x < *cantidad; ++x)
            if ((*indices)[x] == id) duplicada = 1;
        if (duplicada)
            informar(l,v->linea,v->columna,camino,"Dependencia repetida");
        else (void)anadir_indice(l,indices,cantidad,id);
    }
}
static void comprobar_grafo(lector_t *l, const pipeline_t *plan,
                           const nodo_t *lista_etapas)
{
    /* Se reusa graph.c sobre las dependencias directas, incluyendo ciclos. */
    size_t ne = plan->cantidad_etapas;
    nodo_grafo_t *g = calloc(ne ? ne : 1, sizeof *g);
    if (!g) { l->memoria_agotada=1; return; }
    int valido = 1;
    for (size_t i = 0; i < ne; ++i) {
        const etapa_t *e = &plan->etapas[i];
        g[i].nombre = e->nombre;
        g[i].cantidad_dependencias = e->cantidad_dependencias;
        const char **deps = calloc(e->cantidad_dependencias ? e->cantidad_dependencias : 1,
                                   sizeof *deps);
        if (!deps) { l->memoria_agotada=1; valido=0; break; }
        for (size_t j = 0; j < e->cantidad_dependencias; ++j)
            deps[j] = plan->etapas[e->dependencias[j]].nombre;
        g[i].depende = deps;
        if (!e->nombre) valido = 0;
    }
    char error[256] = {0};
    if (valido && validar_grafo(g,ne,error,sizeof error) != 0)
        informar(l,lista_etapas->linea,lista_etapas->columna,
                 "stages",error[0] ? error : "Grafo de etapas invalido");
    for (size_t i = 0; i < ne; ++i) free((void *)g[i].depende);
    free(g);
    for (size_t i = 0; i < ne; ++i) {
        const etapa_t *e = &plan->etapas[i];
        const nodo_t *jobs = obtener(lista_etapas->elementos[i],"jobs");
        size_t nj = e->cantidad_trabajos;
        if (!nj) continue;
        g = calloc(nj,sizeof *g);
        if (!g) { l->memoria_agotada=1; return; }
        valido=1;
        for (size_t j = 0; j < nj; ++j) {
            const trabajo_t *t = &plan->trabajos[e->primer_trabajo+j];
            g[j].nombre = t->nombre;
            g[j].cantidad_dependencias = t->cantidad_dependencias_condicion;
            const char **deps = calloc(t->cantidad_dependencias_condicion ?
                                       t->cantidad_dependencias_condicion : 1,
                                       sizeof *deps);
            if (!deps) { l->memoria_agotada=1; valido=0; break; }
            for (size_t k = 0; k < t->cantidad_dependencias_condicion; ++k)
                deps[k] = plan->trabajos[t->dependencias_condicion[k]].nombre;
            g[j].depende = deps;
            if (!t->nombre) valido=0;
        }
        if (valido && validar_grafo(g,nj,error,sizeof error) != 0) {
            char path[RUTA_CAP]; ruta_indice(path,sizeof path,"stages",i);
            informar(l,jobs ? jobs->linea : 0,jobs ? jobs->columna : 0,
                     path,error[0] ? error : "Grafo de trabajos invalido");
        }
        for (size_t j=0;j<nj;++j) free((void *)g[j].depende);
        free(g);
    }
}
static int ancestro(const pipeline_t *plan, size_t consumidor, size_t productor)
{
    /* DFS iterativo: no depende de recursiones profundas del grafo. */
    size_t n = plan->cantidad_trabajos;
    unsigned char *visto = calloc(n ? n : 1,1);
    size_t *pila = calloc(n ? n : 1,sizeof *pila);
    if (!visto || !pila) { free(visto); free(pila); return -1; }
    size_t usados=0;
    pila[usados++]=consumidor;
    visto[consumidor]=1;
    int resultado=0;
    while (usados) {
        size_t actual=pila[--usados];
        const trabajo_t *t=&plan->trabajos[actual];
        for (size_t k=0;k<t->cantidad_dependencias;++k) {
            size_t d=t->dependencias[k];
            if (d==productor) { resultado=1; goto terminar; }
            if (d<n && !visto[d]) { visto[d]=1; pila[usados++]=d; }
        }
    }
terminar:
    free(visto);free(pila);
    return resultado;
}
typedef struct {
    const char *nombre;
    size_t trabajo;
} publicacion_t;
static void informar_artefacto(lector_t *l, const pipeline_t *plan,
                              const nodo_t *etapas, size_t id, size_t paso,
                              const char *mensaje)
{
    const trabajo_t *t=&plan->trabajos[id];
    const etapa_t *etapa=&plan->etapas[t->indice_etapa];
    size_t local=id-etapa->primer_trabajo;
    char ruta[RUTA_CAP];
    snprintf(ruta,sizeof ruta,"stages[%zu].jobs[%zu].steps[%zu]",
             t->indice_etapa,local,paso);
    const nodo_t *stage=etapas->elementos[t->indice_etapa];
    const nodo_t *jobs=obtener(stage,"jobs");
    const nodo_t *job=jobs && jobs->clase==N_LISTA && local<jobs->cantidad_elementos ?
                      jobs->elementos[local] : NULL;
    const nodo_t *steps=obtener(job,"steps");
    const nodo_t *n=steps && steps->clase==N_LISTA && paso<steps->cantidad_elementos ?
                    steps->elementos[paso] : NULL;
    informar(l,n ? n->linea : 0,n ? n->columna : 0,ruta,mensaje);
}
static void comprobar_artefactos(lector_t *l, const pipeline_t *plan,
                                 const nodo_t *etapas)
{
    size_t capacidad=0;
    for (size_t i=0;i<plan->cantidad_trabajos;++i)
        capacidad+=plan->trabajos[i].cantidad_pasos;
    publicacion_t *pub=calloc(capacidad ? capacidad : 1,sizeof *pub);
    if (!pub) { l->memoria_agotada=1; return; }
    size_t cantidad=0;
    for (size_t i=0;i<plan->cantidad_trabajos;++i) {
        const trabajo_t *t=&plan->trabajos[i];
        for (size_t j=0;j<t->cantidad_pasos;++j) {
            const paso_t *p=&t->pasos[j];
            if (p->tipo!=PASO_PUBLICAR || !p->nombre_artefacto) continue;
            for (size_t k=0;k<cantidad;++k)
                if (!strcmp(p->nombre_artefacto,pub[k].nombre)) {
                    informar_artefacto(l,plan,etapas,i,j,
                                      "Nombre de artefacto publish duplicado");
                    break;
                }
            pub[cantidad++]=(publicacion_t){p->nombre_artefacto,i};
        }
    }
    for (size_t i=0;i<plan->cantidad_trabajos;++i) {
        const trabajo_t *t=&plan->trabajos[i];
        for (size_t j=0;j<t->cantidad_pasos;++j) {
            const paso_t *p=&t->pasos[j];
            if (p->tipo!=PASO_DESCARGAR || !p->nombre_artefacto) continue;
            size_t productor=SIZE_MAX;
            for (size_t k=0;k<cantidad;++k)
                if (!strcmp(p->nombre_artefacto,pub[k].nombre)) {
                    productor=pub[k].trabajo; break;
                }
            int puede=productor==SIZE_MAX ? 0 : ancestro(plan,i,productor);
            if (puede<0) l->memoria_agotada=1;
            if (!puede) informar_artefacto(l,plan,etapas,i,j,
                "Download sin productor ancestro o artefacto inexistente");
        }
    }
    free(pub);
}
static void construir_plan(lector_t *l, const nodo_t *raiz, pipeline_t *plan)
{
    comprobar_claves(l,raiz,"|name||variables||stages|","");
    if (!raiz || raiz->clase!=N_MAPA) return;
    const char *nombre=requerir_escalar(l,raiz,"name","");
    if (nombre) { plan->nombre=duplicar(nombre); if (!plan->nombre) l->memoria_agotada=1; }
    interpretar_variables(l,obtener(raiz,"variables"),plan);
    const nodo_t *etapas=requerir_lista(l,raiz,"stages","");
    if (!etapas || etapas->cantidad_elementos>LIMITE_NODOS) return;
    plan->cantidad_etapas=etapas->cantidad_elementos;
    plan->etapas=calloc(plan->cantidad_etapas,sizeof *plan->etapas);
    if (!plan->etapas) { l->memoria_agotada=1; return; }
    size_t total_trabajos=0;
    for (size_t i=0;i<plan->cantidad_etapas;++i) {
        etapa_t *e=&plan->etapas[i];
        char r[RUTA_CAP]; ruta_indice(r,sizeof r,"stages",i);
        e->indice=i; e->primer_trabajo=total_trabajos;
        const nodo_t *n=etapas->elementos[i];
        interpretar_etapa(l,n,e,r);
        const nodo_t *jobs=requerir_lista(l,n,"jobs",r);
        if (jobs) {
            if (jobs->cantidad_elementos>LIMITE_NODOS-total_trabajos) {
                informar(l,jobs->linea,jobs->columna,r,"Demasiados trabajos");
                return;
            }
            e->cantidad_trabajos=jobs->cantidad_elementos;
            total_trabajos+=e->cantidad_trabajos;
        }
    }
    for (size_t i=0;i<plan->cantidad_etapas;++i) {
        etapa_t *e=&plan->etapas[i];
        for (size_t j=0;j<i;++j)
            if (e->nombre && plan->etapas[j].nombre &&
                strcmp(e->nombre,plan->etapas[j].nombre)==0) {
                char r[RUTA_CAP]; ruta_indice(r,sizeof r,"stages",i);
                informar(l,etapas->elementos[i]->linea,etapas->elementos[i]->columna,
                         r,"Nombre de etapa duplicado"); break;
            }
    }
    plan->cantidad_trabajos=total_trabajos;
    plan->trabajos=calloc(total_trabajos ? total_trabajos : 1,
                          sizeof *plan->trabajos);
    if (!plan->trabajos) { l->memoria_agotada=1; return; }
    for (size_t i=0;i<plan->cantidad_etapas;++i) {
        const etapa_t *e=&plan->etapas[i];
        const nodo_t *jobs=obtener(etapas->elementos[i],"jobs");
        for (size_t j=0;j<e->cantidad_trabajos;++j) {
            size_t id=e->primer_trabajo+j;
            trabajo_t *t=&plan->trabajos[id];
            t->indice_global=id; t->indice_etapa=i;
            char r[RUTA_CAP],base[RUTA_CAP];
            ruta_indice(base,sizeof base,"stages",i);
            ruta_hija(r,sizeof r,base,"jobs");
            ruta_indice(base,sizeof base,r,j);
            interpretar_trabajo(l,jobs->elementos[j],t,base);
            for (size_t k=e->primer_trabajo;k<id;++k)
                if (t->nombre && plan->trabajos[k].nombre &&
                    !strcmp(t->nombre,plan->trabajos[k].nombre)) {
                    informar(l,jobs->elementos[j]->linea,jobs->elementos[j]->columna,
                             base,"Nombre de trabajo duplicado en etapa");
                    break;
                }
        }
    }
    /* Resolver etapas y luego trabajos, sin depender del orden de aparicion. */
    for (size_t i=0;i<plan->cantidad_etapas;++i) {
        etapa_t *e=&plan->etapas[i];
        char r[RUTA_CAP]; ruta_indice(r,sizeof r,"stages",i);
        extraer_dependencias(l,etapas->elementos[i],r,plan,e,1,
                             &e->dependencias,&e->cantidad_dependencias);
    }
    for (size_t i=0;i<plan->cantidad_etapas;++i) {
        const etapa_t *e=&plan->etapas[i];
        const nodo_t *jobs=obtener(etapas->elementos[i],"jobs");
        for (size_t j=0;j<e->cantidad_trabajos;++j) {
            trabajo_t *t=&plan->trabajos[e->primer_trabajo+j];
            char r[RUTA_CAP];
            snprintf(r,sizeof r,"stages[%zu].jobs[%zu]",i,j);
            extraer_dependencias(l,jobs->elementos[j],r,plan,e,0,
                                 &t->dependencias_condicion,
                                 &t->cantidad_dependencias_condicion);
            for (size_t k=0;k<e->cantidad_dependencias;++k) {
                const etapa_t *pre=&plan->etapas[e->dependencias[k]];
                for (size_t m=0;m<pre->cantidad_trabajos;++m)
                    (void)anadir_indice(l,&t->dependencias,&t->cantidad_dependencias,
                                        pre->primer_trabajo+m);
            }
            for (size_t k=0;k<t->cantidad_dependencias_condicion;++k)
                (void)anadir_indice(l,&t->dependencias,&t->cantidad_dependencias,
                                    t->dependencias_condicion[k]);
        }
    }
    if (!l->memoria_agotada) comprobar_grafo(l,plan,etapas);
    if (!l->memoria_agotada) comprobar_artefactos(l,plan,etapas);
}

int parsear_pipeline_yaml(const char *texto, size_t longitud,
                         pipeline_t **salida, errores_yaml_t *errores)
{
    if (!salida || !errores) return -2;
    *salida=NULL;
    *errores=(errores_yaml_t){0};
    lector_t l={.errores=errores};
    int rc=preparar_lineas(&l,texto,longitud);
    if (rc == -1) informar(&l,1,1,"yaml", "Texto vacio, demasiado grande o contiene NUL");
    if (rc != 0 || l.memoria_agotada) {
        free(l.lineas); free(l.buffer);
        return l.memoria_agotada || rc==-2 ? -2 : -1;
    }
    nodo_t *raiz=NULL;
    l.posicion=proxima_util(&l,0);
    if (l.posicion<l.cantidad_lineas) {
        if (l.lineas[l.posicion].indentacion!=0)
            informar(&l,l.lineas[l.posicion].numero,1,"yaml",
                     "El documento debe empezar en columna 1");
        raiz=leer_bloque(&l,l.lineas[l.posicion].indentacion,0);
        l.posicion=proxima_util(&l,l.posicion);
        if (l.posicion<l.cantidad_lineas)
            informar(&l,l.lineas[l.posicion].numero,1,"yaml",
                     "Contenido sobrante fuera del mapa principal");
    } else informar(&l,1,1,"yaml","Documento YAML vacio");
    pipeline_t *plan=calloc(1,sizeof *plan);
    if (!plan) l.memoria_agotada=1;
    if (plan && raiz && !l.memoria_agotada) construir_plan(&l,raiz,plan);
    int salida_rc=l.memoria_agotada ? -2 : (errores->cantidad ? -1 : 0);
    if (!raiz && salida_rc==0) salida_rc=-1;
    if (salida_rc==0) *salida=plan;
    else liberar_pipeline(plan);
    liberar_nodo(raiz);
    free(l.lineas); free(l.buffer);
    return salida_rc;
}

/* Compatibilidad con quien aun utiliza la interfaz de un solo error.
 * La API nueva de produccion es parsear_pipeline_yaml(). */
int validar_yaml(const char *texto, size_t longitud,
                 char *error, size_t capacidad)
{
    pipeline_t *plan=NULL;
    errores_yaml_t errores={0};
    int rc=parsear_pipeline_yaml(texto,longitud,&plan,&errores);
    if (error && capacidad) {
        if (rc && errores.cantidad)
            snprintf(error,capacidad,"%s: %s",errores.items[0].ubicacion,
                     errores.items[0].mensaje);
        else if (rc) snprintf(error,capacidad,"Error interno de YAML");
        else error[0]='\0';
    }
    liberar_pipeline(plan);
    liberar_errores_yaml(&errores);
    return rc==0 ? 0 : -1;
}
