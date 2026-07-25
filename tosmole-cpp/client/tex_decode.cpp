#include "tex_decode.h"

#include "tos/ipf/ipf_archive.h"
#include "dds.h"
#include "stb_image.h"

#include <cctype>
#include <cstring>

using namespace tos::ipf;

namespace {
constexpr uint32_t kRGBA = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM

const IpfEntry* resolve_tex(App& app, const std::string& name,
                            const std::vector<std::string>& dirs) {
    if (name.empty()) return nullptr;
    std::string base = name;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    for (char& c : base) c = (char)tolower((unsigned char)c);

    std::vector<std::string> names{base};
    std::string dds = base;
    auto dot = dds.find_last_of('.');
    dds = (dot == std::string::npos ? dds : dds.substr(0, dot)) + ".dds";
    if (dds != base) names.push_back(dds);

    for (const auto& d : dirs) {
        if (d.empty()) continue;
        for (const auto& n : names)
            if (const IpfEntry* e = app.gameData.resolve(app.vfs, d + "/" + n)) return e;
    }
    for (const auto& n : names)
        if (const IpfEntry* e = app.resolveBasename(n)) return e;
    return nullptr;
}
} // namespace

DecodedTex decode_tex(App& app, const std::string& name, const std::vector<std::string>& dirs) {
    DecodedTex t;
    const IpfEntry* src = resolve_tex(app, name, dirs);
    if (!src) return t;
    try {
        auto data = IpfArchive::extract(*src);
        if (data.size() >= 4 && std::memcmp(data.data(), "DDS ", 4) == 0) {
            auto info = gw2dds::parse_dds(data.data(), data.size());
            if (info) {
                t.ok = true; t.fmt = info->dxgi_format; t.w = info->width; t.h = info->height;
                t.pitch = info->sys_mem_pitch; t.mip = info->mip_count;
                t.bytes.assign(data.begin() + info->data_offset, data.end());
                return t;
            }
        }
        int w, h, comp;
        unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &comp, 4);
        if (px) {
            t.ok = true; t.fmt = kRGBA; t.w = (uint32_t)w; t.h = (uint32_t)h; t.pitch = (uint32_t)(w * 4);
            t.bytes.assign(px, px + (size_t)w * h * 4);
            stbi_image_free(px);
            return t;
        }
    } catch (...) {}
    return t;
}
