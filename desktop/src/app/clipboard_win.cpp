// Clipboard reader. On Windows it reads CF_HTML ("HTML Format") first:
// real HTML from a browser copy, plus the SourceURL. Else it reads the
// plain text through SDL.
#include "app/app.h"

#include <SDL.h>

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ks::app {

namespace {

#ifdef _WIN32

// Read one "Key:value" header field from the CF_HTML description.
std::string headerField(const std::string& h, const std::string& key) {
    size_t p = h.find(key + ":");
    if (p == std::string::npos) return {};
    p += key.size() + 1;
    size_t e = h.find_first_of("\r\n", p);
    return trim(h.substr(p, e == std::string::npos ? std::string::npos : e - p));
}

long headerOffset(const std::string& h, const std::string& key) {
    std::string v = headerField(h, key);
    return v.empty() ? -1 : std::strtol(v.c_str(), nullptr, 10);
}

// Parse the CF_HTML block. The offsets are byte offsets into the UTF-8 data.
bool parseCfHtml(const std::string& raw, ClipboardContent& out) {
    long sh = headerOffset(raw, "StartHTML");
    long eh = headerOffset(raw, "EndHTML");
    long sf = headerOffset(raw, "StartFragment");
    long ef = headerOffset(raw, "EndFragment");
    const long n = static_cast<long>(raw.size());
    out.sourceUrl = headerField(raw, "SourceURL");
    if (sh >= 0 && eh > sh && eh <= n) {
        out.html = raw.substr(static_cast<size_t>(sh), static_cast<size_t>(eh - sh));
    } else if (sf >= 0 && ef > sf && ef <= n) {
        out.html = "<html><body>" + raw.substr(static_cast<size_t>(sf), static_cast<size_t>(ef - sf)) +
                   "</body></html>";
    } else {
        // No usable offsets. Use what follows the header.
        size_t p = raw.find('<');
        if (p == std::string::npos) return false;
        out.html = raw.substr(p);
    }
    return !trim(out.html).empty();
}

bool readCfHtml(ClipboardContent& out) {
    UINT fmt = RegisterClipboardFormatA("HTML Format");
    if (!fmt || !IsClipboardFormatAvailable(fmt)) return false;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(fmt);
    if (h) {
        const char* p = static_cast<const char*>(GlobalLock(h));
        if (p) {
            SIZE_T size = GlobalSize(h);
            std::string raw(p, strnlen(p, size));
            GlobalUnlock(h);
            ok = parseCfHtml(raw, out);
        }
    }
    CloseClipboard();
    return ok;
}

#endif

} // namespace

ClipboardContent readClipboard() {
    ClipboardContent c;
#ifdef _WIN32
    if (readCfHtml(c)) return c;
    c = ClipboardContent{};
#endif
    if (SDL_HasClipboardText()) {
        char* t = SDL_GetClipboardText();
        if (t) {
            c.text = t;
            SDL_free(t);
        }
    }
    return c;
}

} // namespace ks::app
