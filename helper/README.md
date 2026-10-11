# k-rosspointSender local helper

The local helper is a small, optional program. It lets the hosted app
(https://k-engine.org) send books to your reader on the local network.

## Why you need it

The hosted app uses https. The reader (CrossPoint on the Xteink X4 Pro) uses
plain http on your LAN. Browsers block requests from an https page to an http
address. This block is called "mixed content". Chrome can also block requests
from a public site to a private address (Private Network Access).

Browsers treat `127.0.0.1` as a secure address. So the app can talk to a
helper on your own computer. The helper then sends the file to the reader
over plain http.

You do not need the helper when you:

- run the app locally at `http://localhost`, or
- write to the reader in USB Drive mode, or
- download the file and copy it by hand.

## Requirements

- Python 3.10 or newer. No extra packages. The helper uses only the standard library.
- The computer and the reader are on the same network.
- The reader shows the File Transfer screen.

## How to run

```sh
python helper.py
```

The helper listens on `http://127.0.0.1:8134`. Keep the window open while you
send books. Press Ctrl+C to stop it.

Options:

```sh
python helper.py --port 8135                         # another port
python helper.py --allow-origin https://example.org  # allow one more site (repeatable)
```

If you change the port, set the same URL in the app: Settings, "Helper URL".

## What it does

| Endpoint | Request | Answer |
| --- | --- | --- |
| `GET /status` | none | `{ok, version}` |
| `POST /send` | multipart: `file`, `host`, `port`, `folder`, `method` (`webdav` or `crosspoint-http`), optional `username`, `password` | `{ok, remotePath, bytes}` or `{ok: false, error, code}` |
| `POST /test` | JSON `{host, port}` | the reader's `/api/status` as `{ok, info, latencyMs}` |
| `OPTIONS *` | CORS preflight | `204` |

- `webdav`: MKCOL for each folder segment, PUT the file, then HEAD to check the size.
- `crosspoint-http`: POST `/mkdir` for each segment, POST `/upload?path=`. If the
  file exists, POST `/delete` and upload again once. Then check the size with
  `GET /api/files`.

## Security

- The helper listens only on `127.0.0.1`. Other computers cannot reach it.
- Only these origins can call it from a browser: `https://k-engine.org`,
  `http://localhost:<any port>`, `http://127.0.0.1:<any port>`, and origins you
  add with `--allow-origin`. Other origins get HTTP 403.
- The helper checks the `Host` header. This blocks DNS rebinding attacks.
- The helper sends files only to the host and port that you give in the
  request. It checks that the host is a host name or an IPv4 address.
- The maximum upload size is 200 MiB.

## Privacy

- The helper does not connect to the internet. It talks only to your reader.
- It does not save files to disk. It keeps each file in memory until the
  transfer ends.
- It does not log file contents, file names in bodies, or form fields. It logs
  only the request line (method and path) and the status code.
- It has no telemetry and no analytics.
