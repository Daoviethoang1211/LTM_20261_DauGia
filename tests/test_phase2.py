import contextlib
import json
import os
from pathlib import Path
import socket
import selectors
import signal
import sqlite3
import subprocess
import tempfile
import time
import pty
import termios
import unittest
from integration import Server, frame, response, ROOT

PASSWORD = "demo-password-123"


class Peer:
    def __init__(self, server):
        self.sock = server.connect()
        self.sequence = 0
        self.events = []

    def close(self):
        self.sock.close()

    def call(self, command, **payload):
        self.sequence += 1
        rid = str(self.sequence)
        self.sock.sendall(frame({"type": command, "request_id": rid, "payload": payload}))
        while True:
            reply = response(self.sock)
            if "event" in reply:
                self.events.append(reply)
                continue
            if reply.get("request_id") != rid:
                raise AssertionError(reply)
            return reply

    def ok(self, command, **payload):
        reply = self.call(command, **payload)
        if not reply["ok"]:
            raise AssertionError(reply)
        return reply["data"]

    def register_login(self, username):
        self.ok("REGISTER", username=username, password=PASSWORD)
        return self.ok("LOGIN", username=username, password=PASSWORD)


class Phase2Tests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ltm-phase2-")
        self.server = Server(self.directory.name)
        self.peers = []

    def tearDown(self):
        for peer in self.peers:
            peer.close()
        try:
            self.server.close()
        finally:
            self.directory.cleanup()

    def peer(self, username=None):
        peer = Peer(self.server)
        self.peers.append(peer)
        if username:
            peer.register_login(username)
        return peer

    def item(self, peer, room_id, title="Vật phẩm đấu giá", **overrides):
        payload = dict(room_id=room_id, title=title, description="Mô tả tiếng Việt",
                       starting_price_vnd=100000, buy_now_price_vnd=500000,
                       planned_start_at=int(time.time()) + 86400, duration_sec=60)
        payload.update(overrides)
        return peer.ok("ITEM_CREATE", **payload)["item_id"]

    def room(self, owner, publish=True):
        room_id = owner.ok("ROOM_CREATE", name="Phòng demo", description="Danh mục công bố")["id"]
        item_id = self.item(owner, room_id)
        if publish:
            owner.ok("ROOM_PUBLISH", room_id=room_id)
        return room_id, item_id

    def denied(self, peer, command, code, **payload):
        reply = peer.call(command, **payload)
        self.assertFalse(reply["ok"], reply)
        self.assertEqual(reply["error_code"], code)

    def test_accounts_password_change_and_duplicate_login(self):
        first = self.peer()
        self.denied(first, "ROOM_LIST", "AUTH_REQUIRED")
        self.denied(first, "REGISTER", "INVALID_REQUEST", username="bad name", password=PASSWORD)
        first.register_login("alice")
        second = self.peer()
        self.denied(second, "REGISTER", "USERNAME_TAKEN", username="alice", password=PASSWORD)
        self.denied(second, "LOGIN", "INVALID_CREDENTIALS", username="alice", password="wrong-password")
        self.denied(second, "LOGIN", "ALREADY_LOGGED_IN", username="alice", password=PASSWORD)
        self.denied(first, "PASSWORD_CHANGE", "INVALID_CREDENTIALS", old_password="wrong-password", new_password="new-password-123")
        first.ok("PASSWORD_CHANGE", old_password=PASSWORD, new_password="new-password-123")
        first.ok("LOGOUT")
        self.denied(second, "LOGIN", "INVALID_CREDENTIALS", username="alice", password=PASSWORD)
        second.ok("LOGIN", username="alice", password="new-password-123")
        with contextlib.closing(sqlite3.connect(Path(self.directory.name) / "test.db")) as db:
            hashed = db.execute("SELECT password_hash FROM users").fetchone()[0]
            self.assertTrue(hashed.startswith("$argon2id$"))
            self.assertNotIn("password", hashed)
        logs = (Path(self.directory.name) / "server.log").read_text()
        self.assertNotIn(PASSWORD, logs)
        self.assertNotIn("new-password-123", logs)

    def test_draft_privacy_and_catalog_permissions(self):
        owner, guest = self.peer("owner"), self.peer("guest")
        room_id, item_id = self.room(owner, publish=False)
        self.assertEqual(guest.ok("ROOM_LIST")["rooms"], [])
        for command in ("ROOM_DETAIL", "ROOM_JOIN", "ROOM_SNAPSHOT", "ITEM_LIST", "ROOM_PUBLISH"):
            self.denied(guest, command, "NOT_FOUND", room_id=room_id)
        self.denied(guest, "ITEM_DELETE", "NOT_FOUND", room_id=room_id, item_id=item_id)
        owner.ok("ROOM_PUBLISH", room_id=room_id)
        self.assertEqual(len(guest.ok("ROOM_LIST")["rooms"]), 1)
        self.assertEqual(guest.ok("ITEM_LIST", room_id=room_id)["items"][0]["title"], "Vật phẩm đấu giá")
        self.denied(guest, "ITEM_DELETE", "FORBIDDEN", room_id=room_id, item_id=item_id)
        self.denied(owner, "ITEM_DELETE", "ROOM_LOCKED", room_id=room_id, item_id=item_id)
        self.denied(owner, "ROOM_PUBLISH", "ROOM_LOCKED", room_id=room_id)

    def test_item_update_delete_reorder_and_atomic_validation(self):
        owner = self.peer("owner")
        room_id, first = self.room(owner, publish=False)
        second = self.item(owner, room_id, title="Thứ hai")
        third = self.item(owner, room_id, title="Thứ ba")
        owner.ok("ITEM_REORDER", room_id=room_id, item_ids=[third, first, second])
        before = owner.ok("ITEM_LIST", room_id=room_id)["items"]
        self.assertEqual([i["id"] for i in before], [third, first, second])
        self.denied(owner, "ITEM_REORDER", "INVALID_REQUEST", room_id=room_id, item_ids=[first, first, third])
        self.assertEqual(owner.ok("ITEM_LIST", room_id=room_id)["items"], before)
        owner.ok("ITEM_UPDATE", room_id=room_id, item_id=first, title="Đã sửa", description="",
                 starting_price_vnd=200000, buy_now_price_vnd=700000,
                 planned_start_at=int(time.time()) + 86400, duration_sec=90)
        owner.ok("ITEM_DELETE", room_id=room_id, item_id=second)
        self.assertEqual(len(owner.ok("ITEM_LIST", room_id=room_id)["items"]), 2)
        other, other_item = self.room(owner, publish=False)
        self.denied(owner, "ITEM_DELETE", "NOT_FOUND", room_id=room_id, item_id=other_item)
        self.assertEqual(len(owner.ok("ITEM_LIST", room_id=other)["items"]), 1)

    def test_publish_validation_and_item_bounds(self):
        owner = self.peer("owner")
        room_id = owner.ok("ROOM_CREATE", name="Empty", description="")["id"]
        self.denied(owner, "ROOM_PUBLISH", "INVALID_CATALOG", room_id=room_id)
        self.item(owner, room_id, planned_start_at=int(time.time()) - 10)
        self.denied(owner, "ROOM_PUBLISH", "INVALID_CATALOG", room_id=room_id)
        for amount in (-1, 1.5, True, 10**16):
            self.denied(owner, "ITEM_CREATE", "INVALID_REQUEST", room_id=room_id, title="Bad", description="",
                        starting_price_vnd=amount, buy_now_price_vnd=999999,
                        planned_start_at=int(time.time()) + 3600, duration_sec=60)
        self.assertEqual(owner.ok("ROOM_DETAIL", room_id=room_id)["room"]["status"], "DRAFT")

    def test_membership_events_snapshot_and_single_room(self):
        owner, guest, other = self.peer("owner"), self.peer("guest"), self.peer("other")
        room_id, _ = self.room(owner)
        second, _ = self.room(owner)
        guest.ok("ROOM_DETAIL", room_id=second)  # Preview never occupies a room.
        joined = guest.ok("ROOM_JOIN", room_id=room_id)
        self.assertEqual(len(joined["members"]), 1)
        other.ok("ROOM_JOIN", room_id=room_id)
        snap = guest.ok("ROOM_SNAPSHOT", room_id=room_id)
        self.assertEqual(len(snap["members"]), 2)
        self.assertEqual(guest.events[-1]["event"], "MEMBER_JOINED")
        self.assertGreater(guest.events[-1]["version"], joined["room"]["version"])
        self.denied(guest, "ROOM_JOIN", "ALREADY_IN_ROOM", room_id=second)
        other.ok("LOGOUT")
        self.assertEqual(len(guest.ok("ROOM_SNAPSHOT", room_id=room_id)["members"]), 1)
        self.assertEqual(guest.events[-1]["event"], "MEMBER_LEFT")
        guest.ok("ROOM_LEAVE")
        guest.ok("ROOM_JOIN", room_id=second)

    def test_disconnect_releases_login_and_membership(self):
        owner, guest = self.peer("owner"), self.peer("guest")
        room_id, _ = self.room(owner)
        owner.ok("ROOM_JOIN", room_id=room_id)
        guest.ok("ROOM_JOIN", room_id=room_id)
        guest.close()
        retry = self.peer()
        for _ in range(20):
            result = retry.call("LOGIN", username="guest", password=PASSWORD)
            if result["ok"]:
                break
            time.sleep(0.05)
        self.assertTrue(result["ok"], result)
        self.assertEqual(len(owner.ok("ROOM_SNAPSHOT", room_id=room_id)["members"]), 1)
        self.assertEqual(owner.events[-1]["event"], "MEMBER_LEFT")

    def test_pipelined_auth_keeps_response_order(self):
        peer = self.peer()
        peer.sock.sendall(frame({"type": "REGISTER", "request_id": "a", "payload": {"username": "alice", "password": PASSWORD}})
                          + frame({"type": "LOGIN", "request_id": "b", "payload": {"username": "alice", "password": PASSWORD}})
                          + frame({"type": "ROOM_CREATE", "request_id": "c", "payload": {"name": "Pipeline", "description": ""}}))
        for rid in ("a", "b", "c"):
            result = response(peer.sock)
            self.assertEqual(result["request_id"], rid)
            self.assertTrue(result["ok"], result)

    def test_restart_preserves_catalog_but_not_sessions(self):
        owner = self.peer("owner")
        room_id, item_id = self.room(owner)
        owner.ok("ROOM_JOIN", room_id=room_id)
        self.server.close()
        self.server = Server(self.directory.name)
        fresh = self.peer()
        self.denied(fresh, "ROOM_LIST", "AUTH_REQUIRED")
        fresh.ok("LOGIN", username="owner", password=PASSWORD)
        snap = fresh.ok("ROOM_DETAIL", room_id=room_id)
        self.assertEqual(snap["items"][0]["id"], item_id)
        self.assertEqual(snap["members"], [])
        self.assertEqual(snap["room"]["status"], "PUBLISHED")

    def test_phase1_database_upgrade_preserves_existing_rows(self):
        with tempfile.TemporaryDirectory(prefix="ltm-v1-") as directory:
            path = Path(directory) / "test.db"
            with contextlib.closing(sqlite3.connect(path)) as db:
                db.executescript("CREATE TABLE server_starts(id INTEGER PRIMARY KEY, started_at TEXT DEFAULT 'old');"
                                 "INSERT INTO server_starts(id) VALUES(7); PRAGMA user_version=1;")
            server = Server(directory)
            server.close()
            with contextlib.closing(sqlite3.connect(path)) as db:
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)
                self.assertEqual(db.execute("SELECT started_at FROM server_starts WHERE id=7").fetchone()[0], "old")

    def test_two_simultaneous_logins_only_one_wins(self):
        owner = self.peer("alice")
        owner.ok("LOGOUT")
        first, second = self.peer(), self.peer()
        request = {"type": "LOGIN", "request_id": "race", "payload": {"username": "alice", "password": PASSWORD}}
        first.sock.sendall(frame(request))
        second.sock.sendall(frame(request))
        replies = [response(p.sock) for p in (first, second)]
        self.assertEqual(sum(r["ok"] for r in replies), 1)
        self.assertEqual([r["error_code"] for r in replies if not r["ok"]], ["ALREADY_LOGGED_IN"])

    def test_auth_queue_is_bounded_and_does_not_block_ping(self):
        peers = [self.peer() for _ in range(32)]
        for index, peer in enumerate(peers):
            peer.sock.settimeout(10)
            peer.sock.sendall(frame({"type": "REGISTER", "request_id": str(index),
                                      "payload": {"username": f"queued_{index}", "password": PASSWORD}}))
        healthy = self.peer()
        started = time.monotonic()
        self.assertEqual(healthy.ok("PING")["message"], "PONG")
        self.assertLess(time.monotonic() - started, 1.5)
        replies = [response(p.sock) for p in peers]
        self.assertTrue(any(r.get("error_code") == "SERVER_BUSY" for r in replies))
        self.assertTrue(all(r["ok"] or r.get("error_code") == "SERVER_BUSY" for r in replies))

    def test_disconnected_auth_never_authenticates_reused_slot(self):
        owner = self.peer("alice")
        owner.ok("LOGOUT")
        old = self.peer()
        old.sock.sendall(frame({"type": "LOGIN", "request_id": "stale", "payload": {"username": "alice", "password": PASSWORD}}))
        old.close()
        fresh = self.peer()
        time.sleep(0.4)
        self.denied(fresh, "ROOM_LIST", "AUTH_REQUIRED")
        self.assertEqual(fresh.ok("PING")["message"], "PONG")

    def test_menu_complete_organizer_flow_without_json(self):
        planned = time.strftime("%Y-%m-%d %H:%M", time.localtime(time.time() + 86400))
        commands = ["1", "menu_owner", PASSWORD, "2", "menu_owner", PASSWORD,
                    "3", "Phòng menu", "Mô tả demo", "8", "Vật phẩm menu", "Mô tả",
                    "100000", "500000", planned, "60", "14", "1", "12", "6", "1",
                    "7", "1", "0"]
        result = subprocess.run([str(ROOT / "build/client"), "127.0.0.1", str(self.server.port)],
                                input="\n".join(commands) + "\n", text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Thất bại:", result.stdout)
        self.assertIn("PUBLISHED", result.stdout)
        self.assertIn("Vật phẩm menu", result.stdout)
        self.assertNotIn(PASSWORD, result.stdout)

    def test_item_and_version_change_roll_back_together(self):
        owner = self.peer("owner")
        room_id, _ = self.room(owner, publish=False)
        with contextlib.closing(sqlite3.connect(Path(self.directory.name) / "test.db")) as db:
            db.executescript("CREATE TRIGGER reject_version BEFORE UPDATE OF version ON rooms "
                             "BEGIN SELECT RAISE(ABORT, 'rollback test'); END;")
        before = owner.ok("ITEM_LIST", room_id=room_id)["items"]
        self.denied(owner, "ITEM_DELETE", "DATABASE_ERROR", room_id=room_id, item_id=before[0]["id"])
        self.assertEqual(owner.ok("ITEM_LIST", room_id=room_id)["items"], before)

    def test_largest_escaped_catalog_still_fits_snapshot_frame(self):
        owner = self.peer("owner")
        room_id = owner.ok("ROOM_CREATE", name='"'*120, description='"'*512)["id"]
        for _ in range(30):
            self.item(owner, room_id, title='"'*120, description='"'*512)
        self.denied(owner, "ITEM_CREATE", "CATALOG_FULL", room_id=room_id)
        snap = owner.ok("ROOM_PUBLISH", room_id=room_id)
        self.assertEqual(len(snap["items"]), 30)

    def test_menu_receives_events_while_waiting_for_input(self):
        owner, guest, other = self.peer("owner"), self.peer("guest"), self.peer("other")
        room_id, _ = self.room(owner)
        guest.ok("LOGOUT")
        process = subprocess.Popen([str(ROOT / "build/client"), "127.0.0.1", str(self.server.port)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        output = bytearray()

        def wait_for(marker):
            deadline = time.monotonic() + 5
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while marker not in output:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        self.fail(f"menu did not display {marker!r}: {output!r}")
                    chunk = os.read(process.stdout.fileno(), 4096)
                    self.assertTrue(chunk, output)
                    output.extend(chunk)

        try:
            process.stdin.write(f"2\nguest\n{PASSWORD}\n6\n{room_id}\n".encode())
            process.stdin.flush()
            wait_for(f"tham gia phòng #{room_id}".encode())
            other.ok("ROOM_JOIN", room_id=room_id)
            wait_for(b"MEMBER_JOINED")
            other.ok("LOGOUT")
            wait_for(b"MEMBER_LEFT")
            process.stdin.write(b"0\n"); process.stdin.flush()
            self.assertEqual(process.wait(timeout=5), 0)
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
            process.stdin.close(); process.stdout.close(); process.stderr.close()

    def test_password_prompt_hides_input_and_restores_terminal_on_interrupt(self):
        master, slave = pty.openpty()
        process = subprocess.Popen([str(ROOT / "build/client"), "127.0.0.1", str(self.server.port)],
                                   stdin=slave, stdout=slave, stderr=slave)
        try:
            os.write(master, b"1\nterminal_user\n")
            output = bytearray()
            with selectors.DefaultSelector() as selector:
                selector.register(master, selectors.EVENT_READ)
                while "Mật khẩu".encode() not in output:
                    self.assertTrue(selector.select(5), output)
                    output.extend(os.read(master, 4096))
            self.assertFalse(termios.tcgetattr(slave)[3] & termios.ECHO)
            process.send_signal(signal.SIGINT)
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertTrue(termios.tcgetattr(slave)[3] & termios.ECHO)
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
            os.close(master); os.close(slave)


if __name__ == "__main__":
    unittest.main(verbosity=2)
