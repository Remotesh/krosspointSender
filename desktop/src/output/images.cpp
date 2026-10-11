// Image re-encode for the EPUB (port of converters/images.ts).
// CrossPoint shows only JPEG and PNG. Progressive JPEG looks bad on it.
// So we re-encode every image to baseline JPEG with stb_image_write.
#include "output/internal.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace ks::output {

namespace {

void appendBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<std::string*>(ctx);
    out->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

// Images above this pixel count are not decoded (memory guard).
constexpr long long kMaxPixels = 60LL * 1000 * 1000;

} // namespace

std::optional<Processed> processImage(const std::string& bytes, int maxWidth, bool grayscale, int quality) {
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX)) return std::nullopt;
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = static_cast<int>(bytes.size());

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, len, &w, &h, &comp)) return std::nullopt;
    if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > kMaxPixels) return std::nullopt;

    // Decode as RGBA. Then put white behind transparent pixels (JPEG has no alpha).
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) return std::nullopt;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<unsigned char> rgb(count * 3);
    for (size_t i = 0; i < count; ++i) {
        const unsigned a = px[i * 4 + 3];
        for (size_t k = 0; k < 3; ++k) {
            const unsigned c = px[i * 4 + k];
            rgb[i * 3 + k] = static_cast<unsigned char>((c * a + 255u * (255u - a) + 127u) / 255u);
        }
    }
    stbi_image_free(px);

    // Scale down to maxWidth. Never scale up.
    const int maxW = std::max(16, maxWidth > 0 ? maxWidth : 480);
    const double scale = w > maxW ? static_cast<double>(maxW) / w : 1.0;
    const int nw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(h * scale)));
    if (nw != w || nh != h) {
        std::vector<unsigned char> out(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 3);
        if (!stbir_resize_uint8_srgb(rgb.data(), w, h, 0, out.data(), nw, nh, 0, STBIR_RGB)) return std::nullopt;
        rgb.swap(out);
    }

    // Grayscale with per-pixel Rec. 601 luma. We give stb one channel. stb
    // still writes YCbCr, but the chroma is flat and costs few bytes.
    int channels = 3;
    if (grayscale) {
        const size_t n = static_cast<size_t>(nw) * static_cast<size_t>(nh);
        std::vector<unsigned char> g(n);
        for (size_t i = 0; i < n; ++i) {
            const double y = 0.299 * rgb[i * 3] + 0.587 * rgb[i * 3 + 1] + 0.114 * rgb[i * 3 + 2];
            g[i] = static_cast<unsigned char>(std::min(255L, std::lround(y)));
        }
        rgb.swap(g);
        channels = 1;
    }

    // The web clamps the quality to 0.1..1. Here: 10..100, default 85.
    const int q = (quality >= 1 && quality <= 100) ? std::max(10, quality) : 85;
    Processed p;
    p.mime = "image/jpeg";
    p.width = nw;
    p.height = nh;
    p.bytes.reserve(rgb.size() / 4);
    if (!stbi_write_jpg_to_func(appendBytes, &p.bytes, nw, nh, channels, rgb.data(), q) || p.bytes.empty())
        return std::nullopt;
    return p;
}

} // namespace ks::output
