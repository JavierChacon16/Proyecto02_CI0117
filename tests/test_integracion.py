"""Pruebas de la integracion simulada. Solo biblioteca estandar Python."""
import concurrent.futures
import os
from pathlib import Path
import signal
import select
import threading
import sys
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def iniciar(agentes, directorio, *, binario="pipelined", env=None):
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        puerto = s.getsockname()[1]
    p = subprocess.Popen([str(ROOT/binario), str(puerto), str(agentes), directorio],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    for _ in range(150):
        if p.poll() is not None:
            raise AssertionError(p.communicate())
        try:
            with socket.create_connection(('127.0.0.1', puerto), timeout=.1):
                return p, puerto
        except OSError:
            time.sleep(.02)
    p.kill()
    raise AssertionError('No inicia')

def pedir(puerto, datos):
    with socket.create_connection(('127.0.0.1', puerto), timeout=3) as s:
        s.sendall(datos)
        partes = []
        while True:
            b = s.recv(65536)
            if not b:
                return b''.join(partes).decode()
            partes.append(b)

def enviar(puerto, i):
    yaml = f'name: prueba_{i}\nstages: []\n'.encode()
    respuesta = pedir(puerto, f'SUBMIT {len(yaml)}\n'.encode()+yaml)
    assert respuesta.startswith('OK '), respuesta
    return int(respuesta.split()[1])

def estados(puerto):
    salida = pedir(puerto, b'LIST\n')
    lineas = salida.splitlines()
    assert lineas[-1] == 'END'
    filas = [x.split() for x in lineas if x.startswith('RUN ')]
    assert int(lineas[0].split()[1]) == len(filas)
    return filas

def concurrencia(agentes, d):
    p, port = iniciar(agentes, d)
    lento = socket.create_connection(('127.0.0.1', port), timeout=3)
    lento.sendall(b'SUBMIT 100\nname: parcial')
    try:
        assert pedir(port, b'SUBMIT 0\n').startswith('ERR BAD_REQUEST')
        assert pedir(port, b'NOEXISTE\n').startswith('ERR BAD_REQUEST')
        assert pedir(port, b'STATUS 999\n').startswith('ERR NOT_FOUND')
        assert pedir(port, b'SUBMIT 3\na\x00b').startswith('ERR BAD_REQUEST')
        with socket.create_connection(('127.0.0.1', port), timeout=3) as s:
            s.sendall(b'SUBMIT 10\nabc')
            s.shutdown(socket.SHUT_WR)
            assert s.recv(1024).startswith(b'ERR BAD_REQUEST')
        assert estados(port) == []
        # Probar fragmentacion TCP: la cabecera/payload no llegan juntos.
        with socket.create_connection(('127.0.0.1', port), timeout=3) as s:
            for trozo in (b'SUB', b'MIT 8\n', b'name', b': x\n'):
                s.sendall(trozo)
                time.sleep(.01)
            assert s.recv(1024).startswith(b'OK ')
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            ids = list(pool.map(lambda i: enviar(port, i), range(7)))
        assert len(set(ids)) == 7
        assert len(estados(port)) == 8
        max_running = 0
        deadline = time.monotonic()+22
        while time.monotonic() < deadline:
            filas = estados(port)
            running = sum(f[2] == 'running' for f in filas)
            max_running = max(max_running, running)
            assert running <= agentes
            if all(f[2] == 'succeeded' for f in filas): break
            time.sleep(.025)
        else: raise AssertionError('Trabajos sin completar')
        assert max_running == agentes, max_running
        respuesta = pedir(port, f'STATUS {ids[0]}\n'.encode())
        assert ' succeeded ' in respuesta and respuesta.endswith('END\n')
        assert int(respuesta.splitlines()[0].split()[3]) >= 1900
        cli = subprocess.run([str(ROOT/'pipectl'), '127.0.0.1', str(port), 'list'],
                             capture_output=True, text=True, timeout=3)
        assert cli.returncode == 0 and 'LIST 8' in cli.stdout, (cli.returncode, cli.stdout, cli.stderr)
        assert pedir(port, b'SHUTDOWN\n') == 'BYE\n'
        out, err = p.communicate(timeout=4)
        assert p.returncode == 0, (out, err)
        assert f'Maximo de agentes simultaneos: {agentes}/{agentes}' in err, err
        print(f'OK: {agentes} agentes, 8 ejecuciones, IDs, estados, framing y cliente original')
    finally:
        lento.close()
        if p.poll() is None: p.kill(); p.communicate()

def apagar_ocupado(d, remoto):
    p, port = iniciar(2, d)
    sockets = []
    try:
        for i in range(20): enviar(port, i)
        for datos in (b'', b'SUBMIT 100\nabc', b'SUB'):
            s = socket.create_connection(('127.0.0.1', port), timeout=2)
            s.sendall(datos); sockets.append(s)
        inicio = time.monotonic()
        if remoto: assert pedir(port, b'SHUTDOWN\n') == 'BYE\n'
        else: p.send_signal(signal.SIGINT)
        out, err = p.communicate(timeout=4)
        assert p.returncode == 0, (out, err)
        assert time.monotonic()-inicio < 3.5
        assert 'Maximo de agentes simultaneos: 2/2' in err
        print('OK: apagado ocupado', 'SHUTDOWN' if remoto else 'SIGINT')
    finally:
        for s in sockets: s.close()
        if p.poll() is None: p.kill(); p.communicate()

def finalizar(p):
    if p.poll() is None:
        p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=5)
    assert p.returncode == 0, (out, err)
    return err


def argumentos_invalidos(d):
    for n in ('2000000000', '1025', '0', '-1'):
        p = subprocess.run([str(ROOT/'pipelined'), '8080', n, d],
                           capture_output=True, text=True, timeout=3)
        assert p.returncode != 0 and '1..1024' in p.stderr, p.stderr
    print('OK: agentes fuera de 1..1024 rechazados con mensaje claro')


def conexiones_300(d):
    p, port = iniciar(1, d)
    sockets = []
    try:
        # Mantener las 300 conexiones abiertas; no son 300 consultas sucesivas.
        with concurrent.futures.ThreadPoolExecutor(max_workers=50) as pool:
            sockets = list(pool.map(
                lambda _: socket.create_connection(('127.0.0.1', port), timeout=4),
                range(300)))
        rechazadas = set()
        deadline = time.monotonic() + 4
        while len(rechazadas) < 44 and time.monotonic() < deadline:
            disponibles, _, _ = select.select(
                [s for s in sockets if s not in rechazadas], [], [], .1)
            for s in disponibles:
                datos = s.recv(4096)
                assert datos.startswith(b'ERR BUSY'), datos
                rechazadas.add(s)
        assert len(rechazadas) >= 44, len(rechazadas)
        assert p.poll() is None
        for s in sockets: s.close()
        sockets.clear()
        # Comprobar recuperacion de plazas tras desconectar todos.
        for _ in range(50):
            respuesta = pedir(port, b'LIST\n')
            if respuesta.startswith('LIST'): break
            time.sleep(.02)
        assert respuesta == 'LIST 0\nEND\n', respuesta
        print('OK: 300 conexiones abiertas, ERR BUSY y recuperacion')
    finally:
        for s in sockets: s.close()
        finalizar(p)


def lote_ids(port, cantidad=50):
    barrera = threading.Barrier(cantidad)
    def submit_simultaneo(i):
        yaml = f'name: paralelo_{i}\nstages: []\n'.encode()
        with socket.create_connection(('127.0.0.1', port), timeout=5) as s:
            s.sendall(f'SUBMIT {len(yaml)}\n'.encode() + yaml[:-1])
            barrera.wait(timeout=8)
            s.sendall(yaml[-1:])
            respuesta = b''
            while True:
                datos = s.recv(1024)
                if not datos: break
                respuesta += datos
            assert respuesta.startswith(b'OK '), respuesta
            return int(respuesta.split()[1])
    with concurrent.futures.ThreadPoolExecutor(max_workers=cantidad) as pool:
        return list(pool.map(submit_simultaneo, range(cantidad)))


def ids_concurrentes(d):
    p, port = iniciar(2, d)
    try:
        ids = lote_ids(port)
        assert len(ids) == len(set(ids)) == 50, ids
        filas = estados(port)
        assert len(filas) == 50 and len({f[1] for f in filas}) == 50
        print('OK: 50 SUBMIT concurrentes, IDs unicos')
    finally:
        finalizar(p)


def plazo_total(d):
    p, port = iniciar(1, d)
    sockets = []
    try:
        # Tres ataques en paralelo: linea, payload y plazo compartido.
        for _ in range(3):
            sockets.append(socket.create_connection(('127.0.0.1', port), timeout=3))
        sockets[1].sendall(b'SUBMIT 100\n')
        inicio = time.monotonic()
        pendientes = set(sockets)
        respuestas = {s: b'' for s in sockets}
        proximo = 0
        cambio = False
        while pendientes and time.monotonic() - inicio < 13:
            t = time.monotonic() - inicio
            if t >= proximo and proximo <= 9:
                for s in pendientes:
                    # Tercer cliente: terminar cabecera a los 6 segundos;
                    # el payload NO debe recibir otros diez segundos.
                    if s is sockets[2]:
                        if not cambio:
                            s.sendall(b'SUBMIT 100\n' if t >= 6 else b' ')
                            if t >= 6: cambio = True
                        else: s.sendall(b'x')
                    else: s.sendall(b'x')
                proximo += 3
            disponibles, _, _ = select.select(list(pendientes), [], [], .05)
            for s in disponibles:
                datos = s.recv(4096)
                if datos:
                    respuestas[s] += datos
                else:
                    assert 9 <= time.monotonic()-inicio <= 12, time.monotonic()-inicio
                    assert respuestas[s].startswith(b'ERR TIMEOUT'), respuestas[s]
                    pendientes.remove(s)
        assert not pendientes, 'El envio lento eludio el plazo total'
        assert pedir(port, b'LIST\n') == 'LIST 0\nEND\n'
        print('OK: 1 byte cada 3 s expulsado; plazo de 10 s comun a linea y payload')
    finally:
        for s in sockets: s.close()
        finalizar(p)


def equipo_reducido(d):
    env = dict(os.environ, OMP_THREAD_LIMIT='2')
    p, port = iniciar(4, d, env=env)
    try:
        for i in range(4): enviar(port, i)
        assert sum(f[2] == 'running' for f in estados(port)) <= 2
    finally:
        err = finalizar(p)
    assert 'limite efectivo 2' in err and '/2' in err, err
    print('OK: aviso y limite efectivo con OMP_THREAD_LIMIT=2')


def limite_cola(d, bytes_yaml):
    p, port = iniciar(1, d)
    try:
        yaml = b'name: carga\n' + b'x' * (bytes_yaml - len(b'name: carga\n'))
        hubo_busy = False
        for _ in range(1100 if bytes_yaml < 100 else 280):
            with socket.create_connection(('127.0.0.1', port), timeout=4) as s:
                s.sendall(f'SUBMIT {len(yaml)}\n'.encode())
                # Rechazo por cola llena: el servidor drena el payload.
                # Un cliente bien formado no debe recibir BrokenPipe/RST.
                s.sendall(yaml)
                datos = b''
                while True:
                    parte = s.recv(4096)
                    if not parte: break
                    datos += parte
            if datos.startswith(b'ERR BUSY'):
                hubo_busy = True
                break
            assert datos.startswith(b'OK '), datos
        assert hubo_busy, 'No se rechazo al llenar cola'
        filas = estados(port)
        cola = sum(f[2] == 'queued' for f in filas)
        assert cola <= 1000 and cola * bytes_yaml <= 256 * 1024 * 1024
        assert p.poll() is None
        print('OK: limite de', '1000 ejecuciones' if bytes_yaml < 100 else '256 MiB YAML')
    finally:
        finalizar(p)


def errores_accept(d):
    env = dict(os.environ, TEST_ACCEPT_ERRORS='1',
               LD_PRELOAD=str(ROOT/'build/accept_faults.so'))
    p, port = iniciar(1, d, env=env)
    try:
        t = time.monotonic()
        assert pedir(port, b'LIST\n') == 'LIST 0\nEND\n'
        # Cuatro errores con pausas de 50 ms; iniciar no espera el accept.
        assert time.monotonic()-t >= .12
        assert p.poll() is None
        print('OK: recuperacion de EMFILE, ENFILE, ENOBUFS y ENOMEM en accept')
    finally:
        finalizar(p)


def demostrar_carrera(d):
    # La deteccion por stress es probabilistica. No modifica la version normal.
    for intento in range(1, 11):
        p, port = iniciar(1, d, binario='pipelined-race')
        try:
            ids = lote_ids(port, 100)
            if len(set(ids)) != len(ids):
                print(f'Carrera detectada en intento {intento}: '
                      f'{len(ids)} respuestas, {len(set(ids))} IDs distintos')
                return
        finally:
            finalizar(p)
    raise AssertionError('No se observo la carrera en estos intentos; repetir el stress')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as d:
        if '--race' in sys.argv:
            demostrar_carrera(d)
        else:
            for n in (1, 3): concurrencia(n, d)
            for remoto in (False, True): apagar_ocupado(d, remoto)
            argumentos_invalidos(d)
            conexiones_300(d)
            ids_concurrentes(d)
            plazo_total(d)
            equipo_reducido(d)
            errores_accept(d)
            limite_cola(d, 16)
            limite_cola(d, 1024 * 1024)
    print('Todas las pruebas seleccionadas pasaron.')
