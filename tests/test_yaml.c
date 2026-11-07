#include "yaml_parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int validar(const char *etiqueta, const char *yaml,
                   const char *fragmento_error)
{
    pipeline_t *plan = NULL;
    errores_yaml_t errores = {0};
    int rc = parsear_pipeline_yaml(yaml, strlen(yaml), &plan, &errores);
    int bien = fragmento_error ? (rc == -1 && errores.cantidad > 0 && !plan) :
                                (rc == 0 && plan && errores.cantidad == 0);
    if (bien && fragmento_error) {
        bien=0;
        for (size_t i=0; i<errores.cantidad; ++i) {
            if (strstr(errores.items[i].mensaje,fragmento_error)) bien=1;
            if (!errores.items[i].ubicacion ||
                strchr(errores.items[i].ubicacion,' ') ||
                errores.items[i].ubicacion[0] != 'L') bien=0;
        }
    }
    if (!bien) {
        printf("FALLO %s: rc=%d errores=%zu\n",etiqueta,rc,errores.cantidad);
        for (size_t i=0;i<errores.cantidad;++i)
            printf("  %s: %s\n",errores.items[i].ubicacion,errores.items[i].mensaje);
    }
    liberar_pipeline(plan);
    liberar_errores_yaml(&errores);
    return !bien;
}
static int validar_archivo(const char *ruta, int esperado)
{
    FILE *f=fopen(ruta,"rb");
    if (!f) { perror(ruta); return 1; }
    if (fseek(f,0,SEEK_END)) { fclose(f); return 1; }
    long largo=ftell(f);
    if (largo<0 || fseek(f,0,SEEK_SET)) { fclose(f); return 1; }
    char *contenido=malloc((size_t)largo+1);
    if (!contenido) { fclose(f); return 1; }
    size_t leidos=fread(contenido,1,(size_t)largo,f);
    fclose(f);
    contenido[leidos]='\0';
    pipeline_t *plan=NULL; errores_yaml_t err={0};
    int rc=parsear_pipeline_yaml(contenido,leidos,&plan,&err);
    int bien=(rc==0)==esperado;
    if (bien && esperado) {
        bien=plan->cantidad_etapas==2 && plan->cantidad_trabajos==3 &&
             plan->trabajos[2].cantidad_dependencias==2;
    }
    if (bien && !esperado) {
        bien=err.cantidad && strstr(err.items[0].mensaje,"ciclic");
    }
    if (!bien) {
        printf("FALLO archivo %s: rc=%d errores=%zu\n",ruta,rc,err.cantidad);
        for (size_t i=0;i<err.cantidad;++i)
            printf(" %s %s\n",err.items[i].ubicacion,err.items[i].mensaje);
    }
    liberar_pipeline(plan); liberar_errores_yaml(&err); free(contenido);
    return !bien;
}
static int probar_plan_complejo(void)
{
    const char *yaml =
        "name: pruebas\n"
        "variables:\n  MSG: hola\n"
        "stages:\n"
        "  - stage: preparar\n"
        "    jobs:\n"
        "      - job: crear\n"
        "        steps:\n"
        "          - script: |\n"
        "              echo hola\n"
        "              echo mundo\n"
        "          - publish: resultado.txt\n"
        "            artifact: salida\n"
        "  - stage: usar\n"
        "    dependsOn: [preparar]\n"
        "    condition: always()\n"
        "    jobs:\n"
        "      - job: revisar\n"
        "        steps:\n"
        "          - download: salida\n"
        "            path: descargas\n"
        "          - task: Sleep@1\n"
        "            inputs:\n"
        "              seconds: $(ESPERA)\n"
        "          - task: Cpu@1\n"
        "            inputs:\n"
        "              iterations: 1000\n"
        "              seed: 42\n"
        "      - job: dependiente\n"
        "        dependsOn: revisar\n"
        "        condition: failed()\n"
        "        steps:\n"
        "          - task: Histogram@1\n"
        "            inputs:\n"
        "              file: datos.txt\n"
        "              output: hist.csv\n"
        "              bins: 256\n"
        "              threads: 4\n";
    pipeline_t *plan=NULL; errores_yaml_t errs={0};
    int rc=parsear_pipeline_yaml(yaml,strlen(yaml),&plan,&errs);
    int correcto=rc==0 && plan && plan->cantidad_variables==1 &&
        plan->cantidad_trabajos==3 && plan->trabajos[0].cantidad_pasos==2 &&
        plan->trabajos[0].pasos[0].comando &&
        strcmp(plan->trabajos[0].pasos[0].comando,"echo hola\necho mundo\n")==0 &&
        plan->trabajos[1].cantidad_dependencias==1 &&
        plan->trabajos[2].cantidad_dependencias==2 &&
        plan->trabajos[2].cantidad_dependencias_condicion==1 &&
        plan->trabajos[1].pasos[1].inputs[0].valor &&
        strcmp(plan->trabajos[1].pasos[1].inputs[0].valor,"$(ESPERA)")==0;
    if (!correcto) {
        printf("FALLO plan completo: rc=%d errores=%zu\n",rc,errs.cantidad);
        for(size_t i=0;i<errs.cantidad;++i)
            printf(" %s %s\n",errs.items[i].ubicacion,errs.items[i].mensaje);
    }
    liberar_pipeline(plan); liberar_errores_yaml(&errs);
    return !correcto;
}
static int errores_multiples(void)
{
    char buffer[14000];
    size_t pos=0;
    pos+=(size_t)snprintf(buffer+pos,sizeof buffer-pos,
        "name: prueba\nstages:\n  - stage: e\n    jobs:\n"
        "      - job: x\n        steps:\n          - script: echo ok\n");
    for (int i=0;i<70;++i)
        pos+=(size_t)snprintf(buffer+pos,sizeof buffer-pos,
                              "    invalida%d: %d\n",i,i);
    pipeline_t *plan=NULL; errores_yaml_t errs={0};
    int rc=parsear_pipeline_yaml(buffer,pos,&plan,&errs);
    int correcto=rc==-1 && !plan && errs.cantidad==YAML_MAX_ERRORES &&
                 errs.truncados;
    if (!correcto) printf("FALLO errores multiples: rc=%d total=%zu truncados=%d\n",
                           rc,errs.cantidad,errs.truncados);
    liberar_pipeline(plan); liberar_errores_yaml(&errs);
    return !correcto;
}
/* Corrupciones pequenas deterministas: no debe crashear ni dejar plan parcial. */
static int probar_truncamientos(void)
{
    const char *yaml="name: ejemplo\nstages:\n  - stage: construir\n"
        "    jobs:\n      - job: a\n        steps:\n"
        "          - script: echo listo\n";
    size_t largo=strlen(yaml);
    for (size_t n=0;n<=largo;++n) {
        pipeline_t *plan=NULL;
        errores_yaml_t err={0};
        int rc=parsear_pipeline_yaml(yaml,n,&plan,&err);
        if ((rc==0 && !plan) || (rc!=0 && plan)) {
            liberar_pipeline(plan); liberar_errores_yaml(&err);
            return 1;
        }
        liberar_pipeline(plan); liberar_errores_yaml(&err);
    }
    return 0;
}
static int fuzz_simple(void)
{
    unsigned seed=0x938aa12u;
    for (int intento=0;intento<500;++intento) {
        char texto[140];
        size_t largo=(size_t)(intento%sizeof texto);
        for (size_t j=0;j<largo;++j) {
            seed=1664525u*seed+1013904223u;
            static const char alfabeto[]=" abc:-[]#|\n\t01234a\r";
            texto[j]=alfabeto[(seed>>16)%(sizeof alfabeto-1)];
        }
        pipeline_t *plan=NULL; errores_yaml_t errs={0};
        int rc=parsear_pipeline_yaml(texto,largo,&plan,&errs);
        if (rc==0 && !plan) return 1;
        if (rc!=0 && plan) return 1;
        liberar_pipeline(plan); liberar_errores_yaml(&errs);
    }
    return 0;
}
int main(void)
{
    const char *base="name: ejemplo\nstages:\n  - stage: a\n    jobs:\n"
                     "      - job: x\n        steps:\n          - script: echo hola\n";
    int errores=0;
    errores+=validar("simple",base,NULL);
    errores+=validar_archivo("examples/pipeline_demo.yaml",1);
    errores+=validar_archivo("examples/pipeline_ciclo.yaml",0);
    errores+=probar_plan_complejo();
    errores+=validar("clave desconocida",
        "name: x\nfoo: bar\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - script: true\n",
        "Clave desconocida");
    errores+=validar("nombre duplicado",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - script: true\n"
        "      - job: x\n        steps:\n          - script: true\n",
        "duplicado");
    errores+=validar("dependencia inexistente",
        "name: x\nstages:\n  - stage: a\n    dependsOn: noexiste\n"
        "    jobs:\n      - job: x\n        steps:\n          - script: true\n",
        "inexistente");
    errores+=validar("tarea desconocida",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - task: Fantasma@1\n"
        "            inputs:\n              seconds: 1\n",
        "desconocida");
    errores+=validar("rango seconds",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - task: Sleep@1\n"
        "            inputs:\n              seconds: 86401\n",
        "fuera de rango");
    errores+=validar("rango threads",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - task: WordCount@1\n"
        "            inputs:\n              threads: 1025\n"
        "              file: doc.txt\n              output: res.txt\n",
        "fuera de rango");
    errores+=validar("rango bins",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - task: Histogram@1\n"
        "            inputs:\n              threads: 1\n"
        "              file: doc.txt\n              output: res.txt\n"
        "              bins: 257\n",
        "fuera de rango");
    errores+=validar("ruta insegura",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - publish: ../../secreto\n"
        "            artifact: fuga\n", "insegura");
    errores+=validar("alias",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - script: *externo\n", "Anclas");
    errores+=validar("merge",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - <<: valor\n", "merge");
    errores+=validar("publish duplicado",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - publish: a.txt\n            artifact: data\n"
        "      - job: y\n        steps:\n          - publish: b.txt\n            artifact: data\n", "duplicado");
    errores+=validar("download sin ancestro",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - publish: a.txt\n            artifact: data\n"
        "      - job: y\n        steps:\n          - download: data\n            path: dir\n", "ancestro");
    errores+=validar("truncado",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n          - script: echo hola\n"
        "  - stage: a\n    jobs:\n      - job: x\n", "duplicado");
    errores+=validar("timeout fuera de rango",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        timeoutInSeconds: 86401\n"
        "        steps:\n          - script: true\n",
        "fuera de rango");
    errores+=validar("tabuladores",
        "name: x\nstages:\n\t- stage: a\n    jobs: []\n",
        "Tabulador");
    errores+=validar("ciclo de trabajos",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        dependsOn: y\n"
        "        steps:\n          - script: true\n"
        "      - job: y\n        dependsOn: x\n"
        "        steps:\n          - script: true\n",
        "ciclic");
    errores+=validar("faltan inputs",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n"
        "          - task: Sleep@1\n            inputs: []\n",
        "inputs debe ser un mapa");
    errores+=validar("path interpolado inseguro",
        "name: x\nstages:\n  - stage: a\n    jobs:\n"
        "      - job: x\n        steps:\n"
        "          - publish: ../$(NOMBRE)\n            artifact: data\n",
        "insegura");
    {
        char profundo[1024]; size_t n=0;
        for (int i=0;i<34;++i) {
            for (int j=0;j<i;++j) profundo[n++]=' ';
            profundo[n++]='a';profundo[n++]=':';profundo[n++]='\n';
        }
        profundo[n]='\0';
        errores+=validar("profundidad mayor de 32",profundo,"Profundidad");
    }
    errores+=errores_multiples();
    errores+=fuzz_simple();
    errores+=probar_truncamientos();
    if (errores) { printf("FALLARON %d pruebas de YAML\n",errores); return 1; }
    printf("OK: parser propio, DAG, rangos, errores con ubicacion, artefactos y 500 entradas aleatorias y prefijos truncados\n");
    return 0;
}

