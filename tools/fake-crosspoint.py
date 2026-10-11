"""Fake CrossPoint device: /api/status, /api/files, /mkdir, /upload, /delete, WebDAV PUT/MKCOL/OPTIONS/HEAD. CORS on."""
import json, os, re, sys, socket
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
ROOT = sys.argv[1]; PORT = int(sys.argv[2]); UDP_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 0
os.makedirs(ROOT, exist_ok=True)
def fs(p):
    p = p.strip('/')
    return os.path.join(ROOT, *[s for s in p.split('/') if s and s != '..'])
class H(BaseHTTPRequestHandler):
    def log_message(self, f, *a): sys.stderr.write("DEV %s %s\n" % (self.command, self.path))
    def cors(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS,PROPFIND,MKCOL,HEAD,MOVE,COPY')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Allow-Private-Network', 'true')
    def reply(self, code, body=b'', ctype='text/plain'):
        self.send_response(code); self.cors()
        self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_OPTIONS(self):
        self.send_response(204); self.cors(); self.send_header('DAV', '1'); self.send_header('Allow', 'OPTIONS,GET,HEAD,PUT,DELETE,MKCOL,PROPFIND'); self.end_headers()
    def do_GET(self):
        path, _, q = self.path.partition('?')
        qs = dict(x.split('=',1) for x in q.split('&') if '=' in x)
        if path == '/api/status':
            return self.reply(200, json.dumps({"version":"fake-1.0","ip":"127.0.0.1","mode":"STA","device":"X4Pro"}).encode(), 'application/json')
        if path == '/api/files':
            d = fs(qs.get('path','/').replace('%2F','/'))
            items = [{"name":n,"size":os.path.getsize(os.path.join(d,n)),"isDirectory":os.path.isdir(os.path.join(d,n)),"isEpub":n.endswith('.epub')} for n in os.listdir(d)] if os.path.isdir(d) else []
            return self.reply(200, json.dumps(items).encode(), 'application/json')
        f = fs(path)
        if os.path.isfile(f): return self.reply(200, open(f,'rb').read(), 'application/octet-stream')
        self.reply(404, b'not found')
    def do_HEAD(self):
        f = fs(self.path.split('?')[0])
        if os.path.isfile(f):
            self.send_response(200); self.cors(); self.send_header('Content-Length', str(os.path.getsize(f))); self.end_headers()
        else:
            self.send_response(404); self.cors(); self.end_headers()
    def do_MKCOL(self):
        f = fs(self.path)
        if os.path.isdir(f): return self.reply(405)
        os.makedirs(f, exist_ok=True); self.reply(201)
    def do_PUT(self):
        f = fs(self.path); n = int(self.headers.get('Content-Length', 0)); data = self.rfile.read(n)
        if not os.path.isdir(os.path.dirname(f)): return self.reply(409, b'parent missing')
        existed = os.path.exists(f); open(f,'wb').write(data); self.reply(204 if existed else 201)
    def do_POST(self):
        path, _, q = self.path.partition('?'); qs = dict(x.split('=',1) for x in q.split('&') if '=' in x)
        n = int(self.headers.get('Content-Length', 0)); body = self.rfile.read(n)
        ct = self.headers.get('Content-Type','')
        if path == '/mkdir':
            form = dict(x.split('=',1) for x in body.decode().split('&') if '=' in x)
            from urllib.parse import unquote_plus
            os.makedirs(fs(unquote_plus(form.get('path','/'))+'/'+unquote_plus(form.get('name',''))), exist_ok=True); return self.reply(200, b'ok')
        if path == '/upload':
            m = re.search(rb'boundary=([^;\r\n]+)', ct.encode()); b = b'--'+m.group(1)
            parts = body.split(b)
            for p in parts:
                hm = re.search(rb'filename="([^"]+)"', p)
                if hm:
                    name = hm.group(1).decode(); data = p.split(b'\r\n\r\n',1)[1].rsplit(b'\r\n',1)[0]
                    from urllib.parse import unquote
                    d = fs(unquote(qs.get('path','/'))); f = os.path.join(d, name)
                    if not os.path.isdir(d): return self.reply(400, b'Directory does not exist')
                    if os.path.exists(f): return self.reply(400, ('File already exists: '+name).encode())
                    open(f,'wb').write(data); return self.reply(200, ('File uploaded successfully: '+name).encode())
            return self.reply(400, b'no file')
        if path == '/delete':
            from urllib.parse import unquote_plus
            form = dict(x.split('=',1) for x in body.decode().split('&') if '=' in x)
            paths = json.loads(unquote_plus(form['paths'])) if 'paths' in form else [unquote_plus(form['path'])]
            for p in paths:
                f = fs(p)
                if os.path.isfile(f): os.remove(f)
            return self.reply(200, b'deleted')
        self.reply(404, b'?')
import threading
def udp_responder():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.bind(('0.0.0.0', UDP_PORT)); sys.stderr.write("DEV UDP listening on %d\n" % UDP_PORT)
    while True:
        data, addr = u.recvfrom(64)
        sys.stderr.write("DEV UDP %r from %s\n" % (data, addr))
        if data == b'hello': u.sendto(b'crosspoint (on CrossPoint-Reader-FAKE);81', addr)
if UDP_PORT: threading.Thread(target=udp_responder, daemon=True).start()
BIND = sys.argv[4] if len(sys.argv) > 4 else '127.0.0.1'
ThreadingHTTPServer((BIND, PORT), H).serve_forever()
