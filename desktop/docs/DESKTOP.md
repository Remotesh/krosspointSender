# k-rosspointSender desktop (binding spec)

Written in ASD-STE100 style. Short sentences. One fact per sentence.

## Purpose

A small native desktop app. It does what the web app does, without the
browser limits. It fetches any URL (no CORS), it talks to the X4 Pro on the
LAN (no mixed-content block), it finds CrossPoint devices by UDP discovery,
and it also serves as the "local helper app" for the web version.

Target: Windows first (MSYS2 mingw64, g++, ninja). Keep the code portable
(Linux, macOS) with `#ifdef _WIN32` only where a platform API is needed.

## Stack

- C++17. CMake 3.20+. Ninja.
- Dear ImGui 1.93 + SDL2 + OpenGL 3 (vendored `third_party/imgui`).
- libcurl (fetch, WebDAV PUT, multipart POST, WebSocket).
- libxml2 HTMLparser (tolerant HTML to DOM).
- miniz (ZIP/EPUB), md4c (Markdown to HTML), stb_image + stb_image_resize2
  + stb_image_write (images), nlohmann json (settings, library, API),
  cpp-httplib (helper HTTP server on localhost).
- No other dependencies. No telemetry. No network calls the user did not ask for.

## Module layout and ownership

```
src/core/           lead: types.h, util.cpp, stb_impl.cpp
src/extract/        Agent E: html dom, readability, sanitize, wikipedia, markdown, text, fetch
src/output/         Agent F: epub, html/txt/md/zip builders, images, filenames, css presets
src/net/            Agent G: transports, discovery, helper server, profiles, settings, library
src/app/            Agent H: ImGui app, panels, drag-drop, clipboard, background jobs
tests/              each agent owns tests for its modules; lead owns tests/tests.cmake
docs/               lead
```

Each module has `sources.cmake` that sets `KS_<MODULE>_SOURCES` (absolute
paths via `${CMAKE_CURRENT_LIST_DIR}`). Each module has one public header
`src/<module>/<module>.h` that includes `core/types.h`. Agents write only in
their folders. Tests: each agent adds `tests/<module>/test_<module>.cpp` and
registers it in `tests/<module>/tests.cmake` (the lead's `tests/tests.cmake`
includes those). Tests are plain executables that return non-zero on failure.
A tiny assert helper is in `tests/check.h` (lead).

## Shared helpers (core, lead)

`core/types.h` declares them. `core/util.cpp` implements them. Use them.
Do not duplicate uuid, ISO time, file IO, or XML escape code.

## Device facts (CrossPoint)

Same as the web spec (`../docs/ARCHITECTURE.md`, section "Device facts").
Extra for the desktop:

- UDP discovery: send the text `hello` to UDP broadcast port 8134. Each
  CrossPoint replies with `crosspoint (on <hostname>);81`. The sender
  address is the device IP.
- mDNS `crosspoint.local` resolves on most LANs. Try it as a fallback.
- Verified in firmware 1.6.5 source (2026-10-10): the UDP listener on 8134
  runs in every transfer mode (Join Network with the IP + QR screen, Create
  Hotspot, Calibre Wireless). The reply is unicast to the sender. The device
  polls UDP only while that screen is open. The hostname is
  `CrossPoint-Reader-<MAC>`. mDNS has an A record only, no service record.
- "Calibre Wireless" mode is NOT the calibre smart-device protocol. It is the
  same web server: HTTP 80, WebSocket 81, UDP 8134. The CrossPoint calibre
  plugin uploads with the WebSocket protocol. Our `crosspoint-ws` transport
  already speaks it. There is nothing to hijack.
- Wi-Fi access points and ESP32 power save often drop broadcast frames. So
  discovery also sends unicast "hello" to every host of the local /24 and
  then probes `GET /api/status` on port 80 with 32 threads.
  CLI: `krosspoint_sender --discover [--timeout 2000]`.

## Public API per module

### extract/ (Agent E) — `src/extract/extract.h`, namespace `ks::extract`

```cpp
std::optional<std::string> normalizeUrl(const std::string& in);   // adds https://, rejects non-http(s), strips utm_* etc, lowercases host
bool isWikipediaUrl(const std::string& url);
InputKind detectInputKind(const std::string& text, const std::string& fileName = "");

struct FetchResult { bool ok; int status; std::string body; std::string finalUrl; std::string contentType; std::string error; };
FetchResult fetchUrl(const std::string& url, const std::function<bool()>* cancelled = nullptr, int timeoutSec = 20);   // libcurl GET, follows redirects, UA "k-rosspointSender/0.1"
FetchResult fetchWikipedia(const std::string& url);   // MediaWiki action=parse (prop=text|displaytitle, redirects=1, disableeditsection=1, formatversion=2); wraps into <h1 id="firstHeading"> + <div id="mw-content-text">
struct WikiRef { std::string lang, title, host; };
std::optional<WikiRef> parseWikipediaUrl(const std::string& url);

Article extractArticle(const RawInput& in, const ExtractOptions& opts = {});   // dispatch by kind; url/html/bookmarklet with a Wikipedia sourceUrl use the Wikipedia path
Article extractWikipedia(const std::string& html, const std::string& url, const ExtractOptions& opts = {});
std::string sanitizeHtml(const std::string& html, const ExtractOptions& opts, const std::string& baseUrl);  // allowlist; returns a well-formed XHTML fragment
std::string markdownToHtml(const std::string& md);     // md4c, then sanitize
std::string textToHtml(const std::string& txt);        // paragraphs on blank lines; joins hard-wrapped prose; escapes
int countWords(const std::string& html);
std::string htmlToText(const std::string& html);       // text content, block tags become newlines
ImageMap fetchImages(const Article& a, const std::function<bool()>* cancelled = nullptr);   // best effort; data: URLs decoded; never throws
```

Rules:
- Parse with `htmlReadMemory(..., HTML_PARSE_RECOVER|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_NONET)`.
- Readability port: remove script/style/nav/header/footer/aside/form/
  iframe/noscript, elements with role banner/navigation/contentinfo, and
  class/id matching the junk regex (ad, ads, advert, banner, cookie,
  comment, share, social, sidebar, related, recommend, promo, newsletter,
  popup, modal, footer, nav, menu, breadcrumb, subscribe, paywall).
  Score candidate containers by paragraph count, text length, comma count,
  link density (drop > 0.5), and class/id hints (article, content, main,
  post, entry, body, text = +; the junk list = -). Pick the best, also
  include siblings with score >= 20% of the best. Prefer `<article>` and
  `<main>` when present with enough text.
- Metadata: `<title>`, og:title, og:site_name, article:published_time,
  meta author, `<time datetime>`, JSON-LD (headline, author.name,
  datePublished) parsed with nlohmann json, `<html lang>`.
- Sanitizer allowlist: h1-h6, p, br, hr, ul, ol, li, blockquote, pre, code,
  b, strong, i, em, u, s, del, sub, sup, span, a[href], img[src,alt],
  figure, figcaption, table, thead, tbody, tr, th, td, caption, dl, dt, dd.
  Attributes kept: href, src, alt, id, colspan, rowspan. Resolve relative
  URLs against baseUrl. Drop javascript: and data: hrefs. Unwrap unknown
  tags (keep their children). Drop empty p/div. Convert `<div>` to `<p>`
  when it holds only inline content, else unwrap it. A figcaption that
  holds a block element becomes a `<div class="caption">`.
- Headings get ids `sec-1`, `sec-2`, ... and go into `toc`.
- Wikipedia: parse `#mw-content-text .mw-parser-output`. Remove `.infobox`
  unless keepInfobox. Remove `.navbox`, `.vertical-navbox`, `.sidebar`,
  `.mw-editsection`, `.hatnote`, `.ambox`, `.shortdescription`, `#toc`,
  `.toc`, `.metadata`, `.catlinks`, `.mw-jump-link`, `.noprint`,
  `.portal`, `#coordinates`, `.mw-empty-elt`, `style`. Remove sections
  titled See also, External links, Further reading, Bibliography always;
  References, Notes, Citations and `sup.reference`, `.reflist`,
  `.mw-references-wrap` unless keepReferences. `.thumb`/`figure` become
  figure+img+figcaption. Image src: last srcset entry, https. Drop icons
  under 24 px. Title from `#firstHeading`. Author "Wikipedia contributors".
- Output `bodyHtml` must be well-formed XML. Serialize via libxml2
  `xmlNodeDump` of the cleaned nodes in XHTML mode (self-closed voids).

### output/ (Agent F) — `src/output/output.h`, namespace `ks::output`

```cpp
const StyleSettings& preset(StylePreset p);              // same numbers as the web PRESETS
std::string epubCss(const StyleSettings& s);             // same CSS as the web app
OutputFile buildEpub(const Article& a, const BuildOptions& o, const ImageMap& images = {});
OutputFile buildBundleEpub(const std::vector<Article>& as, const std::string& title, const std::string& author, const BuildOptions& o, const ImageMap& images = {});
OutputFile buildHtml(const Article& a, const BuildOptions& o);
OutputFile buildTxt(const Article& a, const BuildOptions& o);
OutputFile buildMarkdown(const Article& a, const BuildOptions& o);
OutputFile buildZip(const Article& a, const BuildOptions& o, const ImageMap& images = {});
OutputFile testEpub();                                   // "krosspoint-test.epub"
std::string sanitizeFilename(const std::string& name, const FilenameRules& r = {});
std::string makeFilename(const Article& a, OutputFormat f, const FilenameRules& r);
struct Processed { std::string bytes; std::string mime; int width; int height; };
std::optional<Processed> processImage(const std::string& bytes, int maxWidth, bool grayscale, int quality);   // stb decode (jpg/png/gif/bmp), resize, gray, JPEG baseline out; nullopt when undecodable (webp, svg)
struct Estimate { int minutes; int pages; };
Estimate estimateReading(const Article& a, const StyleSettings& s);  // 230 wpm; words/page 210 compact, 170 comfortable, 120 large, 190 reference, scaled by fontSizePercent
```

Rules: identical EPUB structure to the web app (see `../docs/ARCHITECTURE.md`
"epub/"): mimetype first and STORED (miniz `MZ_NO_COMPRESSION`), container,
OPF (EPUB 3 + NCX fallback), nav.xhtml, toc.ncx with unique ids and
gap-free playOrder (parent takes its number before children), style.css,
cover.xhtml (text title page), chapter-NNN.xhtml split at top-level h1/h2
when more than one, images/img-NNN.jpg. No directory entries in the ZIP.
Never leave a remote img src: drop the img and emit `<p class="img-alt">`.
The result must pass epubcheck.

### net/ (Agent G) — `src/net/net.h`, namespace `ks::net`

```cpp
Transport& transport(TransferMethod m);                  // singleton per method
std::vector<TransferMethod> methods();
struct Discovered { std::string ip; std::string hostname; int wsPort; std::string raw; };
std::vector<Discovered> discoverCrossPoint(int timeoutMs = 1500);   // UDP broadcast "hello" to 8134 on every interface; parse "crosspoint (on <host>);81"
struct Explanation { std::string title; std::vector<std::string> reasons; std::vector<std::string> fallbacks; };
Explanation explain(const TransferError& e);

// profiles
std::vector<DeviceProfile> listProfiles();
std::optional<DeviceProfile> getProfile(const std::string& id);
DeviceProfile saveProfile(DeviceProfile p);              // fills id/timestamps; throws std::runtime_error with the problem list
void deleteProfile(const std::string& id);
void markConnected(const std::string& id);
DeviceProfile defaultProfile(FirmwareType f);
std::vector<std::string> validateProfile(const DeviceProfile& p);

// settings
AppSettings loadSettings();
void saveSettings(const AppSettings& s);

// library (files under dataDir()/library/<id>/entry.json + images/)
void saveEntry(const LibraryEntry& e, const ImageMap* images = nullptr);
std::optional<LibraryEntry> getEntry(const std::string& id);
std::vector<LibraryEntry> listEntries();                 // newest first
ImageMap loadImages(const std::string& id);
void deleteEntry(const std::string& id);
void clearLibrary();
void addTransfer(const std::string& articleId, const TransferRecord& r);

// helper server for the web app
class HelperServer {
public:
    bool start(int port, const std::vector<std::string>& allowedOrigins);   // 127.0.0.1 only
    void stop();
    bool running() const;
    std::string lastError() const;
};
```

Transports (one file each): `webdav.cpp`, `crosspoint_http.cpp`,
`crosspoint_ws.cpp`, `folder.cpp`, `ble_mock.cpp`. Same behaviour as the
web spec: WebDAV MKCOL each segment then PUT then HEAD verify; HTTP
`/mkdir` then `/upload?path=` multipart `file`, on "already exists"
`/delete` then retry once, verify via `/api/files`; WS protocol on port 81
with 16 KiB chunks (libcurl `CURLWS_BINARY`, `curl_ws_send`/`curl_ws_recv`);
Folder writes `<folder>/<targetFolder>/<name>` and verifies size; BLE mock
simulates pair/metadata/chunks/checksum/commit with progress and documents
the future K-Reader protocol in its header comment.
Progress callbacks use `CURLOPT_XFERINFOFUNCTION`. Timeouts: connect 5 s.
`testConnection`: GET `/api/status` for CrossPoint methods (put the JSON in
`info`), OPTIONS `/` for WebDAV, directory exists for Folder.

Helper server endpoints (match `../helper/README.md` so the web app works
unchanged): `GET /status` -> `{"ok":true,"version":"desktop-0.1"}`;
`POST /send` multipart `file`, `host`, `port`, `folder`, `method`
(`webdav` | `crosspoint-http`) -> `{"ok":true,"remotePath":..,"bytes":..}`
or `{"ok":false,"error":..,"code":..}`; `POST /test` JSON `{host,port}`
proxies `/api/status`. CORS: `Access-Control-Allow-Origin` echoes an
allowed origin only; `OPTIONS` -> 204 with
`Access-Control-Allow-Private-Network: true`. Never allow origin `null`.
Also `GET /discover` -> JSON list from `discoverCrossPoint()`.

Persistence: JSON files in `dataDir()`: `settings.json`, `devices.json`,
`library/<id>/entry.json`. Atomic writes via `writeFile`.

### app/ (Agent H) — `src/app/`

SDL2 + OpenGL3 + ImGui, docking not required. Window title
"k-rosspointSender". Layout: three columns like the web app.

- Left "Source": tabs URL / Wikipedia / Paste HTML / Paste text / Markdown /
  File. A big drop target: `SDL_DROPFILE` loads a file by extension,
  `SDL_DROPTEXT` with a URL fetches it, other text is pasted text. "Paste"
  button reads the clipboard. On Windows read `CF_HTML` first (real HTML
  from a browser copy, strip the CF_HTML header, use SourceURL as
  sourceUrl), else `SDL_GetClipboardText`. Fetch status and error text with
  the fallback list.
- Center "Preview": the article as plain text with headings (render
  `htmlToText` with heading markers; ImGui cannot render HTML). Show
  reading estimate, word count, image count, heading count. A "Device
  preview" checkbox renders the text wrapped to a 480x800 frame.
- Right: Metadata (title, author, date, source, tags, filename preview via
  `makeFilename`), Output (format, include cover/source note/images,
  Export... with a native save dialog on Windows via `GetSaveFileNameW`, or
  write into `settings.lastExportDir`), Device (profile combo, method
  combo, Discover button -> fills host from `discoverCrossPoint`, Test
  Connection, Send Test EPUB, Send, status line with the five states and a
  progress bar, explain block with fallback buttons on failure).
- Top bar: Library window (list, re-export, send, delete, clear, multi-
  select -> Bundle), Settings window (all style and advanced settings,
  helper server toggle and port and origins, data dir path), Devices window
  (4-step wizard: Add Device, Choose Transfer Method, Test Connection, Send
  Test Article), Log window (every network action, one line each).
- Background jobs: fetch, extract, image fetch, build, send run on
  `std::thread`; results posted to the UI through a mutex-protected queue
  drained each frame. The UI never blocks. Cancel button sets the cancel
  flag.
- Every export and send saves the article to the library.
- Start the helper server at boot when enabled. Show its state in the
  status bar ("Helper: on 127.0.0.1:8134").
- Font: "crisp" system monospace (Cascadia Mono, then Consolas) at 16 px
  times the display scale, else the ImGui default. UI scale presets and
  slider in Settings. Theme: weatherDesktop Korra Dark by default, Paper
  Light, and user JSON themes (k-os style Theme struct with color_fields).
  UI prefs (theme, scale, font, keybinds, geometry) live in
  `<data>/ui_prefs.json`; custom theme in `<data>/ui_theme.json`.
  Old text: ImGui default font, 16 px. Theme: dark,
  amber accent (`#e07942`).
- Also a CLI mode: `krosspoint_sender --send <file> --host <ip> [--port 80]
  [--folder /Books/K-rosspoint/] [--method webdav|http]` and
  `--convert <url-or-file> --out <file.epub>` that run without a window
  and print the result. On Windows the WIN32 subsystem has no console;
  attach with `AttachConsole(ATTACH_PARENT_PROCESS)` when args are given.

## Tests (ctest)

- extract: normalizeUrl, detectInputKind, sanitize allowlist, extractArticle
  on `../tests/fixtures/article-blog.html`, extractWikipedia on
  `../tests/fixtures/wikipedia-sample.html` (infobox and refs removed, kept
  on opt-in), ids on headings, parseWikipediaUrl, textToHtml, markdownToHtml.
- output: sanitizeFilename, makeFilename, buildEpub (mimetype first + stored
  via miniz read-back, OPF has dc:title/dc:source, nav + NCX present, NCX
  playOrder gap-free and ids unique, chapters parse with libxml2 XML parser
  (not HTML), no remote img), bundle chapter count, txt/md content,
  processImage on a generated PNG (resize to 480, gray, JPEG magic bytes),
  estimateReading.
- net: profile validate/save/load/delete in a temp data dir
  (`KS_DATA_DIR` env var overrides `dataDir()`), settings round trip,
  library save/list/delete/clear/addTransfer, transports against a tiny
  in-process cpp-httplib fake device (WebDAV PUT/MKCOL/HEAD, /upload with
  "already exists" then /delete retry, /api/status), explain(), BLE mock
  reaches Complete, helper server `/status` and `/send` through the fake
  device, UDP discovery parser on the reply string.

## UI as built (2026-10-10, after the real-device test)

- Top menu bar, right side: device combo (saved profiles, then
  `add device...` and `manage devices...`), amber `send` button, status pill
  with a thin progress bar and a tooltip that holds the full message or the
  failure explanation. With no profile an `add device` button replaces the
  combo and `send` is disabled.
- Devices window (`win_devices.cpp`, `device_setup.cpp`): `found on the
  network` list with `discover` (auto-runs once) and `add` per row; `saved
  devices` with `use`, `edit`, `delete`, and a tools popup (`test
  connection`, `send test epub`); inline add/edit form with `test`,
  `confirm` (enabled after a good test; sends the test EPUB, marks connected,
  saves, selects as default) and `cancel`. The old 4-step wizard is merged
  into this window.
- Columns (`layout.cpp`): two splitters (6 px hit zone, min 220 px sides,
  center min 320 px, double-click resets), collapsible side columns (24 px
  strip). Prefs: `left_width`, `right_width`, `left_collapsed`,
  `right_collapsed`, `device_fit`. Actions: `toggle_left_panel` (Ctrl+[),
  `toggle_right_panel` (Ctrl+]). `view` menu has the toggles.
- Right column: `metadata` and `output` only. Source column: short tab
  labels, dashed drop zone fills the free space. Preview: text capped at 92
  characters and centered; `[ fit ]` / `[ 100% ]` for the device frame.
- Test hook: with env `KS_UI_TEST=<file>` the app reads `move`, `click`,
  `double`, `drag`, `action <id>` commands and feeds them to ImGui
  (`uitest.cpp`). Unset = off.

## Web parity as built (2026-10-10, Agents N and O)

- `src/spider/`: `normalizeWikipediaTitle`, `scanWikipediaLinks`,
  `fetchPackPages`, `scanWebLinks`, `rewriteLinks`, `packSlug`, `newPack`,
  `fillPagesFromCandidates`, `setFetchOverride` (test hook),
  `CancelledError`. Cancel: a cancelled scan throws `CancelledError`; a
  cancelled fetch returns the pages so far with the last progress Cancelled.
- `src/output/pack.cpp` + `pack_parts.*`: `buildPackEpub`. `dom.cpp` has the
  definition-list fix and keeps the `ext`/`source` classes.
- `src/extract/fetch.cpp`: `fetchUrlRetry` (429/503, Retry-After, 2/4/8 s).
- `src/net/`: `listRemoteFolder`, `makeRemoteFolder`, pack storage under
  `<data>/packs/<id>/`, `PackRecord` (file name + send history),
  `AppSettings.packLimits` in settings.json.
- `src/app/`: `[pack]` tab wizard (pack_wizard, pack_select, pack_jobs),
  pack mode in the center column (panel_pack), library packs
  (win_library_packs), settings section, device-form folder browser
  (folder_browser), CLI `--pack <title-or-url> --out <file> [--depth]
  [--max] [--lang]`. Font fallback Segoe UI Symbol for the arrow glyph.
- Verified: ctest 9/9; CLI live pack (Steam engine, 4 articles, 8 images)
  and the spider live test both pass epubcheck; GUI driven with KS_UI_TEST
  against the fake device: scan, select, limit toast, build, send,
  library re-send with delete-and-retry, folder browser creating
  `/Books/K-rosspoint/`. Screenshots in `build/shots3/`.
- The web fake device (`tools/fake-crosspoint.py`) has no PROPFIND; WebDAV
  listing is tested only by the C++ in-process fake in test_net.

## Verification done 2026-10-10

- Full build (`build/`, RelWithDebInfo) and Release build (`build-rel/`) with
  no warnings. `ctest`: test_extract, test_extract_live, test_output,
  test_net all pass (227 + 463 + 240 checks).
- CLI `--convert https://en.wikipedia.org/wiki/Electronic_paper` gave a
  323 KB EPUB with 10 images. epubcheck 5.3: zero errors. Same for the
  stripped standalone `dist/` build with a clean PATH (TLS works).
- CLI `--send` to the fake CrossPoint server (`../tools/fake-crosspoint.py`)
  over WebDAV (MKCOL, PUT, HEAD) and CrossPoint HTTP (twice: the second run
  hit "already exists", deleted and retried). Bytes identical on the fake
  SD card.
- GUI: Ctrl+V with the blog fixture on the clipboard loads the article,
  fills the metadata, renders headings, shows the stats line and the helper
  server state. UTF-8 (the degree sign) renders correctly.
- Git Bash note: run the exe with `MSYS2_ARG_CONV_EXCL="*"` or from
  PowerShell, else Git Bash rewrites `/Books/K-rosspoint/` into a Windows path.
- A transient g++ internal compiler error appeared once after a machine
  glitch; a rebuild cleared it.
- Not yet tested on a real X4 Pro: WebSocket upload on port 81, UDP
  discovery on a real LAN, WebDAV and HTTP against the ESP32, Folder method
  in USB Drive mode, helper mode with the hosted web app in Chrome.
- `scripts/package.sh [build-dir]` fills `dist/` (exe + 15 mingw64 DLLs,
  about 15 MB).

## Done criteria

- `cmake -S desktop -B desktop/build -G Ninja && cmake --build desktop/build`
  with no warnings treated as errors; `ctest` passes.
- The app starts, accepts a dropped URL, shows the preview, exports an
  EPUB that passes epubcheck, and sends it to the fake device.
- `krosspoint_sender --convert <url> --out x.epub` works from a console.

## Addendum 2026-10-10: web parity (Spider Pack mode, folder browser)

Mirror of the web app addendum in `../docs/ARCHITECTURE.md` ("Addendum
2026-10-10: Spider Pack mode"). Types are in `core/types.h` under "spider
packs". `AppSettings.packLimits` holds the defaults.

### spider/ (Agent N) — `src/spider/spider.h`, namespace `ks::spider`

```cpp
struct WikiRoot { std::string lang, title, url; };
std::optional<WikiRoot> normalizeWikipediaTitle(const std::string& input, const std::string& lang = "en");  // URL or title
std::vector<PackCandidate> scanWikipediaLinks(const WikiRoot& root, const PackLimits& limits, const PackProgressFn& onProgress, const bool* cancel = nullptr);
Pack fetchPackPages(Pack pack, const ExtractOptions& opts, const PackProgressFn& onProgress, const bool* cancel = nullptr);
std::vector<PackCandidate> scanWebLinks(const Article& a, const std::string& baseUrl, const PackLimits& limits);
struct Rewritten { std::vector<PackPage> pages; std::vector<PackExternalLink> external; };
Rewritten rewriteLinks(const std::vector<PackPage>& pages, PackSourceKind kind);
std::string packSlug(const std::string& title);
Pack newPack(PackSourceKind kind, const std::string& rootKey, const std::string& rootUrl, const std::string& title, const PackLimits& limits);
void fillPagesFromCandidates(Pack& pack, const std::vector<PackCandidate>& cands);   // one PackPage per candidate, state Queued
```

Rules (same as the web `src/spider/*.ts`, port them):
- MediaWiki API `action=parse&page=<title>&prop=text|links&redirects=1&disableeditsection=1&format=json&formatversion=2`.
  Breadth-first to `limits.depth`. `requestDelayMs` between calls. Never
  more than `maxArticles * 5` candidates. Namespaces Help, File, Template,
  Category, Talk, Special, Portal, User, Wikipedia, Draft, Module,
  MediaWiki are dropped, plus edit links, `#cite` anchors, links inside
  `.navbox`, `.vertical-navbox`, `.sidebar`, `.infobox`, `.reflist`,
  `.mw-references-wrap`, `sup.reference`, `.hatnote`, `.catlinks`, `#toc`,
  `.mw-editsection`, `.metadata`, `.ambox`, language links, interwiki
  (`.extiw`), redlinks (`.new`), self links. Body-text links rank before
  links that occur only in figures/captions. Disambiguation flagged via
  `action=query&prop=pageprops&ppprop=disambiguation&redirects=1&titles=`
  batched 50. Redirect aliases merge into the target; the `redirect` flag
  stays only when no link used the real title. Root = candidate 0,
  selected. Depth-1 non-disambiguation candidates are selected until
  `maxArticles` (root included).
- HTTP 429/503: wait `Retry-After` (max 60 s) else 2, 4, 8 s, 3 retries,
  cancellable, progress message "rate limited, waiting Ns". Put this retry
  in a shared helper used by `fetchWikipedia` in extract/ too (you may edit
  `src/extract/fetch.cpp` for that one change).
- `fetchPackPages`: sequential over `pack.pages`; `selected == false` ->
  Skipped; already Done pages are not refetched; Wikipedia pages via
  `extract::fetchWikipedia` + `extract::extractWikipedia`
  (limits.includeInfobox/includeReferences override opts); web pages via
  `fetchUrl` + `extractArticle`; truncate `Article.images` to
  `maxImagesPerArticle` and remove the extra `<img>` (and empty figures)
  from bodyHtml; per-page errors recorded; progress per page; ends
  Complete, or Failed when every page failed; Cancelled when `*cancel`.
- `rewriteLinks`: bundled target -> `href="<fileName>"` (fragment kept only
  for a self link to an existing id); same-wiki article link not bundled ->
  class `ext` + `<span class="ext">&#x2197;</span>` marker and an entry in
  `external` (deduped by key, fromKey = first holder); other sites and
  other-language wikis unchanged; junk (edit, redlink, non-article
  namespace, javascript, empty) unwrapped; same-page `#id` kept only when
  the id exists. Assign unique `fileName` = `packSlug(title) + ".xhtml"`
  when empty; reserved names index, nav, toc, external-links, missing,
  attribution, cover, title get `_2`. Idempotent. Inputs not mutated.
- `packSlug`: lowercase ascii, fold Latin accents, non-alnum runs -> `_`,
  trim `_`, max 48, never empty ("page").
- Use libxml2 for the HTML walks, nlohmann json for the API.

### output/ additions (Agent N) — in `src/output/output.h`

```cpp
OutputFile buildPackEpub(const Pack& pack, const BuildOptions& o, const ImageMap& images, const PackProgressFn& onProgress = {});
```
Structure exactly as the web pack (`EPUB/package.opf`, `EPUB/nav.xhtml`,
`EPUB/toc.ncx`, `EPUB/css/krosspoint.css`, `EPUB/articles/index.xhtml`,
`<slug>.xhtml`, `external-links.xhtml`, `missing.xhtml`,
`attribution.xhtml`, `EPUB/images/<slug>_NN.jpg`). Each article:
`<section><h1>title</h1><p class="source">Source: <site>, captured <date></p>...`.
`dc:creator` = pack.author, else "Wikipedia contributors" for Wikipedia,
else "Various". Size limit: drop images from the page with the most image
bytes first, rebuild, then throw `std::runtime_error("Pack is X MB; the
limit is Y MB. Lower the article count or the image limit.")`. missing.xhtml
also lists Failed/Skipped pages. Attribution: every page title + URL,
capture dates, the CC BY-SA 4.0 sentence for Wikipedia, "Generated by
k-rosspointSender". Also port the two web fixes into the shared body
preparation (`src/output/dom.cpp`): a `<dl>` with no `<dd>` becomes
`<p><b>dt</b></p>` paragraphs (dangling `<dt>` likewise, `<dd>` without
`<dt>` gets an empty `<dt/>`), and the `ext` class survives on `<a>` and
`<span>`. Must pass epubcheck.

### net/ additions (Agent O) — in `src/net/net.h`

```cpp
struct RemoteEntry { std::string name; uint64_t size; bool isDirectory; };
std::vector<RemoteEntry> listRemoteFolder(const DeviceProfile& p, const std::string& path);   // crosspoint-http: GET /api/files?path=; webdav: PROPFIND Depth 1; others throw TransferError Unsupported
void makeRemoteFolder(const DeviceProfile& p, const std::string& path, const std::string& name); // POST /mkdir or MKCOL (405 ok)
// packs (files under dataDir()/packs/<id>/pack.json + images/)
void savePack(const Pack& pack, const ImageMap* images = nullptr);
std::optional<Pack> getPack(const std::string& id);
std::vector<Pack> listPacks();              // newest first
ImageMap loadPackImages(const std::string& id);
void deletePack(const std::string& id);
```
`clearLibrary()` also clears packs. `AppSettings.packLimits` round-trips
through settings.json.

### app/ additions (Agent O)

- Source column gets a `[pack]` tab = the 5-step Spider wizard (root:
  Wikipedia URL/title + language, or "current article + its links"; scan:
  depth 0/1/2 with a warning on 2, inline limits, scan button, progress
  "scanned N / M", cancel; select: checkbox list with depth badge, flags,
  linkCount, filter box, select all/none, remove, add by title/URL, counter
  "N selected / max M" that blocks over-selection; build: fetch pages with
  per-page state pills and errors, images per page, buildPackEpub, cancel
  keeps fetched pages; done: the pack is the current output).
- Center column in pack mode: article list with state pills; click shows
  that page's text; stats line sums the pack. Metadata column edits pack
  title/author/filename. Send/export use the pack file.
- Library window: packs section (open, re-export from stored pages + cached
  images, send, delete).
- Settings: "spider packs (defaults)" section bound to
  `AppSettings.packLimits`.
- Devices window form gets a destination folder browser (current path,
  sub-folders from `listRemoteFolder`, up, new folder, "use this folder");
  text field only for Folder/BLE.
- CLI: `--pack <wikipedia-url-or-title> --out <file.epub> [--depth 1]
  [--max 20] [--lang en]` builds a pack with the auto-selection; and
  `--send` works for it as before.
- All jobs on worker threads with cancel; `request_frame` wake.
