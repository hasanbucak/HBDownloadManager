#!/usr/bin/env python3
"""
Test HTTP server for HB-Download-Manager test suite (HTTP/1.1, keep-alive).

Payload: 16 MiB, so that 4/8/16 segments of >= 512 KiB really exist.
The pattern repeats every 251 bytes (a prime), so a misplaced segment cannot hide behind the period.

Endpoints (all GET/HEAD):
  /normal           206 ranges, strong ETag "normal-v1", honours If-Range
  /ignore-range     always 200 with the full body, never 206
  /many-segments    like /normal but throttled, so that more than 6 segments must queue
  /slow             alias of /many-segments
  /drop-halfway     the first non-probe request sends half of its body and drops the connection
  /forbidden        403 with an HTML body
  /traversal-header Content-Disposition path traversal attempts (?type=quotes|absolute|backslash|rfc5987)
  /stall            the first ?stalls=N (default 1) non-probe requests send half the body, then stall
  /changing         v1 for the probe, v2 (inverted bytes, new ETag) from then on; honours If-Range
  /no-range         200 only, no Accept-Ranges, throttled
  /weak-etag        ranges with a WEAK ETag; the server records whether If-Range arrived
  /shrinking        probe sees 16 MiB, the real GET is only 4 MiB (Content-Length adoption)
  /flaky            probe ok; the first segment request is dropped before headers, later ones get 200 + full body
  /stats            JSON counters per path (requests, if_range, max_concurrent, stalled, dropped)
  /reset            clears all counters and server state

Usage:
  python3 test_server.py [--port 8899] [--host 127.0.0.1]     (port 0 = pick a free one)
  python3 test_server.py --self-test
"""

import sys
import os
import re
import json
import time
import socket
import hashlib
import argparse
import threading
import urllib.request
import urllib.error
import http.client
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver

PAYLOAD_SIZE = 16 * 1024 * 1024
_PERIOD = 251
TEST_PAYLOAD = (bytes(range(_PERIOD)) * (PAYLOAD_SIZE // _PERIOD + 1))[:PAYLOAD_SIZE]
PAYLOAD_SHA256 = hashlib.sha256(TEST_PAYLOAD).hexdigest()
# /changing serves this after the probe: every byte inverted
CHANGED_PAYLOAD = TEST_PAYLOAD.translate(bytes(b ^ 0xFF for b in range(256)))
CHANGED_SHA256 = hashlib.sha256(CHANGED_PAYLOAD).hexdigest()
# /shrinking serves this prefix to the real GET
SHRINK_SIZE = 4 * 1024 * 1024
SHRINK_PAYLOAD = TEST_PAYLOAD[:SHRINK_SIZE]
SHRINK_SHA256 = hashlib.sha256(SHRINK_PAYLOAD).hexdigest()

RANGE_RE = re.compile(r"^bytes=(\d+)-(\d*)$")

LOCK = threading.Lock()
STATS = {}   # path -> {"requests", "if_range", "inflight", "max_concurrent", "stalled", "dropped"}
STATE = {}   # free-form per-endpoint state (counters)


def reset_all():
    with LOCK:
        STATS.clear()
        STATE.clear()


def _entry(path):
    return STATS.setdefault(path, {"requests": 0, "if_range": 0, "inflight": 0,
                                   "max_concurrent": 0, "stalled": 0, "dropped": 0})


def stat_inc(path, key, n=1):
    with LOCK:
        _entry(path)[key] += n


def state_next(key):
    """Returns the 1-based count of how often this key was asked for."""
    with LOCK:
        STATE[key] = STATE.get(key, 0) + 1
        return STATE[key]


class DownloadTestRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        if os.environ.get("VERBOSE"):
            sys.stderr.write(f"[{self.log_date_time_string()}] {format % args}\n")

    def handle(self):
        # A client that aborts a reply (pause, stall timeout, fallback) breaks the pipe: not an error here
        try:
            super().handle()
        except (OSError, ValueError):
            self.close_connection = True

    # ---- low-level helpers -------------------------------------------------------------------

    def write_bytes(self, data):
        if self.command != "HEAD":
            self.wfile.write(data)

    def is_probe(self):
        return self.headers.get("Range", "").strip() == "bytes=0-0"

    def drop_connection(self):
        self.close_connection = True
        try:
            self.wfile.flush()
        except Exception:
            pass
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            self.connection.close()
        except Exception:
            pass

    def write_body(self, data, start, stop, chunk, delay, stall_at=None):
        """Writes data[start:stop] in chunks. stall_at: hang after that many bytes (the client must give up)."""
        sent = 0
        for off in range(start, stop, chunk):
            piece = data[off:min(off + chunk, stop)]
            self.wfile.write(piece)
            self.wfile.flush()
            sent += len(piece)
            if stall_at is not None and sent >= stall_at:
                # Hang without closing: the client has to notice the missing activity itself
                time.sleep(20)
                self.close_connection = True
                return
            if delay:
                time.sleep(delay)

    def send_data(self, path, data, etag=None, ranges=True, chunk=65536, delay=0.0,
                  stall=False, drop_half=False):
        """Serves data with Range / If-Range semantics (RFC 7233)."""
        size = len(data)
        probe = self.is_probe()
        range_header = self.headers.get("Range")
        if_range = self.headers.get("If-Range")
        if if_range is not None:
            stat_inc(path, "if_range")

        start, end, partial = 0, size - 1, False
        if ranges and range_header:
            m = RANGE_RE.match(range_header.strip())
            if m:
                honour = True
                if if_range is not None:
                    # strong comparison only: a weak or missing server ETag never matches
                    honour = (etag is not None and not etag.startswith("W/") and if_range.strip() == etag)
                if honour:
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else size - 1
                    end = min(end, size - 1)
                    if start >= size or start > end:
                        self.send_response(416, "Range Not Satisfiable")
                        self.send_header("Content-Range", f"bytes */{size}")
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    partial = True

        body_len = end - start + 1
        if partial:
            self.send_response(206, "Partial Content")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200, "OK")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(body_len))
        if ranges:
            self.send_header("Accept-Ranges", "bytes")
        if etag:
            self.send_header("ETag", etag)
        if drop_half:
            self.send_header("Connection", "close")
        self.end_headers()

        if self.command == "HEAD":
            return

        # Concurrency bookkeeping: only real (non-probe) transfers count
        counted = not probe
        if counted:
            with LOCK:
                e = _entry(path)
                e["inflight"] += 1
                e["max_concurrent"] = max(e["max_concurrent"], e["inflight"])
        try:
            if drop_half:
                half = max(1, body_len // 2)
                self.write_body(data, start, start + half, chunk, 0.0)
                self.drop_connection()
                return
            stall_at = (body_len // 2) if stall else None
            self.write_body(data, start, end + 1, chunk, delay, stall_at)
        finally:
            if counted:
                with LOCK:
                    _entry(path)["inflight"] -= 1

    def send_json(self, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(200, "OK")
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.write_bytes(body)

    # ---- routing -----------------------------------------------------------------------------

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        raw_path = self.path
        path = raw_path.split("?")[0]
        query = raw_path.split("?")[1] if "?" in raw_path else ""
        stat_inc(path, "requests")

        if path == "/normal":
            self.send_data(path, TEST_PAYLOAD, etag='"normal-v1"')

        elif path == "/ignore-range":
            self.send_data(path, TEST_PAYLOAD, ranges=False)

        elif path in ("/many-segments", "/slow"):
            # 32 KiB every 20 ms per reply: a 1 MiB segment needs ~0.65 s, so 16 segments
            # cannot all run at once and the parallel cap of 6 must queue the rest
            self.send_data(path, TEST_PAYLOAD, etag='"slow-v1"', chunk=32768, delay=0.02)

        elif path == "/drop-halfway":
            if self.is_probe():
                self.send_data(path, TEST_PAYLOAD)
            elif state_next("drop-halfway") == 1:
                stat_inc(path, "dropped")
                self.send_data(path, TEST_PAYLOAD, drop_half=True)
            else:
                self.send_data(path, TEST_PAYLOAD)

        elif path == "/forbidden":
            html_body = (
                b"<!DOCTYPE html>\n"
                b"<html><head><title>403 Forbidden</title></head>\n"
                b"<body><h1>403 Forbidden</h1><p>Expired cookie or access denied.</p></body></html>\n"
            )
            self.send_response(403, "Forbidden")
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html_body)))
            self.end_headers()
            self.write_bytes(html_body)

        elif path == "/traversal-header":
            dummy_data = b"Sample file content for directory traversal test.\n"
            self.send_response(200, "OK")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(dummy_data)))
            if "type=quotes" in query:
                self.send_header("Content-Disposition", 'attachment; filename="../../evil_shell.sh"')
            elif "type=absolute" in query:
                self.send_header("Content-Disposition", 'attachment; filename="/etc/cron.d/malicious_cron"')
            elif "type=backslash" in query:
                self.send_header("Content-Disposition", 'attachment; filename="..\\..\\malicious.bat"')
            else:
                self.send_header("Content-Disposition", "attachment; filename*=UTF-8''..%2F..%2Fescape.txt")
            self.end_headers()
            self.write_bytes(dummy_data)

        elif path == "/stall":
            m = re.search(r"stalls=(\d+)", query)
            stalls = int(m.group(1)) if m else 1
            if self.is_probe():
                self.send_data(path, TEST_PAYLOAD, etag='"stall-v1"')
            else:
                stall_now = state_next("stall") <= stalls
                if stall_now:
                    stat_inc(path, "stalled")
                self.send_data(path, TEST_PAYLOAD, etag='"stall-v1"', stall=stall_now)

        elif path == "/changing":
            # The probe sees v1; from then on the remote file is v2 (same size, other bytes, other ETag)
            if self.is_probe() and state_next("changing-probe") == 1:
                with LOCK:
                    STATE["changing-version"] = 2   # flip BEFORE answering: no race with the next request
                self.send_data(path, TEST_PAYLOAD, etag='"etag-v1"')
            else:
                with LOCK:
                    version = STATE.get("changing-version", 1)
                if version == 1:
                    self.send_data(path, TEST_PAYLOAD, etag='"etag-v1"')
                else:
                    self.send_data(path, CHANGED_PAYLOAD, etag='"etag-v2"')

        elif path == "/no-range":
            self.send_data(path, TEST_PAYLOAD, ranges=False, chunk=65536, delay=0.004)

        elif path == "/weak-etag":
            self.send_data(path, TEST_PAYLOAD, etag='W/"weak-v1"')

        elif path == "/shrinking":
            if self.headers.get("Range"):
                # the probe: claims the full size (no range support); its body is never read
                self.send_data(path, TEST_PAYLOAD, ranges=False)
            else:
                self.send_data(path, SHRINK_PAYLOAD, ranges=False)

        elif path == "/flaky":
            if self.is_probe():
                self.send_data(path, TEST_PAYLOAD)
            elif state_next("flaky") == 1:
                # drop before any header: the client sees a connect-style failure and schedules a retry
                stat_inc(path, "dropped")
                self.drop_connection()
            else:
                # ignores Range and has no ETag: the client falls back to a single stream (slow enough
                # that the queued retry of the dropped segment fires while the stream is running)
                self.send_data(path, TEST_PAYLOAD, ranges=False, chunk=1024 * 1024, delay=0.15)

        elif path == "/stats":
            with LOCK:
                snapshot = json.loads(json.dumps({"paths": STATS, "state": STATE}))
            self.send_json(snapshot)

        elif path == "/reset":
            reset_all()
            self.send_json({"ok": True})

        else:
            self.send_response(404, "Not Found")
            self.send_header("Content-Length", "0")
            self.end_headers()


class ThreadedHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128


def run_self_test(host="127.0.0.1", port=0):
    """Starts the server on a free port and checks every scenario. Returns the exit code."""
    server = ThreadedHTTPServer((host, port), DownloadTestRequestHandler)
    actual_port = server.server_address[1]
    base_url = f"http://{host}:{actual_port}"

    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    print(f"[TEST_SERVER] Server started at {base_url}")
    print("[TEST_SERVER] Running verification for scenarios...")

    results = {"passed": 0, "failed": 0}

    def check(name, fn):
        try:
            fn()
            print(f"  [PASS] {name}")
            results["passed"] += 1
        except Exception as e:  # noqa: BLE001 - a self-test reports every failure
            print(f"  [FAIL] {name}: {e!r}")
            results["failed"] += 1

    def get(path, headers=None):
        req = urllib.request.Request(f"{base_url}{path}", headers=headers or {})
        return urllib.request.urlopen(req)

    def stats():
        with get("/stats") as r:
            return json.loads(r.read())["paths"]

    def t_payload():
        assert len(TEST_PAYLOAD) == 16 * 1024 * 1024
        assert len(TEST_PAYLOAD) >= 16 * 512 * 1024, "needs >= 16 segments of 512 KiB"

    def t_normal_range():
        with get("/normal", {"Range": "bytes=100-199"}) as resp:
            data = resp.read()
            assert resp.status == 206, resp.status
            assert resp.headers.get("Content-Range") == f"bytes 100-199/{len(TEST_PAYLOAD)}"
            assert data == TEST_PAYLOAD[100:200]

    def t_if_range():
        # matching strong validator: range honoured
        with get("/normal", {"Range": "bytes=0-9", "If-Range": '"normal-v1"'}) as resp:
            resp.read()
            assert resp.status == 206, resp.status
        # different validator: full body, 200
        with get("/normal", {"Range": "bytes=0-9", "If-Range": '"other"'}) as resp:
            assert resp.status == 200, resp.status
            assert int(resp.headers["Content-Length"]) == len(TEST_PAYLOAD)
            resp.read()

    def t_weak_etag():
        reset_all()
        with get("/weak-etag", {"Range": "bytes=0-9", "If-Range": 'W/"weak-v1"'}) as resp:
            resp.read()
            assert resp.status == 200, f"weak validator must not match, got {resp.status}"
        with get("/weak-etag", {"Range": "bytes=0-9"}) as resp:
            resp.read()
            assert resp.status == 206
        assert stats()["/weak-etag"]["if_range"] == 1

    def t_ignore_range():
        with get("/ignore-range", {"Range": "bytes=0-499"}) as resp:
            data = resp.read()
            assert resp.status == 200
            assert "Content-Range" not in resp.headers
            assert data == TEST_PAYLOAD

    def t_drop_halfway():
        reset_all()
        try:
            with get("/drop-halfway", {"Range": "bytes=0-19999"}) as resp:
                expected = int(resp.headers.get("Content-Length", 20000))
                got = resp.read()
                assert len(got) < expected, f"expected a premature close, got {len(got)}"
        except (http.client.IncompleteRead, ConnectionResetError, urllib.error.URLError):
            pass
        # the second request is served in full
        with get("/drop-halfway", {"Range": "bytes=0-19999"}) as resp:
            assert len(resp.read()) == 20000

    def t_forbidden():
        try:
            get("/forbidden")
            raise AssertionError("expected 403")
        except urllib.error.HTTPError as e:
            assert e.code == 403
            assert "text/html" in e.headers.get("Content-Type", "")
            body = e.read()
            assert b"403 Forbidden" in body and b"Expired cookie or access denied" in body

    def t_traversal():
        for t, token in [("rfc5987", "..%2F..%2Fescape.txt"),
                         ("quotes", "../../evil_shell.sh"),
                         ("absolute", "/etc/cron.d/malicious_cron")]:
            with get(f"/traversal-header?type={t}") as resp:
                cd = resp.headers.get("Content-Disposition", "")
                assert token in cd, cd

    def t_changing():
        reset_all()
        with get("/changing", {"Range": "bytes=0-0"}) as resp:
            resp.read()
            assert resp.headers["ETag"] == '"etag-v1"'
        # now the file is v2: an If-Range with the old validator gets 200 + the new body
        with get("/changing", {"Range": "bytes=0-9", "If-Range": '"etag-v1"'}) as resp:
            assert resp.status == 200
            assert resp.headers["ETag"] == '"etag-v2"'
            assert hashlib.sha256(resp.read()).hexdigest() == CHANGED_SHA256
        with get("/changing", {"Range": "bytes=0-9", "If-Range": '"etag-v2"'}) as resp:
            resp.read()
            assert resp.status == 206

    def t_no_range():
        with get("/no-range", {"Range": "bytes=0-9"}) as resp:
            assert resp.status == 200
            assert "Accept-Ranges" not in resp.headers
            assert hashlib.sha256(resp.read()).hexdigest() == PAYLOAD_SHA256

    def t_shrinking():
        with get("/shrinking", {"Range": "bytes=0-0"}) as resp:
            assert int(resp.headers["Content-Length"]) == PAYLOAD_SIZE
        with get("/shrinking") as resp:
            assert int(resp.headers["Content-Length"]) == SHRINK_SIZE
            assert hashlib.sha256(resp.read()).hexdigest() == SHRINK_SHA256

    def t_flaky():
        reset_all()
        with get("/flaky", {"Range": "bytes=0-0"}) as resp:
            resp.read()
            assert resp.status == 206
        # first segment request: dropped before headers
        try:
            get("/flaky", {"Range": "bytes=1048576-2097151"})
            raise AssertionError("expected a dropped connection")
        except (http.client.RemoteDisconnected, ConnectionResetError, urllib.error.URLError):
            pass
        # the following ranged request answers 200 + full body (Range ignored)
        with get("/flaky", {"Range": "bytes=1048576-2097151"}) as resp:
            assert resp.status == 200
            assert int(resp.headers["Content-Length"]) == PAYLOAD_SIZE

    def t_stats_reset():
        reset_all()
        assert "paths" in json.loads(get("/stats").read())

    check("payload size (16 MiB)", t_payload)
    check("normal 206 range", t_normal_range)
    check("If-Range honoured with a strong ETag", t_if_range)
    check("weak ETag never matches If-Range, recorded in /stats", t_weak_etag)
    check("ignore-range answers 200 full body", t_ignore_range)
    check("drop-halfway drops the first request only", t_drop_halfway)
    check("403 Forbidden with HTML body", t_forbidden)
    check("Content-Disposition traversal headers", t_traversal)
    check("/changing flips to v2 after the probe", t_changing)
    check("/no-range has no Accept-Ranges", t_no_range)
    check("/shrinking Content-Length differs between probe and GET", t_shrinking)
    check("/flaky drop then 200", t_flaky)
    check("/stats and /reset", t_stats_reset)

    server.shutdown()
    server.server_close()

    passed, failed = results["passed"], results["failed"]
    print(f"\n[TEST_SERVER] Self-test complete: {passed}/{passed + failed} checks PASSED.")
    return 0 if failed == 0 else 1


def main():
    parser = argparse.ArgumentParser(description="HB-Download-Manager Test HTTP Server")
    parser.add_argument("--host", default="127.0.0.1", help="Host to bind (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8899, help="Port to listen on (default: 8899, 0 = free port)")
    parser.add_argument("--self-test", action="store_true", help="Run automated verification of all scenarios and exit")

    args = parser.parse_args()

    if args.self_test:
        sys.exit(run_self_test(args.host, 0))

    server = ThreadedHTTPServer((args.host, args.port), DownloadTestRequestHandler)
    actual_port = server.server_address[1]
    print(f"READY http://{args.host}:{actual_port}", flush=True)
    print(__doc__, flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
