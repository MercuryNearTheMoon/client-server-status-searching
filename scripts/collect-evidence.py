"""Linux-only evidence collection. No screenshots are fabricated."""
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import time

OUT = Path('submission/evidence')
OUT.mkdir(parents=True, exist_ok=True)
HEADER = struct.Struct('!IBBHI')


def frame(command=1, magic=0x53494e46):
    return HEADER.pack(magic, 1, 1, 0, 1) + bytes([command])


def exact(sock, length):
    result = b''
    while len(result) < length:
        data = sock.recv(length-len(result))
        if not data:
            raise AssertionError('unexpected EOF')
        result += data
    return result


def response(sock):
    magic, version, kind, flags, length = HEADER.unpack(exact(sock, 12))
    assert (magic, version, flags) == (0x53494e46, 1, 0) and length <= 4096
    return kind, exact(sock, length)


class Service:
    def __init__(self, name, baseline=False, debug=False):
        self.log = (OUT / (name + '-server.txt')).open('w')
        binary = 'build/evidence/baseline' if baseline else ('build/evidence/debug/server' if debug else 'build/server')
        args = [] if baseline else ['--port', '0', '--timeout', '1', '--log-level', 'error']
        self.proc = subprocess.Popen([binary, *args], stdout=subprocess.PIPE, stderr=self.log,
                                     start_new_session=True)
        assert select.select([self.proc.stdout], [], [], 5)[0], 'server startup timed out'
        ready = self.proc.stdout.readline().decode()
        assert ready.startswith('READY port='), ready
        self.port = int(ready.split()[1].split('=')[1])

    def socket(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=4)

    def children(self):
        pids = Path(f'/proc/{self.proc.pid}/task/{self.proc.pid}/children').read_text().split()
        return {int(pid): Path(f'/proc/{pid}/stat').read_text().split()[2] for pid in pids}

    def close(self):
        self.proc.terminate()
        try: self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            raise AssertionError('cleanup timeout')
        self.proc.stdout.close()
        self.log.close()
        assert self.proc.returncode == 0


def until(predicate):
    end = time.monotonic() + 3
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.02)
    raise AssertionError('observation timeout')


def robustness():
    observations = {}
    for baseline in [True, False]:
        name = 'before' if baseline else 'after'
        service = Service('robustness-' + name, baseline)
        try:
            with service.socket() as sock:
                # Header only: avoid unread payload causing a reset on close.
                sock.sendall(frame(magic=0)[:12])
                if baseline:
                    assert sock.recv(1) == b''
                    invalid = 'connection closed without structured error'
                else:
                    kind, payload = response(sock)
                    assert kind == 3 and payload[:2] == b'\0\1'
                    invalid = 'error code 1: ' + payload[2:].decode()
            # The baseline deliberately keeps completed children until final cleanup.
            with service.socket() as sock:
                start = time.monotonic()
                readable = bool(select.select([sock], [], [], 1.5)[0])
                if baseline:
                    assert not readable
                    idle = 'still waiting after 1.5 seconds (baseline capped at 3 seconds)'
                else:
                    assert readable
                    kind, payload = response(sock)
                    assert kind == 3 and payload[:2] == b'\0\4'
                    idle = f'error code 4 after {time.monotonic()-start:.3f} seconds'
            # Real valid request afterwards confirms service is still usable.
            with service.socket() as sock:
                sock.sendall(frame())
                kind, payload = response(sock)
                assert kind == 2 and payload
            time.sleep(.35)
            children = service.children()
            if baseline: assert any(state == 'Z' for state in children.values())
            else: assert not children
            observations[name] = dict(invalid_frame=invalid, idle=idle,
                                      children=children, subsequent_query='PASS')
        finally: service.close()
    return observations


def run_client(service, commands, binary='build/client', level=None):
    args = [binary, '--port', str(service.port)]
    if level: args += ['--log-level', level]
    environment = dict(os.environ, LD_LIBRARY_PATH=str(Path(binary).resolve().parent/'lib'))
    return subprocess.run([*args, *commands], capture_output=True, text=True, timeout=8, env=environment)


def concurrency_and_capture():
    # Longer timeout avoids idle expiration while we inspect the established sockets.
    service = Service('capture')
    clients = []
    capture_log = (OUT / 'tcpdump-capture.txt').open('w+')
    capture = subprocess.Popen(['tcpdump', '-i', 'lo', '-nn', '-s', '0', '-U', '-w',
                                str(OUT/'capture.pcap'), f'tcp port {service.port}'],
                               stdout=capture_log, stderr=capture_log)
    try:
        end = time.monotonic() + 5
        while time.monotonic() < end:
            capture_log.flush()
            if 'listening on' in (OUT/'tcpdump-capture.txt').read_text(): break
            if capture.poll() is not None: raise AssertionError('tcpdump could not start')
            time.sleep(.05)
        else: raise AssertionError('tcpdump not ready')
        result = run_client(service, ['HOSTNAME', 'UNAME', 'UPTIME', 'MEMORY', 'LOADAVG'])
        assert result.returncode == 0, result.stderr
        (OUT/'queries.txt').write_text(result.stdout)
        until(lambda: not service.children())
        for _ in range(10):
            clients.append(subprocess.Popen(['build/client', '--port', str(service.port), 'HOLD', '3'],
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        until(lambda: len(service.children()) == 10)
        states = service.children()
        assert all(state != 'Z' for state in states.values())
        assert all(client.poll() is None for client in clients)
        ps = subprocess.check_output(['ps', '-o', 'pid,ppid,stat,args', '-p',
                                      ','.join(map(str, [service.proc.pid, *states]))], text=True)
        connections = subprocess.check_output(['ss', '-tnp', 'state', 'established',
                                               f'( sport = :{service.port} )'], text=True)
        assert len(connections.strip().splitlines()) - 1 == 10, connections
        start = time.monotonic()
        query = run_client(service, ['HOSTNAME'])
        latency = time.monotonic()-start
        assert query.returncode == 0 and latency < 1, query.stderr
        assert all(client.poll() is None for client in clients)
        results = []
        for client in clients:
            stdout, stderr = client.communicate(timeout=6)
            assert client.returncode == 0 and stdout.strip() == 'held_seconds=3', stderr
            results.append(stdout.strip())
        until(lambda: not service.children())
        (OUT/'concurrency.txt').write_text(ps + '\n' + connections +
            f'\nQuery during 10 HOLD clients: {query.stdout.strip()} ({latency:.4f}s)\n' +
            '\n'.join(f'client {i+1}: {result}' for i, result in enumerate(results)) +
            '\nAll children reaped: PASS\n')
        # Include a deliberate RST for packet analysis.
        with service.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        time.sleep(1.2)  # Allow tcpdump's capture buffer to drain before SIGINT.
        return dict(port=service.port, server_pid=service.proc.pid, child_pids=list(states),
                    concurrent_connections=10, query_latency_seconds=latency)
    finally:
        for client in clients:
            if client.poll() is None: client.kill(); client.wait()
        if capture.poll() is None:
            capture.send_signal(signal.SIGINT)
            capture.wait(timeout=5)
        capture_log.close()
        service.close()


def debug_matrix():
    service = Service('debug-matrix')
    records = []
    try:
        for compile_debug in [0, 1]:
            for runtime in ['info', 'debug']:
                # Both builds have compile log ceiling 3, isolating ENABLE_DEBUG.
                binary = f'build/evidence/debug{compile_debug}/client'
                result = run_client(service, ['HOSTNAME'], binary, runtime)
                assert result.returncode == 0, result.stderr
                present = '[DEBUG]' in result.stderr
                assert present == (compile_debug == 1 and runtime == 'debug')
                records.append(f'ENABLE_DEBUG={compile_debug} runtime={runtime} DEBUG_present={present}\n'
                               + result.stderr + 'stdout: ' + result.stdout)
        (OUT/'debug-matrix.txt').write_text('\n'.join(records))
    finally: service.close()


if __name__ == '__main__':
    info = {'timestamp_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            'robustness': robustness(), 'concurrency': concurrency_and_capture()}
    debug_matrix()
    (OUT/'observations.json').write_text(json.dumps(info, indent=2)+'\n')
    print(json.dumps(info, indent=2))
