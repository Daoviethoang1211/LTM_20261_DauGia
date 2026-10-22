"""Black-box TCP tests with temporary databases and no third-party Python packages."""
import concurrent.futures
import contextlib
import json
from pathlib import Path
import selectors
import socket
import sqlite3
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAX = 65536


def frame(value):
    body = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode()
    return struct.pack("!I", len(body)) + body


def ping(request_id="test", **payload):
    return {"type": "PING", "request_id": request_id, "payload": payload}


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("connection closed inside response")
        data.extend(chunk)
    return data


def response(sock):
    size, = struct.unpack("!I", exact(sock, 4))
    if not 0 < size <= MAX:
        raise AssertionError(f"invalid response length: {size}")
    return json.loads(exact(sock, size))


class Server:
    def __init__(self, directory):
        self.log = open(Path(directory) / "server.log", "ab")
        self.proc = subprocess.Popen(
            [str(ROOT / "build/server"), "127.0.0.1", "0", str(Path(directory) / "test.db")],
            stdout=subprocess.PIPE, stderr=self.log)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.proc.stdout, selectors.EVENT_READ)
                if not selector.select(5):
                    raise RuntimeError("server startup timed out")
            line = self.proc.stdout.readline().decode().strip()
            if not line.startswith("READY "):
                raise RuntimeError(f"server failed to start: {line!r}")
            self.port = int(line.split()[1])
        except BaseException:
            self.close(crash=True)
            raise

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        sock.settimeout(3)
        return sock

    def close(self, crash=False):
        if self.proc.poll() is None:
            self.proc.kill() if crash else self.proc.terminate()
        try:
            code = self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise AssertionError("server did not stop within 5s")
        finally:
            self.proc.stdout.close()
            self.log.close()
        if not crash and code != 0:
            raise AssertionError(f"server exit code {code}")


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ltm-network-")
        cls.server = Server(cls.directory.name)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.server.close()
        finally:
            cls.directory.cleanup()

    def test_three_clients_concurrently(self):
        barrier = threading.Barrier(3)

        def worker(index):
            with self.server.connect() as sock:
                barrier.wait(timeout=3)
                for number in range(20):
                    request_id = f"{index}-{number}"
                    sock.sendall(frame(ping(request_id)))
                    reply = response(sock)
                    self.assertTrue(reply["ok"])
                    self.assertEqual(reply["request_id"], request_id)

        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(worker, range(3)))

    def test_fragmented_header_body_and_vietnamese(self):
        with self.server.connect() as sock:
            for byte in frame(ping("split", text="Đấu giá trực tuyến 🔨")):
                sock.sendall(bytes([byte]))
                time.sleep(0.001)
            self.assertEqual(response(sock)["data"]["echo"]["text"], "Đấu giá trực tuyến 🔨")

    def test_coalesced_frames_and_tiny_response_reads(self):
        with self.server.connect() as sock:
            sock.sendall(b"".join(frame(ping(str(i))) for i in range(30)))
            for i in range(30):
                header = b"".join(exact(sock, 1) for _ in range(4))
                size, = struct.unpack("!I", header)
                body = b"".join(exact(sock, 1) for _ in range(size))
                self.assertEqual(json.loads(body)["request_id"], str(i))

    def test_bad_json_and_requests_recover(self):
        cases = [
            (b"{invalid}", "INVALID_JSON"), (b'{} {}', "INVALID_JSON"),
            (b'{"text":"\xff"}', "INVALID_JSON"), (b'[]', "INVALID_JSON"),
            (b'{"x":1,}', "INVALID_JSON"), (b'{/*comment*/}', "INVALID_JSON"),
            (b'{"x":NaN}', "INVALID_JSON"), (b'{}\x00', "INVALID_JSON"),
            (b'{"x":[Infinity]}', "INVALID_JSON"), (b'{"x":1e999}', "INVALID_JSON"),
            ('{"x":"\\é"}'.encode(), "INVALID_JSON"),
            ('{"x":é}'.encode(), "INVALID_JSON"),
            ({"type": "PING", "payload": {}}, "INVALID_REQUEST"),
            ({"type": "PING", "request_id": "x", "payload": []}, "INVALID_REQUEST"),
            ({"type": "PING\u0000suffix", "request_id": "x", "payload": {}}, "INVALID_REQUEST"),
            ({"type": "OTHER", "request_id": "x", "payload": {}}, "UNKNOWN_COMMAND"),
        ]
        with self.server.connect() as sock:
            for value, code in cases:
                sock.sendall(frame(value))
                reply = response(sock)
                self.assertFalse(reply["ok"], repr(value))
                self.assertEqual(reply["error_code"], code, repr(value))
            sock.sendall(frame(ping()))
            self.assertTrue(response(sock)["ok"])

    def test_invalid_frame_size_closes_only_offending_client(self):
        for length in (0, MAX + 1, 0xFFFFFFFF):
            with self.server.connect() as sock:
                sock.sendall(struct.pack("!I", length))
                self.assertEqual(response(sock)["error_code"], "INVALID_FRAME_SIZE")
                self.assertEqual(sock.recv(1), b"")
        with self.server.connect() as sock:
            sock.sendall(frame(ping()))
            self.assertTrue(response(sock)["ok"])

    def test_utf8_boundaries_and_invalid_sequences(self):
        text = "\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"
        invalid = [b"\x80", b"\xc0\xaf", b"\xe0\x80\xaf", b"\xed\xa0\x80",
                   b"\xf4\x90\x80\x80", b"\xf0\x80\x80\xaf", b"\xe1\xba", b"\xc2A"]
        with self.server.connect() as sock:
            sock.sendall(frame(ping("unicode", text=text)))
            self.assertEqual(response(sock)["data"]["echo"]["text"], text)
            for raw in invalid:
                sock.sendall(frame(b'{"text":"' + raw + b'"}'))
                self.assertEqual(response(sock)["error_code"], "INVALID_JSON")

    def test_maximum_frame(self):
        prefix = b'{"type":"PING","request_id":"max","payload":{"text":"'
        suffix = b'"}}'
        body = prefix + b"a" * (MAX - len(prefix) - len(suffix)) + suffix
        with self.server.connect() as sock:
            sock.sendall(frame(body))
            self.assertEqual(response(sock)["error_code"], "RESPONSE_TOO_LARGE")
            sock.sendall(frame(ping()))
            self.assertTrue(response(sock)["ok"])

    def test_disconnect_mid_frame_and_half_close(self):
        for partial in (b"\x00\x00", struct.pack("!I", 100) + b"{abc"):
            with self.server.connect() as sock:
                sock.sendall(partial)
        with self.server.connect() as sock:
            sock.sendall(frame(ping("half-close")))
            sock.shutdown(socket.SHUT_WR)
            self.assertEqual(response(sock)["request_id"], "half-close")
            self.assertEqual(sock.recv(1), b"")

    def test_slow_reader_does_not_block_healthy_client(self):
        slow = self.server.connect()
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.settimeout(0.1)
        try:
            try:
                slow.sendall(frame(ping("slow", text="a" * 60000)) * 100)
            except (socket.timeout, ConnectionError):
                pass
            with self.server.connect() as healthy:
                healthy.sendall(frame(ping("healthy")))
                self.assertEqual(response(healthy)["request_id"], "healthy")
            time.sleep(11)
            logs = (Path(self.directory.name) / "server.log").read_text()
            self.assertTrue("event=slow_client" in logs or "event=io_timeout" in logs)
        finally:
            slow.close()

    def test_partial_frame_timeout(self):
        with self.server.connect() as sock:
            sock.sendall(b"\x00")
            sock.settimeout(12)
            self.assertEqual(sock.recv(1), b"")

    def test_c_client(self):
        result = subprocess.run(
            [str(ROOT / "build/client"), "127.0.0.1", str(self.server.port),
             json.dumps(ping("c-client", text="Tiếng Việt"), ensure_ascii=False)],
            check=True, capture_output=True, text=True, timeout=5)
        self.assertEqual(json.loads(result.stdout)["request_id"], "c-client")


class StorageTests(unittest.TestCase):
    def test_restart_and_crash_preserve_committed_data(self):
        with tempfile.TemporaryDirectory(prefix="ltm-storage-") as directory:
            for expected in (1, 2, 3):
                server = Server(directory)
                try:
                    with server.connect() as sock:
                        sock.sendall(frame(ping()))
                        self.assertEqual(response(sock)["data"]["server_start_count"], expected)
                finally:
                    server.close(crash=expected == 2)
            with contextlib.closing(sqlite3.connect(Path(directory) / "test.db")) as db:
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 1)
                self.assertEqual(db.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                self.assertEqual(db.execute("SELECT count(*) FROM server_starts").fetchone()[0], 3)

    def test_failed_startup_rolls_back_migration(self):
        with tempfile.TemporaryDirectory(prefix="ltm-rollback-") as directory:
            path = Path(directory) / "test.db"
            with contextlib.closing(sqlite3.connect(path)) as db:
                db.executescript("CREATE TABLE server_starts(id INTEGER PRIMARY KEY);"
                                 "CREATE TRIGGER reject_start BEFORE INSERT ON server_starts "
                                 "BEGIN SELECT RAISE(ABORT, 'test rejection'); END;")
            run = subprocess.run([str(ROOT / "build/server"), "127.0.0.1", "0", str(path)],
                                 capture_output=True, timeout=5)
            self.assertNotEqual(run.returncode, 0)
            with contextlib.closing(sqlite3.connect(path)) as db:
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 0)
                self.assertEqual(db.execute("SELECT count(*) FROM server_starts").fetchone()[0], 0)

    def test_future_schema_is_rejected_without_modification(self):
        with tempfile.TemporaryDirectory(prefix="ltm-future-") as directory:
            path = Path(directory) / "test.db"
            with contextlib.closing(sqlite3.connect(path)) as db:
                db.execute("PRAGMA user_version=99")
            run = subprocess.run([str(ROOT / "build/server"), "127.0.0.1", "0", str(path)],
                                 capture_output=True, timeout=5)
            self.assertNotEqual(run.returncode, 0)
            with contextlib.closing(sqlite3.connect(path)) as db:
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 99)


if __name__ == "__main__":
    unittest.main(verbosity=2)
