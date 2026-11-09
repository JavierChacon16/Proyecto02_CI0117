import concurrent.futures
from pathlib import Path
import os
import re
import socket
import subprocess
import tempfile
import time

RAIZ=Path(__file__).resolve().parents[1]


def iniciar(agentes=2, *, entorno=None):
    sock=socket.socket()
    sock.bind(('127.0.0.1',0))
    puerto=sock.getsockname()[1]
    sock.close()
    tmp=tempfile.TemporaryDirectory(prefix='ci0117-integracion-')
    p=subprocess.Popen([str(RAIZ/'pipelined'),str(puerto),str(agentes),tmp.name],
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                       env={**os.environ,**(entorno or {})})
    for _ in range(200):
        if p.poll() is not None:
            out,err=p.communicate()
            raise AssertionError(f'Servidor finalizo: {out} {err}')
        try:
            with socket.create_connection(('127.0.0.1',puerto),.1):
                return p,puerto,tmp
        except OSError:time.sleep(.015)
    p.kill();p.communicate();tmp.cleanup()
    raise AssertionError('Servidor no inicia')


def pedir(puerto,datos,tiempo=5):
    with socket.create_connection(('127.0.0.1',puerto),2) as s:
        s.settimeout(tiempo)
        s.sendall(datos)
        partes=[]
        while True:
            b=s.recv(65536)
            if not b:break
            partes.append(b)
        return b''.join(partes).decode(errors='replace')


def enviar(puerto,yaml):
    if isinstance(yaml,str):yaml=yaml.encode()
    respuesta=pedir(puerto,f'SUBMIT {len(yaml)}\n'.encode()+yaml)
    assert respuesta.startswith('OK '),respuesta
    return int(respuesta.split()[1])


def esperar(puerto,id,plazo=5):
    fin=time.monotonic()+plazo
    while time.monotonic()<fin:
        respuesta=pedir(puerto,f'STATUS {id}\n'.encode())
        primera=respuesta.splitlines()[0]
        assert primera.startswith(f'STATUS {id} '),respuesta
        estado=primera.split()[2]
        if estado not in ('running','queued'):
            return estado,respuesta
        time.sleep(.025)
    raise AssertionError(f'Ejecucion {id} no termino. Ultimo status: {respuesta}')


def terminar(p,puerto,tmp):
    try:
        if p.poll() is None:
            assert pedir(puerto,b'SHUTDOWN\n',7)=='BYE\n'
            out,err=p.communicate(timeout=6)
            assert p.returncode==0,(out,err)
            return err
    finally:
        if p.poll() is None:
            p.kill();p.communicate()
        tmp.cleanup()
    return ''


def base(nombre='demo',script='echo listo'):
    return (f'name: {nombre}\nstages:\n  - stage: hacer\n    jobs:\n'
            f'      - job: ejecutar\n        steps:\n          - script: {script}\n')


def protocolo_y_yaml():
    p,puerto,tmp=iniciar()
    try:
        assert pedir(puerto,b'SUBMIT 0\n').startswith('ERR BAD_REQUEST')
        assert pedir(puerto,b'NOEXISTE\n').startswith('ERR BAD_REQUEST')
        assert pedir(puerto,b'STATUS 999\n').startswith('ERR NOT_FOUND')
        assert pedir(puerto,b'SUBMIT 3\na\x00b').startswith('ERR BAD_REQUEST')
        r=pedir(puerto,b'SUBMIT 19\nname: p\nstages: []\n')
        assert r.startswith('INVALID ') and 'L' in r and r.rstrip().endswith('END'),r
        assert pedir(puerto,b'LIST\n')=='LIST 0\nEND\n'
        print('OK: protocolo, NUL, no encontrados y YAML invalidos con ubicacion')
    finally: terminar(p,puerto,tmp)


def pipeline_real():
    p,puerto,tmp=iniciar()
    try:
        yaml=(RAIZ/'examples/pipeline_demo.yaml').read_bytes()
        n=enviar(puerto,yaml)
        estado,s=esperar(puerto,n)
        assert estado=='succeeded',s
        assert s.count('JOB ')==3 and s.count('STEP ')==3,s
        l=pedir(puerto,f'LOGS {n} - 0\n'.encode())
        assert l.startswith('LOGS ') and l.rstrip().endswith('END'),l
        for fragmento in ('Preparando proyecto','Compilando proyecto','Ejecutando pruebas'):
            assert fragmento in l,l
        print('OK: SUBMIT real, DAG, 3 scripts, STATUS jerarquico y LOGS')
    finally: terminar(p,puerto,tmp)


def variables_artifacts():
    p,puerto,tmp=iniciar()
    try:
        yaml='''name: vars_artefactos
stages:
  - stage: generar
    jobs:
      - job: crear
        steps:
          - script: |
              echo '##setvariable SALUDO=hola'
              printf contenido > dato.txt
          - publish: dato.txt
            artifact: muestra
  - stage: usar
    jobs:
      - job: comprobar
        steps:
          - download: muestra
            path: copia.txt
          - script: |
              cat copia.txt
              echo $(SALUDO)
'''
        n=enviar(puerto,yaml)
        estado,s=esperar(puerto,n)
        assert estado=='succeeded',s
        l=pedir(puerto,f'LOGS {n} - 0\n'.encode())
        assert 'contenido' in l and 'hola' in l,l
        print('OK: setvariable, propagacion por dependencia, publish y download')
    finally: terminar(p,puerto,tmp)


def fallo_y_timeout():
    p,puerto,tmp=iniciar()
    try:
        yaml='''name: continuable
stages:
  - stage: a
    jobs:
      - job: j
        steps:
          - script: exit 3
            continueOnError: true
          - script: echo segunda
'''
        n=enviar(puerto,yaml)
        estado,s=esperar(puerto,n)
        assert estado=='partiallySucceeded',s
        assert 'segunda' in pedir(puerto,f'LOGS {n} - 0\n'.encode())
        yaml='''name: vencer
stages:
  - stage: a
    jobs:
      - job: j
        timeoutInSeconds: 1
        steps:
          - script: sleep 60
'''
        n=enviar(puerto,yaml)
        antes=time.monotonic()
        estado,s=esperar(puerto,n,4)
        assert estado=='failed' and time.monotonic()-antes<3,s
        print('OK: continueOnError parcialmente exitoso y plazo de script')
    finally: terminar(p,puerto,tmp)


def cancelacion():
    p,puerto,tmp=iniciar()
    try:
        yaml='''name: cancelar
stages:
  - stage: largo
    jobs:
      - job: espera
        steps:
          - script: sleep 60
'''
        n=enviar(puerto,yaml)
        fin=time.monotonic()+3
        while time.monotonic()<fin:
            s=pedir(puerto,f'STATUS {n}\n'.encode())
            if s.splitlines()[0].split()[2]=='running':break
            time.sleep(.01)
        antes=time.monotonic()
        r=pedir(puerto,f'CANCEL {n}\n'.encode())
        assert r==f'OK {n}\n',r
        estado,s=esperar(puerto,n,3)
        assert estado=='canceled' and time.monotonic()-antes<1.0,s
        print('OK: CANCEL de script activo, proceso terminado rapidamente')
    finally: terminar(p,puerto,tmp)


def concurrente():
    p,puerto,tmp=iniciar(2)
    try:
        yaml='''name: paralelo
stages:
  - stage: trabajo
    jobs:
      - job: a
        steps:
          - task: Sleep@1
            inputs:
              seconds: 0.3
      - job: b
        steps:
          - task: Sleep@1
            inputs:
              seconds: 0.3
      - job: c
        steps:
          - task: Sleep@1
            inputs:
              seconds: 0.3
'''
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
            ids=list(ex.map(lambda _:enviar(puerto,yaml),range(4)))
        assert len(set(ids))==4,ids
        max_activos=0
        fin=time.monotonic()+6
        while time.monotonic()<fin:
            estados=[];activos=0
            for n in ids:
                r=pedir(puerto,f'STATUS {n}\n'.encode())
                estados.append(r.splitlines()[0].split()[2])
                activos+=sum(' running ' in line for line in r.splitlines()
                             if line.startswith('JOB '))
            max_activos=max(max_activos,activos)
            # STATUS de cada ejecucion se consulta en instantes distintos:
            # sumar sus running NO es una foto atomica global.
            if all(x=='succeeded' for x in estados):break
            time.sleep(.02)
        else:raise AssertionError(estados)
        err=terminar(p,puerto,tmp)
        m=re.search(r'Maximo de agentes simultaneos: (\d+)/(\d+)',err)
        assert m and int(m.group(1))==2 and int(m.group(2))==2,err
        print('OK: 4 pipelines, 12 trabajos, 2 agentes, IDs unicos y limite global')
    finally:
        if p.poll() is None: p.kill();p.communicate()
        tmp.cleanup()


def datos_y_follow():
    p,puerto,tmp=iniciar(2)
    try:
        yaml="""name: tareas
stages:
  - stage: ejecutar
    jobs:
      - job: calculos
        steps:
          - script: printf 'hola hola mundo\\n' > palabras.txt
          - task: Cpu@1
            inputs:
              iterations: 20000
              seed: 17
              threads: 2
          - script: echo CPU=$(CPU_RESULT)
          - task: WordCount@1
            inputs:
              file: palabras.txt
              output: palabras.csv
              top: 3
              threads: 2
          - task: Histogram@1
            inputs:
              file: palabras.txt
              output: hist.csv
              bins: 16
              threads: 2
          - script: |
              cat palabras.csv
              echo Finalizado
"""
        n=enviar(puerto,yaml)
        # --follow debe entregar la salida y finalizar al quedar terminal.
        logs=pedir(puerto,f'LOGS {n} - 1\n'.encode(),tiempo=5)
        estado,s=esperar(puerto,n)
        assert estado=='succeeded',s
        assert 'CPU=' in logs and 'hola' in logs and 'Finalizado' in logs,logs
        assert logs.startswith('LOGS ') and logs.rstrip().endswith('END'),logs
        print('OK: Cpu, WordCount, Histogram y LOGS --follow por TCP')
    finally: terminar(p,puerto,tmp)


def shutdown_activo():
    p,puerto,tmp=iniciar(2)
    try:
        yaml='''name: apagado
stages:
  - stage: a
    jobs:
      - job: lento
        steps:
          - script: sleep 60
'''
        n=enviar(puerto,yaml)
        fin=time.monotonic()+2
        while time.monotonic()<fin:
            if pedir(puerto,f'STATUS {n}\n'.encode()).startswith(f'STATUS {n} running '):break
            time.sleep(.01)
        antes=time.monotonic()
        err=terminar(p,puerto,tmp)
        assert time.monotonic()-antes<4,err
        print('OK: SHUTDOWN con script activo y gracia de dos segundos')
    finally:
        if p.poll() is None:p.kill();p.communicate()
        tmp.cleanup()


def main():
    protocolo_y_yaml()
    pipeline_real()
    variables_artifacts()
    fallo_y_timeout()
    cancelacion()
    concurrente()
    datos_y_follow()
    shutdown_activo()
    print('OK: test_integracion.py - extremo a extremo completo')


if __name__=='__main__':main()
