# K-rosspointSender architecture (binding spec)

Written in ASD-STE100 style. Short sentences. One fact per sentence.

## Purpose

K-rosspointSender turns web articles, Wikipedia pages, pasted HTML, pasted text,
Markdown, TXT files and local HTML files into clean EPUB, HTML, TXT, Markdown
or ZIP files. The files are tuned for the Xteink X4 Pro (480x800 e-ink)
running CrossPoint firmware, or a future K-Reader firmware.

## Core rule

All work happens in the browser. Nothing is sent to a backend. No telemetry,
no analytics, no remote logging. The app is a static site. The only network
calls are: the user-requested article fetch, and the user-requested device
transfer on the LAN.

## Stack

- Vite 8 + vanilla TypeScript 5 (strict). No framework.
- `@mozilla/readability` for article extraction.
- `dompurify` for sanitizing.
- `jszip` for EPUB and ZIP packaging.
- `turndown` for HTML to Markdown. `marked` for Markdown to HTML.
- `idb` for IndexedDB.
- `vitest` + `jsdom` + `fake-indexeddb` for tests.
- Build output: `dist/` with `base: './'`. It runs from any path.

## Module layout and ownership

```
src/types.ts            shared contract (frozen; see below)
src/extractors/         Agent A
src/converters/         Agent B
src/epub/               Agent B
src/transports/         Agent C
src/deviceProfiles/     Agent C
src/storage/            Agent C
src/settings/           Agent C
src/ui/                 Agent D
src/main.ts             Agent D
index.html              Agent D
bookmarklet/            Agent A
helper/                 Agent C (design doc + reference implementation)
tests/                  each agent owns tests for its own modules
docs/                   lead
```

Each agent writes only to its own folders. Shared code goes through the
public functions listed below. All public functions are exported from the
folder's `index.ts`.

## Device facts (CrossPoint, verified 2026-10-10 from source)

- Screen 480x800 portrait. Device profile in CrossPoint's optimizer:
  `{ width: 480, height: 800 }`.
- Formats: `.epub` (2 and 3), `.txt`, `.xtc`, `.bmp`. No HTML, no Markdown.
- No required book folder. Default upload folder is `/`. Firmware data is in
  `/.crosspoint/`. We default `targetFolder` to `/Books/Inbox/`. The folder
  must exist. Create it with WebDAV MKCOL or POST /mkdir first.
- HTTP server on port 80 while the device shows the File Transfer screen.
  No auth. CORS enabled on every response. OPTIONS returns 204.
- `POST /upload?path=<dir>` multipart, field `file`. 200 text
  `File uploaded successfully: <name>`. 400 text on error.
  Current master rejects a duplicate with `File already exists: <name>`.
- `GET /api/files?path=<dir>` JSON array of `{name,size,isDirectory,isEpub}`.
- `POST /delete` form `paths=<JSON array>`.
- `POST /mkdir` form `name`, `path`.
- `GET /api/status` JSON `{version, ip, mode, rssi, freeHeap, uptime, device}`.
- WebDAV at root on port 80: OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE,
  MKCOL, MOVE, COPY. PUT overwrites. 201 new, 204 replaced.
- WebSocket upload on port 81: send text `START:<filename>:<size>:<path>`,
  wait for `READY`, send binary frames, receive `PROGRESS:<recv>:<total>`,
  end with `DONE` or `ERROR:<msg>`.
- UDP discovery port 8134 (not reachable from a browser).
- mDNS name `crosspoint.local`. Hotspot: SSID `CrossPoint-Reader`, IP
  `192.168.4.1`.
- USB Drive mode on the X4 Pro exposes the SD card as a mass storage drive.
- EPUB renderer: HTML tags h1-h6, p, li, div, br, blockquote, ul, ol, hr,
  b/strong, i/em, u, del, sub, sup, span, a, img, table/tr/td/th, ruby.
  CSS properties read: text-align, text-indent, direction, font-style,
  font-weight, text-decoration, vertical-align, margin-*, padding-*, width,
  height, display, list-style-type. Units em, rem, pt. Everything else is
  ignored, including font-size and font-family.
- Images inside EPUB: JPEG and PNG only. Progressive JPEG is low quality.
  Convert everything else to baseline JPEG. Keep width <= 480 px.
- TOC: NCX and EPUB 3 nav both work. Write both.
- OPDS: device can browse an OPDS Atom catalog over HTTP Basic auth and
  download `application/epub+zip` acquisition links.

## Browser limits that shape the design

1. k-engine.org is HTTPS. A device on `http://192.168.x.x` is mixed content.
   Browsers block that fetch. So LAN transfer from the hosted app will fail
   in most browsers. The UI must say this and offer: USB folder write,
   manual download, run the app locally (`http://localhost`), or the local
   helper app.
2. Chrome Private Network Access may also block or prompt.
3. Article fetch depends on the target site sending CORS headers. Most do not.
   The UI must detect failure and offer paste HTML, bookmarklet, upload file,
   paste text.

## Public API per module

### extractors/ (Agent A)

```ts
normalizeUrl(input: string): string | null             // trims, adds https://, rejects non-http(s), strips tracking params, lowercases host
isWikipediaUrl(url: string): boolean
detectInputKind(text: string, fileName?: string): InputKind   // url vs html vs markdown vs text
fetchPage(url: string, signal?: AbortSignal): Promise<{ html: string; finalUrl: string }>  // throws FetchError {code:'cors'|'network'|'http'|'mixed-content'|'private-network'|'timeout', status?}
fetchWikipedia(url: string, signal?: AbortSignal): Promise<{ html: string; finalUrl: string }>  // MediaWiki action=parse with origin=*; plain article pages send no CORS headers
parseWikipediaUrl(url: string): { lang: string; title: string; host: string } | null
extractArticle(input: RawInput, opts?: ExtractOptions): Promise<Article>   // kind 'html'/'url'/'bookmarklet' with a Wikipedia sourceUrl also use the Wikipedia path
extractWikipedia(html: string, url: string, opts?: ExtractOptions): Article
sanitizeHtml(html: string, opts?: { keepLinks?: boolean; keepImages?: boolean; baseUrl?: string }): string
buildToc(bodyHtml: string): { bodyHtml: string; toc: TocEntry[] }  // adds ids to h1-h6
markdownToHtml(md: string): string
textToHtml(txt: string): string                         // paragraphs on blank lines, escapes
parseBookmarkletPayload(raw: string): BookmarkletPayload // accepts JSON or base64 JSON; throws on bad input
countWords(html: string): number
```

Rules:
- `extractArticle` dispatches on `input.kind`. For `url` and `wikipedia` the
  `content` is already-fetched HTML. The extractor never fetches by itself.
- Sanitizer allowlist: h1-h6, p, br, hr, ul, ol, li, blockquote, pre, code,
  b, strong, i, em, u, s, del, sub, sup, span, a[href], img[src,alt],
  figure, figcaption, table, thead, tbody, tr, th, td, caption, dl, dt, dd.
  Strip all attributes except href, src, alt, id, colspan, rowspan.
  Resolve relative `href` and `src` against `baseUrl`. Drop `javascript:` and
  `data:` hrefs. Keep `data:` image src only for images.
- Remove: script, style, nav, header, footer, aside, form, iframe, noscript,
  elements with role=navigation/banner/contentinfo, common ad/cookie/comment
  class and id patterns.
- Wikipedia: parse the `#mw-content-text .mw-parser-output` block. Remove
  `.infobox` unless keepInfobox. Remove `.navbox`, `.vertical-navbox`,
  `.sidebar`, `.mw-editsection`, `.hatnote`, `.ambox`, `.reference`,
  `.mw-references-wrap`, `sup.reference`, `#toc`, `.toc`, `.metadata`,
  `.catlinks`, `.mw-jump-link`, `.noprint`, `.portal`, `#coordinates`.
  Remove sections titled References, Notes, Citations, External links,
  See also, Further reading, Bibliography, unless keepReferences keeps the
  first three. Keep `.thumb` images with captions as figure/figcaption.
  Use `//upload.wikimedia.org` images with https. Title from `#firstHeading`.
  Author = "Wikipedia contributors". siteName = "Wikipedia".
- Every heading in `bodyHtml` has an `id` (`sec-1`, `sec-2`, ...).
- Images keep their absolute `src`. `Article.images` lists them in order.
- `countWords` counts whitespace-separated tokens of the text content.

### converters/ (Agent B)

```ts
buildHtml(article: Article, opts: BuildOptions): OutputFile
buildTxt(article: Article, opts: BuildOptions): OutputFile
buildMarkdown(article: Article, opts: BuildOptions): OutputFile
buildZip(article: Article, opts: BuildOptions, images?: Map<string, Blob>): Promise<OutputFile>   // epub + html + txt + md
sanitizeFilename(name: string, rules?: Partial<FilenameRules>): string   // strips reserved chars, trims, limits length, never empty ("untitled")
makeFilename(article: Article, format: OutputFormat, rules: FilenameRules): string
processImage(blob: Blob, opts: { maxWidth: number; grayscale: boolean; quality: number }): Promise<{ blob: Blob; mime: 'image/jpeg' | 'image/png'; width: number; height: number } | null>   // null = cannot convert (no canvas, unknown format); never throws
fetchImages(article: Article, signal?: AbortSignal): Promise<Map<string, Blob>>  // best effort, skips failures, never throws
estimateReading(article: Article, style: StyleSettings): { minutes: number; pages: number }
```

Rules:
- `estimateReading`: 230 words per minute. Pages = ceil(words / wordsPerPage)
  where wordsPerPage is 210 for compact, 170 comfortable, 120 large,
  190 reference, scaled by `fontSizePercent` (100 = base).
- Filenames: remove `<>:"/\|?*` and control chars, collapse whitespace,
  trim dots and spaces, max length applies before the extension.
- `processImage` uses canvas. Output JPEG baseline. Output PNG only when the
  source is PNG with transparency and `grayscale` is false. Never output
  WebP, GIF, SVG.

### epub/ (Agent B)

```ts
buildEpub(article: Article, opts: BuildOptions, images?: Map<string, Blob>): Promise<OutputFile>
buildBundleEpub(articles: Article[], meta: { title: string; author?: string }, opts: BuildOptions, images?: Map<string, Blob>): Promise<OutputFile>   // one chapter per article
epubCss(style: StyleSettings): string
PRESETS: Record<StylePreset, StyleSettings>
sendTestEpub(): Promise<OutputFile>   // a tiny valid EPUB named "krosspoint-test.epub"
```

Rules:
- EPUB 3 with EPUB 2 fallbacks: `mimetype` first and stored (no compression),
  `META-INF/container.xml`, `OEBPS/content.opf` (package version 3.0 with
  `dc:title`, `dc:creator`, `dc:language`, `dc:identifier` (urn:uuid),
  `dc:date`, `dc:source` (URL), `dc:subject` per tag, `dcterms:modified`),
  `OEBPS/nav.xhtml` (EPUB 3 nav with `epub:type="toc"`), `OEBPS/toc.ncx`,
  `OEBPS/style.css`, `OEBPS/cover.xhtml` (generated title page, no image
  needed), `OEBPS/chapter-001.xhtml` ..., `OEBPS/images/img-001.jpg` ...
- The cover page is XHTML text: title, author, site name, source URL,
  capture date, word count. No raster cover.
- Each chapter is valid XHTML 1.1 / XHTML5 (self-closing void tags, entities
  escaped). Serialize through the DOM, not string concat.
- The source note is a final `<section>` with "Source: <url>, captured
  <date>, generated by K-rosspointSender".
- CSS: simple. No floats, columns, @font-face, animations, position. Use only
  properties CrossPoint reads plus font-size/line-height/font-family-generic
  for other readers. Always include
  `img{max-width:100%;height:auto} table{max-width:100%;table-layout:fixed}`.
- Images: if `images` map has a blob for an `ArticleImage.src`, run
  `processImage` and write it. Else drop the `<img>` and leave the alt text
  in a `<p class="img-alt">`. Never leave a remote `src` in the EPUB.
- `PRESETS`:
  - compact: 90%, margin 0.4em, lh 1.25, para 0.3em
  - comfortable: 100%, margin 0.8em, lh 1.45, para 0.6em
  - large: 125%, margin 0.8em, lh 1.5, para 0.7em
  - reference: 95%, margin 0.5em, lh 1.35, para 0.4em, tables kept, links kept
- `buildBundleEpub`: one XHTML chapter per article, nav lists each article
  and nested headings, cover lists all titles.

### transports/ (Agent C)

```ts
getTransport(method: TransferMethod): TransportAdapter
listTransports(): TransportAdapter[]
explainBlock(err: TransferError, origin: string): { title: string; reasons: string[]; fallbacks: string[] }
```

Adapters (one file each): `webdav.ts`, `crosspointHttp.ts`,
`crosspointWs.ts`, `usb.ts`, `download.ts`, `opds.ts`, `bleMock.ts`,
`helper.ts` (talks to the optional local helper at `settings.helperUrl`).

Rules:
- Base URL = `http://${host}:${port}`. If `location.protocol === 'https:'`
  and host is not localhost, LAN adapters set `limitations()` to say
  mixed content will block, and `send` rejects early with
  `TransferError('mixed-content')` only after a real attempt fails. Try
  first, then explain.
- `webdav.send`: MKCOL target folder (ignore 405/409 when it exists),
  PUT `${folder}/${name}` with the blob, then `HEAD` to verify
  Content-Length when the server returns it. States: connecting (OPTIONS),
  uploading (PUT with XHR upload progress), verifying (HEAD), complete.
- `crosspointHttp.send`: POST `/mkdir` (ignore errors), then POST
  `/upload?path=<folder>` multipart field `file`. If the response is 400 and
  contains "already exists", POST `/delete` with `paths=["<path>"]` and
  retry once. Verify with `GET /api/files?path=` and compare size.
- `crosspointWs.send`: as documented above on port 81, chunk 16 KiB,
  progress from PROGRESS messages, verify via DONE.
- `crosspointHttp.testConnection`: GET `/api/status` with a 5 s timeout.
  `webdav.testConnection`: OPTIONS `/`. On failure map the browser error to
  codes: TypeError from fetch on an https page + http host => 'mixed-content',
  other TypeError => 'network', status 401/403 => 'auth', 404 => 'not-found'.
- `usb`: `showDirectoryPicker({ mode: 'readwrite', id: 'krosspoint-x4' })`.
  Store the handle in IndexedDB (`storage` exposes `saveHandle/loadHandle`).
  On send: `queryPermission`, then `requestPermission` if needed, create
  sub-folders from `targetFolder`, `getFileHandle(name, {create:true})`,
  write, close, then `getFile()` and compare size (verifying).
  Not supported => `TransferError('unsupported')`.
- `download`: anchor + `URL.createObjectURL`, always supported, complete at
  once.
- `opds`: `send` builds a ZIP with `catalog.xml` (OPDS 1.2 Atom acquisition
  feed listing the file) + the EPUB, and downloads it. It also exports
  `buildOpdsCatalog(entries: {title, author, fileName, bytes, updated}[], baseUrl: string): string`.
- `bleMock`: `isSupported()` returns `'bluetooth' in navigator`. `send` runs a
  simulated pipeline (pair, metadata, chunks with progress, checksum,
  commit) with no real radio. Document the future K-Reader BLE protocol in
  the file header: service UUID placeholder, characteristics for
  metadata/chunk/control/status, chunk size 244, CRC32 verify, commit to
  `/Books/Inbox/`.
- `helper`: not in `TransferMethod`; get it with `getHelperTransport()`. It POSTs `${helperUrl}/send` multipart with `file`, `host`, `port`,
  `folder`, `method`. GET `${helperUrl}/status`.
- Nothing fails silently. Every rejection is a `TransferError` with a human
  message.

### deviceProfiles/ (Agent C)

```ts
listProfiles(): Promise<DeviceProfile[]>
getProfile(id: string): Promise<DeviceProfile | undefined>
saveProfile(p: Partial<DeviceProfile> & { name: string }): Promise<DeviceProfile>   // fills id, timestamps, defaults
deleteProfile(id: string): Promise<void>
markConnected(id: string): Promise<void>
defaultProfile(firmware: FirmwareType): Omit<DeviceProfile, 'id'|'createdAt'|'updatedAt'>   // crosspoint: host crosspoint.local, port 80, webdav, /Books/Inbox/, epub
validateProfile(p: Partial<DeviceProfile>): string[]   // list of problems, empty when OK
```

### storage/ (Agent C)

IndexedDB database `krosspoint` version 1. Stores: `articles` (key
`article.id`, index `sourceUrl`, index `updatedAt`), `devices` (key `id`),
`settings` (key `'app'`), `handles` (key string, value FileSystemDirectoryHandle).

```ts
saveArticle(entry: LibraryEntry): Promise<void>
getArticle(id: string): Promise<LibraryEntry | undefined>
listArticles(): Promise<LibraryEntry[]>            // newest first
deleteArticle(id: string): Promise<void>
clearLibrary(): Promise<void>
addTransfer(articleId: string, rec: TransferRecord): Promise<void>
findBySourceUrl(url: string): Promise<LibraryEntry | undefined>
saveHandle(key: string, h: FileSystemDirectoryHandle): Promise<void>
loadHandle(key: string): Promise<FileSystemDirectoryHandle | undefined>
deleteHandle(key: string): Promise<void>
exportLibraryJson(): Promise<Blob>               // without image blobs
```

### settings/ (Agent C)

```ts
DEFAULT_SETTINGS: AppSettings
loadSettings(): Promise<AppSettings>              // merges stored over defaults
saveSettings(s: AppSettings): Promise<void>
resetSettings(): Promise<AppSettings>
```

### ui/ (Agent D)

Single page. Desktop-first three-column layout, collapsing to one column
under 900 px. Site look: k-engine.org palette (bg `#1c120f`, fg `#e6d6c4`,
amber `#e07942`, tan `#e0a773`, muted `#5c3f29`), monospace headings.

Left panel (Source): tabs URL / Wikipedia / Paste HTML / Paste text /
Markdown / File / Bookmarklet. Drag-and-drop zone over the whole panel.
"Fetch" button with status. On fetch failure show the `FetchError` reason
and the fallback list.

Center panel (Preview): cleaned article in a device-shaped frame
(480x800 aspect, scaled, scrollable, uses the same CSS as the EPUB).
Toggles: images on/off, preset selector, zoom. Below: reading minutes and
page estimate, word count, image count.

Right panel: Metadata editor (title, author, date, source URL, tags,
filename with live preview of `makeFilename`). Output (format select,
include cover, source note, images). Device (profile select, method, Test
Connection, Send Test EPUB, Send, status line with the five states).
Export (Download, Write to USB folder, Save ZIP).

Top bar: app name, Library button (drawer: list, re-export, send, delete,
clear all, bundle selected into one EPUB), Settings button (advanced
settings: CSS editor, filename rules, image compression, max image width,
grayscale, hyphenation, link handling, table handling, helper URL),
Devices button (setup wizard: Add Device, Choose Transfer Method, Test
Connection, Send Test Article).

The UI imports only the public functions above. No module imports from
`ui/`.

## Bookmarklet (Agent A)

`bookmarklet/bookmarklet.js`: captures `document.documentElement.outerHTML`,
URL, title, date; builds a `BookmarkletPayload`; opens the app URL with the
payload in `location.hash` as base64 JSON when under 1.5 MB, else copies
the JSON to the clipboard and alerts the user to paste it. The app reads
`#payload=` on load. Also `bookmarklet/build.mjs` that minifies it into a
`javascript:` URL and writes `bookmarklet/bookmarklet.txt`. The app URL is
a placeholder `__APP_URL__` replaced at build time with `./` relative or a
user-provided absolute URL.

## Local helper app (Agent C, design + reference)

`helper/README.md` and `helper/helper.py` (stdlib only, Python 3.10+).
Listens on `127.0.0.1:8134`. CORS allow-list for `https://k-engine.org` and
`http://localhost:*`. Endpoints: `GET /status` -> `{ok, version}`;
`POST /send` multipart `file`, `host`, `port`, `folder`, `method`
(`webdav` or `crosspoint-http`) -> `{ok, remotePath, bytes}` or
`{ok:false, error}`. `POST /test` `{host, port}` -> proxies `/api/status`.
Streams progress is out of scope. No logging of file contents.

## Testing

`npm test` runs vitest in jsdom. Required unit tests:

- extractors: normalizeUrl, detectInputKind, sanitizeHtml allowlist,
  extractArticle on `tests/fixtures/article-blog.html`, extractWikipedia on
  `tests/fixtures/wikipedia-sample.html` (infobox and references removed,
  kept when opted in), buildToc ids, parseBookmarkletPayload, and a mocked
  `fetchPage` CORS failure (`fetch` rejects with TypeError => FetchError
  code `cors` or `network`).
- converters/epub: sanitizeFilename, makeFilename, buildEpub (unzip with
  JSZip, check mimetype is first and stored, container.xml path, OPF has
  dc:title/dc:source, nav and NCX exist, chapter XHTML parses with
  DOMParser without parsererror, no remote img src), buildBundleEpub
  chapter count, buildTxt/buildMarkdown content, estimateReading.
- transports/storage/devices: profile save/load/delete/validate,
  settings merge, IndexedDB article round trip and delete and clear,
  transfer record append, every adapter implements the interface and
  `send` rejects with TransferError on a mocked failing fetch, webdav
  uses PUT and crosspointHttp uses multipart POST (mock fetch and assert),
  opds catalog XML validity, bleMock reaches `complete`.

## Verification done 2026-10-10

- `npm run build` and `npm test` pass (151 tests).
- Playwright drove the built file at `http://localhost`: paste HTML, file
  upload, live Wikipedia fetch (E Ink, 4 images), EPUB download, device
  wizard, WebDAV send to a fake CrossPoint server (MKCOL, PUT, HEAD verify).
- Generated EPUBs pass `epubcheck` 5.3 with zero errors and warnings.
- Not yet tested on a real X4 Pro. Points to check there: the `?path=` value
  without a trailing slash for `/upload`, URL-encoded names in WebDAV PUT
  paths, and how the device renders the generated CSS.

## Done criteria

- `npm run build` passes typecheck and emits `dist/`.
- `npm test` passes.
- The built app runs from `file://` and from `http://localhost`.
- README covers every item in the user brief.

## Addendum 2026-10-10: Spider Pack mode and handoff deltas

Source: the user's handoff document (K-rosspointSender Agent Handoff). Deltas
to what is built:

1. Default target folder is now `/Books/K-rosspoint/` (web and desktop).
2. Default transfer method for a CrossPoint profile is now `crosspoint-http`
   (multipart `POST /upload`). WebDAV and WebSocket stay as options.
   The HTTP transport creates the folder (`POST /mkdir`) and verifies with
   `GET /api/files?path=` after the upload (already the case).
3. Device wizard steps: Add Device, Test Connection, Choose Destination
   Folder (a folder browser over `GET /api/files` with a "new folder" that
   calls `POST /mkdir`), Send Test EPUB.
4. Spider Pack mode (types in `src/types.ts` under "Spider packs").

### spider/ (Agent K) — `src/spider/index.ts`

```ts
normalizeWikipediaTitle(input: string, lang?: string): { lang: string; title: string; url: string }   // accepts a URL or a title
scanWikipediaLinks(root: { lang: string; title: string }, limits: PackLimits, onProgress: (p: PackProgress) => void, signal?: AbortSignal): Promise<PackCandidate[]>
fetchPackPages(pack: Pack, extractOpts: ExtractOptions, onProgress: (p: PackProgress) => void, signal?: AbortSignal): Promise<Pack>
scanWebLinks(article: Article, baseUrl: string, limits: PackLimits): PackCandidate[]
rewriteLinks(pages: PackPage[], kind: PackSourceKind): { pages: PackPage[]; external: Pack['externalLinks'] }
packSlug(title: string): string
newPack(kind: PackSourceKind, rootKey: string, rootUrl: string, title: string, limits: PackLimits): Pack
DEFAULT_PACK_LIMITS: PackLimits
```

Rules:
- `scanWikipediaLinks`: MediaWiki API with `origin=*`. Per page use
  `action=parse&prop=text|links` (or `action=query&prop=links&plnamespace=0&pllimit=max`).
  Breadth-first to `limits.depth`. `requestDelayMs` between calls. Stop at
  `maxArticles * 5` candidates. Filter namespaces Help, File, Template,
  Category, Talk, Special, Portal, User, Wikipedia, Draft, Module,
  MediaWiki. Drop edit links, citation anchors (`#cite`), navbox links
  (links outside the `.mw-parser-output` body text), language links,
  redlinks (class `new`). Mark disambiguation pages with
  `action=query&prop=pageprops&ppprop=disambiguation` batched 50 titles.
  Order candidates by depth, then by first appearance in the body text.
  `linkCount` = how many scanned pages link to it. The root is candidate 0
  and selected. Depth-1 candidates are selected up to `maxArticles`; the
  rest are not selected.
- `fetchPackPages`: fetch the selected candidates in order (Wikipedia via
  `fetchWikipedia` + `extractWikipedia`; web via `fetchPage` +
  `extractArticle`). `requestDelayMs` between fetches. Record per-page
  errors. Never throw except on abort. Truncate `Article.images` to
  `maxImagesPerArticle`.
- `scanWebLinks`: links from the cleaned article body. Same domain only
  when `sameDomainOnly`. Dedupe. Drop fragments, mailto, javascript.
  `selected=false`.
- `rewriteLinks`: in each page's bodyHtml, a link whose target key is a
  bundled page becomes `href="<fileName>"`. A link to a known but not
  bundled page stays external and gets class `ext` plus a " ↗" marker.
  Citation and reference links keep their URL. Junk links (edit, nav) are
  unwrapped. Wikipedia target key from href: `/wiki/<Title>` (decode,
  underscores to spaces, strip the fragment), also for
  `//xx.wikipedia.org/wiki/`.

### epub/ additions (Agent L)

```ts
buildPackEpub(pack: Pack, opts: BuildOptions, images?: Map<string, Blob>, onProgress?: (p: PackProgress) => void): Promise<OutputFile>
```

Structure (EPUB 3 + NCX):
```
mimetype
META-INF/container.xml
EPUB/package.opf
EPUB/nav.xhtml
EPUB/toc.ncx
EPUB/css/krosspoint.css
EPUB/articles/index.xhtml            title page + "Included articles" list with links
EPUB/articles/<slug>.xhtml           one per page: <section><h1>title</h1><p class="source">Source: Wikipedia, captured <date></p>...</section>
EPUB/articles/external-links.xhtml   links that stay external, grouped by the article that holds them
EPUB/articles/missing.xhtml          "Missing linked articles": candidates seen but not bundled (title + URL)
EPUB/articles/attribution.xhtml      source titles + URLs, capture date, CC BY-SA 4.0 note for Wikipedia, "generated by K-rosspointSender"
EPUB/images/<slug>_01.jpg ...
```
Rules: call `rewriteLinks` first. Enforce `maxTotalEpubSizeMB` by dropping
images from the largest pages first, then fail with a clear error if still
over. Nav lists index, each article (nested headings), external links,
missing, attribution. `estimatedPages` summed. `dc:title` = pack title,
`dc:creator` "Wikipedia contributors" for Wikipedia packs. Must pass
epubcheck.

### UI additions (Agent M)

- Left panel gets a `[ pack ]` source tab = the Spider wizard: 1 Enter root
  (Wikipedia URL or title; or "current article + its links" for web),
  2 Scan links (depth, limits, Scan button, progress, cancel), 3 Select
  pages (checkbox list with search filter, select all / none, remove, add a
  page by title/URL, counts "N selected / max M"), 4 Build pack (progress
  per page, errors list, cancel), 5 Send or export (uses the normal device
  and output controls).
- Center panel in pack mode: article list of the pack (state per page,
  click to preview that page's cleaned text in the frame).
- Packs are saved to the library (new store `packs` keyed by id; storage
  exports savePack/getPack/listPacks/deletePack) with re-export.
- Settings: pack limit defaults.
- Device wizard gets the "Choose Destination Folder" step with the folder
  browser. Transports export `listRemoteFolder(profile, path)` and
  `makeRemoteFolder(profile, path, name)` for the crosspoint-http and webdav
  adapters.
- Defaults: `/Books/K-rosspoint/`, method `crosspoint-http`.
