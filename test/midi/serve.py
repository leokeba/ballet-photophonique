# The launcher as GitHub Pages serves it, on localhost (a secure context, for
# Web MIDI), with /api proxied to a controller so the page reaches it
# same-origin over HTTP, as the Pages launcher does by address. The elements
# (midi.js, recorder.js...) are served by their file names. A streamed
# subscription (?sub) is answered with a plain value, which the page takes as
# "no stream" and polls: this proxy relays whole responses only.
# usage: serve.py PORT NODE_URL INDEX_HTML UI_JS ELEMENT_JS...
import http.server
import os
import sys
import urllib.error
import urllib.request

port, node, index, ui = int(sys.argv[1]), sys.argv[2].rstrip("/"), sys.argv[3], sys.argv[4]
files = {"/": (index, "text/html"), "/index.html": (index, "text/html"), "/ui.js": (ui, "text/javascript")}
for element in sys.argv[5:]:
    files["/" + os.path.basename(element)] = (element, "text/javascript")


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _proxy(self):
        query = self.path.partition("?")[2].split("&")
        if self.command == "GET" and "sub" in query:
            return self._send(200, "application/json", b"null")
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n) if n else None
        req = urllib.request.Request(node + self.path, data=data, method=self.command)
        if self.headers.get("Content-Type"):
            req.add_header("Content-Type", self.headers["Content-Type"])
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                self._send(r.status, r.headers.get("Content-Type", "application/json"), r.read())
        except urllib.error.HTTPError as e:
            self._send(e.code, e.headers.get("Content-Type", "application/json"), e.read())

    def do_GET(self):
        path = self.path.split("?")[0]
        if path.startswith("/api/"):
            return self._proxy()
        if path in files:
            name, ctype = files[path]
            return self._send(200, ctype, open(name, "rb").read())
        self._send(404, "text/plain", b"not found")

    do_PUT = do_POST = do_PATCH = _proxy


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
