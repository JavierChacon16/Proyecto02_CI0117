#define _POSIX_C_SOURCE 200809L
#include "vars.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* mutex protege los mapas por trabajo y su bandera de publicacion. */
typedef struct {
    variable_definicion_t *items;
    size_t n;
    int publicado;
} mapa_t;
struct variables_ejecucion {
    pthread_mutex_t mu;
    mapa_t *trabajos;
    size_t cantidad;
};

static const char *mapa_buscar(const mapa_t *m, const char *nombre)
{
    if (!m) return NULL;
    for (size_t i=0; i<m->n; ++i)
        if (!strcmp(m->items[i].nombre,nombre)) return m->items[i].valor;
    return NULL;
}

variables_ejecucion_t *vars_crear(const pipeline_t *plan)
{
    if (!plan) return NULL;
    variables_ejecucion_t *v=calloc(1,sizeof *v);
    if (!v) return NULL;
    if (pthread_mutex_init(&v->mu,NULL)) { free(v); return NULL; }
    v->cantidad=plan->cantidad_trabajos;
    v->trabajos=calloc(v->cantidad?v->cantidad:1,sizeof *v->trabajos);
    if (!v->trabajos) { pthread_mutex_destroy(&v->mu);free(v);return NULL; }
    return v;
}
void vars_destruir(variables_ejecucion_t *v)
{
    if (!v) return;
    for (size_t j=0;j<v->cantidad;++j) {
        for (size_t k=0;k<v->trabajos[j].n;++k) {
            free(v->trabajos[j].items[k].nombre);
            free(v->trabajos[j].items[k].valor);
        }
        free(v->trabajos[j].items);
    }
    free(v->trabajos);
    pthread_mutex_destroy(&v->mu);
    free(v);
}
int vars_definir(variables_ejecucion_t *v, size_t j,
                 const char *nombre, const char *valor)
{
    if (!v || j>=v->cantidad || !nombre || !*nombre || !valor) return -1;
    pthread_mutex_lock(&v->mu);
    mapa_t *m=&v->trabajos[j];
    if (m->publicado) { pthread_mutex_unlock(&v->mu); return -1; }
    for (size_t k=0;k<m->n;++k) {
        if (!strcmp(nombre,m->items[k].nombre)) {
            char *copia=strdup(valor);
            if (!copia) { pthread_mutex_unlock(&v->mu); return -1; }
            free(m->items[k].valor);m->items[k].valor=copia;
            pthread_mutex_unlock(&v->mu);return 0;
        }
    }
    char *nom=strdup(nombre),*val=strdup(valor);
    if (!nom || !val) {free(nom);free(val);pthread_mutex_unlock(&v->mu);return -1;}
    variable_definicion_t *p=realloc(m->items,(m->n+1)*sizeof *p);
    if (!p) {free(nom);free(val);pthread_mutex_unlock(&v->mu);return -1;}
    m->items=p;m->items[m->n++]=(variable_definicion_t){nom,val};
    pthread_mutex_unlock(&v->mu);return 0;
}
void vars_publicar(variables_ejecucion_t *v,size_t j)
{
    if (!v || j>=v->cantidad) return;
    pthread_mutex_lock(&v->mu);
    v->trabajos[j].publicado=1;
    pthread_mutex_unlock(&v->mu);
}
const char *obtener_variable(const pipeline_t *plan,const char *nombre)
{
    if (!plan || !nombre) return NULL;
    for (size_t k=0;k<plan->cantidad_variables;++k)
        if (!strcmp(nombre,plan->variables[k].nombre)) return plan->variables[k].valor;
    return NULL;
}
static void marcar_ancestros(const pipeline_t *p,size_t j,unsigned char *anc,
                            size_t *pila)
{
    size_t n=0;
    for (size_t i=0;i<p->trabajos[j].cantidad_dependencias;++i) {
        size_t d=p->trabajos[j].dependencias[i];
        if (d<p->cantidad_trabajos && !anc[d]) {anc[d]=1;pila[n++]=d;}
    }
    while (n) {
        size_t d=pila[--n];
        for (size_t i=0;i<p->trabajos[d].cantidad_dependencias;++i) {
            size_t a=p->trabajos[d].dependencias[i];
            if (a<p->cantidad_trabajos && !anc[a]) {anc[a]=1;pila[n++]=a;}
        }
    }
}
static const char *buscar_visible(const pipeline_t *p,variables_ejecucion_t *v,
                                  size_t j,const char *nombre,
                                  const unsigned char *anc)
{
    if (v && j<v->cantidad) {
        const char *local=mapa_buscar(&v->trabajos[j],nombre);
        if (local) return local;
        /* El mayor indice_global prevalece en conflictos de ancestros. */
        for (size_t k=v->cantidad;k>0;--k) {
            size_t a=k-1;
            if (a!=j && anc[a] && v->trabajos[a].publicado) {
                const char *r=mapa_buscar(&v->trabajos[a],nombre);
                if (r) return r;
            }
        }
    }
    return obtener_variable(p,nombre);
}
char *vars_resolver(const pipeline_t *p,variables_ejecucion_t *v,
                    size_t j,const char *texto)
{
    if (!p || !texto || j>=p->cantidad_trabajos) return NULL;
    size_t n=p->cantidad_trabajos;
    unsigned char *anc=calloc(n?n:1,1);
    size_t *pila=malloc((n?n:1)*sizeof *pila);
    if (!anc || !pila) {free(anc);free(pila);return NULL;}
    marcar_ancestros(p,j,anc,pila);
    size_t cap=strlen(texto)+1,pos=0;
    char *out=malloc(cap);
    if (!out) {free(anc);free(pila);return NULL;}
    if (v) pthread_mutex_lock(&v->mu);
    for (const char *s=texto;*s;) {
        const char *val=NULL;
        if (s[0]=='$' && s[1]=='(') {
            const char *fin=strchr(s+2,')');
            if (fin && (size_t)(fin-(s+2))<128) {
                char nom[128];size_t l=(size_t)(fin-(s+2));
                memcpy(nom,s+2,l);nom[l]=0;
                val=buscar_visible(p,v,j,nom,anc);
                if (!val) {free(out);out=NULL;break;}
                s=fin+1;
            }
        }
        if (!val) {
            char letra[2]={*s++,0};
            if (pos+2>cap) {cap=cap*2+2;char *t=realloc(out,cap);if(!t){free(out);out=NULL;break;}out=t;}
            out[pos++]=letra[0];
        } else {
            size_t l=strlen(val);
            if (pos+l+1>cap) {cap=(pos+l+1)*2;char *t=realloc(out,cap);if(!t){free(out);out=NULL;break;}out=t;}
            memcpy(out+pos,val,l);pos+=l;
        }
    }
    if (out) out[pos]=0;
    if (v) pthread_mutex_unlock(&v->mu);
    free(anc);free(pila);
    return out;
}
char *resolver_variables_texto(const pipeline_t *p,const char *t)
{
    if (!p || !t) return NULL;
    /* Solo compatibilidad de las variables YAML de primer nivel. */
    size_t cap=strlen(t)+1,pos=0;
    char *out=malloc(cap);
    if (!out) return NULL;
    for (const char *s=t;*s;) {
        const char *v=NULL;
        if (s[0]=='$' && s[1]=='(') {
            const char *fin=strchr(s+2,')');
            if (fin && fin-(s+2)<128) {
                char nombre[128];size_t n=(size_t)(fin-(s+2));
                memcpy(nombre,s+2,n);nombre[n]=0;
                v=obtener_variable(p,nombre);
                if (!v) {free(out);return NULL;}
                s=fin+1;
            }
        }
        if (!v) {
            if (pos+2>cap) {cap=cap*2+2;char *x=realloc(out,cap);if(!x){free(out);return NULL;}out=x;}
            out[pos++]=*s++;
        } else {
            size_t n=strlen(v);
            if (pos+n+1>cap) {cap=(pos+n+1)*2;char*x=realloc(out,cap);if(!x){free(out);return NULL;}out=x;}
            memcpy(out+pos,v,n);pos+=n;
        }
    }
    out[pos]=0;
    return out;
}
