#define _POSIX_C_SOURCE 200809L
#include "motor.h"
#include "script.h"
#include "tasks.h"
#include "vars.h"
#include "artifacts.h"
#include "logs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

/* Ejecutor de un trabajo. Nunca retiene servidor_t.mu durante I/O o
 * ejecucion. El motor es quien gestiona estados, agentes y dependencias. */
typedef struct {
    ejecucion_t *e;
    const etapa_t *etapa;
    const trabajo_t *trabajo;
} callback_ctx_t;

static void escribir_log(ejecucion_t *e,const etapa_t *et,const trabajo_t *t,
                        const char *texto)
{
    if (!e->bitacoras || !texto) return;
    char prefijo[512];
    int n=snprintf(prefijo,sizeof prefijo,"[%s/%s] ",et->nombre,t->nombre);
    if (n>0 && (size_t)n<sizeof prefijo)
        (void)bitacoras_agregar(e->bitacoras,et->nombre,t->nombre,prefijo,(size_t)n);
    (void)bitacoras_agregar(e->bitacoras,et->nombre,t->nombre,texto,strlen(texto));
    if (!strchr(texto,'\n'))
        (void)bitacoras_agregar(e->bitacoras,et->nombre,t->nombre,"\n",1);
}

static int linea_script(void *ctx,const script_linea_t *linea)
{
    callback_ctx_t *c=ctx;
    if (linea->canal==SCRIPT_CANAL_STDOUT && !linea->fragmentada &&
        linea->longitud>14 && !memcmp(linea->datos,"##setvariable ",14)) {
        const char *ini=linea->datos+14;
        const char *fin=memchr(ini,'=',linea->longitud-14);
        if (fin && fin>ini && fin-ini<128) {
            char nombre[128];size_t len=(size_t)(fin-ini);
            memcpy(nombre,ini,len);nombre[len]=0;
            int valido=(isalpha((unsigned char)nombre[0]) || nombre[0]=='_');
            for(size_t i=1;i<len;++i)
                if(!isalnum((unsigned char)nombre[i])&&nombre[i]!='_') valido=0;
            size_t nv=linea->longitud-(size_t)(fin-linea->datos)-1;
            while(nv && (fin[1+nv-1]=='\n' || fin[1+nv-1]=='\r')) --nv;
            if (valido && nv<65536) {
                char *valor=malloc(nv+1);
                if (!valor) return -1;
                memcpy(valor,fin+1,nv);valor[nv]=0;
                int rc=vars_definir(c->e->variables,c->trabajo->indice_global,nombre,valor);
                free(valor);
                if (rc) return -1;
            }
        }
    }
    if (c->e->bitacoras) {
        char prefijo[512];
        int n=snprintf(prefijo,sizeof prefijo,"[%s/%s] ",
                       c->etapa->nombre,c->trabajo->nombre);
        if (n>0 && (size_t)n<sizeof prefijo &&
            bitacoras_agregar(c->e->bitacoras,c->etapa->nombre,c->trabajo->nombre,
                              prefijo,(size_t)n)) return -1;
        if (bitacoras_agregar(c->e->bitacoras,c->etapa->nombre,c->trabajo->nombre,
                             linea->datos,linea->longitud)) return -1;
    }
    return 0;
}

static int ruta_relativa_segura(const char *ruta)
{
    if (!ruta || !*ruta || *ruta=='/') return 0;
    const char *p=ruta;
    while (*p) {
        const char *inicio=p;
        while(*p && *p!='/') ++p;
        size_t n=(size_t)(p-inicio);
        if (!n || (n==1 && *inicio=='.') ||
            (n==2 && inicio[0]=='.' && inicio[1]=='.')) return 0;
        if (*p) ++p;
    }
    return 1;
}
static char *ruta_trabajo(const trabajo_t *t,const char *rel)
{
    if (!t->directorio_trabajo || !ruta_relativa_segura(rel)) return NULL;
    size_t a=strlen(t->directorio_trabajo),b=strlen(rel);
    char *r=malloc(a+b+2);
    if (!r) return NULL;
    memcpy(r,t->directorio_trabajo,a);r[a]='/';memcpy(r+a+1,rel,b+1);
    return r;
}
static const char *input_crudo(const paso_t *p,const char *nombre)
{
    for(size_t i=0;i<p->cantidad_inputs;++i)
        if(!strcmp(p->inputs[i].nombre,nombre)) return p->inputs[i].valor;
    return NULL;
}
static char *input_valor(const ejecucion_t *e,const trabajo_t *t,
                         const paso_t *p,const char *nombre)
{
    const char *crudo=input_crudo(p,nombre);
    if (!crudo) return NULL;
    return vars_resolver(e->plan,e->variables,t->indice_global,crudo);
}
static int entero_u64(const char *s,uint64_t *out)
{
    if(!s || !*s || *s=='-' || isspace((unsigned char)*s)) return -1;
    errno=0;char *fin=NULL;
    unsigned long long valor=strtoull(s,&fin,10);
    if(errno || fin==s || *fin) return -1;
    *out=(uint64_t)valor;return 0;
}
static int real_positivo(const char *s,double *out)
{
    if(!s||!*s) return -1;
    errno=0;char *fin=NULL;
    double valor=strtod(s,&fin);
    if(errno || fin==s || *fin || !(valor>=0 && valor<=86400)) return -1;
    *out=valor;return 0;
}
static int parse_hilos(const char *texto,int *hilos)
{
    if (!texto) { *hilos=1; return 0; }
    uint64_t n;
    if (entero_u64(texto,&n) || n<1 || n>1024) return -1;
    *hilos=(int)n;return 0;
}
static int ejecutar_tarea(servidor_t *s,ejecucion_t *e,trabajo_t *t,
                          paso_t *p,const tarea_contexto_t *ctx)
{
    char *a=NULL,*b=NULL,*h=NULL,*out=NULL,*top=NULL;
    int rc=TAREA_FALLO, hilos=1,extras=0;
    if(p->tipo==PASO_SLEEP) {
        a=input_valor(e,t,p,"seconds");double segundos=0;
        if(a && !real_positivo(a,&segundos)) rc=tarea_sleep_controlada(segundos,ctx);
    } else if (p->tipo==PASO_CPU) {
        a=input_valor(e,t,p,"iterations");b=input_valor(e,t,p,"seed");
        h=input_valor(e,t,p,"threads");uint64_t n=0,seed=0,result=0;
        if(a&&b&&!entero_u64(a,&n)&&!entero_u64(b,&seed)&&!parse_hilos(h,&hilos)) {
            extras=motor_reservar_extras(s,hilos);
            if(extras>=0) {
                rc=tarea_cpu_controlada(n,seed,1+extras,&result,ctx);
                (void)motor_devolver_extras(s,extras);
                if(rc==TAREA_OK) {
                    char valor[64];snprintf(valor,sizeof valor,"%" PRIu64,result);
                    if(vars_definir(e->variables,t->indice_global,"CPU_RESULT",valor)) rc=TAREA_FALLO;
                }
            }
        }
    } else if (p->tipo==PASO_WORDCOUNT || p->tipo==PASO_HISTOGRAM) {
        a=input_valor(e,t,p,"file");b=input_valor(e,t,p,"output");
        h=input_valor(e,t,p,"threads");out=input_valor(e,t,p,"bins");
        top=input_valor(e,t,p,"top");uint64_t num=0;
        char *archivo=ruta_trabajo(t,a),*salida=ruta_trabajo(t,b);
        if(archivo && salida && !parse_hilos(h,&hilos)) {
            extras=motor_reservar_extras(s,hilos);
            if(extras>=0) {
                if(p->tipo==PASO_HISTOGRAM) {
                    if(out && !entero_u64(out,&num) && num>=1 && num<=256)
                        rc=tarea_histogram_controlada(archivo,(unsigned)num,1+extras,salida,ctx);
                } else {
                    num=10;
                    if (!top || (!entero_u64(top,&num) && num>=1 && num<=1000000))
                        rc=tarea_wordcount_controlada(archivo,1+extras,salida,(size_t)num,ctx);
                }
                (void)motor_devolver_extras(s,extras);
            }
        }
        free(archivo);free(salida);
    }
    free(a);free(b);free(h);free(out);free(top);
    return rc;
}

estado_t ejecutor_real(servidor_t *s,ejecucion_t *e,trabajo_t *t,void *contexto)
{
    (void)contexto;
    if(!s || !e || !t || !e->plan) return FAILED;
    const etapa_t *et=&e->plan->etapas[t->indice_etapa];
    callback_ctx_t cb={e,et,t};
    unsigned timeout=t->timeout_segundos?t->timeout_segundos:s->timeout_defecto_segundos;
    if(!timeout) timeout=300;
    struct timespec deadline={0};
    (void)clock_gettime(CLOCK_MONOTONIC,&deadline);
    deadline.tv_sec+=(time_t)timeout;
    tarea_contexto_t ctx={.cancelar=&e->cancelar_solicitado,.deadline=deadline};
    int parcial=0;
    for(size_t i=0;i<t->cantidad_pasos;++i) {
        paso_t *p=&t->pasos[i];
        if(atomic_load(&e->cancelar_solicitado)) {vars_publicar(e->variables,t->indice_global);return CANCELED;}
        if(motor_paso_iniciar(s,e,t->indice_global,i)) {vars_publicar(e->variables,t->indice_global);return FAILED;}
        int codigo=-1;
        int resultado=TAREA_FALLO;
        if (p->tipo==PASO_SCRIPT) {
            char *comando=vars_resolver(e->plan,e->variables,t->indice_global,p->comando);
            if(comando) {
                script_opciones_t opciones={.comando=comando,
                    .directorio_trabajo=t->directorio_trabajo,
                    .plazo=&deadline,.cancelar=&e->cancelar_solicitado,
                    .al_linea=linea_script,.contexto=&cb};
                script_resultado_t r={.codigo_salida=-1};
                resultado=ejecutar_script_capturado(&opciones,&r);
                codigo=r.codigo_salida;
                free(comando);
            } else escribir_log(e,et,t,"Variable indefinida o memoria insuficiente");
        } else if(p->tipo>=PASO_SLEEP && p->tipo<=PASO_HISTOGRAM) {
            resultado=ejecutar_tarea(s,e,t,p,&ctx);
            codigo=resultado==TAREA_OK?0:-1;
        } else if(p->tipo==PASO_PUBLICAR || p->tipo==PASO_DESCARGAR) {
            char *ruta=vars_resolver(e->plan,e->variables,t->indice_global,
                p->tipo==PASO_PUBLICAR?p->ruta_publicar:p->ruta_descargar);
            char *archivo=ruta_trabajo(t,ruta);
            if(archivo) {
                int ar=p->tipo==PASO_PUBLICAR?
                    artifacts_publicar(s->directorio,e->id,archivo,p->nombre_artefacto):
                    artifacts_descargar(s->directorio,e->id,p->nombre_artefacto,archivo);
                resultado=ar?TAREA_FALLO:TAREA_OK;
                codigo=ar?-1:0;
            }
            free(ruta);free(archivo);
        }
        estado_t nuevo=resultado==TAREA_CANCELADO?CANCELED:
                       resultado==TAREA_OK?SUCCEEDED:FAILED;
        if(resultado==TAREA_TIMEOUT) escribir_log(e,et,t,"Trabajo vencio su plazo");
        else if(nuevo==FAILED) escribir_log(e,et,t,"Paso finalizo con error");
        (void)motor_paso_finalizar(s,e,t->indice_global,i,nuevo,codigo);
        if(nuevo==CANCELED) {vars_publicar(e->variables,t->indice_global);return CANCELED;}
        if(nuevo==FAILED) {
            if(!p->continuar_si_falla) {vars_publicar(e->variables,t->indice_global);return FAILED;}
            parcial=1;
        }
    }
    vars_publicar(e->variables,t->indice_global);
    return parcial?PARTIALLY_SUCCEEDED:SUCCEEDED;
}
