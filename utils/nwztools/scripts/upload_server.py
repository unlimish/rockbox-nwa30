#!/usr/bin/env python3
"""Tiny LAN-only PUT upload receiver, so Windows can push rockbox.log
straight to this Mac's ~/Downloads without going through Discord.

Usage on the Mac:   python3 upload_server.py [port]   (default 8081)
Usage on Windows:   curl.exe -T rockbox.log http://<mac-ip>:8081/rockbox.log
"""
import http.server
import os
import sys

DEST_DIR = os.path.expanduser("~/Downloads")

class UploadHandler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path.lstrip("/"))
        if not name:
            self.send_error(400, "missing filename")
            return
        length = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(length)
        dest = os.path.join(DEST_DIR, name)
        with open(dest, "wb") as f:
            f.write(data)
        print(f"received {len(data)} bytes -> {dest}")
        self.send_response(201)
        self.end_headers()

    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
    http.server.HTTPServer(("0.0.0.0", port), UploadHandler).serve_forever()
