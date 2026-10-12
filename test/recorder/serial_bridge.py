# The launcher on localhost with /api bridged to a controller's serial API
# (one envelope per line, docs/DESIGN.md section 10), for testing the
# elements without a network in common. No subscriptions: the page polls.
# The port is opened with DTR and RTS low, so the board isn't reset.
#
#   uv run --with pyserial python test/recorder/serial_bridge.py 8131 /dev/cu.wchusbserial10 . ../TesserUI/web/dist
#   node test/recorder/e2e_recorder.mjs http://127.0.0.1:8131 'http://127.0.0.1:8131/?api=/api&ws=' shot.png
import http.server, json, sys, threading, time, urllib.parse, serial

port, dev, repo, dist = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
files = {"/": (f"{repo}/pages/index.html", "text/html"), "/index.html": (f"{repo}/pages/index.html", "text/html"),
         "/ui.js": (f"{dist}/ui.js", "text/javascript")}
for el in ("midi", "recorder"):
    for p in (f"/{el}.js", f"/ext/{el}.js", f"/api/ext/{el}.js"):
        files[p] = (f"{repo}/web/elements/{el}.js", "text/javascript")
s = serial.Serial(); s.port = dev; s.baudrate = 115200; s.timeout = 0.05
s.dtr = s.rts = False; s.open(); time.sleep(0.3); s.reset_input_buffer()
lock = threading.Lock()
ids = [0]
CODES = {"ok": 200, "not_found": 404, "invalid_value": 400, "bad_request": 400, "read_only": 403,
         "not_allowed": 405, "unauthorized": 401, "forbidden": 403, "busy": 409, "too_large": 413}


def envelope(e):
    with lock:
        ids[0] += 1
        e["id"] = ids[0]
        s.write((json.dumps(e) + "\n").encode())
        end = time.time() + 6
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.startswith(b'{"id":%d,' % e["id"]):
                    return json.loads(line)
        return {"status": "timeout", "body": None}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _api(self, op):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query, keep_blank_values=True)
        if "sub" in q:  # no streams: a value, so the page polls
            return self._send(200, "application/json", b"null")
        e = {"op": op, "path": urllib.parse.unquote(u.path[4:]) or "/"}
        for k, v in q.items():
            v = v[0]
            e[k] = int(v) if v.isdigit() else {"true": True, "false": False}.get(v, v)
        if op == "set":
            n = int(self.headers.get("Content-Length") or 0)
            e["body"] = json.loads(self.rfile.read(n) or b"null")
        r = envelope(e)
        self._send(CODES.get(r.get("status"), 500), "application/json", json.dumps(r.get("body")).encode())

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in files:
            name, ctype = files[path]
            return self._send(200, ctype, open(name, "rb").read())
        if path.startswith("/api"):
            return self._api("get")
        self._send(404, "text/plain", b"not found")

    def do_PUT(self):
        self._api("set")

    do_POST = do_PATCH = do_PUT


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
