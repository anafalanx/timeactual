"""Loopback-only download fixture; no dependency outside Python's stdlib."""
import http.server
import pathlib
import sys
import time

root = pathlib.Path(sys.argv[1])


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        names = {'/base.zip': 'base.zip', '/overlay.zip': 'overlay.zip',
                 '/next.zip': 'next.zip', '/slow.zip': 'next.zip', '/bad.zip': 'bad.zip'}
        name = names.get(self.path)
        if not name:
            self.send_error(404)
            return
        data = (root / name).read_bytes()
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            half = len(data) // 2
            self.wfile.write(data[:half])
            self.wfile.flush()
            if self.path == '/slow.zip':
                (root / 'started').write_text('body started', encoding='ascii')
                deadline = time.monotonic() + 30
                while not (root / 'continue').exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
            self.wfile.write(data[half:])
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(f'PORT {server.server_port}', flush=True)
server.serve_forever()
