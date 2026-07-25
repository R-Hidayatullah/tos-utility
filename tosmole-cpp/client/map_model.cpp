#include "map_model.h"
#include "tex_decode.h"

#include "tos/emfx/xac.h"
#include "tos/ipf/ipf_archive.h"
#include "tos/ipf/ipf_fs.h"
#include "xac_geometry.h"
#include "game_data.h"
#include "tinymath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace tos::ipf;

namespace {

// --- ground grid (one map at a time) --------------------------------------
struct Vec3 { float x, y, z; };
std::vector<Vec3> g_pos;         // baked world-space positions
std::vector<uint32_t> g_idx;     // triangle indices
float g_gMinX = 0, g_gMinZ = 0, g_gCell = 1;
int g_gNX = 0, g_gNZ = 0;
std::vector<std::vector<uint32_t>> g_cells;  // cell -> triangle base index (i in g_idx)

std::string entry_vpath(const IpfEntry* e) {
    return IpfFileSystem::normalize(IpfFileSystem::stem(e->container) + "/" + e->path);
}

void build_grid() {
    g_cells.clear();
    if (g_idx.size() < 3 || g_pos.empty()) return;
    float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (const auto& p : g_pos) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
    }
    float w = std::max(1.f, maxX - minX), d = std::max(1.f, maxZ - minZ);
    const int target = 96;
    g_gCell = std::max(1.f, std::max(w, d) / target);
    g_gNX = (int)(w / g_gCell) + 1;
    g_gNZ = (int)(d / g_gCell) + 1;
    g_gMinX = minX; g_gMinZ = minZ;
    g_cells.assign((size_t)g_gNX * g_gNZ, {});
    for (size_t i = 0; i + 2 < g_idx.size(); i += 3) {
        const Vec3& a = g_pos[g_idx[i]]; const Vec3& b = g_pos[g_idx[i + 1]]; const Vec3& c = g_pos[g_idx[i + 2]];
        float tminx = std::min({a.x, b.x, c.x}), tmaxx = std::max({a.x, b.x, c.x});
        float tminz = std::min({a.z, b.z, c.z}), tmaxz = std::max({a.z, b.z, c.z});
        int cx0 = std::max(0, (int)((tminx - minX) / g_gCell));
        int cx1 = std::min(g_gNX - 1, (int)((tmaxx - minX) / g_gCell));
        int cz0 = std::max(0, (int)((tminz - minZ) / g_gCell));
        int cz1 = std::min(g_gNZ - 1, (int)((tmaxz - minZ) / g_gCell));
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx)
                g_cells[(size_t)cz * g_gNX + cx].push_back((uint32_t)i);
    }
}

} // namespace

bool map_ground(float x, float z, float& outY, float ceiling) {
    if (g_cells.empty()) return false;
    int cx = (int)((x - g_gMinX) / g_gCell), cz = (int)((z - g_gMinZ) / g_gCell);
    if (cx < 0 || cz < 0 || cx >= g_gNX || cz >= g_gNZ) return false;
    const auto& cell = g_cells[(size_t)cz * g_gNX + cx];
    bool hit = false; float best = -1e30f;
    for (uint32_t i : cell) {
        const Vec3& a = g_pos[g_idx[i]]; const Vec3& b = g_pos[g_idx[i + 1]]; const Vec3& c = g_pos[g_idx[i + 2]];
        // barycentric in XZ
        float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::fabs(d) < 1e-9f) continue;
        float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
        float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
        float w = 1 - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) continue;
        float y = u * a.y + v * b.y + w * c.y;
        if (y > ceiling) continue;             // ignore canopy / roofs above the walker
        if (y > best) { best = y; hit = true; }
    }
    if (hit) outY = best;
    return hit;
}

MapData load_map(App& app, const std::string& worldVpath) {
    MapData out;
    g_pos.clear(); g_idx.clear(); g_cells.clear();

    std::vector<uint8_t> bytes;
    std::string vp = worldVpath;
    if (!app.readAsset(vp, bytes)) {
        // try basename resolution
        std::string base = vp;
        auto sl = base.find_last_of('/');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        const IpfEntry* e = app.resolveBasename(base);
        if (e) { vp = entry_vpath(e); if (!app.readAsset(vp, bytes)) return out; }
        else return out;
    }

    tosb::World3D world = tosb::parse3dworld(bytes.data(), bytes.size());
    if (!world.ok) return out;

    std::string dirPrefix = world.modelIpf;
    if (!dirPrefix.empty() && !world.modelPath.empty()) dirPrefix += "/";
    dirPrefix += world.modelPath;

    std::vector<modelgfx::Vertex> verts;
    std::vector<uint32_t> indices;
    struct MGroup { uint32_t off, count; std::string tex, texDir;
                    bool alphaBlend, alphaTest, zwriteOff, lightOff; float alphaRef; };
    std::vector<MGroup> mgroups;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};

    for (const auto& wm : world.models) {
        std::string f = wm.file;
        for (char& c : f) c = (char)tolower((unsigned char)c);
        if (f.find("obbcol") != std::string::npos) continue;  // collision hull, not rendered

        std::string mvp = dirPrefix.empty() ? wm.file : (dirPrefix + "/" + wm.file);
        const IpfEntry* e = app.gameData.resolve(app.vfs, mvp);
        if (!e) {
            std::string b = wm.file;
            auto sl = b.find_last_of("/\\");
            if (sl != std::string::npos) b = b.substr(sl + 1);
            e = app.resolveBasename(b);
        }
        if (!e) continue;

        tosb::XacGeometry geo;
        try {
            auto md = IpfArchive::extract(*e);
            auto actor = tos::emfx::parseXac(md.data(), md.size());
            geo = tosb::buildXacGeometry(actor, /*bakeNodeTransforms=*/false);
        } catch (...) { continue; }
        if (!geo.ok) continue;

        tmath::Mat4 S = wm.hasScale ? tmath::scaling(wm.scale[0], wm.scale[1], wm.scale[2]) : tmath::identity();
        tmath::Mat4 R = wm.hasRot ? tmath::rotationQuat(wm.rot[0], wm.rot[1], wm.rot[2], wm.rot[3]) : tmath::identity();
        tmath::Mat4 T = wm.hasPos ? tmath::translation(wm.pos[0], wm.pos[1], wm.pos[2]) : tmath::identity();
        tmath::Mat4 W = tmath::mul(tmath::mul(S, R), T);

        uint32_t baseVertex = (uint32_t)verts.size();
        uint32_t baseIndex = (uint32_t)indices.size();
        std::string texDir = app.gameData.textureDir(entry_vpath(e));

        for (auto v : geo.verts) {
            tmath::Vec3 p = tmath::transformPoint({v.px, v.py, v.pz}, W);
            tmath::Vec3 n = tmath::transformNormal({v.nx, v.ny, v.nz}, W);
            v.px = p.x; v.py = p.y; v.pz = p.z; v.nx = n.x; v.ny = n.y; v.nz = n.z;
            v.bone[0] = v.bone[1] = v.bone[2] = v.bone[3] = 0;
            v.weight[0] = 1; v.weight[1] = v.weight[2] = v.weight[3] = 0;
            verts.push_back(v);
            mn[0] = std::min(mn[0], p.x); mx[0] = std::max(mx[0], p.x);
            mn[1] = std::min(mn[1], p.y); mx[1] = std::max(mx[1], p.y);
            mn[2] = std::min(mn[2], p.z); mx[2] = std::max(mx[2], p.z);
        }
        for (uint32_t idx : geo.indices) indices.push_back(baseVertex + idx);
        for (const auto& grp : geo.groups)
            mgroups.push_back({baseIndex + grp.indexOffset, grp.indexCount, grp.textureName, texDir,
                               grp.alphaBlend, grp.alphaTest, grp.zwriteOff, grp.lightOff, grp.alphaRef});
    }

    if (verts.empty() || indices.empty()) return out;

    // Keep positions for ground queries.
    g_pos.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) g_pos[i] = {verts[i].px, verts[i].py, verts[i].pz};
    g_idx = indices;
    build_grid();

    m3d::Model* mdl = m3d::create();
    m3d::set_geometry(mdl, verts.data(), verts.size(), indices.data(), indices.size(), 1);

    std::unordered_map<std::string, DecodedTex> cache;
    for (const auto& g : mgroups) {
        const DecodedTex* t = nullptr;
        if (!g.tex.empty()) {
            std::string key = g.texDir + "\x1f" + g.tex;
            auto it = cache.find(key);
            if (it == cache.end()) {
                std::vector<std::string> dirs = world.texDirs;
                if (!g.texDir.empty()) dirs.push_back(g.texDir);
                it = cache.emplace(key, decode_tex(app, g.tex, dirs)).first;
            }
            if (it->second.ok) t = &it->second;
        }
        m3d::GroupState st;
        st.alphaBlend = g.alphaBlend; st.alphaTest = g.alphaTest;
        st.zwriteOff = g.zwriteOff; st.lightOff = g.lightOff; st.alphaRef = g.alphaRef;
        if (t) m3d::add_group(mdl, g.off, g.count, t->fmt, t->w, t->h, t->pitch, t->bytes.data(), t->mip, st);
        else   m3d::add_group(mdl, g.off, g.count, 0, 0, 0, 0, nullptr, 1, st);
    }

    out.model = mdl;
    for (int k = 0; k < 3; ++k) out.center[k] = 0.5f * (mn[k] + mx[k]);
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    out.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

    // Spawn: sample a spiral around the map centre and pick the point with the
    // LOWEST ground — open terrain sits below tree canopy / rooftops, so this
    // avoids spawning the character on top of a tree.
    float bestX = out.center[0], bestZ = out.center[2], bestY = 1e30f;
    bool found = false;
    float step = out.radius * 0.04f + 1.f;
    for (int r = 0; r <= 10; ++r) {
        for (int a = 0; a < 12; ++a) {
            float ang = a * (6.2831853f / 12.f);
            float sx = out.center[0] + std::cos(ang) * step * r;
            float sz = out.center[2] + std::sin(ang) * step * r;
            float gy;
            if (map_ground(sx, sz, gy) && gy < bestY) { bestY = gy; bestX = sx; bestZ = sz; found = true; }
        }
    }
    out.spawn[0] = bestX; out.spawn[2] = bestZ;
    out.spawn[1] = found ? bestY : mn[1];
    out.ok = true;
    return out;
}
