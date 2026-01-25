"""
Fast OTA Upload Server
"""
import os
import json
import hashlib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from datetime import datetime

FIRMWARE_DIR = os.environ.get("FIRMWARE_DIR", "/firmware")
PORT = int(os.environ.get("PORT", "8081"))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"status": "ok"})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/upload":
            self._json(404, {"error": "not found"})
            return

        try:
            content_type = self.headers.get("Content-Type", "")
            content_length = int(self.headers.get("Content-Length", 0))

            boundary = content_type.split("boundary=")[1].encode()
            body = self.rfile.read(content_length)

            parts = body.split(b"--" + boundary)
            firmware_data = None
            form_data = {}

            for part in parts:
                if b'name="firmware"' in part:
                    idx = part.find(b"\r\n\r\n")
                    if idx > 0:
                        firmware_data = part[idx + 4:].rstrip(b"\r\n--")
                else:
                    for field in ["version", "board"]:
                        if f'name="{field}"'.encode() in part:
                            idx = part.find(b"\r\n\r\n")
                            if idx > 0:
                                form_data[field] = part[idx + 4:].rstrip(b"\r\n--").decode()

            if not firmware_data:
                self._json(400, {"error": "no firmware"})
                return

            md5 = hashlib.md5(firmware_data).hexdigest()

            with open(f"{FIRMWARE_DIR}/firmware.bin", "wb") as f:
                f.write(firmware_data)

            manifest = {
                "version": form_data.get("version", "0.0.0"),
                "file": "firmware.bin",
                "md5": md5,
                "size": len(firmware_data),
                "board": form_data.get("board", "unknown"),
                "timestamp": datetime.utcnow().isoformat() + "Z"
            }

            with open(f"{FIRMWARE_DIR}/manifest.json", "w") as f:
                json.dump(manifest, f, indent=2)

            print(f"[OK] v{manifest['version']} - {len(firmware_data)} bytes")
            self._json(200, {"status": "ok", "md5": md5, "size": len(firmware_data)})

        except Exception as e:
            print(f"[ERR] {e}")
            self._json(500, {"error": str(e)})

    def _json(self, code, data):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    print(f"[OTA] Fast server on port {PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()