# Proyecto 02 - Servidor de pipelines concurrentes

**CI-0117 - Programación Paralela y Concurrente**  
Universidad de Costa Rica, II ciclo de 2026.

**Integrantes:** Jorge Antonio Durán Chinchilla (C02679), Kenneth Fabricio Delgado Cárdenas (C22540) y Javier Chacón Rosales (C32052).

## ¿Qué hace?

El proyecto incluye un servidor (`pipelined`) y un cliente de consola (`pipectl`). El cliente envía pipelines definidos en YAML; el servidor los valida y coordina la ejecución de sus trabajos respetando dependencias y un límite global de agentes OpenMP.

El cliente permite enviar pipelines, consultar estados y bitácoras, cancelar ejecuciones, listar ejecuciones y apagar el servidor.

## Requisitos

- Linux, GCC compatible con C23 o C2x, `make` y soporte de OpenMP y Pthreads.
- Python 3 para las pruebas automatizadas.
- No requiere instalar libyaml: el proyecto utiliza un parser YAML propio para el subconjunto definido.

Abre una terminal en la carpeta que contiene el `Makefile`.

## 1. Compilar

```bash
make clean
make -j4 EXTRA=-Werror
```

Se generan los ejecutables `pipelined` (servidor) y `pipectl` (cliente).

## 2. Ejercutar pipeline avanzados

```bash
bash scripts/ejecutar_pipeline_avanzado.sh
```


Ejecuta el yaml avanzado y genera resultados


## 3. Iniciar el servidor

**Terminal 1:**

```bash
./pipelined 8080 2 ./workspace
```

- `8080`: puerto TCP de ejemplo (puede cambiarse).
- `2`: límite de agentes para los trabajos de todos los pipelines.
- `./workspace`: directorio de trabajo.

Deja esta terminal abierta mientras haces las pruebas.

## 4. Enviar comandos

**Terminal 2:** abre otra terminal en la misma carpeta.

```bash
# Enviar el YAML de ejemplo y recibir un identificador.
./pipectl 127.0.0.1 8080 submit examples/pipeline_demo.yam

# Enviar yaml avanzado
./pipectl 127.0.0.1 8080 submit examples/pipeline_demo_avanzado.yaml

# Consultar las ejecuciones y su estado.
./pipectl 127.0.0.1 8080 list
./pipectl 127.0.0.1 8080 status 1

# Consultar bitácoras; --follow transmite líneas nuevas.
./pipectl 127.0.0.1 8080 logs 1
./pipectl 127.0.0.1 8080 logs 1 --follow

# Cancelar una ejecución que siga en curso.
./pipectl 127.0.0.1 8080 cancel 1

# Apagar el servidor ordenadamente.
./pipectl 127.0.0.1 8080 shutdown
```

**Nota:** el `1` es solo un ejemplo. Debes usar el ID que te devuelva `submit`. Para probar `cancel`, envía un pipeline que aún esté en ejecución. `--follow` puede permanecer abierto hasta que termine.

## 5. Ejecutar las pruebas

**Pruebas completas y compilación sin advertencias:**

```bash
make clean
make -j4 EXTRA=-Werror test
```

**Demostración de condición de carrera con `-DSIN_PROTECCION`:**

```bash
make clean
make -j4 EXTRA=-Werror test-race
```

La versión con protección debe asignar IDs únicos. La versión sin protección expone propuestas de ID repetidas; el motor rechaza las colisiones antes de publicar duplicados.

**Pruebas con AddressSanitizer y UBSan:**

```bash
make clean
ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' \
UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
make -j4 EXTRA='-g -fsanitize=address,undefined -fno-omit-frame-pointer -Werror' test
```

Si las pruebas pasan, el comando termina con código `0`. Tras usar sanitizadores, reconstruye la versión normal:

```bash
make clean
make -j4 EXTRA=-Werror
```

## 6. Limpiar archivos compilados

```bash
make clean
```

Este comando elimina ejecutables y objetos de compilación; no elimina necesariamente el directorio `workspace` ni los artefactos generados por las ejecuciones.

## Consideraciones

- El parser admite el subconjunto YAML desarrollado para el enunciado, no todo el estándar YAML.
- Los scripts deben ser de confianza: el manejo actual de artefactos no cubre todas las medidas de seguridad para rutas y enlaces simbólicos.
- Las mediciones disponibles de Cpu, Histogram y WordCount corresponden a tareas aisladas. Para completar el análisis de rendimiento del enunciado también hacen falta pipelines anchos, profundos y combinados y datos de un segundo ambiente.
