#define _POSIX_C_SOURCE 200809L
#include "artifacts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

int artifacts_inicializar_directorio(const char *dir_base, int id_ejecucion)
{
    if (!dir_base) 
    return -1;

    char ruta[512];
    
    snprintf(ruta, sizeof(ruta), "%s/ejecucion_%d", dir_base, id_ejecucion);
    if (mkdir(ruta, 0700) < 0 && errno != EEXIST) 
    return -1;

    snprintf(ruta, sizeof(ruta), "%s/ejecucion_%d/artifacts", dir_base, id_ejecucion);
    if (mkdir(ruta, 0700) < 0 && errno != EEXIST) 
    return -1;

    return 0;
}

int artifacts_publicar(const char *dir_base, int id_ejecucion, const char *ruta_origen, const char *nombre_artefacto)
{
    if (!dir_base || !ruta_origen || !nombre_artefacto) 
    return -1;

    char ruta_destino[512];
    snprintf(ruta_destino, sizeof(ruta_destino), "%s/ejecucion_%d/artifacts/%s", 
    dir_base, id_ejecucion, nombre_artefacto);

    FILE *src = fopen(ruta_origen, "rb");
    if (!src)
    return -1;

    FILE *dst = fopen(ruta_destino, "wb");
    if (!dst) {
    fclose(src);
    return -1;
    }

    char buffer[8192];
    size_t bytes;
    int error = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
        error = 1;
        break;
        }
    }

    fclose(src);
    fclose(dst);
    return error ? -1 : 0;
}

int artifacts_descargar(const char *dir_base, int id_ejecucion, const char *nombre_artefacto, const char *ruta_destino)
{
    if (!dir_base || !nombre_artefacto || !ruta_destino) 
    return -1;

    char ruta_origen[512];
    snprintf(ruta_origen, sizeof(ruta_origen), "%s/ejecucion_%d/artifacts/%s", 
    dir_base, id_ejecucion, nombre_artefacto);

    FILE *src = fopen(ruta_origen, "rb");
    if (!src) 
    return -1; // download inexistente

    FILE *dst = fopen(ruta_destino, "wb");
    if (!dst) {
    fclose(src);
    return -1;
    }

    char buffer[8192];
    size_t bytes;
    int error = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
        error = 1;
        break;
        }
    }

    fclose(src);
    fclose(dst);
    return error ? -1 : 0;
}