# k-rosspointSender desktop

The standalone version of k-rosspointSender. A small native app (C++,
Dear ImGui, SDL2, OpenGL). It does what the web app does, without the
browser limits:

- It fetches any article URL. There is no CORS block.
- It sends files to the Xteink X4 Pro over Wi-Fi. There is no mixed-content
  block.
- It finds CrossPoint devices on the LAN by UDP discovery.
- It serves as the local helper for the web version at
  `http://127.0.0.1:8134`, so the hosted app on k-engine.org can send over
  the LAN through it.
- Drag a link, a file, or text onto the window. Or paste a copied web page.
- It sends over Bluetooth LE (Windows) to a CrossPoint build with
  "Receive via Bluetooth" (ESP32-S3 boards such as the X4 Pro). On the
  reader open File Transfer > Receive via Bluetooth. No pairing. The
  protocol is `../docs/BLE_PROTOCOL.md`.
- "Send a file..." (top bar, Ctrl+Shift+F, or drop a non-article file)
  pushes any file to the SD card as it is, for example `firmware.bin` to
  `/` for the reader's Settings > System > SD Card Firmware Update.

## Privacy

Everything runs on your PC. The app makes only the network calls you ask
for: the article fetch, the image fetches, and the device transfer. The
helper server listens on `127.0.0.1` only and accepts requests only from
the allowed origins in Settings. Settings, device profiles and the library
live in your user data folder (`%APPDATA%\krosspoint-sender` on Windows,
`~/.local/share/krosspoint-sender` on Linux). No telemetry.

## Build (Windows, MSYS2 mingw64)

```
pacman -S mingw-w64-x86_64-{toolchain,cmake,ninja,SDL2,curl,libxml2,zlib,cppwinrt}
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S desktop -B desktop/build -G Ninja
cmake --build desktop/build
ctest --test-dir desktop/build
```

The binary is `desktop/build/krosspoint_sender.exe`. It needs the mingw64
runtime DLLs for SDL2, libcurl, libxml2 and zlib next to it, or on PATH.
`scripts/package.sh` (if present) copies them into `desktop/dist/`.

`cppwinrt` gives the C++/WinRT headers for the Bluetooth LE code
(`src/net/ble_win.cpp`, compiled as C++20). It adds no DLL.

Linux: install `libsdl2-dev libcurl4-openssl-dev libxml2-dev zlib1g-dev`,
then the same CMake commands. Bluetooth is not available there.

## Use

1. Start the app. Put the X4 Pro on the File Transfer screen.
2. Press Discover in the Device panel, or type the IP shown on the device.
3. Drop a link on the window, or paste a page, or open a file.
4. Check the preview and the metadata.
5. Press Send. Or Export to save the EPUB.

The Library keeps every converted article. Select several and press Bundle
to make one EPUB with one chapter per article.

## Spider packs

The `[pack]` tab builds one EPUB from a Wikipedia root article and the
pages it links to, the same way as the web app: enter the root, scan at
depth 0, 1 or 2, pick the pages, build, send. Bundled pages link to each
other inside the EPUB. Links to other articles stay online and show an
arrow. Packs live in the Library with re-export and send. Limits are in
Settings under "spider packs".

## Command line

```
krosspoint_sender --pack "Steam engine" --out pack.epub [--depth 1] [--max 20] [--lang en]
krosspoint_sender --convert <url-or-file> --out article.epub
krosspoint_sender --send article.epub --host 192.168.1.50 [--port 80] [--folder /Books/K-rosspoint/] [--method webdav|http]
krosspoint_sender --send article.epub --method ble [--ble AA:BB:CC:DD:EE:FF | --ble CrossPoint-3A7F]
krosspoint_sender --discover        (LAN and Bluetooth)   --discover-ble   (Bluetooth only)
krosspoint_sender --help
```

## Helper mode for the web app

Settings > Helper server. The default is on, port 8134, origins
`https://k-engine.org`, `http://localhost`, `http://127.0.0.1`. The web app
sends the EPUB to the helper, and the helper sends it to the device. The
file never leaves your machine except to the device.

## Layout

```
src/core/     shared types and helpers
src/extract/  HTML parsing, article extraction, Wikipedia, Markdown, fetch
src/output/   EPUB, HTML, TXT, Markdown, ZIP builders, images, filenames
src/net/      transports, discovery, helper server, profiles, settings, library
src/app/      ImGui app, panels, drag-and-drop, clipboard, jobs, CLI
tests/        ctest executables per module
docs/         DESKTOP.md (binding spec)
third_party/  imgui, stb, miniz, md4c, nlohmann json, cpp-httplib
```
