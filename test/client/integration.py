"""Independent wire fixtures plus optional real-server interoperability."""
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

CLIENT = os.path.abspath(sys.argv[1])
SERVER = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
del sys.argv[1:]
HEADER = struct.Struct('!IBBHI')


def frame(data=b'hello', **changes):
    fields = dict(magic=0x53494E46, version=1, kind=2, flags=0, length=len(data))
    fields.update(changes)
    return HEADER.pack(*fields.values()) + data


def exact(sock, count):
    result = b''
    while len(result) < count:
        data = sock.recv(count - len(result))
        if not data:
            raise AssertionError('client disconnected before complete request')
        result += data
    return result


class ClientTests(unittest.TestCase):
    def fake(self, replies, commands=(), timeout=2, fragment=False, drip=False):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        seen, errors = [], []
        def peer():
            try:
                with listener.accept()[0] as sock:
                    sock.settimeout(5)
                    for reply in replies:
                        header = HEADER.unpack(exact(sock, 12))
                        self.assertEqual(header[:4], (0x53494E46, 1, 1, 0))
                        self.assertLessEqual(header[4], 5)
                        seen.append(exact(sock, header[4]))
                        if drip:
                            for byte in reply[:4]:
                                try:
                                    sock.sendall(bytes([byte]))
                                except (BrokenPipeError, ConnectionResetError):
                                    break
                                time.sleep(.4)
                        elif reply is None:
                            time.sleep(1.3)
                        elif fragment:
                            for byte in reply:
                                sock.sendall(bytes([byte]))
                                time.sleep(.001)
                        else:
                            sock.sendall(reply)
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        try:
            run = subprocess.run([CLIENT, '--host', '127.0.0.1', '--port', str(listener.getsockname()[1]),
                                  '--timeout', str(timeout), *commands], capture_output=True, timeout=8)
        finally:
            worker.join(6)
            listener.close()
        self.assertFalse(worker.is_alive(), 'fake peer did not exit')
        if errors: raise errors[0]
        self.assertNotIn(b'AddressSanitizer', run.stderr)
        self.assertNotIn(b'runtime error:', run.stderr)
        return run, seen

    def test_default_hostname(self):
        run, seen = self.fake([frame()])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, b'hello\n')
        self.assertEqual(seen, [b'\1'])

    def test_all_commands_same_connection(self):
        run, seen = self.fake([frame(str(i).encode()) for i in range(6)],
                             ['hostname', 'uname', 'uptime', 'memory', 'loadavg', 'hold', '2'])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(seen, [bytes([i]) for i in range(1, 6)] + [b'\6'+struct.pack('!I', 2)])
        self.assertEqual(run.stdout, b'0\n1\n2\n3\n4\n5\n')

    def test_fragmented_response(self):
        run, _ = self.fake([frame(b'segmented')], fragment=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, b'segmented\n')

    def test_full_payload_boundary(self):
        run, _ = self.fake([frame(b'x' * 4096)])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, b'x' * 4096 + b'\n')

    def test_invalid_response_headers(self):
        for changes in [dict(magic=0), dict(version=2), dict(kind=1), dict(flags=1),
                        dict(length=4097), dict(length=0)]:
            with self.subTest(changes=changes):
                run, _ = self.fake([frame(b'', **changes)])
                self.assertEqual(run.returncode, 1)
                self.assertIn(b'invalid response header', run.stderr)
                self.assertFalse(run.stdout)

    def test_error_response(self):
        run, _ = self.fake([frame(struct.pack('!H', 6)+b'server busy', kind=3)])
        self.assertEqual(run.returncode, 1)
        self.assertIn(b'server error 6: server busy', run.stderr)
        self.assertFalse(run.stdout)

    def test_malformed_error(self):
        run, _ = self.fake([frame(b'\0', kind=3)])
        self.assertEqual(run.returncode, 1)
        self.assertIn(b'invalid error response', run.stderr)

    def test_truncated_response(self):
        for reply in [b'', frame()[:4], frame()[:-1]]:
            run, _ = self.fake([reply])
            self.assertEqual(run.returncode, 1)
            self.assertFalse(run.stdout)

    def test_timeout(self):
        run, _ = self.fake([None], timeout=1)
        self.assertEqual(run.returncode, 1)
        self.assertIn(b'receive header', run.stderr)

    def test_drip_total_deadline(self):
        run, _ = self.fake([frame()], timeout=1, drip=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn(b'receive header', run.stderr)

    def test_cli_errors_and_help(self):
        for args in [['--port', '0'], ['--port', '65536'], ['--timeout', '0'], ['--host', ''],
                     ['--log-level', 'oops'], ['UNKNOWN'], ['HOLD'], ['HOLD', '0'],
                     ['HOLD', '31'], ['HOLD', '-1'], ['HOSTNAME'] * 65]:
            run = subprocess.run([CLIENT, *args], capture_output=True, timeout=3)
            self.assertEqual(run.returncode, 2, (args, run.stderr))
        run = subprocess.run([CLIENT, '--help'], capture_output=True, timeout=3)
        self.assertEqual(run.returncode, 0)
        self.assertIn(b'Usage:', run.stdout)

    def test_connection_refused(self):
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            run = subprocess.run([CLIENT, '--port', str(reserved.getsockname()[1]), '--timeout', '1'],
                                 capture_output=True, timeout=3)
        self.assertEqual(run.returncode, 1)
        self.assertIn(b'connect', run.stderr)

    @unittest.skipUnless(SERVER, 'real server requires Linux server binary')
    def test_real_server(self):
        with tempfile.TemporaryFile() as log:
            server = subprocess.Popen([SERVER, '--port', '0', '--log-level', 'error'],
                                      stdout=subprocess.PIPE, stderr=log)
            try:
                self.assertTrue(select.select([server.stdout], [], [], 5)[0])
                line = server.stdout.readline().decode()
                self.assertTrue(line.startswith('READY port='), line)
                port = line.split()[1].split('=')[1]
                run = subprocess.run([CLIENT, '--host', 'localhost', '--port', port,
                                      'HOSTNAME', 'UNAME', 'UPTIME', 'MEMORY', 'LOADAVG', 'HOLD', '1'],
                                     capture_output=True, timeout=8)
                self.assertEqual(run.returncode, 0, run.stderr)
                lines = run.stdout.splitlines()
                self.assertEqual(len(lines), 6)
                self.assertEqual(lines[0].decode(), socket.gethostname())
                self.assertEqual(lines[-1], b'held_seconds=1')
            finally:
                server.terminate()
                try: server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                server.stdout.close()
            self.assertEqual(server.returncode, 0)
            log.seek(0)
            diagnostics = log.read()
            self.assertNotIn(b'AddressSanitizer', diagnostics)
            self.assertNotIn(b'runtime error:', diagnostics)


unittest.main(verbosity=2)
