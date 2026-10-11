# k-rosspointSender

k-rosspointSender is a local-first web app. It turns web articles, Wikipedia
pages, pasted HTML, pasted text, Markdown, TXT files and local HTML files into
clean EPUB, HTML, TXT, Markdown or ZIP files. The output is tuned for the
Xteink X4 Pro (480x800 e-ink) running CrossPoint firmware, or a future
K-Reader firmware. It then sends the file to the device, or lets you export it.

Live app: `https://k-engine.org/sender/` (static site). You can also run it
on your own PC. See "Run locally".

## Privacy and local-first behavior

- All article cleanup, metadata editing, EPUB generation, device profiles and
  history happen in your browser.
- The app has no backend. It is a static site. It makes no calls to
  k-engine.org after it loads.
- The only network calls are the ones you start: a fetch of the article URL
  you paste, a fetch of article images, and the upload to your device on
  your LAN.
- Your library, device profiles and settings live in your browser's
  IndexedDB. Delete them from the Library and Settings drawers at any time.
- No telemetry, no analytics, no ads, no remote logging, no cloud sync.
- You can confirm this in your browser: open DevTools, open the Network tab,
  and use the app.

## Quick start

1. Open the app.
2. Paste a URL in the Source panel and press Fetch. Or paste HTML or text, or
   drop a file on the Source panel.
3. Check the cleaned article in the Preview panel. Toggle images, change the
   style preset.
4. Edit the title, author, source URL, tags and filename in the Metadata
   panel.
5. Choose an output format. EPUB is the default.
6. Send the file to your device, or download it.

## Input modes

| Mode | What to do |
|---|---|
| Website URL | Paste the URL. The app fetches the page in your browser. |
| Wikipedia URL | Paste the URL or a title. The app reads the article through the MediaWiki API, which allows cross-origin reads, then uses the Wikipedia cleanup mode. |
| Paste HTML | Copy the page source or a selection and paste it. |
| Paste text | Paste plain text. Blank lines become paragraphs. |
| Markdown | Paste Markdown or drop a `.md` file. |
| File | Drop or pick a `.html`, `.htm`, `.txt` or `.md` file. |
| Bookmarklet | Capture a page you are looking at. See below. |

## Why some websites cannot be fetched directly

Your browser only lets a web page read another site when that site allows
it with CORS headers. Most news sites and blogs do not send those headers.
The fetch then fails before any data arrives. Other causes:

- The site needs a login or has a paywall.
- The site is `http://` and the app is `https://` (mixed content).
- The site is on your LAN or on `localhost` (private network access).
- The site blocks scripts or rate-limits requests.

When a fetch fails, the app tells you why and offers these fallbacks:

1. Paste the page HTML. In most browsers: right-click, "View page source",
   select all, copy, paste into Paste HTML.
2. Use the bookmarklet (below).
3. Save the page as HTML in your browser and upload the file.
4. Select the article text, copy, and paste it into Paste text.
5. Run the optional local helper app (below).
6. Download the page another way and import it.

## Bookmarklet fallback

The bookmarklet runs inside the page you are reading. It copies the page
HTML, URL and title, and hands them to k-rosspointSender. The page HTML never
goes anywhere else.

Install:

1. Open the Bookmarklet tab in the Source panel.
2. Drag the "Send to k-rosspointSender" link to your bookmarks bar.

Use:

1. Open an article.
2. Click the bookmarklet.
3. For pages under about 1.5 MB, the app opens with the article loaded.
4. For bigger pages, the bookmarklet copies a payload to your clipboard.
   Paste it into the Bookmarklet tab.

The source is in `bookmarklet/bookmarklet.js`. `node bookmarklet/build.mjs
https://k-engine.org/sender/` writes the `javascript:` URL to
`bookmarklet/bookmarklet.txt`.

## Wikipedia mode

The app detects `*.wikipedia.org` URLs. Plain Wikipedia pages do not allow
cross-origin reads, so the app fetches the article HTML through the MediaWiki
`action=parse` API, which does. Pasted Wikipedia page source with a
Wikipedia source URL takes the same cleanup path. The app then:

- Keeps the article intro, headings, lists, readable tables and images with
  captions.
- Removes infoboxes by default. Turn on "Keep infobox" in Settings to keep
  them.
- Removes References, Notes, Citations, External links, See also and Further
  reading by default. Turn on "Keep references" to keep the first three.
- Removes nav boxes, edit links, hatnotes, maintenance boxes and category
  links.

## Output formats

- **EPUB** (default). Title page, table of contents, cleaned body, images,
  source note and metadata (title, author, source URL, capture date, tags).
- **HTML**. One clean file with the same CSS.
- **TXT**. Plain text with underlined headings.
- **Markdown**.
- **ZIP**. All four plus `article.json`.

### E-ink styling

Presets: Compact, Comfortable, Large Text, Reference/Technical. Settings let
you change font size, margins, line height, paragraph spacing, image max
width, grayscale, link handling, table handling, hyphenation and custom CSS.

Note on CrossPoint: its EPUB renderer reads only a small set of CSS
properties (text-align, text-indent, margins, padding, font-style,
font-weight, text-decoration, list-style-type). It ignores font-size and
font-family. Font size comes from the device settings. The presets still
change the preview, the HTML export, and other readers. Images are converted
to baseline JPEG at 480 px max width, grayscale by default, because CrossPoint
decodes only JPEG and PNG.

### Spider packs

The `pack` source tab builds a small offline reference EPUB from one root
article and the pages it links to. Wikipedia first:

1. Enter a Wikipedia URL or title.
2. Scan links. Depth 1 is the default. Depth 2 is possible but warned, since
   it grows fast. Limits: 20 articles, 2 images per article, 25 MB,
   500 ms between requests.
3. Select the pages. Junk namespaces, edit links, citations, nav boxes,
   language links and red links are filtered out. Disambiguation pages are
   flagged.
4. Build. Links between bundled pages become internal EPUB links. Links to
   other pages stay external and get a small arrow. The pack gets a title
   page, an included-articles list, an external-links page, a
   missing-linked-articles page, and an attribution page with every source
   URL and the CC BY-SA note.
5. Send or export like any article.

For a general web page, the pack is the current article plus the links
you select from it, same domain only by default. There is no whole-site
crawl, no login or paywall bypass.

### Bundles

To avoid many small files on the device, open the Library, select several
articles, and press "Bundle into one EPUB". The bundle has one chapter per
article and a nested table of contents.

## Sending to CrossPoint

### Over Wi-Fi (same LAN)

On the device: Home > File Transfer > Join a Network. The screen shows the
IP address and `http://crosspoint.local/`. The device web server runs only
while that screen is open. The app supports three CrossPoint transfer
methods:

| Method | Port | Notes |
|---|---|---|
| CrossPoint HTTP | 80 | Default. `POST /upload`. The app creates the folder, deletes and retries when the file exists, and verifies with `/api/files`. |
| WebDAV | 80 | `PUT` overwrites an existing file. |
| CrossPoint WebSocket | 81 | Streams the file with progress messages. |

Steps:

1. Open Devices > Add Device. Pick CrossPoint. Enter the IP or
   `crosspoint.local`. Keep port 80. Set the target folder, for example
   `/Books/K-rosspoint/`.
2. Press Test Connection. The app calls `GET /api/status` and shows the
   firmware version.
3. Press Send Test EPUB. A tiny EPUB lands in the target folder.
4. Convert an article and press Send.

Status shows: connecting, uploading, verifying, complete, failed. A failure
always shows the reason and the fallback options.

**Browser limit.** `https://k-engine.org` is a secure page. The device is
`http://`. Browsers block that upload as mixed content. Chrome also applies
Private Network Access rules. So from the hosted app, LAN upload usually
fails. Use one of these:

- Save an offline copy of the page (button in the Device panel, or the
  download on the releases page) and open it from your disk. A page opened
  from disk is not https, so the LAN upload works. Verified in Chrome, Edge
  and Firefox.
- Write to the USB folder (below).
- Download the EPUB and copy it.
- Run the app locally at `http://localhost` (below). From an `http://`
  origin the LAN upload works, because CrossPoint sends CORS headers.
- Run the local helper app (below).

### Finding the reader

The host step of Devices > Add Device has a **Find reader** button next to
the host field. It looks for a CrossPoint reader that shows the File
Transfer screen and fills the host when you click a result (and the device
name, when the name field is empty). Typing the host by hand works as
before. A browser cannot send UDP, cannot ask mDNS, and cannot learn its own
LAN address, so the search runs these stages in order:

| Stage | What it does | Limits |
|---|---|---|
| 1. Desktop helper | When the desktop app answers at the Helper URL (`http://127.0.0.1:8134`), the page calls its `GET /discover`. That is the desktop UDP broadcast and probe, so it finds a reader on any subnet. 3 s for the helper to answer, 6 s for its discovery. | Needs the desktop app to run. The Python helper has no `/discover`. Some browsers block a public https page from calling 127.0.0.1; then the stage fails quietly. |
| 2. `crosspoint.local` | `GET http://crosspoint.local/api/status`, 2 s. The operating system resolves the `.local` name (Windows 10 and later, macOS, most Linux). | Fails when the OS or the network does not do mDNS, or when the reader is in hotspot mode without a name. |
| 3. Subnet sweep | `GET http://a.b.c.x/api/status` for x = 1 to 254, 32 requests at a time, 700 ms each (about 6 s per subnet). Order: the subnet you type (for example `192.168.1`), the subnets of your saved devices, then 192.168.0, 192.168.1, 192.168.4 (the reader's hotspot), 10.0.0, 192.168.2, 192.168.50. A hit counts only when the answer looks like CrossPoint (`version` and `mode`). | It cannot know your real subnet. If your network uses another range, type it. Private ranges only. |

By default the sweep runs only when stages 1 and 2 found nothing, and it
stops after the first subnet with a hit. A subnet that you type is always
swept. Cancel stops the search at once and keeps the readers found so far.

**It works only where Wi-Fi sends work.** Stages 2 and 3 are plain http to
the LAN, the same as a Wi-Fi send. On the hosted https page they are
skipped (mixed content), and the app tells you to use "Save an offline
copy". From the offline copy (file://) or from `http://localhost` all three
stages run. The helper stage runs everywhere it can reach 127.0.0.1.
Chrome may ask once for permission to access devices on the local network;
allow it. Bluetooth needs no search: the browser chooser is the scan.

### Over USB

CrossPoint on the X4 Pro has a USB Drive mode (Home > File Transfer > USB
Drive). The SD card mounts as a drive. In Chrome or Edge:

1. Press "Write to USB folder" in the Export panel.
2. Pick the mounted drive, or a folder on it.
3. The app writes the EPUB into the target folder and verifies the size.

The browser remembers the folder for the device profile when it allows it.
Firefox and Safari do not support the File System Access API. The app then
offers the manual download.

### OPDS catalog

The OPDS method builds a ZIP with an OPDS catalog XML and the EPUB. Serve the
unzipped folder with any static web server on your LAN. Add that URL in
CrossPoint under Settings > OPDS. The device can then browse and download.

### Send a file

"Send a file" puts any file from your disk on the SD card, as it is. The
app does not convert it. Open it in the Device panel (Device > Send a file).

1. Choose one or more files. The app sends them in turn.
2. Set the destination folder. The default is the target folder of the
   active device. The "/" button sets the card root.
3. Change the file name if necessary (one file only).
4. Press "Send file". The status line of the Device panel shows the progress.
   "Cancel" stops the send.

The send uses the transport of the active device: the same folder rules,
overwrite rules, verification and cancel as an article send. For example,
CrossPoint HTTP upload refuses a file that already exists. WebDAV replaces it.
The app records each send in the transfer log (kind `file`). It does not
make a library entry. Files over 256 MB are refused.

If you drop a file that is not an article (not HTML, Markdown or TXT) on the
Source panel, the app does not try to convert it. It opens "Send a file"
with that file.

**Firmware update without removing the SD card:**

1. Send `firmware.bin` to `/`.
2. On the reader, open Settings > System > SD Card Firmware Update.
3. Pick the file.

After a `.bin` file is sent, the app shows this menu path. Use Wi-Fi or USB
for firmware. Bluetooth shows a warning for files over 3 MB and refuses files
over 16 MB.

## Run locally

```
npm install
npm run dev        # http://localhost:5173
```

Or build and serve the static output:

```
npm run build
npx vite preview   # or: python -m http.server -d dist 8000
```

An `http://localhost` origin can upload to the device over the LAN.

## Desktop app (standalone version)

`desktop/` holds a native app (C++, Dear ImGui, SDL2). It fetches any URL
without CORS limits, sends to the X4 Pro over Wi-Fi without the
mixed-content block, finds CrossPoint devices by UDP discovery, takes
dropped links, files and pasted pages, and also acts as the local helper
for this web app. See `desktop/README.md`.

## Optional local helper app

`helper/helper.py` is a small Python 3 script. It listens on
`127.0.0.1:8134`. The browser posts the file to it. The helper sends the
file to the device over WebDAV or CrossPoint HTTP. It avoids the browser's
mixed-content and private-network rules. It stores nothing and logs no
content. Set the helper URL in Settings. See `helper/README.md`.

## Bluetooth

The "Bluetooth (CrossPoint BLE mode)" method sends with Web Bluetooth. The
protocol is in `docs/BLE_PROTOCOL.md`. It needs the CrossPoint firmware
build with Bluetooth file transfer (branch `ble-transfer`, ESP32-S3 boards
such as the X4 Pro). Stock CrossPoint has no Bluetooth transfer.

- Browser: Chrome or Edge (desktop or Android). Firefox and Safari have no
  Web Bluetooth.
- Origin: works from `https://k-engine.org` and from a copy of the page
  opened from your disk. The Wi-Fi mixed-content block does not apply.
- Speed: about 10 to 50 KB/s. Good for articles. Files over 3 MB show a
  warning. Files over 16 MB are refused (use Wi-Fi or USB).

Steps:

1. On the reader: File Transfer > Receive via Bluetooth. The screen shows
   the name, for example `CrossPoint-3A7F`.
2. Devices > Add Device. Choose "Bluetooth (CrossPoint BLE mode)". No host
   is needed.
3. Press Connect. The browser shows its Bluetooth window. Choose the
   reader. The app stores the reader name and the browser's device id in the
   profile.
4. Send. The app uses the reader chosen in step 3. After a page reload it
   finds the reader again with `navigator.bluetooth.getDevices()` when the
   browser supports it. If it cannot, it shows the Bluetooth window on the
   Send click, or asks you to press Connect again.

The browser shows the Bluetooth window only during a click. That is why
Connect comes first.

## Deploy on k-engine.org

The app is a static Vite build.

1. `npm run build`. The output is `dist/`.
2. Copy `dist/` to the server, for example `/opt/k-engine-sender`.
3. Add a Caddy route. The `/media/*` policy on k-engine.org blocks all
   network access, so the app needs its own path and its own CSP:

```
handle_path /sender/* {
    header Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob: https: http:; connect-src 'self' https: http: ws: wss: blob: data:; worker-src 'self' blob:; frame-ancestors 'self'; base-uri 'none'; form-action 'none'; object-src 'none'"
    root * /srv/sender
    file_server
}
```

`connect-src https: http: ws:` lets the user fetch articles and talk to a
device. The browser still blocks mixed content on its own.

4. Rebuild the bookmarklet with the final URL:
   `node bookmarklet/build.mjs https://k-engine.org/sender/`.

## Validation

The EPUB builder was checked with `epubcheck` 5.3 (zero errors) on
generated files, and the full flow was driven in a real browser against a
fake CrossPoint server. Test on a real X4 Pro before you rely on it.

## Development

```
npm test           # vitest unit tests
npm run typecheck
npm run build
```

Layout:

```
src/extractors/     URL normalize, fetch, Readability, Wikipedia, sanitize
src/converters/     HTML/TXT/MD/ZIP builders, filenames, images, estimates
src/epub/           EPUB builder, presets, CSS, bundles, test EPUB
src/transports/     WebDAV, CrossPoint HTTP/WS, USB, download, OPDS, Bluetooth (+ dev mock), helper
src/storage/        IndexedDB library and handles
src/deviceProfiles/ profiles
src/settings/       app settings
src/ui/             panels, drawers, wizard
bookmarklet/        capture script and builder
helper/             optional local helper app
docs/               architecture spec
```

## License

MIT.
