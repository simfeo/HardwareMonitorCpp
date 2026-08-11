#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#
# hardware_monitor_cpp web monitor. Serves a self-contained dashboard (load graphs + live values) and a
# /api/data JSON endpoint backed by the hardware_monitor_cpp C library, loaded via ctypes.
#
# Run:  python hardware_monitor_server.py [--host 0.0.0.0] [--port 8000]
# (For CPU temperature/power, run as administrator / root.)

import argparse
import ctypes
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")


def load_library():
    names = ["hardware_monitor_cpp_c.dll", "libhardware_monitor_cpp_c.so", "libhardware_monitor_cpp_c.dylib"]
    for n in names:
        path = os.path.join(HERE, n)
        if os.path.exists(path):
            lib = ctypes.CDLL(path)
            lib.hmc_create.restype = ctypes.c_void_p
            lib.hmc_poll_json.restype = ctypes.c_char_p
            lib.hmc_poll_json.argtypes = [ctypes.c_void_p]
            lib.hmc_destroy.argtypes = [ctypes.c_void_p]
            return lib
    sys.exit("Could not find the hardware_monitor_cpp shared library next to this script (%s)." % ", ".join(names))


LIB = load_library()
HANDLE = LIB.hmc_create()

# Requests are served on their own threads and ctypes drops the GIL for the duration of a
# foreign call, so polls really do overlap. The snapshot buffer lives in the handle and is
# rewritten in place by each poll, which would let one request clear the string another is
# still reading. One handle means one poll at a time.
POLL_LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, content_type, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, rel, content_type):
        path = os.path.normpath(os.path.join(WEB, rel))
        if not path.startswith(WEB) or not os.path.isfile(path):
            self._send(404, "text/plain", "not found")
            return
        with open(path, "rb") as f:
            self._send(200, content_type, f.read())

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/" or path == "/index.html":
            self._send_file("index.html", "text/html; charset=utf-8")
        elif path == "/api/data":
            with POLL_LOCK:
                data = LIB.hmc_poll_json(HANDLE) or b"{}"
            self._send(200, "application/json", data)
        else:
            # serve any other static asset from web/
            self._send_file(path.lstrip("/"), "application/octet-stream")

    def log_message(self, *args):
        pass  # quiet


def main():
    ap = argparse.ArgumentParser(description="hardware_monitor_cpp web monitor")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("hardware_monitor_cpp web monitor on http://%s:%d  (Ctrl+C to stop)" % (args.host, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        LIB.hmc_destroy(HANDLE)


if __name__ == "__main__":
    main()
