
#ifndef SCRIPT_H
#define SCRIPT_H

/* Ejecuta un comando de shell.
 *  0: termina correctamente.
 * -1: error o codigo de salida distinto de cero.
 * -2: supera el tiempo limite.
 * timeout_seg igual a 0 significa sin limite.
 */
int ejecutar_script(const char *comando, unsigned int timeout_seg,
                   int *salida);

#endif

