"""A stand-in for rfmap-server in tests (no tests of its own): the endpoints the set-up and probes call."""
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):  # keep test output quiet
        pass

    def _send(self, status: int, body) -> None:
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length)) if length else None

    def do_GET(self):
        self.server.calls.append(("GET", self.path, None, dict(self.headers)))
        if self.path == "/api/v1/health":
            self._send(200, {"ok": True, "version": "0.1.0"})
        else:
            self._send(404, {"message": "no such route"})

    def do_POST(self):
        body = self._body()
        self.server.calls.append(("POST", self.path, body, dict(self.headers)))
        if self.path == "/api/v1/missions":
            n = sum(1 for call in self.server.calls if call[1] == "/api/v1/missions")
            self._send(201, {"missionId": f"m{n}", "ownerKey": f"k{n}", "session": f"s{n}", "revision": 1})
        elif self.path.endswith("/links") and self.headers.get("X-Owner-Key"):
            self._send(201, {"token": "t-" + self.path.split("/")[4], "role": body["role"]})
        else:
            self._send(403, {"message": "owner key required"})


class FakeServer:
    """Serves on 127.0.0.1 at a free port until stop(); `calls` lists what it was asked."""

    def __init__(self):
        self._http = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self._http.calls = []
        self._thread = threading.Thread(target=self._http.serve_forever, args=(0.05,), daemon=True)
        self._thread.start()

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self._http.server_address[1]}/api/v1"

    @property
    def calls(self) -> list:
        return self._http.calls

    def stop(self) -> None:
        self._http.shutdown()
        self._http.server_close()
