
#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdatomic.h>

/* 0: exito; -1: error o salida no cero; -2: timeout; -3: cancelado.
 * log_ruta: NULL para heredar stdout/stderr, de lo contrario agrega ambos
 * flujos al archivo indicado.
 * cancelar: NULL o bandera atomica cuyo valor 1 solicita cancelacion.
 */
int ejecutar_script_controlado(const char *comando, unsigned timeout_seg,
                              int *salida, const char *log_ruta,
                              const atomic_int *cancelar);

/* Interfaz anterior, conservada para los llamadores existentes. */
int ejecutar_script(const char *comando, unsigned timeout_seg, int *salida);

#endif


