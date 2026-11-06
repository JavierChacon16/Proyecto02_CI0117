#ifndef MOTOR_H
#define MOTOR_H

/* A1 - API interna del motor. No sustituye los headers congelados v2. */
#include "state.h"

/* El ejecutor se invoca SIN servidor_t.mu, una vez por job. Debe devolver
 * SUCCEEDED, PARTIALLY_SUCCEEDED, FAILED o CANCELED. Para la integracion
 * real debera atender cancelar_solicitado y devolver tras limpiar recursos.
 * Puede informar los pasos mediante motor_paso_iniciar/finalizar. */
typedef estado_t (*motor_ejecutor_trabajo_t)(servidor_t *s, ejecucion_t *e,
                                           trabajo_t *trabajo, void *contexto);

typedef struct {
    servidor_t *servidor;
    motor_ejecutor_trabajo_t ejecutar;
    void *contexto;
} motor_planificador_t;

/* Ejecutable como entrada de pthread_create; crea un solo equipo OpenMP. */
void *motor_planificar_inyectado(void *argumento);
/* planificar(s) (state.h) usa un ejecutor provisional que devuelve FAILED:
 * SUBMIT sigue deshabilitado hasta conectar el ejecutor real en otra tarea. */

/* Publica un plan validado con identidad y ownership del llamador. El motor
 * conserva los punteros hasta que el planificador termina y se han cerrado
 * las consultas; no libera e ni e->plan (el integrador se hace cargo). */
int motor_encolar_ejecucion(servidor_t *s, ejecucion_t *e);
/* Error interno de preparacion antes del despacho: QUEUED -> FAILED.
 * Desencola atomicamente si el job ya era elegible. */
int motor_fallar_trabajo_preparacion(servidor_t *s, ejecucion_t *e,
                                     size_t indice_trabajo);

/* cancelar_ejecucion() esta declarado en state.h: codigos CANCELAR_*. */
void motor_solicitar_apagado(servidor_t *s);
/* Bloquea SIN retener mu durante las esperas; al llegar t0+2s solicita
 * cancelacion atomica de trabajos activos, y espera que terminen.
 * Solo ejecutar fuera de hilos agentes y antes de destruir el servidor. */
int motor_esperar_apagado(servidor_t *s);

/* Se solicita 'hilos_totales' por tarea; devuelve numero de extras reservadas
 * (0..hilos_totales-1), sin esperar, bajo mu. Ejecutar con 1+resultado hilos.
 * Devolver el numero exacto incluso ante error/cancelacion. */
int motor_reservar_extras(servidor_t *s, int hilos_totales);
int motor_devolver_extras(servidor_t *s, int extras);

/* Interfaz opcional para un ejecutor real o falso que informe estados de
 * pasos; 0=aceptado, -1=transicion incorrecta o indices invalidos. */
int motor_paso_iniciar(servidor_t *s, ejecucion_t *e,
                      size_t indice_trabajo, size_t indice_paso);
int motor_paso_finalizar(servidor_t *s, ejecucion_t *e,
                        size_t indice_trabajo, size_t indice_paso,
                        estado_t resultado, int codigo_salida);
#endif
