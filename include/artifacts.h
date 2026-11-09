#ifndef ARTIFACTS_H
#define ARTIFACTS_H

#include <stddef.h>

int artifacts_inicializar_directorio(const char *dir_base, int id_ejecucion);
int artifacts_publicar(const char *dir_base, int id_ejecucion, const char *ruta_origen, const char *nombre_artefacto);
int artifacts_descargar(const char *dir_base, int id_ejecucion, const char *nombre_artefacto, const char *ruta_destino);

#endif