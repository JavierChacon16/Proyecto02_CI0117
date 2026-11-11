#!/usr/bin/env bash
# Demostracion avanzada CI-0117: ejecuta un unico YAML en servidor local,
# comprueba el estado final y guarda archivos, logs y estado en resultados/.
set -euo pipefail

cd "$(dirname "$0")/.."
PUERTO="${PUERTO:-18987}"
AGENTES="${AGENTES:-2}"
YAML="examples/pipeline_demo_avanzado.yaml"
FECHA="$(date +%Y%m%d_%H%M%S)"
SALIDA="resultados/demo_avanzado_${FECHA}_$$"
mkdir -p "$SALIDA/workspace"

if [[ ! -x ./pipelined || ! -x ./pipectl ]]; then
    echo 'Compilando programas antes de la prueba...'
    make -j4 EXTRA=-Werror
fi

./pipelined "$PUERTO" "$AGENTES" "$SALIDA/workspace" \
    > "$SALIDA/servidor.log" 2>&1 &
PID_SERVIDOR=$!

cerrar_servidor() {
    if kill -0 "$PID_SERVIDOR" 2>/dev/null; then
        ./pipectl 127.0.0.1 "$PUERTO" shutdown > "$SALIDA/shutdown.txt" 2>&1 || true
        # Dar oportunidad al apagado ordenado antes de forzar el cierre.
        for _ in $(seq 1 50); do
            if ! kill -0 "$PID_SERVIDOR" 2>/dev/null; then break; fi
            sleep 0.1
        done
        if kill -0 "$PID_SERVIDOR" 2>/dev/null; then
            kill "$PID_SERVIDOR" 2>/dev/null || true
        fi
        wait "$PID_SERVIDOR" 2>/dev/null || true
    fi
}
trap cerrar_servidor EXIT

LISTO=0
for _ in $(seq 1 100); do
    if grep -q 'Servidor escuchando' "$SALIDA/servidor.log"; then
        LISTO=1
        break
    fi
    if ! kill -0 "$PID_SERVIDOR" 2>/dev/null; then
        echo 'Error: el servidor termino antes de aceptar conexiones.' >&2
        cat "$SALIDA/servidor.log" >&2
        exit 1
    fi
    sleep 0.1
done
if [[ "$LISTO" -ne 1 ]]; then
    echo 'Error: no se pudo confirmar el inicio del servidor.' >&2
    cat "$SALIDA/servidor.log" >&2
    exit 1
fi

./pipectl 127.0.0.1 "$PUERTO" submit "$YAML" > "$SALIDA/submit.txt"
cat "$SALIDA/submit.txt"
ID="$(awk '$1 == "OK" && $2 ~ /^[0-9]+$/ {print $2; exit}' "$SALIDA/submit.txt")"
if [[ -z "$ID" ]]; then
    echo 'Error: SUBMIT no devolvio un identificador.' >&2
    exit 1
fi

ESTADO=''
for _ in $(seq 1 200); do
    ./pipectl 127.0.0.1 "$PUERTO" status "$ID" > "$SALIDA/status.txt"
    ESTADO="$(awk 'NR==1 && $1=="STATUS" {print $3}' "$SALIDA/status.txt")"
    case "$ESTADO" in
        succeeded|failed|partiallySucceeded|canceled|skipped) break ;;
    esac
    sleep 0.1
done
./pipectl 127.0.0.1 "$PUERTO" list > "$SALIDA/list.txt"
./pipectl 127.0.0.1 "$PUERTO" logs "$ID" > "$SALIDA/bitacora.txt"
cp "$YAML" "$SALIDA/pipeline_ejecutado.yaml"

if [[ "$ESTADO" != 'succeeded' ]]; then
    echo "Error: el pipeline termino en estado '$ESTADO'." >&2
    cat "$SALIDA/status.txt" >&2
    echo "Revisa $SALIDA/bitacora.txt y $SALIDA/servidor.log" >&2
    exit 1
fi

CARPETA="$SALIDA/workspace/run_${ID}/resultados/consolidar/resultados"
for F in resumen.txt palabras.txt histograma.csv cpu.txt auditoria.txt; do
    if [[ ! -s "$CARPETA/$F" ]]; then
        echo "Error: falta o esta vacio $CARPETA/$F" >&2
        exit 1
    fi
    cp "$CARPETA/$F" "$SALIDA/$F"
done
if ! grep -q 'Curso: CI0117' "$SALIDA/resumen.txt" || \
   ! grep -q 'Auditoria correcta' "$SALIDA/auditoria.txt"; then
    echo 'Error: contenido de los resultados inesperado.' >&2
    exit 1
fi

printf '\nPRUEBA CORRECTA: %s\n' "$ESTADO"
printf 'ID de ejecucion: %s\n' "$ID"
printf 'Resultados guardados en: %s/\n' "$SALIDA"
printf 'Contenido principal:\n'
ls -1 "$SALIDA" | grep -E '^(resumen\.txt|palabras\.txt|histograma\.csv|cpu\.txt|auditoria\.txt|status\.txt|bitacora\.txt)$'
