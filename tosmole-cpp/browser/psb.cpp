#include "psb.h"
#include <cctype>
#include <cstring>

namespace tosb {

namespace {
constexpr size_t kHeaderMin = 0x40;
constexpr size_t kCustomDataRel = 0x344;  // "Custom Data" tag offset within an emitter
constexpr size_t kFixed = 0x4B0;           // fixed emitter block size (before texture entries)
constexpr size_t kTexStride = 0x120;       // per-texture entry size
constexpr size_t kTexRel = 0x4B4;          // first texture path offset within an emitter
constexpr size_t kDeclTexOff = 0x234;      // editor's distinct-texture count

uint32_t rd32(const uint8_t* d, size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) |
           ((uint32_t)d[o + 3] << 24);
}

float rdf(const uint8_t* d, size_t o) {
    float f;
    std::memcpy(&f, d + o, sizeof(f));
    return f;
}

// Emitter param offsets (reversed from the .psb param block, validated against
// candle/fire effects): base tint RGB and the peak particle-size gradient.
constexpr size_t kColorOff = 0x120;   // 3 floats RGB (alpha lives in the gradient)
constexpr size_t kSizeLo = 0x17C, kSizeHi = 0x18C;

std::string cstrAt(const uint8_t* d, size_t size, size_t off) {
    std::string s;
    while (off < size && d[off]) { s += (char)d[off]; ++off; }
    return s;
}

// Basename of a possibly-Windows path, lowercased (the editor stores absolute
// "C:\...\light019.tga" paths; the shipped texture is keyed by basename).
std::string baseLower(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    auto sl = s.find_last_of('/');
    if (sl != std::string::npos) s = s.substr(sl + 1);
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
}  // namespace

PsbFile parsePsb(const uint8_t* d, size_t size) {
    PsbFile f;
    if (!d || size < kHeaderMin || std::memcmp(d, "PSB", 3) != 0 || d[3] != 0) return f;

    f.version = rd32(d, 4);
    uint32_t payload = rd32(d, 8);
    uint32_t hdr = rd32(d, 0xC);
    if (hdr < kHeaderMin) hdr = (uint32_t)kHeaderMin;
    size_t end = (size_t)hdr + payload;
    if (end > size) end = size;

    // Enumerate emitters via the once-per-emitter "Custom Data" tag.
    std::vector<size_t> starts;
    for (size_t i = hdr; i + 11 <= end; ++i) {
        if (std::memcmp(d + i, "Custom Data", 11) == 0 && i >= hdr + kCustomDataRel)
            starts.push_back(i - kCustomDataRel);
    }
    if (starts.empty()) return f;

    for (size_t k = 0; k < starts.size(); ++k) {
        size_t st = starts[k];
        size_t nx = (k + 1 < starts.size()) ? starts[k + 1] : end;
        size_t sz = (nx > st) ? (nx - st) : kFixed;
        int nTex = (sz >= kFixed) ? (int)((sz - kFixed) / kTexStride) : 0;
        if (nTex < 1) nTex = 1;

        PsbEmitter e;
        e.offset = (uint32_t)st;
        e.size = (uint32_t)sz;
        e.name = cstrAt(d, size, st);
        if (st + kDeclTexOff + 4 <= size) e.declaredTexCount = (int)rd32(d, st + kDeclTexOff);
        // Base tint (RGB) — clamp to [0,1]; alpha stays 1 (texture carries shape).
        if (st + kColorOff + 12 <= size) {
            for (int c = 0; c < 3; ++c) {
                float v = rdf(d, st + kColorOff + 4 * c);
                e.color[c] = v < 0 ? 0 : (v > 1 ? 1 : v);
            }
            e.color[3] = 1.0f;
        }
        // Peak particle size across the size gradient slots.
        float mx = 0;
        for (size_t o = kSizeLo; o + 4 <= size && o < kSizeHi; o += 4) {
            float v = rdf(d, st + o);
            if (v > mx && v < 1e5f) mx = v;
        }
        if (mx > 1e-4f) e.particleSize = mx;
        for (int t = 0; t < nTex; ++t) {
            size_t to = st + kTexRel + (size_t)t * kTexStride;
            e.textures.push_back(to < size ? baseLower(cstrAt(d, size, to)) : std::string());
        }
        f.emitters.push_back(std::move(e));
    }
    f.ok = true;
    return f;
}

}  // namespace tosb
