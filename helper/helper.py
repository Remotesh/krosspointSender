#!/usr/bin/env python3
"""k-rosspointSender local helper.

The hosted app runs on https. The reader speaks plain http on the LAN.
Browsers block that as mixed content. This helper runs on your computer,
on 127.0.0.1:8134. Browsers treat 127.0.0.1 as a secure origin, so the app
can call it. The helper then sends the file to the reader over the LAN.

Stdlib only. Python 3.10 or newer.

Endpoints:
  GET  /status -> {"ok": true, "version": "..."}
  POST /send   multipart/form-data: file, host, port, folder, method
               (method is "webdav" or "crosspoint-http"; optional username, password)
               -> {"ok": true, "remotePath": "...", "bytes": N}
               |  {"ok": false, "error": "...", "code": "..."}
  POST /test   JSON {"host": "...", "port": 80} -> proxies GET /api/status
  OPTIONS *    204 with CORS headers

Privacy: the helper never logs file contents. It logs only the method,
the path and the status of each request. It does not call the internet.
"""

from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.1.0"
DEFAULT_PORT = 8134
MAX_UPLOAD_BYTES = 200 * 1024 * 1024  # 200 MiB
CONNECT_TIMEOUT = 5.0
UPLOAD_TIMEOUT = 300.0

# Origins that may call the helper. Extra origins come from --allow-origin.
ALLOWED_ORIGINS = {"https://k-engine.org"}
ALLOWED_ORIGIN_PATTERNS = [
    re.compile(r"^http://localhost(:\d{1,5})?$"),
    re.compile(r"^http://127\.0\.0\.1(:\d{1,5})?$"),
]

HOST_LABEL = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")


class HelperError(Exception):
    """An error with a TransferErrorCode for the app."""

    def __init__(self, code: str, message: str, status: int = 502):
        super().__init__(message)
        self.code = code
        self.status = status


# ───────────── validation ─────────────


def valid_host(host: str) -> bool:
    """True for a host name or an IPv4 address. No scheme, no port, no spaces."""
    if not host or len(host) > 253:
        return False
    if re.fullmatch(r"\d+(\.\d+){3}", host):
        try:
            ipaddress.IPv4Address(host)
            return True
        except ValueError:
            return False
    return all(HOST_LABEL.match(label) for label in host.split("."))


def parse_port(value: str | int | None) -> int:
    try:
        port = int(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        raise HelperError("unknown", "The port must be a number.", 400)
    if not 1 <= port <= 65535:
        raise HelperError("unknown", "The port must be from 1 to 65535.", 400)
    return port


def folder_segments(folder: str) -> list[str]:
    parts = re.split(r"[\\/]+", folder or "/")
    return [p for p in (s.strip() for s in parts) if p and p not in (".", "..")]


def safe_filename(name: str) -> str:
    name = re.split(r"[\\/]", name or "")[-1]
    name = re.sub(r'[<>:"|?*\x00-\x1f]', "", name).strip(" .")
    if not name:
        raise HelperError("unknown", "The file has no usable name.", 400)
    return name


# ───────────── multipart parsing (no cgi module) ─────────────


def parse_multipart(body: bytes, content_type: str) -> tuple[dict[str, str], dict[str, tuple[str, bytes]]]:
    """Parses multipart/form-data. Returns (fields, files).

    files maps the field name to (filename, data).
    """
    m = re.search(r'boundary=(?:"([^"]+)"|([^;\s]+))', content_type, re.I)
    if not m:
        raise HelperError("unknown", "The multipart boundary is missing.", 400)
    boundary = (m.group(1) or m.group(2)).encode("latin-1")
    delimiter = b"--" + boundary

    fields: dict[str, str] = {}
    files: dict[str, tuple[str, bytes]] = {}

    parts = body.split(delimiter)
    # parts[0] is the preamble. The last part starts with "--" (the close delimiter).
    for raw in parts[1:]:
        if raw.startswith(b"--"):
            break
        if raw.startswith(b"\r\n"):
            raw = raw[2:]
        if raw.endswith(b"\r\n"):
            raw = raw[:-2]
        head, sep, data = raw.partition(b"\r\n\r\n")
        if not sep:
            continue
        headers: dict[str, str] = {}
        for line in head.decode("utf-8", "replace").split("\r\n"):
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        disp = headers.get("content-disposition", "")
        name_m = re.search(r'(?<![a-z*])name="((?:[^"\\]|\\.)*)"', disp, re.I)
        if not name_m:
            continue
        name = name_m.group(1)
        file_m = re.search(r'filename="((?:[^"\\]|\\.)*)"', disp, re.I)
        file_star = re.search(r"filename\*=UTF-8''([^;]+)", disp, re.I)
        if file_m or file_star:
            filename = urllib.parse.unquote(file_star.group(1)) if file_star else file_m.group(1)  # type: ignore[union-attr]
            files[name] = (filename, data)
        else:
            fields[name] = data.decode("utf-8", "replace")
    return fields, files


def build_multipart(field: str, filename: str, data: bytes, mime: str) -> tuple[bytes, str]:
    boundary = "----krosspoint" + uuid.uuid4().hex
    quoted = filename.replace("\\", "\\\\").replace('"', '\\"')
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{quoted}"\r\n'
        f"Content-Type: {mime}\r\n\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("ascii")
    return head + data + tail, f"multipart/form-data; boundary={boundary}"


# ───────────── device requests ─────────────


def device_request(
    method: str,
    url: str,
    data: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = CONNECT_TIMEOUT,
) -> tuple[int, dict[str, str], bytes]:
    """Sends one request. Returns (status, headers, body). HTTP errors are returned, not raised."""
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, {k.lower(): v for k, v in resp.headers.items()}, resp.read()
    except urllib.error.HTTPError as e:
        body = e.read() if e.fp else b""
        return e.code, {k.lower(): v for k, v in (e.headers or {}).items()}, body
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        reason = getattr(e, "reason", e)
        raise HelperError("not-found", f"Could not reach the device: {reason}") from None


def status_error(status: int, what: str, body: bytes = b"") -> HelperError:
    text = body.decode("utf-8", "replace").strip()[:200]
    code = "auth" if status in (401, 403) else "not-found" if status == 404 else "device"
    detail = f": {text}" if text else ""
    return HelperError(code, f"{what} failed with HTTP {status}{detail}")


def auth_header(username: str | None, password: str | None) -> dict[str, str]:
    if not username:
        return {}
    token = base64.b64encode(f"{username}:{password or ''}".encode("utf-8")).decode("ascii")
    return {"Authorization": f"Basic {token}"}


def quote_path(path: str) -> str:
    return urllib.parse.quote(path, safe="/")


def send_webdav(base: str, folder: str, name: str, data: bytes, mime: str, auth: dict[str, str]) -> str:
    path = "/"
    for seg in folder_segments(folder):
        path += seg + "/"
        status, _, body = device_request("MKCOL", base + quote_path(path), headers=auth)
        if status < 300 or status in (301, 405, 409):
            continue
        raise status_error(status, f"Creating the folder {path}", body)

    remote = path + name
    url = base + quote_path(remote)
    headers = {**auth, "Content-Type": mime, "Content-Length": str(len(data))}
    status, _, body = device_request("PUT", url, data=data, headers=headers, timeout=UPLOAD_TIMEOUT)
    if not 200 <= status < 300:
        raise status_error(status, "The upload", body)

    status, h, _ = device_request("HEAD", url, headers=auth)
    if not 200 <= status < 300:
        raise status_error(status, "The check after upload")
    length = h.get("content-length")
    if length and length.isdigit() and int(length) != len(data):
        raise HelperError("device", f"The device reports {length} bytes, but the file has {len(data)} bytes.")
    return remote


def send_crosspoint_http(base: str, folder: str, name: str, data: bytes, mime: str) -> str:
    segs = folder_segments(folder)
    parent = "/"
    for seg in segs:
        form = urllib.parse.urlencode({"name": seg, "path": parent}).encode("ascii")
        try:
            device_request("POST", base + "/mkdir", data=form, headers={"Content-Type": "application/x-www-form-urlencoded"})
        except HelperError:
            pass  # Ignore. A real failure shows in the upload.
        parent = f"/{seg}" if parent == "/" else f"{parent}/{seg}"

    dir_path = "/" + "/".join(segs) if segs else "/"
    remote = (dir_path.rstrip("/") + "/" + name) if segs else "/" + name
    upload_url = f"{base}/upload?path={urllib.parse.quote(dir_path, safe='')}"

    def upload() -> tuple[int, bytes]:
        body, ctype = build_multipart("file", name, data, mime)
        status, _, resp = device_request(
            "POST", upload_url, data=body, headers={"Content-Type": ctype}, timeout=UPLOAD_TIMEOUT
        )
        return status, resp

    status, resp = upload()
    if status == 400 and b"already exists" in resp.lower():
        form = urllib.parse.urlencode({"paths": json.dumps([remote])}).encode("utf-8")
        dstatus, _, _ = device_request(
            "POST", base + "/delete", data=form, headers={"Content-Type": "application/x-www-form-urlencoded"}
        )
        if not 200 <= dstatus < 300:
            raise HelperError("exists", f"{name} already exists on the device, and the device did not delete it.")
        status, resp = upload()
        if status == 400 and b"already exists" in resp.lower():
            raise HelperError("exists", f"{name} already exists on the device.")
    if not 200 <= status < 300:
        raise status_error(status, "The upload", resp)

    status, _, listing = device_request("GET", f"{base}/api/files?path={urllib.parse.quote(dir_path, safe='')}")
    if not 200 <= status < 300:
        raise status_error(status, "The check after upload")
    try:
        entries = json.loads(listing.decode("utf-8"))
    except ValueError:
        raise HelperError("device", "The device returned a file list that is not JSON.") from None
    found = next((e for e in entries if isinstance(e, dict) and e.get("name") == name and not e.get("isDirectory")), None)
    if not found:
        raise HelperError("device", f"The upload finished, but {name} is not in {dir_path}.")
    size = found.get("size")
    if isinstance(size, int) and size != len(data):
        raise HelperError("device", f"The device reports {size} bytes, but the file has {len(data)} bytes.")
    return remote


# ───────────── HTTP server ─────────────


class Handler(BaseHTTPRequestHandler):
    server_version = f"KrosspointHelper/{VERSION}"
    protocol_version = "HTTP/1.1"
    extra_origins: set[str] = set()

    # Log the request line only. Never log bodies.
    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        sys.stderr.write("[helper] %s - %s\n" % (self.address_string(), fmt % args))

    # ── CORS and host checks ──

    def origin_allowed(self, origin: str | None) -> bool:
        if origin is None:
            return True  # Not a browser (curl). Local use only.
        if origin in ALLOWED_ORIGINS or origin in self.extra_origins:
            return True
        return any(p.match(origin) for p in ALLOWED_ORIGIN_PATTERNS)

    def host_allowed(self) -> bool:
        """Blocks DNS rebinding. The Host header must name the loopback interface."""
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0].strip("[]").lower()
        return host in ("127.0.0.1", "localhost", "::1")

    def cors_headers(self) -> None:
        origin = self.headers.get("Origin")
        if origin and self.origin_allowed(origin):
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Max-Age", "600")
            # Chrome Private Network Access preflight.
            if self.headers.get("Access-Control-Request-Private-Network") == "true":
                self.send_header("Access-Control-Allow-Private-Network", "true")

    def send_json(self, status: int, obj: dict) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def guard(self) -> bool:
        if not self.host_allowed():
            self.send_json(403, {"ok": False, "error": "Bad Host header.", "code": "auth"})
            return False
        if not self.origin_allowed(self.headers.get("Origin")):
            self.send_json(403, {"ok": False, "error": "This origin may not use the helper.", "code": "auth"})
            return False
        return True

    def read_body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            raise HelperError("unknown", "Bad Content-Length.", 400) from None
        if length < 0 or length > MAX_UPLOAD_BYTES:
            raise HelperError("unknown", f"The request is too large (limit {MAX_UPLOAD_BYTES} bytes).", 413)
        return self.rfile.read(length) if length else b""

    # ── methods ──

    def do_OPTIONS(self) -> None:  # noqa: N802
        if not self.host_allowed() or not self.origin_allowed(self.headers.get("Origin")):
            self.send_response(403)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(204)
        self.cors_headers()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        if not self.guard():
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == "/status":
            self.send_json(200, {"ok": True, "version": VERSION})
        else:
            self.send_json(404, {"ok": False, "error": "Not found.", "code": "not-found"})

    def do_POST(self) -> None:  # noqa: N802
        if not self.guard():
            return
        path = urllib.parse.urlsplit(self.path).path
        try:
            if path == "/send":
                self.handle_send()
            elif path == "/test":
                self.handle_test()
            else:
                self.read_body()
                self.send_json(404, {"ok": False, "error": "Not found.", "code": "not-found"})
        except HelperError as e:
            self.send_json(e.status, {"ok": False, "error": str(e), "code": e.code})
        except Exception as e:  # noqa: BLE001
            self.send_json(500, {"ok": False, "error": f"Helper error: {type(e).__name__}", "code": "unknown"})

    def handle_send(self) -> None:
        ctype = self.headers.get("Content-Type", "")
        if not ctype.lower().startswith("multipart/form-data"):
            raise HelperError("unknown", "Send multipart/form-data.", 400)
        fields, files = parse_multipart(self.read_body(), ctype)
        if "file" not in files:
            raise HelperError("unknown", 'The "file" field is missing.', 400)
        filename, data = files["file"]
        name = safe_filename(filename)
        host = fields.get("host", "").strip()
        if not valid_host(host):
            raise HelperError("unknown", "The host must be a host name or an IPv4 address.", 400)
        port = parse_port(fields.get("port", "80"))
        folder = fields.get("folder", "/Books/Inbox/")
        if not folder.startswith("/"):
            raise HelperError("unknown", 'The folder must start with "/".', 400)
        method = fields.get("method", "webdav")
        mime = "application/epub+zip" if name.lower().endswith(".epub") else "application/octet-stream"
        base = f"http://{host}:{port}"

        if method == "webdav":
            remote = send_webdav(base, folder, name, data, mime, auth_header(fields.get("username"), fields.get("password")))
        elif method == "crosspoint-http":
            remote = send_crosspoint_http(base, folder, name, data, mime)
        else:
            raise HelperError("unsupported", f"Unknown method {method!r}. Use webdav or crosspoint-http.", 400)
        self.send_json(200, {"ok": True, "remotePath": remote, "bytes": len(data)})

    def handle_test(self) -> None:
        try:
            req = json.loads(self.read_body().decode("utf-8") or "{}")
        except ValueError:
            raise HelperError("unknown", "Send JSON {host, port}.", 400) from None
        host = str(req.get("host", "")).strip()
        if not valid_host(host):
            raise HelperError("unknown", "The host must be a host name or an IPv4 address.", 400)
        port = parse_port(req.get("port", 80))
        t0 = time.monotonic()
        status, _, body = device_request("GET", f"http://{host}:{port}/api/status")
        latency = int((time.monotonic() - t0) * 1000)
        if not 200 <= status < 300:
            raise status_error(status, "The status request")
        try:
            info = json.loads(body.decode("utf-8"))
        except ValueError:
            info = {"raw": body.decode("utf-8", "replace")[:200]}
        self.send_json(200, {"ok": True, "info": info, "latencyMs": latency})


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="k-rosspointSender local helper")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"listen port (default {DEFAULT_PORT})")
    ap.add_argument(
        "--allow-origin",
        action="append",
        default=[],
        metavar="ORIGIN",
        help="extra allowed origin, e.g. https://example.org (repeatable)",
    )
    args = ap.parse_args(argv)
    Handler.extra_origins = {o.rstrip("/") for o in args.allow_origin}

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print(f"k-rosspointSender helper {VERSION} listens on http://127.0.0.1:{args.port}")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
