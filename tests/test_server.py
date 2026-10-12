#!/usr/bin/env python3
"""
Test HTTP server for HB-Download-Manager test suite.
Simulates realistic server behaviors and edge cases:
  a) Normal 206 Partial Content range requests
  b) Server ignoring Range and returning 200 with full body
  c) Premature connection close halfway
  d) 403 Forbidden with HTML body
  e) Content-Disposition filename traversal attempts

Usage:
  python3 test_server.py [--port 8899] [--host 127.0.0.1]
  python3 test_server.py --self-test
"""

import sys
import os
import re
import socket
import hashlib
import argparse
import threading
import urllib.request
import urllib.error
import http.client
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver

# Deterministic test payload (128 KiB)
PAYLOAD_SIZE = 128 * 1024
TEST_PAYLOAD = bytes([(i * 31 + 17) % 256 for i in range(PAYLOAD_SIZE)])
PAYLOAD_SHA256 = hashlib.sha256(TEST_PAYLOAD).hexdigest()

class DownloadTestRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # Quiet logger unless VERBOSE is set
        if os.environ.get("VERBOSE"):
            sys.stderr.write(f"[{self.log_date_time_string()}] {format % args}\n")

    def do_HEAD(self):
        parsed_path = self.path.split('?')[0]
        if parsed_path in ("/normal", "/ignore-range"):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(TEST_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", f'"{PAYLOAD_SHA256[:16]}"')
            self.end_headers()
        elif parsed_path == "/forbidden":
            self.send_response(403)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
        else:
            self.send_response(200)
            self.send_header("Content-Length", "1024")
            self.end_headers()

    def do_GET(self):
        parsed_path = self.path.split('?')[0]
        query = self.path.split('?')[1] if '?' in self.path else ""

        # Scenario A: Normal 206 Partial Content range requests
        if parsed_path == "/normal":
            range_header = self.headers.get("Range")
            if range_header and range_header.startswith("bytes="):
                match = re.match(r"bytes=(\d+)-(\d*)", range_header)
                if match:
                    start = int(match.group(1))
                    end = int(match.group(2)) if match.group(2) else len(TEST_PAYLOAD) - 1
                    if end >= len(TEST_PAYLOAD):
                        end = len(TEST_PAYLOAD) - 1

                    chunk = TEST_PAYLOAD[start:end + 1]
                    self.send_response(206, "Partial Content")
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(TEST_PAYLOAD)}")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.send_header("Accept-Ranges", "bytes")
                    self.end_headers()
                    self.wfile.write(chunk)
                    return

            # No Range provided: 200 OK full body
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(TEST_PAYLOAD)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self.wfile.write(TEST_PAYLOAD)
            return

        # Scenario B: Server ignoring Range and returning 200 with full body
        elif parsed_path == "/ignore-range":
            # Deliberately ignore Range header even if sent by client
            self.send_response(200, "OK")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(TEST_PAYLOAD)))
            # No Content-Range header!
            self.end_headers()
            self.wfile.write(TEST_PAYLOAD)
            return

        # Scenario C: Premature connection close halfway
        elif parsed_path == "/drop-halfway":
            range_header = self.headers.get("Range")
            target_len = 20000
            start = 0
            if range_header and range_header.startswith("bytes="):
                match = re.match(r"bytes=(\d+)-(\d*)", range_header)
                if match:
                    start = int(match.group(1))
                    end = int(match.group(2)) if match.group(2) else start + target_len - 1
                    target_len = end - start + 1
                    self.send_response(206, "Partial Content")
                    self.send_header("Content-Range", f"bytes {start}-{start + target_len - 1}/{len(TEST_PAYLOAD)}")
            else:
                self.send_response(200, "OK")

            # Advertise target_len, but only write 50% and close connection abruptly
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(target_len))
            self.send_header("Connection", "close")
            self.end_headers()

            half_chunk = TEST_PAYLOAD[start:start + (target_len // 2)]
            self.wfile.write(half_chunk)
            self.wfile.flush()

            # Abrupt socket termination
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
            except Exception:
                pass
            return

        # Scenario D: 403 Forbidden with HTML body
        elif parsed_path == "/forbidden":
            html_body = (
                b"<!DOCTYPE html>\n"
                b"<html><head><title>403 Forbidden</title></head>\n"
                b"<body><h1>403 Forbidden</h1><p>Expired cookie or access denied.</p></body></html>\n"
            )
            self.send_response(403, "Forbidden")
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html_body)))
            self.end_headers()
            self.wfile.write(html_body)
            return

        # Scenario E: Content-Disposition filename traversal attempts
        elif parsed_path == "/traversal-header":
            dummy_data = b"Sample file content for directory traversal test.\n"
            self.send_response(200, "OK")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(dummy_data)))

            if "type=quotes" in query:
                # Malicious quoted filename with directory escape
                self.send_header("Content-Disposition", 'attachment; filename="../../evil_shell.sh"')
            elif "type=absolute" in query:
                # Malicious absolute path
                self.send_header("Content-Disposition", 'attachment; filename="/etc/cron.d/malicious_cron"')
            elif "type=backslash" in query:
                # Malicious Windows backslash escape
                self.send_header("Content-Disposition", 'attachment; filename="..\\..\\malicious.bat"')
            else:
                # RFC 5987 percent-encoded traversal
                self.send_header("Content-Disposition", "attachment; filename*=UTF-8''..%2F..%2Fescape.txt")

            self.end_headers()
            self.wfile.write(dummy_data)
            return

        else:
            self.send_response(404, "Not Found")
            self.end_headers()

class ThreadedHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

def run_self_test(host="127.0.0.1", port=0):
    """Spawns test server, runs verification against all 5 scenarios, returns exit code."""
    server = ThreadedHTTPServer((host, port), DownloadTestRequestHandler)
    actual_port = server.server_address[1]
    base_url = f"http://{host}:{actual_port}"

    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    print(f"[TEST_SERVER] Server started at {base_url}")
    print(f"[TEST_SERVER] Running verification for scenarios (a)-(e)...")

    passed = 0
    total = 5

    try:
        # a) Test 206 Partial Content range requests
        req_a = urllib.request.Request(f"{base_url}/normal", headers={"Range": "bytes=100-199"})
        with urllib.request.urlopen(req_a) as resp:
            data = resp.read()
            assert resp.status == 206, f"Expected status 206, got {resp.status}"
            assert resp.headers.get("Content-Range") == f"bytes 100-199/{len(TEST_PAYLOAD)}"
            assert len(data) == 100, f"Expected 100 bytes, got {len(data)}"
            assert data == TEST_PAYLOAD[100:200], "Range payload mismatch"
            print("  [PASS] Scenario (a): Normal 206 Partial Content range requests verified")
            passed += 1

        # b) Server ignoring Range and returning 200 with full body
        req_b = urllib.request.Request(f"{base_url}/ignore-range", headers={"Range": "bytes=0-499"})
        with urllib.request.urlopen(req_b) as resp:
            data = resp.read()
            assert resp.status == 200, f"Expected status 200 (ignored range), got {resp.status}"
            assert "Content-Range" not in resp.headers, "Unexpected Content-Range header"
            assert len(data) == len(TEST_PAYLOAD), f"Expected full body ({len(TEST_PAYLOAD)}), got {len(data)}"
            assert data == TEST_PAYLOAD, "Full body mismatch"
            print("  [PASS] Scenario (b): Server ignoring Range (200 OK full body) verified")
            passed += 1

        # c) Premature connection close halfway
        req_c = urllib.request.Request(f"{base_url}/drop-halfway", headers={"Range": "bytes=0-19999"})
        try:
            with urllib.request.urlopen(req_c) as resp:
                expected_len = int(resp.headers.get("Content-Length", 20000))
                received_data = resp.read()
                # Server sent only half and closed socket
                assert len(received_data) < expected_len, f"Expected premature close, got full {len(received_data)}"
                print("  [PASS] Scenario (c): Premature connection close halfway verified")
                passed += 1
        except (http.client.IncompleteRead, ConnectionResetError, urllib.error.URLError) as e:
            # Also acceptable in some socket implementations
            print("  [PASS] Scenario (c): Premature connection close halfway verified (raised disconnection exception)")
            passed += 1

        # d) 403 Forbidden with HTML body
        req_d = urllib.request.Request(f"{base_url}/forbidden")
        try:
            urllib.request.urlopen(req_d)
            assert False, "Expected 403 HTTPError"
        except urllib.error.HTTPError as e:
            assert e.code == 403, f"Expected status 403, got {e.code}"
            assert "text/html" in e.headers.get("Content-Type", "")
            body = e.read()
            assert b"403 Forbidden" in body
            assert b"Expired cookie or access denied" in body
            print("  [PASS] Scenario (d): 403 Forbidden with HTML error body verified")
            passed += 1

        # e) Content-Disposition filename traversal attempts
        for t, expected_token in [("rfc5987", "..%2F..%2Fescape.txt"),
                                 ("quotes", "../../evil_shell.sh"),
                                 ("absolute", "/etc/cron.d/malicious_cron")]:
            req_e = urllib.request.Request(f"{base_url}/traversal-header?type={t}")
            with urllib.request.urlopen(req_e) as resp:
                cd = resp.headers.get("Content-Disposition", "")
                assert expected_token in cd, f"Expected '{expected_token}' in Content-Disposition: {cd}"
        print("  [PASS] Scenario (e): Content-Disposition filename traversal attempts verified")
        passed += 1

    finally:
        server.shutdown()
        server.server_close()

    print(f"\n[TEST_SERVER] Self-test complete: {passed}/{total} scenarios PASSED.")
    return 0 if passed == total else 1

def main():
    parser = argparse.ArgumentParser(description="HB-Download-Manager Test HTTP Server")
    parser.add_argument("--host", default="127.0.0.1", help="Host to bind (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8899, help="Port to listen on (default: 8899)")
    parser.add_argument("--self-test", action="store_true", help="Run automated verification of all scenarios and exit")

    args = parser.parse_args()

    if args.self_test:
        sys.exit(run_self_test(args.host, 0))

    server = ThreadedHTTPServer((args.host, args.port), DownloadTestRequestHandler)
    print(f"Starting test server on http://{args.host}:{args.port}")
    print("Endpoints available:")
    print("  GET /normal           - 206 Partial Content (or 200 without Range)")
    print("  GET /ignore-range     - 200 OK full body (ignores Range)")
    print("  GET /drop-halfway     - Premature connection close after 50% data")
    print("  GET /forbidden        - 403 Forbidden HTML error page")
    print("  GET /traversal-header - Content-Disposition path traversal header")
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()
        server.server_close()

if __name__ == "__main__":
    main()
