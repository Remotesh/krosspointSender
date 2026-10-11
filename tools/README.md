# Test tools

- `fake-crosspoint.py <root> <port>`: a fake CrossPoint device. It serves
  `/api/status`, `/api/files`, `/upload`, `/mkdir`, `/delete` and WebDAV
  (OPTIONS, MKCOL, PUT, HEAD) with CORS on. Files land in `<root>`.
- `smoke.mjs <app-url> <device-port> <out-dir>`: drives the built app in a
  browser with Playwright. It pastes HTML, uploads a file, fetches a live
  Wikipedia article, downloads an EPUB, runs the device wizard against the
  fake device, sends the article, and opens the Library.

Run:

```
npm run build
python tools/fake-crosspoint.py /tmp/sd 18080 &
python -m http.server 18090 -d dist &
node tools/smoke.mjs http://localhost:18090/index.html 18080 /tmp/smoke-out
```

Then check the EPUBs with `pip install epubcheck` and
`python -m epubcheck /tmp/sd/Books/Inbox/<name>.epub`.
