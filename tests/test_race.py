"""Demuestra la carrera en siguiente_id sin permitir IDs duplicados en el motor.
La prueba normal exige IDs unicos. La version desprotegida registra los IDs
propuestos antes de encolar; esos duplicados son evidencia de la carrera.
"""
import concurrent.futures
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import threading
import os

RAIZ=Path(__file__).resolve().parents[1]
YAML=b'name: carrera\nstages:\n  - stage: construccion\n    jobs:\n      - job: trabajo\n        steps:\n          - task: Sleep@1\n            inputs:\n              seconds: 0.05\n'

def iniciar(binario):
    sock=socket.socket();sock.bind(('127.0.0.1',0));puerto=sock.getsockname()[1];sock.close()
    d=tempfile.TemporaryDirectory(prefix='ci0117-race-')
    p=subprocess.Popen([str(RAIZ/binario),str(puerto),'2',d.name],
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                       env={**os.environ,'OMP_THREAD_LIMIT':'4'})
    for _ in range(150):
        if p.poll() is not None: raise AssertionError(f'No inicia {binario}')
        try:
            with socket.create_connection(('127.0.0.1',puerto),.1):return p,puerto,d
        except OSError:time.sleep(.02)
    p.kill();p.communicate();raise AssertionError(f'No inicia {binario}')

def pedir(port,datos):
    with socket.create_connection(('127.0.0.1',port),4) as s:
        s.settimeout(8);s.sendall(datos)
        salida=b''
        while True:
            b=s.recv(4096)
            if not b:break
            salida+=b
        return salida.decode(errors='replace').strip()

def ejecutar(binario):
    p,port,tmp=iniciar(binario)
    try:
        ids=[];errores=[]
        for tanda in range(4):
            barrera=threading.Barrier(25)
            def enviar(_):
                barrera.wait(timeout=6)
                return pedir(port,f'SUBMIT {len(YAML)}\n'.encode()+YAML)
            with concurrent.futures.ThreadPoolExecutor(max_workers=25) as pool:
                respuestas=list(pool.map(enviar,range(25)))
            for r in respuestas:
                if re.fullmatch(r'OK \d+',r):ids.append(int(r.split()[1]))
                else:errores.append(r)
        assert pedir(port,b'SHUTDOWN\n')=='BYE'
        out,err=p.communicate(timeout=7)
        assert p.returncode==0,(out,err)
        return ids,errores,err
    finally:
        if p.poll() is None:p.kill();p.communicate()
        tmp.cleanup()

def main():
    ids,errores,_=ejecutar('pipelined')
    assert len(ids)==100 and len(set(ids))==100,(len(ids),errores)
    print('OK: version protegida, 100 SUBMIT reales e IDs unicos')
    ids,errores,log=ejecutar('pipelined-race')
    propuestas=[int(x) for x in re.findall(r'SIN_PROTECCION ID_PROPUESTO (\d+)',log)]
    duplicadas=len(propuestas)-len(set(propuestas))
    assert len(propuestas)==100,(len(propuestas),log[:1000])
    assert duplicadas>0,(duplicadas,errores)
    assert len(set(ids))==len(ids), 'El motor publico un ID duplicado'
    print(f'OK: SIN_PROTECCION, {duplicadas} propuestas de ID repetidas; '
          f'{len(errores)} encolados rechazados. Motor no publica duplicados.')

if __name__=='__main__':main()
