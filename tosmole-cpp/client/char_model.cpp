#include "char_model.h"
#include "tex_decode.h"

#include "tos/emfx/xac.h"
#include "tos/ipf/ipf_archive.h"
#include "xac_geometry.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

using namespace tos::ipf;

LoadedPart load_part(App& app, const std::string& vpath) {
    LoadedPart out;
    std::vector<uint8_t> bytes;
    if (!app.readAsset(vpath, bytes)) return out;

    tosb::XacGeometry geo;
    try {
        auto actor = tos::emfx::parseXac(bytes.data(), bytes.size());
        geo = tosb::buildXacGeometry(actor, /*bakeNodeTransforms=*/true);
    } catch (...) { return out; }
    if (!geo.ok || geo.verts.empty()) return out;

    m3d::Model* mdl = m3d::create();
    m3d::set_geometry(mdl, geo.verts.data(), geo.verts.size(),
                      geo.indices.data(), geo.indices.size(), (int)geo.bones.size());

    std::string texDir = app.gameData.textureDir(vpath);
    std::unordered_map<std::string, DecodedTex> cache;
    for (const auto& grp : geo.groups) {
        const DecodedTex* t = nullptr;
        if (!grp.textureName.empty()) {
            auto it = cache.find(grp.textureName);
            if (it == cache.end())
                it = cache.emplace(grp.textureName, decode_tex(app, grp.textureName, {texDir})).first;
            if (it->second.ok) t = &it->second;
        }
        m3d::GroupState st;
        st.alphaBlend = grp.alphaBlend; st.alphaTest = grp.alphaTest;
        st.zwriteOff = grp.zwriteOff; st.lightOff = grp.lightOff; st.alphaRef = grp.alphaRef;
        if (t) m3d::add_group(mdl, grp.indexOffset, grp.indexCount, t->fmt, t->w, t->h, t->pitch,
                              t->bytes.data(), t->mip, st);
        else   m3d::add_group(mdl, grp.indexOffset, grp.indexCount, 0, 0, 0, 0, nullptr, 1, st);
    }

    out.model = mdl;
    out.center[0] = geo.center[0]; out.center[1] = geo.center[1]; out.center[2] = geo.center[2];
    out.radius = geo.radius;
    out.ok = true;
    return out;
}

// ---- default head/hair discovery (scan pc/faces once) ---------------------
namespace {
struct HeadHair { std::string head, hair; };
std::unordered_map<std::string, HeadHair> g_hh;  // "warrior_m" -> vpaths
bool g_hhBuilt = false;

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

void build_hh(App& app) {
    // per folder: style -> vpath, for head_std and hair_std parts.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> heads, hairs;
    for (const auto& vf : app.vfs.files()) {
        const std::string& p = vf.vpath;  // lowercased
        auto pos = p.find("/pc/faces/");
        if (pos == std::string::npos) continue;
        size_t s = pos + 10;                 // after "/pc/faces/"
        size_t e = p.find('/', s);
        if (e == std::string::npos) continue;
        std::string folder = p.substr(s, e - s);       // e.g. "warrior_m"
        std::string fname = p.substr(e + 1);
        if (fname.find('/') != std::string::npos) continue;  // must be directly in folder
        std::string hsuf = "_" + folder + "_head_std.xac";
        std::string rsuf = "_" + folder + "_hair_std.xac";
        if (ends_with(fname, hsuf))
            heads[folder][fname.substr(0, fname.size() - hsuf.size())] = p;
        else if (ends_with(fname, rsuf))
            hairs[folder][fname.substr(0, fname.size() - rsuf.size())] = p;
    }
    for (auto& kv : heads) {
        const std::string& folder = kv.first;
        auto hit = hairs.find(folder);
        // prefer a style that has BOTH head and hair
        for (auto& sv : kv.second) {
            if (hit != hairs.end()) {
                auto it2 = hit->second.find(sv.first);
                if (it2 != hit->second.end()) { g_hh[folder] = {sv.second, it2->second}; break; }
            }
        }
        if (g_hh.find(folder) == g_hh.end() && !kv.second.empty())
            g_hh[folder] = {kv.second.begin()->second, ""};
    }
    g_hhBuilt = true;
}
} // namespace

void default_head_hair(App& app, const std::string& classFolder, const std::string& gender,
                       std::string& headVpath, std::string& hairVpath) {
    if (!g_hhBuilt) build_hh(app);
    headVpath.clear(); hairVpath.clear();
    auto it = g_hh.find(classFolder + "_" + gender);
    if (it != g_hh.end()) { headVpath = it->second.head; hairVpath = it->second.hair; }
}
