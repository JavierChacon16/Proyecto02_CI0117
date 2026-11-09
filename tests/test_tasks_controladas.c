#define _POSIX_C_SOURCE 200809L
#include "tasks.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { atomic_int *cancelar; int ms; } cancelador_t;
static void *cancelar_tarde(void *a)
{
    cancelador_t *c = a;
    struct timespec t = {0, (long)c->ms * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
    atomic_store(c->cancelar, 1);
    return NULL;
}
static struct timespec plazo_ms(int ms)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    t.tv_nsec += (long)ms * 1000000L;
    if (t.tv_nsec >= 1000000000L) {t.tv_sec++; t.tv_nsec -= 1000000000L;}
    return t;
}
static double lapso(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec-a.tv_sec) + (double)(b.tv_nsec-a.tv_nsec)/1e9;
}
static int iguales(const char *x, const char *y)
{
    FILE *a=fopen(x,"rb"), *b=fopen(y,"rb");
    if (!a || !b) {if (a)fclose(a);if (b)fclose(b);return 0;}
    int c,d,eq=1;
    do {c=fgetc(a);d=fgetc(b);if(c!=d){eq=0;break;}} while(c!=EOF);
    fclose(a);fclose(b);return eq;
}
int main(void)
{
    atomic_int cancelado;
    atomic_init(&cancelado,0);
    tarea_contexto_t ctx = {.cancelar=&cancelado};
    cancelador_t cambia = {&cancelado,50};
    pthread_t hilo;
    struct timespec ini, fin;
    assert(pthread_create(&hilo,NULL,cancelar_tarde,&cambia)==0);
    clock_gettime(CLOCK_MONOTONIC,&ini);
    assert(tarea_sleep_controlada(60,&ctx)==TAREA_CANCELADO);
    clock_gettime(CLOCK_MONOTONIC,&fin);
    assert(pthread_join(hilo,NULL)==0);
    assert(lapso(ini,fin)<0.2);
    puts("OK: Sleep 60s cancelado <200ms");

    atomic_store(&cancelado,0);
    ctx.deadline=plazo_ms(105);
    clock_gettime(CLOCK_MONOTONIC,&ini);
    assert(tarea_sleep_controlada(60,&ctx)==TAREA_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC,&fin);
    assert(lapso(ini,fin)<0.35);
    puts("OK: Sleep con deadline absoluto");

    ctx.deadline=(struct timespec){0};
    uint64_t referencia=0,r=0;
    for(int h=1;h<=8;h*=2) {
        assert(tarea_cpu_controlada(1234567,7,h,&r,&ctx)==TAREA_OK);
        if(h==1) referencia=r;
        else assert(referencia==r);
    }
    puts("OK: Cpu determinista con 1,2,4,8 hilos");

    atomic_store(&cancelado,1);
    assert(tarea_cpu_controlada(1000000000,7,4,&r,&ctx)==TAREA_CANCELADO);
    atomic_store(&cancelado,0);
    ctx.deadline=plazo_ms(-1);
    assert(tarea_cpu_controlada(1000000000,7,4,&r,&ctx)==TAREA_TIMEOUT);
    ctx.deadline=(struct timespec){0};
    puts("OK: Cpu cancelacion y timeout");

    /* La cancelacion debe cortar una tarea CPU que YA empezo. */
    cancelador_t canc_cpu = {&cancelado, 35};
    assert(pthread_create(&hilo, NULL, cancelar_tarde, &canc_cpu) == 0);
    clock_gettime(CLOCK_MONOTONIC, &ini);
    assert(tarea_cpu_controlada(UINT64_C(4000000000), 99, 4, &r, &ctx)
           == TAREA_CANCELADO);
    clock_gettime(CLOCK_MONOTONIC, &fin);
    assert(pthread_join(hilo, NULL) == 0);
    assert(lapso(ini, fin) < 1.0);
    atomic_store(&cancelado, 0);
    puts("OK: cancelar Cpu durante calculo sin recorrer resto de iteraciones");

    char dir[]="/tmp/ci0117_tasks_XXXXXX";
    assert(mkdtemp(dir));
    char entrada[512],salida[4][512],wc[4][512];
    snprintf(entrada,sizeof entrada,"%s/in.txt",dir);
    FILE *f=fopen(entrada,"wb");
    assert(f);
    const char *contenido="Hola Mundo hola mundo ABC_123  XYZ\n";
    for(int i=0;i<6000;i++)
        assert(fwrite(contenido,1,strlen(contenido),f)==strlen(contenido));
    assert(fclose(f)==0);
    for(int i=0;i<4;i++) {
        int h=1<<i;
        snprintf(salida[i],sizeof salida[i],"%s/h%d.csv",dir,h);
        snprintf(wc[i],sizeof wc[i],"%s/w%d.csv",dir,h);
        assert(tarea_histogram_controlada(entrada,16,h,salida[i],&ctx)==TAREA_OK);
        assert(tarea_wordcount_controlada(entrada,h,wc[i],20,&ctx)==TAREA_OK);
        if(i>0) {assert(iguales(salida[0],salida[i]));assert(iguales(wc[0],wc[i]));}
    }
    puts("OK: Histogram y WordCount byte a byte identicos con 1,2,4,8 hilos");

    atomic_store(&cancelado,1);
    assert(tarea_histogram_controlada(entrada,16,4,salida[0],&ctx)==TAREA_CANCELADO);
    assert(tarea_wordcount_controlada(entrada,4,wc[0],20,&ctx)==TAREA_CANCELADO);
    atomic_store(&cancelado,0);
    ctx.deadline=plazo_ms(-1);
    assert(tarea_histogram_controlada(entrada,16,4,salida[0],&ctx)==TAREA_TIMEOUT);
    assert(tarea_wordcount_controlada(entrada,4,wc[0],20,&ctx)==TAREA_TIMEOUT);
    puts("OK: Histogram y WordCount cancelacion y timeout");

    for(int i=0;i<4;i++){unlink(salida[i]);unlink(wc[i]);}
    unlink(entrada);assert(rmdir(dir)==0);
    puts("OK: test_tasks_controladas Fase 2 completo");
    return 0;
}
