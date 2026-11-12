"""Real TCP integration tests. Linux /proc independently verifies fork/reaping."""
import os
import pathlib
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = os.path.abspath(sys.argv[1])
del sys.argv[1:]
HEADER = struct.Struct('!IBBHI')
MAGIC = 0x53494E46


def frame(payload, **changes):
    fields = dict(magic=MAGIC, version=1, kind=1, flags=0, length=len(payload))
    fields.update(changes)
    return HEADER.pack(*fields.values()) + payload


def exact(sock, count):
    data = b''
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise AssertionError('unexpected EOF')
        data += part
    return data


def response(sock):
    magic, version, kind, flags, length = HEADER.unpack(exact(sock, 12))
    assert (magic, version, flags) == (MAGIC, 1, 0)
    assert length <= 4096
    return kind, exact(sock, length)


class Server:
    def __init__(self, *args):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([BINARY, '--port', '0', '--timeout', '2',
                                         '--log-level', 'error', *args],
                                        stdout=subprocess.PIPE, stderr=self.log,
                                        start_new_session=True)
        if not select.select([self.process.stdout], [], [], 5)[0]:
            self.close()
            raise AssertionError('server not ready')
        line = self.process.stdout.readline().decode()
        if not line.startswith('READY port='):
            self.close()
            raise AssertionError('bad startup: ' + line)
        self.port = int(line.split()[1].split('=')[1])

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=5)

    def children(self):
        path = pathlib.Path(f'/proc/{self.process.pid}/task/{self.process.pid}/children')
        return [int(x) for x in path.read_text().split()] if path.exists() else []

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait()
                raise AssertionError('server shutdown timeout')
        self.log.seek(0)
        logs = self.log.read().decode(errors='replace')
        self.log.close()
        self.process.stdout.close()
        assert self.process.returncode == 0, logs
        assert 'AddressSanitizer' not in logs and 'runtime error:' not in logs, logs


def eventually(predicate):
    until = time.monotonic() + 3
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError('condition did not become true')


class Integration(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.addCleanup(self.server.close)

    def healthy(self):
        with self.server.connect() as sock:
            sock.sendall(frame(b'\1'))
            kind, data = response(sock)
            self.assertEqual(kind, 2)
            self.assertTrue(data)

    def error(self, sock, code):
        kind, payload = response(sock)
        self.assertEqual(kind, 3)
        self.assertEqual(struct.unpack('!H', payload[:2])[0], code)
        self.assertTrue(payload[2:])

    def test_queries_pipelined(self):
        with self.server.connect() as sock:
            sock.sendall(b''.join(frame(bytes([c])) for c in range(1, 6)))
            results = [response(sock) for _ in range(5)]
            self.assertTrue(all(kind == 2 and data for kind, data in results))
            self.assertEqual(results[0][1].decode(), socket.gethostname())
            self.assertIn(b'Linux', results[1][1])
            self.assertIn(b'uptime_seconds=', results[2][1])
            self.assertIn(b'total_bytes=', results[3][1])
            self.assertIn(b'load1=', results[4][1])

    def test_fragmented(self):
        with self.server.connect() as sock:
            for byte in frame(b'\1'):
                sock.sendall(bytes([byte]))
                time.sleep(.005)
            self.assertEqual(response(sock)[0], 2)

    def test_invalid_headers(self):
        for changes in [dict(magic=0), dict(version=2), dict(kind=2),
                        dict(flags=1), dict(length=4097)]:
            with self.subTest(changes=changes), self.server.connect() as sock:
                sock.sendall(frame(b'', **changes))
                self.error(sock, 1)
                self.assertEqual(sock.recv(1), b'')
        self.healthy()

    def test_command_errors_keep_connection(self):
        with self.server.connect() as sock:
            for payload, code in [(b'', 3), (b'\xff', 2), (b'\1x', 3),
                                  (b'\6', 3), (b'\6'+struct.pack('!I', 0), 3),
                                  (b'\6'+struct.pack('!I', 31), 3)]:
                sock.sendall(frame(payload))
                self.error(sock, code)
            sock.sendall(frame(b'\1'))
            self.assertEqual(response(sock)[0], 2)

    def test_truncated_frames(self):
        for data in [frame(b'\1')[:5], frame(b'\6\0\0\0\1')[:-2]]:
            with self.server.connect() as sock:
                sock.sendall(data)
                sock.shutdown(socket.SHUT_WR)
                self.error(sock, 1)
        self.healthy()

    def test_idle_timeout(self):
        with self.server.connect() as sock:
            self.error(sock, 4)
            self.assertEqual(sock.recv(1), b'')
        self.healthy()

    def test_drip_deadline(self):
        with self.server.connect() as sock:
            data = frame(b'\1')
            for byte in data[:4]:
                sock.sendall(bytes([byte]))
                time.sleep(.55)
            self.error(sock, 4)
        self.healthy()

    def test_ten_simultaneous_holds(self):
        sockets = [self.server.connect() for _ in range(10)]
        try:
            for sock in sockets:
                sock.sendall(frame(b'\6' + struct.pack('!I', 2)))
            eventually(lambda: len(self.server.children()) == 10)
            for pid in self.server.children():
                self.assertNotEqual(pathlib.Path(f'/proc/{pid}/stat').read_text().split()[2], 'Z')
            start = time.monotonic()
            self.healthy()
            self.assertLess(time.monotonic() - start, 1)
            self.assertFalse(select.select(sockets, [], [], 0)[0])
            for sock in sockets:
                self.assertEqual(response(sock), (2, b'held_seconds=2'))
        finally:
            for sock in sockets: sock.close()
        eventually(lambda: not self.server.children())

    def test_disconnect_and_reap(self):
        for _ in range(20):
            with self.server.connect() as sock:
                sock.sendall(frame(b'\1'))
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        eventually(lambda: not self.server.children())
        self.healthy()

    def test_capacity(self):
        limited = Server('--max-clients', '1')
        try:
            with limited.connect() as first:
                first.sendall(frame(b'\6' + struct.pack('!I', 1)))
                eventually(lambda: len(limited.children()) == 1)
                with limited.connect() as second:
                    self.error(second, 6)
                self.assertEqual(response(first), (2, b'held_seconds=1'))
        finally:
            limited.close()

    def test_shutdown_with_children(self):
        sock = self.server.connect()
        self.addCleanup(sock.close)
        sock.sendall(frame(b'\6' + struct.pack('!I', 30)))
        eventually(lambda: len(self.server.children()) == 1)
        pids = self.server.children()
        self.server.process.terminate()
        self.server.process.wait(timeout=3)
        for pid in pids:
            self.assertFalse(pathlib.Path(f'/proc/{pid}').exists())

    def test_invalid_cli_and_bind(self):
        for args in [('--port', '-1'), ('--timeout', '0'), ('--cache-ttl', 'x'),
                     ('--max-clients', '257'), ('--log-level', 'oops'), ('extra',)]:
            run = subprocess.run([BINARY, *args], capture_output=True, timeout=3)
            self.assertNotEqual(run.returncode, 0)
        run = subprocess.run([BINARY, '--port', str(self.server.port)], capture_output=True, timeout=3)
        self.assertNotEqual(run.returncode, 0)


unittest.main(verbosity=2)
