#!/usr/bin/env python3
"""Local dev server for the browser build (localhost only).

Serves web/ and build-web/ with the COOP/COEP headers that SharedArrayBuffer (WebAssembly threads)
needs. Development extras:
  --game DIR   serves DIR (your game folder) at /__game/ with a manifest, for http://localhost:PORT/?dev
  --out DIR    saves screenshots (POST /__shot) and the page's console log (POST /__log) into DIR

usage: python3 tools/serve_web.py [--port 8123] [--game ~/path/to/game] [--out /tmp/web-out]
"""
import argparse, json, os, sys
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, unquote

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TYPES = {'.wasm': 'application/wasm', '.js': 'text/javascript', '.html': 'text/html', '.png': 'image/png'}


def under(base, rel):
    """rel resolved inside base, or None (no path traversal)."""
    p = os.path.realpath(os.path.join(base, rel))
    return p if p == base or p.startswith(base + os.sep) else None


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        if self.server.verbose: super().log_message(fmt, *args)

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def send_file(self, path):
        try:
            size = os.path.getsize(path)
            f = open(path, 'rb')
        except OSError:
            return self.send_error(404)
        self.send_response(200)
        self.send_header('Content-Type', TYPES.get(os.path.splitext(path)[1].lower(), 'application/octet-stream'))
        self.send_header('Content-Length', str(size))
        self.end_headers()
        with f:
            while chunk := f.read(1 << 20):
                self.wfile.write(chunk)

    def do_GET(self):
        url = urlparse(self.path)
        path = unquote(url.path)
        game = self.server.game
        if path.startswith('/__game/'):
            if not game: return self.send_error(404, 'start the server with --game')
            rel = path[len('/__game/'):]
            if rel == 'manifest.json':
                files = []
                for d, _, names in os.walk(game):
                    for n in names:
                        p = os.path.join(d, n)
                        files.append({'path': os.path.relpath(p, game).replace(os.sep, '/'), 'size': os.path.getsize(p)})
                body = json.dumps(files).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                return self.wfile.write(body)
            p = under(game, rel)
            return self.send_file(p) if p and os.path.isfile(p) else self.send_error(404)
        rel = path.lstrip('/') or 'index.html'
        for base in (os.path.join(ROOT, 'web'), self.server.build):
            p = under(os.path.realpath(base), rel)
            if p and os.path.isfile(p): return self.send_file(p)
        self.send_error(404)

    def do_POST(self):
        url = urlparse(self.path)
        out = self.server.out
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if not out: return self.send_error(404, 'start the server with --out')
        if url.path == '/__shot':
            name = os.path.basename(parse_qs(url.query).get('name', ['shot.png'])[0])
            with open(os.path.join(out, name), 'wb') as f: f.write(data)
        elif url.path == '/__log':
            with open(os.path.join(out, 'log.txt'), 'ab') as f: f.write(data)
        else:
            return self.send_error(404)
        self.send_response(204)
        self.end_headers()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8123)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build-web'), help='folder with PopulousTB.js/.wasm')
    ap.add_argument('--game', help='game folder to serve at /__game/ (for ?dev)')
    ap.add_argument('--out', help='folder for posted screenshots and logs')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    srv = ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    srv.build = os.path.realpath(a.build)
    srv.game = os.path.realpath(os.path.expanduser(a.game)) if a.game else None
    srv.out = os.path.realpath(a.out) if a.out else None
    srv.verbose = a.verbose
    if srv.out: os.makedirs(srv.out, exist_ok=True)
    print(f'http://localhost:{a.port}/' + ('?dev' if srv.game else ''), file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
