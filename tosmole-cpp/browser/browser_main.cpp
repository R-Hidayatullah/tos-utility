// tosbrowser — native Win32 + D3D11 browser for Tree of Savior IPF archives.
//
// First iteration: scan the game root (data/ + patch/, latest-wins), present a
// lazy folder tree of all files, and preview textures (DDS via GPU BCn upload,
// JPG/PNG/TGA via stb_image) and text/hex. Model rendering + effects come later.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unordered_map>

#include "tos/ipf/ipf_archive.h"
#include "tos/ipf/ipf_fs.h"
#include "tos/ies/ies.h"
#include "tos/tsv/tsv.h"
#include "tos/emfx/xac.h"
#include "tos/emfx/xsm.h"
#include "tos/emfx/xsmtime.h"
#include "dds.h"
#include "d3d_renderer.h"
#include "model_render.h"
#include "xac_geometry.h"
#include "game_data.h"
#include "stb_image.h"

// FSB sound-bank support (RE'd fsbtool: FSB4/FSB5 parse + decode) plus a
// non-blocking miniaudio player. These are C headers with extern "C" guards.
#include "fsb5.h"
#include "decode.h"
#include "fsb_player.h"

// Fork Particle .psb container inspector (map/skill effect system).
#include "psb.h"

using namespace tos::ipf;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
namespace {

IpfFileSystem g_vfs;

struct Node {
    std::string name;
    bool isFile = false;
    const IpfEntry* entry = nullptr;
    std::map<std::string, Node> kids;
    bool populated = false;
};
Node g_root;

HWND g_hMain = nullptr, g_hTree = nullptr, g_hPreview = nullptr, g_hModel = nullptr,
     g_hText = nullptr, g_hStatus = nullptr, g_hList = nullptr;
// Model-viewer toolbar (display toggles + animation picker).
HWND g_hToolbar = nullptr, g_hChkMesh = nullptr, g_hChkWire = nullptr, g_hChkTex = nullptr,
     g_hChkSkel = nullptr, g_hChkFx = nullptr, g_hAnimLabel = nullptr, g_hAnimCombo = nullptr;
constexpr int kTreeWidth = 380;
constexpr int kStatusHeight = 24;
constexpr int kToolbarHeight = 34;

// Current game root + the side tables (xac.ies + *_duplicates.xml) that resolve
// aliased assets and model texture directories.
std::string g_gameRoot;
tosb::GameData g_gameData;

// Control IDs.
enum {
    IDC_CHK_MESH = 2001, IDC_CHK_WIRE, IDC_CHK_TEX, IDC_CHK_SKEL, IDC_CHK_FX, IDC_CBO_ANIM,
    IDM_FILE_OPEN = 3001, IDM_FILE_EXIT,
    IDM_MAPS_LIST = 3101,
    IDM_SOUND_STOP = 3201,
};

// All animation (.xsm) vpaths in the VFS, for the per-model animation dropdown.
std::vector<std::string> g_allXsm;
// Animation vpaths currently offered in the dropdown (parallel to combo items,
// but shifted by one: index 0 in the combo is "(bind pose)" => no entry here).
std::vector<std::string> g_animPaths;

enum class Mode { Image, Text, Model, Table };
Mode g_mode = Mode::Image;

// --- Map list (from ies/map.ies, localized via release/languageData/English) ---
struct MapEntry {
    std::string cls;        // ClassName (e.g. "c_Klaipe")
    std::string enName;     // resolved English display name
    std::string type;       // MapType (City/Field/Dungeon/...)
    std::string group;      // Group (region key)
    std::string worldVpath; // bg/hi_entity/<cls>.3dworld (lowercased)
    bool hasWorld = false;  // a .3dworld exists in the VFS
};
std::vector<MapEntry> g_maps;
bool g_mapsBuilt = false;
// Language dictionaries (built lazily from the English TSV folder + wholedicid.xml).
std::unordered_map<std::string, std::string> g_id2eng;  // TSV id  -> English
std::unordered_map<std::string, std::string> g_kor2eng; // Korean  -> English
std::unordered_map<std::string, std::string> g_kor2id;  // Korean  -> TSV id (wholedicid)
bool g_langLoaded = false;
// When the ListView currently holds the map list (vs. an IES table), a
// double-click / Enter opens the selected map.
bool g_listIsMaps = false;

// --- FSB sound bank preview ------------------------------------------------
// An .fsb is a bank of many named subsounds. Opening one parses its headers and
// lists the subsounds in the ListView; double-clicking a row decodes just that
// subsound and plays it (non-blocking). The parsed fsb5 borrows g_fsbBuf, so the
// buffer must outlive it.
std::vector<uint8_t> g_fsbBuf;
fsb5 g_fsb{};
bool g_fsbOpen = false;     // g_fsb currently holds a parsed bank
bool g_listIsFsb = false;   // the ListView currently shows an FSB subsound list

// --- Fork Particle .psb inspector ------------------------------------------
tosb::PsbFile g_psb;
bool g_listIsPsb = false;   // the ListView currently shows a .psb emitter list

// Fork Particle master index (name -> psb file + display params), built lazily
// from effect/forkparticle/forkparticle.xml. Used to render a map's .3deffect
// placements as billboards.
std::unordered_map<std::string, tosb::FpParticleDef> g_fpIndex;
bool g_fpLoaded = false;

// "Effects" toolbar toggle state — gates both map particle billboards and fog.
bool g_effectsOn = true;

void closeFsb() {
    if (g_fsbOpen) { fsb5_close(&g_fsb); g_fsbOpen = false; }
    g_fsbBuf.clear();
    g_fsbBuf.shrink_to_fit();
}

// texture preview view state (forwarded to the 2D D3D renderer)
float g_zoom = 1.0f, g_panX = 0.0f, g_panY = 0.0f;
bool g_dragging = false;
POINT g_dragStart{};

// basename (lowercased) -> best entry, for resolving model textures by filename
std::unordered_map<std::string, const IpfEntry*> g_byBasename;

// Current model skeleton (for animation). Empty when no model / no skeleton.
std::vector<tosb::XacBone> g_bones;

// Compute skin matrices (invBind * animWorld) for each bone and upload them.
void uploadSkin(const std::vector<tmath::Mat4>& animWorld) {
    if (g_bones.empty()) return;
    std::vector<float> mats(g_bones.size() * 16);
    for (size_t i = 0; i < g_bones.size(); ++i) {
        tmath::Mat4 skin = (i < animWorld.size())
                               ? tmath::mul(g_bones[i].invBind, animWorld[i])
                               : tmath::identity();
        std::memcpy(&mats[i * 16], &skin.m[0][0], 16 * sizeof(float));
    }
    modelgfx::upload_bones(mats.data(), (int)g_bones.size());
}

// Build a line list (bone -> parent segments) from per-bone world matrices and
// upload it for the skeleton overlay.
void pushSkeletonLines(const std::vector<tmath::Mat4>& world) {
    if (g_bones.empty()) { modelgfx::upload_skeleton_lines(nullptr, 0); return; }
    auto jointPos = [&](int i) {
        const auto& m = world[i].m; return tmath::Vec3{m[3][0], m[3][1], m[3][2]};
    };
    std::vector<float> pts;
    pts.reserve(g_bones.size() * 6);
    for (size_t i = 0; i < g_bones.size(); ++i) {
        int p = g_bones[i].parent;
        if (p < 0 || (size_t)p >= world.size()) continue;
        tmath::Vec3 a = jointPos((int)i), b = jointPos(p);
        pts.push_back(a.x); pts.push_back(a.y); pts.push_back(a.z);
        pts.push_back(b.x); pts.push_back(b.y); pts.push_back(b.z);
    }
    modelgfx::upload_skeleton_lines(pts.data(), (int)(pts.size() / 3));
}

// Apply a full pose: upload skin matrices AND refresh the skeleton overlay.
void applyPose(const std::vector<tmath::Mat4>& world) {
    uploadSkin(world);
    pushSkeletonLines(world);
}

// Bind pose = skin matrices are identity (animWorld == bindWorld).
void uploadBindPose() {
    std::vector<tmath::Mat4> world(g_bones.size());
    for (size_t i = 0; i < g_bones.size(); ++i) world[i] = g_bones[i].bindWorld;
    applyPose(world);
}

// --- XSM playback ---------------------------------------------------------
struct Animation {
    tos::emfx::XsmMotion motion;
    std::vector<int> nodeToSub;   // bone index -> submotion index (-1 = none)
    bool active = false;
    float duration = 0;           // the XSM's own key duration
    float loopDuration = 0;       // playback loop period (from .xsmtime if present)
    bool retimed = false;
    unsigned long long startMs = 0;
} g_anim;

tmath::Vec3 sampleVec3(const std::vector<tos::emfx::XsmVec3Key>& k, float t, tmath::Vec3 def) {
    if (k.empty()) return def;
    if (t <= k.front().time) return {k.front().x, k.front().y, k.front().z};
    if (t >= k.back().time)  return {k.back().x, k.back().y, k.back().z};
    for (size_t i = 1; i < k.size(); ++i)
        if (k[i].time >= t) {
            const auto& a = k[i - 1]; const auto& b = k[i];
            float u = (t - a.time) / std::max(1e-6f, b.time - a.time);
            return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
        }
    return {k.back().x, k.back().y, k.back().z};
}

// NLERP quaternion sampling. Returns {x,y,z,w}.
void sampleQuat(const std::vector<tos::emfx::XsmQuatKey>& k, float t, const float def[4], float out[4]) {
    if (k.empty()) { for (int i = 0; i < 4; ++i) out[i] = def[i]; return; }
    const tos::emfx::XsmQuatKey* a = &k.front(); const tos::emfx::XsmQuatKey* b = &k.front();
    float u = 0;
    if (t <= k.front().time) { a = b = &k.front(); }
    else if (t >= k.back().time) { a = b = &k.back(); }
    else for (size_t i = 1; i < k.size(); ++i)
        if (k[i].time >= t) { a = &k[i - 1]; b = &k[i]; u = (t - a->time) / std::max(1e-6f, b->time - a->time); break; }
    float bx = b->x, by = b->y, bz = b->z, bw = b->w;
    float d = a->x * bx + a->y * by + a->z * bz + a->w * bw;
    if (d < 0) { bx = -bx; by = -by; bz = -bz; bw = -bw; }
    float x = a->x + (bx - a->x) * u, y = a->y + (by - a->y) * u,
          z = a->z + (bz - a->z) * u, w = a->w + (bw - a->w) * u;
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len < 1e-8f) { for (int i = 0; i < 4; ++i) out[i] = def[i]; return; }
    out[0] = x / len; out[1] = y / len; out[2] = z / len; out[3] = w / len;
}

// Apply a parsed XSM to the current skeleton (match submotions to bones by name).
// xsmtimeLoop > 0 overrides the playback loop period (the .xsmtime timeline).
void applyXsm(const tos::emfx::XsmMotion& motion, float xsmtimeLoop = 0) {
    if (g_bones.empty()) return;
    g_anim.motion = motion;
    g_anim.duration = motion.duration();
    g_anim.retimed = xsmtimeLoop > 1e-4f;
    g_anim.loopDuration = g_anim.retimed ? xsmtimeLoop : g_anim.duration;
    g_anim.nodeToSub.assign(g_bones.size(), -1);
    for (size_t i = 0; i < g_bones.size(); ++i)
        for (size_t s = 0; s < g_anim.motion.subMotions.size(); ++s)
            if (g_anim.motion.subMotions[s].name == g_bones[i].name) { g_anim.nodeToSub[i] = (int)s; break; }
    g_anim.active = g_anim.duration > 1e-4f;
    g_anim.startMs = GetTickCount64();
}

// Sample the animation at the current wall-clock time and upload skin matrices.
void tickAnimation() {
    if (!g_anim.active || g_bones.empty()) return;
    // Loop over the (possibly xsmtime-retimed) period, sample the motion at the
    // proportional point in its own key-time range.
    float t = 0;
    float loop = g_anim.loopDuration > 0 ? g_anim.loopDuration : g_anim.duration;
    if (loop > 0) {
        float elapsed = (GetTickCount64() - g_anim.startMs) / 1000.0f;
        float phase = std::fmod(elapsed, loop) / loop;
        t = phase * g_anim.duration;
    }
    std::vector<tmath::Mat4> animWorld(g_bones.size());
    for (size_t i = 0; i < g_bones.size(); ++i) {
        const tosb::XacBone& b = g_bones[i];
        tmath::Vec3 pos = b.localPos, scale = b.localScale;
        float rot[4] = {b.localRot[0], b.localRot[1], b.localRot[2], b.localRot[3]};
        if (g_anim.nodeToSub[i] >= 0) {
            const auto& sm = g_anim.motion.subMotions[g_anim.nodeToSub[i]];
            pos = sampleVec3(sm.posKeys, t, b.localPos);
            scale = sampleVec3(sm.scaleKeys, t, b.localScale);
            sampleQuat(sm.rotKeys, t, b.localRot, rot);
        }
        tmath::Mat4 S = tmath::scaling(scale.x, scale.y, scale.z);
        tmath::Mat4 R = tmath::rotationQuat(rot[0], rot[1], rot[2], rot[3]);
        tmath::Mat4 T = tmath::translation(pos.x, pos.y, pos.z);
        tmath::Mat4 local = tmath::mul(tmath::mul(S, R), T);
        animWorld[i] = (b.parent >= 0) ? tmath::mul(local, animWorld[b.parent]) : local;
    }
    applyPose(animWorld);
}

constexpr uint32_t DXGI_R8G8B8A8_UNORM = 28;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string extLower(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = p.substr(dot + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

bool isImageExt(const std::string& e) {
    return e == "dds" || e == "jpg" || e == "jpeg" || e == "png" || e == "tga" || e == "bmp" || e == "gif";
}
bool isTextExt(const std::string& e) {
    static const char* t[] = {"xml","lua","txt","skn","effect","lst","fx","3deffect","sani",
                              "def","json","h","hlsl","fxh","xsd","csv","ini","cfg","sprbin"};
    for (auto s : t) if (e == s) return true;
    return false;
}

std::wstring utf8ToWide(const char* s, int len) {
    if (len <= 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, w.data(), n);
    return w;
}

// CRLF-normalized wide text for the Win32 EDIT control.
std::wstring toEditText(const std::vector<uint8_t>& data, size_t limit) {
    size_t n = std::min(data.size(), limit);
    std::wstring w = utf8ToWide(reinterpret_cast<const char*>(data.data()), (int)n);
    std::wstring out;
    out.reserve(w.size() + 64);
    for (wchar_t c : w) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out += c;
    }
    if (data.size() > limit) out += L"\r\n... (truncated)";
    return out;
}

std::wstring hexDump(const std::vector<uint8_t>& d, size_t limit) {
    size_t n = std::min(d.size(), limit);
    std::wstring out;
    char line[128];
    for (size_t i = 0; i < n; i += 16) {
        int p = std::snprintf(line, sizeof(line), "%08zX  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < n) p += std::snprintf(line + p, sizeof(line) - p, "%02X ", d[i + j]);
            else          p += std::snprintf(line + p, sizeof(line) - p, "   ");
        }
        line[p++] = ' ';
        for (size_t j = 0; j < 16 && i + j < n; ++j) {
            uint8_t c = d[i + j];
            line[p++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[p] = 0;
        for (char* s = line; *s; ++s) out += (wchar_t)(unsigned char)*s;
        out += L"\r\n";
    }
    if (d.size() > limit) out += L"... (truncated)";
    return out;
}

void setStatus(const std::wstring& s) {
    if (g_hStatus) SendMessageW(g_hStatus, WM_SETTEXT, 0, (LPARAM)s.c_str());
}

// ---------------------------------------------------------------------------
// Folder tree
// ---------------------------------------------------------------------------
void buildTree() {
    auto files = g_vfs.files();
    for (const auto& vf : files) {
        Node* cur = &g_root;
        size_t start = 0;
        const std::string& p = vf.vpath;
        while (start < p.size()) {
            size_t slash = p.find('/', start);
            std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            bool leaf = (slash == std::string::npos);
            Node& child = cur->kids[part];
            if (child.name.empty()) child.name = part;
            if (leaf) { child.isFile = true; child.entry = vf.entry; }
            cur = &child;
            if (leaf) break;
            start = slash + 1;
        }
    }
}

void showToolbar(bool on);

void showMode(Mode m) {
    g_mode = m;
    ShowWindow(g_hText,    m == Mode::Text  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hPreview, m == Mode::Image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hModel,   m == Mode::Model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hList,    m == Mode::Table ? SW_SHOW : SW_HIDE);
    showToolbar(m == Mode::Model);
}

void buildBasenameIndex() {
    for (const auto& vf : g_vfs.files()) {
        const std::string& p = vf.vpath;
        auto slash = p.find_last_of('/');
        std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        auto it = g_byBasename.find(base);
        if (it == g_byBasename.end() || vf.entry->archiveVersion > it->second->archiveVersion)
            g_byBasename[base] = vf.entry;
    }
}

// A texture decoded into a GPU-ready subresource (BCn or RGBA).
struct DecodedTex {
    bool ok = false;
    uint32_t fmt = 0, w = 0, h = 0, pitch = 0;
    uint32_t mipCount = 1;   // >1 when the source (DDS) carried a mip chain in `bytes`
    std::vector<uint8_t> bytes;
};

// Resolve a texture reference to a real archive entry, mirroring the reference
// client's rule (see tosmole home.html findTexturePath + api.rs mesh_map):
//   * try each candidate directory (the .3dworld <TexDir>s for map models, or the
//     xac.ies "Path" for character models) joined with the texture name, going
//     through the duplicates table — a full path is often stored once under a
//     canonical "source" and aliased, so it isn't physically present as written;
//   * also try the name with its extension normalized to .dds (the .xac usually
//     names the source .tga/.png whose shipped form is a .dds);
//   * finally fall back to a global basename lookup.
const IpfEntry* resolveTextureEntry(const std::string& name,
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
            if (const IpfEntry* e = g_gameData.resolve(g_vfs, d + "/" + n)) return e;
    }
    for (const auto& n : names) {
        auto it = g_byBasename.find(n);
        if (it != g_byBasename.end()) return it->second;
    }
    return nullptr;
}

// Resolve a texture (through the candidate dirs) and decode it to a GPU-ready
// subresource. The stored file extension is NOT reliable — ToS ships plenty of
// ".tga"/".png" entries whose bytes are really DDS (and vice versa) — so decode
// by sniffing the content magic, not the name.
DecodedTex decodeTexture(const std::string& name, const std::vector<std::string>& dirs) {
    DecodedTex t;
    const IpfEntry* src = resolveTextureEntry(name, dirs);
    if (!src) return t;
    try {
        auto data = IpfArchive::extract(*src);
        // DDS by magic ("DDS "), regardless of the entry's extension.
        if (data.size() >= 4 && std::memcmp(data.data(), "DDS ", 4) == 0) {
            auto info = gw2dds::parse_dds(data.data(), data.size());
            if (info) {
                t.ok = true; t.fmt = info->dxgi_format; t.w = info->width; t.h = info->height;
                t.pitch = info->sys_mem_pitch;
                t.mipCount = info->mip_count;   // upload the whole chain (bytes span all mips)
                t.bytes.assign(data.begin() + info->data_offset, data.end());
                return t;
            }
        }
        // Otherwise let stb_image sniff JPG/PNG/TGA/BMP/GIF from the bytes.
        int w, h, comp;
        unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &comp, 4);
        if (px) {
            t.ok = true; t.fmt = DXGI_R8G8B8A8_UNORM; t.w = (uint32_t)w; t.h = (uint32_t)h;
            t.pitch = (uint32_t)(w * 4);
            t.bytes.assign(px, px + (size_t)w * h * 4);
            stbi_image_free(px);
            return t;
        }
    } catch (...) {}
    return t;
}

// Build the model in the renderer: one draw group per submesh, each with its
// own (cached) material texture resolved through the VFS.
int uploadXacModel(const tosb::XacGeometry& geo, const std::string& texDir = "") {
    modelgfx::begin_model(geo.verts.data(), geo.verts.size(),
                          geo.indices.data(), geo.indices.size(),
                          geo.center[0], geo.center[1], geo.center[2], geo.radius);
    std::unordered_map<std::string, DecodedTex> cache;
    int textured = 0;
    for (const auto& grp : geo.groups) {
        const DecodedTex* t = nullptr;
        if (!grp.textureName.empty()) {
            auto cit = cache.find(grp.textureName);
            if (cit == cache.end()) cit = cache.emplace(grp.textureName, decodeTexture(grp.textureName, {texDir})).first;
            if (cit->second.ok) { t = &cit->second; ++textured; }
        }
        modelgfx::GroupState st;
        st.alphaBlend = grp.alphaBlend; st.alphaTest = grp.alphaTest;
        st.zwriteOff = grp.zwriteOff; st.lightOff = grp.lightOff; st.alphaRef = grp.alphaRef;
        if (t) modelgfx::add_group(grp.indexOffset, grp.indexCount, t->fmt, t->w, t->h, t->pitch, t->bytes.data(), st, t->mipCount);
        else   modelgfx::add_group(grp.indexOffset, grp.indexCount, 0, 0, 0, 0, nullptr, st);
    }
    g_bones = geo.bones;
    g_anim.active = false;         // a new skeleton invalidates any prior animation
    uploadBindPose();              // start in bind pose; animation replaces the skin matrices
    modelgfx::begin_particles();   // a single model carries no map effects
    modelgfx::set_fog(false, 0, 0, 0, 0, 0);  // fog is a map-only setting
    return textured;
}

// Toolbar (model display toggles + animation picker) visibility follows the mode.
void showToolbar(bool on) {
    int sw = on ? SW_SHOW : SW_HIDE;
    for (HWND h : {g_hChkMesh, g_hChkWire, g_hChkTex, g_hChkSkel, g_hChkFx, g_hAnimLabel, g_hAnimCombo})
        if (h) ShowWindow(h, sw);
}

// vpath -> filename without extension, lowercased.
std::string baseNoExt(const std::string& vpath) {
    auto slash = vpath.find_last_of('/');
    std::string b = (slash == std::string::npos) ? vpath : vpath.substr(slash + 1);
    auto dot = b.find_last_of('.');
    if (dot != std::string::npos) b = b.substr(0, dot);
    for (char& c : b) c = (char)tolower((unsigned char)c);
    return b;
}
// vpath -> immediate parent folder name, lowercased.
std::string parentFolder(const std::string& vpath) {
    auto slash = vpath.find_last_of('/');
    if (slash == std::string::npos) return "";
    auto s2 = vpath.find_last_of('/', slash - 1);
    std::string f = (s2 == std::string::npos) ? vpath.substr(0, slash)
                                              : vpath.substr(s2 + 1, slash - s2 - 1);
    for (char& c : f) c = (char)tolower((unsigned char)c);
    return f;
}

// Parse an .xsm entry, look up its sibling .xsmtime, and start playback on the
// current skeleton. Returns false if there is no model loaded or parsing fails.
bool loadAnimationEntry(const IpfEntry* e) {
    if (!e || g_bones.empty()) return false;
    std::vector<uint8_t> data;
    try { data = IpfArchive::extract(*e); } catch (...) { return false; }
    try {
        auto motion = tos::emfx::parseXsm(data.data(), data.size());
        float xsmtimeLoop = 0;
        {
            std::string sibling = e->path;
            auto dot = sibling.find_last_of('.');
            if (dot != std::string::npos) sibling = sibling.substr(0, dot) + ".xsmtime";
            std::string vp = IpfFileSystem::stem(e->container) + "/" + sibling;
            if (const IpfEntry* xt = g_vfs.find(vp)) {
                try {
                    auto xd = IpfArchive::extract(*xt);
                    auto tt = tos::emfx::parseXsmTime(xd.data(), xd.size());
                    if (tt.sizeExact) xsmtimeLoop = tt.duration();
                } catch (...) {}
            }
        }
        applyXsm(motion, xsmtimeLoop);
        std::wstring st = utf8ToWide(e->path.c_str(), (int)e->path.size());
        int mapped = 0; for (int s : g_anim.nodeToSub) if (s >= 0) ++mapped;
        wchar_t m[300];
        std::swprintf(m, 300, L"%ls   |   %zu submotions, %d bones, key %.2fs, loop %.2fs%ls %ls",
                      st.c_str(), motion.subMotions.size(), mapped, g_anim.duration,
                      g_anim.loopDuration, g_anim.retimed ? L" (xsmtime)" : L"",
                      g_anim.active ? L"[playing]" : L"[static]");
        setStatus(m);
        showMode(Mode::Model);
        return true;
    } catch (...) { return false; }
}

// Return to bind pose (stop any active animation).
void stopAnimation() {
    g_anim.active = false;
    if (!g_bones.empty()) uploadBindPose();
}

// Derive match keys from a model stem. ToS names models like "alpaka_set" or
// "monster_slave_set" but keeps their animations in a folder / filename prefix
// of the bare creature name ("alpaka", "slave"). So we also try the stem with a
// trailing "_set" and a leading "monster_"/"npc_" stripped.
std::vector<std::string> animKeys(const std::string& stem) {
    std::vector<std::string> keys;
    auto add = [&](std::string k) {
        if (k.size() >= 3 && std::find(keys.begin(), keys.end(), k) == keys.end())
            keys.push_back(std::move(k));
    };
    auto stripSuffix = [](std::string s, const char* suf) {
        size_t n = strlen(suf);
        if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) s.resize(s.size() - n);
        return s;
    };
    auto stripPrefix = [](std::string s, const char* pre) {
        size_t n = strlen(pre);
        if (s.size() > n && s.compare(0, n, pre) == 0) s = s.substr(n);
        return s;
    };
    add(stem);
    std::string a = stripSuffix(stem, "_set"); add(a);
    for (const char* p : {"monster_", "npc_", "boss_"}) {
        add(stripPrefix(stem, p));
        add(stripPrefix(a, p));
    }
    return keys;
}

// Fill the animation dropdown with .xsm files that belong to the given model,
// matched by name/folder key (alpaka_set -> the alpaka_* clips in folder alpaka).
// Index 0 is always "(bind pose)".
void populateAnimDropdown(const std::string& modelPath) {
    g_animPaths.clear();
    if (!g_hAnimCombo) return;
    SendMessageA(g_hAnimCombo, CB_RESETCONTENT, 0, 0);
    SendMessageA(g_hAnimCombo, CB_ADDSTRING, 0, (LPARAM) "(bind pose)");

    std::vector<std::string> keys = animKeys(baseNoExt(modelPath));
    struct Cand { std::string label, vpath; };
    std::vector<Cand> cands;
    for (const auto& xp : g_allXsm) {
        std::string xb = baseNoExt(xp), folder = parentFolder(xp);
        bool match = false;
        for (const auto& k : keys) {
            if (folder == k || xb == k || xb.rfind(k + "_", 0) == 0) { match = true; break; }
        }
        if (match) cands.push_back({xb, xp});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.label < b.label; });
    for (const auto& c : cands) {
        SendMessageA(g_hAnimCombo, CB_ADDSTRING, 0, (LPARAM)c.label.c_str());
        g_animPaths.push_back(c.vpath);
    }
    SendMessageA(g_hAnimCombo, CB_SETCURSEL, 0, 0);
}

// Combobox selection -> play that animation (or return to bind pose at index 0).
void onAnimSelect() {
    if (!g_hAnimCombo) return;
    int sel = (int)SendMessageA(g_hAnimCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0) { stopAnimation(); return; }
    int idx = sel - 1;
    if (idx < 0 || idx >= (int)g_animPaths.size()) return;
    if (const IpfEntry* e = g_vfs.find(g_animPaths[idx])) loadAnimationEntry(e);
}

// Full normalized virtual path of an archive entry (stem/dirName), matching the
// keys used by the VFS index and the duplicates tables.
std::string entryVpath(const IpfEntry* e) {
    return IpfFileSystem::normalize(IpfFileSystem::stem(e->container) + "/" + e->path);
}

// Upload a single identity skin matrix so the model shader has a valid bone
// buffer (render() requires g_boneSRV). Used for pre-baked, world-space meshes
// (map scenes) whose vertices all reference bone 0.
void uploadIdentityBone() {
    g_bones.clear();
    g_anim.active = false;
    tmath::Mat4 id = tmath::identity();
    float m[16];
    std::memcpy(m, &id.m[0][0], 16 * sizeof(float));
    modelgfx::upload_bones(m, 1);
    modelgfx::upload_skeleton_lines(nullptr, 0);
}

// ---------------------------------------------------------------------------
// Map list: parse ies/map.ies and resolve English names from the language data.
// ---------------------------------------------------------------------------
std::string trimSpace(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "gele" -> "Gele", "guild_agit" -> "Guild Agit": a readable fallback name from
// an internal region/group key.
std::string prettify(const std::string& s) {
    std::string out;
    bool up = true;
    for (char c : s) {
        if (c == '_' || c == '-') { out += ' '; up = true; }
        else if (up) { out += (char)toupper((unsigned char)c); up = false; }
        else out += (char)tolower((unsigned char)c);
    }
    return out;
}

// Find the row-array slot of a string column by its id (slots follow declIdx
// order among the type==1 columns, matching IESColumnData::texts indexing).
int stringColumnSlot(const tos::ies::IESRoot& ies, const char* col) {
    std::vector<const tos::ies::IESColumn*> strs;
    for (const auto& c : ies.columns) if (c.typeData == 1) strs.push_back(&c);
    std::stable_sort(strs.begin(), strs.end(),
                     [](auto a, auto b) { return a->declIdx < b->declIdx; });
    for (size_t i = 0; i < strs.size(); ++i)
        if (strs[i]->column == col) return (int)i;
    return -1;
}

// Load every *.tsv in release/languageData/English into two dictionaries
// (id -> English, Korean -> English) plus wholedicid.xml (Korean -> id). Lazy;
// safe to call when the folder is missing (leaves dictionaries empty).
void ensureLanguageLoaded() {
    if (g_langLoaded) return;
    g_langLoaded = true;
    namespace fs = std::filesystem;
    std::string folder = g_gameRoot + "/release/languageData/English";
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        for (const auto& de : fs::directory_iterator(folder, ec)) {
            if (ec) break;
            if (!de.is_regular_file()) continue;
            auto p = de.path();
            if (p.extension() != ".tsv") continue;
            try {
                auto table = tos::tsv::parseTsvFile(p.string());
                for (const auto& r : table) {
                    if (r.size() < 3) continue;
                    if (!r[0].empty() && !r[1].empty()) g_id2eng.emplace(r[0], r[1]);
                    std::string kor = trimSpace(r[2]);
                    if (!kor.empty() && !r[1].empty()) g_kor2eng.emplace(kor, r[1]);
                }
            } catch (...) {}
        }
    }
    // wholedicid.xml maps a Korean "original" string to a "@dicID_^*$<ID>$*^" key.
    if (const IpfEntry* e = g_vfs.find("language/wholedicid.xml")) {
        try {
            auto data = IpfArchive::extract(*e);
            std::string x(reinterpret_cast<const char*>(data.data()), data.size());
            size_t pos = 0;
            while ((pos = x.find("original=\"", pos)) != std::string::npos) {
                pos += 10;
                size_t e1 = x.find('"', pos);
                if (e1 == std::string::npos) break;
                std::string orig = trimSpace(x.substr(pos, e1 - pos));
                size_t dp = x.find("dicid=\"", e1);
                if (dp == std::string::npos) break;
                dp += 7;
                size_t de = x.find('"', dp);
                if (de == std::string::npos) break;
                std::string dic = x.substr(dp, de - dp);
                size_t a = dic.find('$'), b = dic.rfind('$');
                if (a != std::string::npos && b > a && !orig.empty())
                    g_kor2id.emplace(orig, dic.substr(a + 1, b - a - 1));
                pos = de;
            }
        } catch (...) {}
    }
}

// Resolve the best English display name for a map row.
std::string resolveMapName(const std::string& cls, const std::string& korName,
                           const std::string& engName, const std::string& group) {
    std::string name = trimSpace(korName);
    // 1) Korean name -> wholedicid id -> English (most specific).
    if (!name.empty()) {
        auto it = g_kor2id.find(name);
        if (it != g_kor2id.end()) {
            auto e = g_id2eng.find(it->second);
            if (e != g_id2eng.end() && !e->second.empty()) return e->second;
        }
        // 2) Direct Korean -> English (reverse TSV lookup).
        auto k = g_kor2eng.find(name);
        if (k != g_kor2eng.end() && !k->second.empty()) return k->second;
    }
    // 3) The map's own EngName column, when it isn't just the ClassName placeholder.
    std::string eng = trimSpace(engName);
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    };
    if (!eng.empty() && !ieq(eng, cls)) return eng;
    // 4) Prettified region/group key ("gele" -> "Gele").
    if (!group.empty()) return prettify(group);
    // 5) The raw Korean name, else the ClassName.
    if (!name.empty()) return name;
    return cls;
}

// Parse ies/map.ies into g_maps (once per game root).
void buildMapList() {
    if (g_mapsBuilt) return;
    g_mapsBuilt = true;
    g_maps.clear();
    ensureLanguageLoaded();

    const IpfEntry* e = g_vfs.find("ies/map.ies");
    if (!e) return;
    tos::ies::IESRoot ies;
    try {
        auto data = IpfArchive::extract(*e);
        ies = tos::ies::IESRoot::fromBytes(data.data(), data.size());
    } catch (...) { return; }

    int sClass = stringColumnSlot(ies, "ClassName"), sName = stringColumnSlot(ies, "Name"),
        sEng = stringColumnSlot(ies, "EngName"), sGroup = stringColumnSlot(ies, "Group"),
        sType = stringColumnSlot(ies, "MapType");
    auto cell = [](const tos::ies::IESColumnData& row, int slot) -> std::string {
        return (slot >= 0 && slot < (int)row.texts.size()) ? row.texts[slot].text : "";
    };

    for (const auto& row : ies.data) {
        MapEntry m;
        m.cls = cell(row, sClass);
        if (m.cls.empty()) continue;
        m.type = trimSpace(cell(row, sType));
        m.group = trimSpace(cell(row, sGroup));
        m.enName = resolveMapName(m.cls, cell(row, sName), cell(row, sEng), m.group);
        std::string low = m.cls;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        m.worldVpath = "bg/hi_entity/" + low + ".3dworld";
        m.hasWorld = g_vfs.find(m.worldVpath) != nullptr;
        g_maps.push_back(std::move(m));
    }
    // Renderable maps first, then by type, then by English name.
    std::sort(g_maps.begin(), g_maps.end(), [](const MapEntry& a, const MapEntry& b) {
        if (a.hasWorld != b.hasWorld) return a.hasWorld > b.hasWorld;
        if (a.type != b.type) return a.type < b.type;
        return a.enName < b.enName;
    });
}

// Populate the ListView with the full map list and switch to it. Double-clicking
// a row (see WM_NOTIFY) opens that map's .3dworld.
void showMapList() {
    buildMapList();
    g_listIsMaps = true;
    g_listIsFsb = false;
    g_listIsPsb = false;

    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    HWND hHdr = (HWND)SendMessageW(g_hList, LVM_GETHEADER, 0, 0);
    int nCol = hHdr ? (int)SendMessageW(hHdr, HDM_GETITEMCOUNT, 0, 0) : 0;
    for (int i = nCol - 1; i >= 0; --i) SendMessageW(g_hList, LVM_DELETECOLUMN, i, 0);

    auto addCol = [&](int idx, const wchar_t* title, int width) {
        LVCOLUMNW lc{};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lc.pszText = const_cast<LPWSTR>(title);
        lc.cx = width; lc.iSubItem = idx;
        SendMessageW(g_hList, LVM_INSERTCOLUMNW, idx, (LPARAM)&lc);
    };
    addCol(0, L"Map (English)", 240);
    addCol(1, L"ClassName", 180);
    addCol(2, L"Type", 90);
    addCol(3, L"Region", 130);
    addCol(4, L"3D", 40);

    for (size_t r = 0; r < g_maps.size(); ++r) {
        const MapEntry& m = g_maps[r];
        std::wstring en = utf8ToWide(m.enName.c_str(), (int)m.enName.size());
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)r; it.iSubItem = 0; it.lParam = (LPARAM)r;
        it.pszText = const_cast<LPWSTR>(en.c_str());
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        auto setSub = [&](int col, const std::string& s) {
            std::wstring w = utf8ToWide(s.c_str(), (int)s.size());
            LVITEMW sub{};
            sub.mask = LVIF_TEXT; sub.iItem = (int)r; sub.iSubItem = col;
            sub.pszText = const_cast<LPWSTR>(w.c_str());
            SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&sub);
        };
        setSub(1, m.cls);
        setSub(2, m.type);
        setSub(3, m.group);
        setSub(4, m.hasWorld ? "\xE2\x9C\x93" : ""); // check mark
    }
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, nullptr, TRUE);
    showMode(Mode::Table);

    int withWorld = 0; for (const auto& m : g_maps) if (m.hasWorld) ++withWorld;
    wchar_t st[160];
    std::swprintf(st, 160, L"Map list: %zu maps (%d renderable)   |   double-click a row to open",
                  g_maps.size(), withWorld);
    setStatus(st);
}

// Render a parsed IES table into the ListView (report view). Columns follow the
// ToS layout: the row key, then numeric columns and string columns each ordered
// by declIdx (type 0 = numeric -> floats[], type 1 = string -> texts[]).
void showIesTable(const tos::ies::IESRoot& ies) {
    g_listIsMaps = false;
    g_listIsFsb = false;
    g_listIsPsb = false;
    // Order each type group by declIdx and remember its slot in the row arrays.
    struct Col { const tos::ies::IESColumn* c; bool numeric; int slot; };
    std::vector<const tos::ies::IESColumn*> nums, strs;
    for (const auto& c : ies.columns) (c.typeData == 0 ? nums : strs).push_back(&c);
    auto byDecl = [](const tos::ies::IESColumn* a, const tos::ies::IESColumn* b) {
        return a->declIdx < b->declIdx;
    };
    std::stable_sort(nums.begin(), nums.end(), byDecl);
    std::stable_sort(strs.begin(), strs.end(), byDecl);

    std::unordered_map<const tos::ies::IESColumn*, int> numSlot, strSlot;
    for (size_t i = 0; i < nums.size(); ++i) numSlot[nums[i]] = (int)i;
    for (size_t i = 0; i < strs.size(); ++i) strSlot[strs[i]] = (int)i;

    // Display order: all columns by (declIdx, typeData) — numeric before string
    // at equal decl, matching the reference layout.
    std::vector<Col> disp;
    disp.reserve(ies.columns.size());
    for (const auto& c : ies.columns) {
        bool numeric = (c.typeData == 0);
        disp.push_back({&c, numeric, numeric ? numSlot[&c] : strSlot[&c]});
    }
    std::stable_sort(disp.begin(), disp.end(), [](const Col& a, const Col& b) {
        if (a.c->declIdx != b.c->declIdx) return a.c->declIdx < b.c->declIdx;
        return a.c->typeData < b.c->typeData;
    });

    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    // Clear any previous content (items + columns).
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    HWND hHdr = (HWND)SendMessageW(g_hList, LVM_GETHEADER, 0, 0);
    int nCol = hHdr ? (int)SendMessageW(hHdr, HDM_GETITEMCOUNT, 0, 0) : 0;
    for (int i = nCol - 1; i >= 0; --i) SendMessageW(g_hList, LVM_DELETECOLUMN, i, 0);

    // Column 0 = row key; then one per display column.
    auto addCol = [&](int idx, const std::wstring& title, int width) {
        LVCOLUMNW lc{};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lc.pszText = const_cast<LPWSTR>(title.c_str());
        lc.cx = width; lc.iSubItem = idx;
        SendMessageW(g_hList, LVM_INSERTCOLUMNW, idx, (LPARAM)&lc);
    };
    addCol(0, L"ClassName", 180);
    for (size_t i = 0; i < disp.size(); ++i) {
        std::wstring name = utf8ToWide(disp[i].c->column.c_str(), (int)disp[i].c->column.size());
        addCol((int)i + 1, name, disp[i].numeric ? 90 : 150);
    }

    // Rows (cap to keep the UI responsive on very large tables).
    constexpr size_t kMaxRows = 50000;
    size_t rowCount = std::min(ies.data.size(), kMaxRows);
    for (size_t r = 0; r < rowCount; ++r) {
        const auto& row = ies.data[r];
        std::wstring key = utf8ToWide(row.rowText.text.c_str(), (int)row.rowText.text.size());
        LVITEMW it{};
        it.mask = LVIF_TEXT; it.iItem = (int)r; it.iSubItem = 0;
        it.pszText = const_cast<LPWSTR>(key.c_str());
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        for (size_t i = 0; i < disp.size(); ++i) {
            std::wstring cell;
            const Col& col = disp[i];
            if (col.numeric) {
                if (col.slot < (int)row.floats.size()) {
                    float f = row.floats[col.slot];
                    wchar_t buf[64];
                    if (f == (float)(long long)f)
                        std::swprintf(buf, 64, L"%lld", (long long)f);
                    else
                        std::swprintf(buf, 64, L"%g", f);
                    cell = buf;
                }
            } else if (col.slot < (int)row.texts.size()) {
                const std::string& t = row.texts[col.slot].text;
                cell = utf8ToWide(t.c_str(), (int)t.size());
            }
            LVITEMW sub{};
            sub.mask = LVIF_TEXT; sub.iItem = (int)r; sub.iSubItem = (int)i + 1;
            sub.pszText = const_cast<LPWSTR>(cell.c_str());
            SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&sub);
        }
    }
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, nullptr, TRUE);
    showMode(Mode::Table);
}

// Build a whole .3dworld map: resolve every placed .xac (following duplicate
// aliases), bake its geometry into world space by the model's pos/rot/scale, and
// upload the merged scene. Returns the number of models successfully placed.
int previewWorld(const std::vector<uint8_t>& data, const std::wstring& st) {
    tosb::World3D world = tosb::parse3dworld(data.data(), data.size());
    if (!world.ok) return -1;

    std::string dirPrefix = world.modelIpf;
    if (!dirPrefix.empty() && !world.modelPath.empty()) dirPrefix += "/";
    dirPrefix += world.modelPath;

    tosb::XacGeometry merged;
    struct MGroup { uint32_t off, count; std::string tex, texDir;
                    bool alphaBlend, alphaTest, zwriteOff, lightOff; float alphaRef; };
    std::vector<MGroup> mgroups;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    int placed = 0;

    int skippedCollision = 0;
    for (const auto& wm : world.models) {
        // Skip collision-only models. Files named "obbcol"/"*_obbcol.xac" are the
        // map's OBB collision hull (imc3d::ModelCollisionMesh / OBBCollisionTree in
        // the client) — untextured boxes used for physics, never rendered in-game.
        // They are what previously showed up as gray cubes over the scene.
        {
            std::string f = wm.file;
            for (char& c : f) c = (char)tolower((unsigned char)c);
            if (f.find("obbcol") != std::string::npos) { ++skippedCollision; continue; }
        }
        std::string vpath = dirPrefix.empty() ? wm.file : (dirPrefix + "/" + wm.file);
        const IpfEntry* e = g_gameData.resolve(g_vfs, vpath);
        if (!e) {
            // The .3dworld ModelDir IpfName is sometimes stale (says "bg_hi" but the
            // model actually ships in "bg_hi3"); fall back to a global basename
            // lookup (also follows the xac duplicates table via resolve()).
            std::string b = wm.file;
            auto sl = b.find_last_of("/\\");
            if (sl != std::string::npos) b = b.substr(sl + 1);
            for (char& c : b) c = (char)tolower((unsigned char)c);
            auto it = g_byBasename.find(b);
            if (it != g_byBasename.end()) e = it->second;
        }
        if (!e) continue;
        tosb::XacGeometry geo;
        try {
            auto md = IpfArchive::extract(*e);
            auto actor = tos::emfx::parseXac(md.data(), md.size());
            geo = tosb::buildXacGeometry(actor, /*bakeNodeTransforms=*/false);
        } catch (...) { continue; }
        if (!geo.ok) continue;

        // Model world matrix: scale, then rotate (quat), then translate.
        tmath::Mat4 S = wm.hasScale ? tmath::scaling(wm.scale[0], wm.scale[1], wm.scale[2])
                                    : tmath::identity();
        tmath::Mat4 R = wm.hasRot ? tmath::rotationQuat(wm.rot[0], wm.rot[1], wm.rot[2], wm.rot[3])
                                  : tmath::identity();
        tmath::Mat4 T = wm.hasPos ? tmath::translation(wm.pos[0], wm.pos[1], wm.pos[2])
                                  : tmath::identity();
        tmath::Mat4 W = tmath::mul(tmath::mul(S, R), T);

        uint32_t baseVertex = (uint32_t)merged.verts.size();
        uint32_t baseIndex = (uint32_t)merged.indices.size();
        std::string texDir = g_gameData.textureDir(entryVpath(e));

        for (auto v : geo.verts) {
            tmath::Vec3 p = tmath::transformPoint({v.px, v.py, v.pz}, W);
            tmath::Vec3 n = tmath::transformNormal({v.nx, v.ny, v.nz}, W);
            v.px = p.x; v.py = p.y; v.pz = p.z;
            v.nx = n.x; v.ny = n.y; v.nz = n.z;
            v.bone[0] = v.bone[1] = v.bone[2] = v.bone[3] = 0;
            v.weight[0] = 1; v.weight[1] = v.weight[2] = v.weight[3] = 0;
            merged.verts.push_back(v);
            mn[0] = std::min(mn[0], p.x); mx[0] = std::max(mx[0], p.x);
            mn[1] = std::min(mn[1], p.y); mx[1] = std::max(mx[1], p.y);
            mn[2] = std::min(mn[2], p.z); mx[2] = std::max(mx[2], p.z);
        }
        for (uint32_t idx : geo.indices) merged.indices.push_back(baseVertex + idx);
        for (const auto& grp : geo.groups)
            mgroups.push_back({baseIndex + grp.indexOffset, grp.indexCount, grp.textureName, texDir,
                               grp.alphaBlend, grp.alphaTest, grp.zwriteOff, grp.lightOff, grp.alphaRef});
        ++placed;
    }

    // NOTE: a map's .3dprop is NOT extra geometry — it lists the SAME models as
    // the .3dworld with per-model render metadata (RenderType/IsBloom/NoCol). The
    // .3dworld above already places every model, so nothing to add here.

    if (merged.verts.empty() || merged.indices.empty()) return 0;
    for (int k = 0; k < 3; ++k) merged.center[k] = 0.5f * (mn[k] + mx[k]);
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    merged.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (merged.radius < 1e-4f) merged.radius = 1.0f;

    modelgfx::begin_model(merged.verts.data(), merged.verts.size(),
                          merged.indices.data(), merged.indices.size(),
                          merged.center[0], merged.center[1], merged.center[2], merged.radius);
    std::unordered_map<std::string, DecodedTex> cache;
    for (const auto& g : mgroups) {
        const DecodedTex* t = nullptr;
        if (!g.tex.empty()) {
            std::string key = g.texDir + "\x1f" + g.tex;
            auto cit = cache.find(key);
            if (cit == cache.end()) {
                // Candidate texture dirs: the map's <TexDir>s first, then the
                // per-model xac.ies "Path" (for map models placed from char dirs).
                std::vector<std::string> dirs = world.texDirs;
                if (!g.texDir.empty()) dirs.push_back(g.texDir);
                cit = cache.emplace(key, decodeTexture(g.tex, dirs)).first;
            }
            if (cit->second.ok) t = &cit->second;
        }
        // A map group with no resolvable texture is a null-material helper /
        // collision box (e.g. barrack's 12-tri proxy cubes) — the client doesn't
        // draw these, so skip it rather than showing a gray box.
        if (!t) continue;
        modelgfx::GroupState st;
        st.alphaBlend = g.alphaBlend; st.alphaTest = g.alphaTest;
        st.zwriteOff = g.zwriteOff; st.lightOff = g.lightOff; st.alphaRef = g.alphaRef;
        modelgfx::add_group(g.off, g.count, t->fmt, t->w, t->h, t->pitch, t->bytes.data(), st, t->mipCount);
    }
    uploadIdentityBone();

    wchar_t m[360];
    std::swprintf(m, 360, L"%ls   |   map: %d/%zu models, %zu verts, %zu tris   (%d collision hidden)",
                  st.c_str(), placed, world.models.size(), merged.verts.size(),
                  merged.indices.size() / 3, skippedCollision);
    setStatus(m);
    showMode(Mode::Model);
    return placed;
}

// Populate the ListView with the parsed bank's subsounds and switch to it.
// Double-clicking a row (see WM_NOTIFY) decodes and plays that subsound.
void showFsbList() {
    g_listIsMaps = false;
    g_listIsFsb = true;
    g_listIsPsb = false;

    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    HWND hHdr = (HWND)SendMessageW(g_hList, LVM_GETHEADER, 0, 0);
    int nCol = hHdr ? (int)SendMessageW(hHdr, HDM_GETITEMCOUNT, 0, 0) : 0;
    for (int i = nCol - 1; i >= 0; --i) SendMessageW(g_hList, LVM_DELETECOLUMN, i, 0);

    auto addCol = [&](int idx, const wchar_t* title, int width, int fmt) {
        LVCOLUMNW lc{};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        lc.pszText = const_cast<LPWSTR>(title);
        lc.cx = width; lc.iSubItem = idx; lc.fmt = fmt;
        SendMessageW(g_hList, LVM_INSERTCOLUMNW, idx, (LPARAM)&lc);
    };
    addCol(0, L"#",      60,  LVCFMT_RIGHT);
    addCol(1, L"Name",   280, LVCFMT_LEFT);
    addCol(2, L"Format", 90,  LVCFMT_LEFT);
    addCol(3, L"Ch",     40,  LVCFMT_RIGHT);
    addCol(4, L"Rate",   80,  LVCFMT_RIGHT);
    addCol(5, L"Length", 90,  LVCFMT_RIGHT);

    for (uint32_t r = 0; r < g_fsb.num_samples; ++r) {
        const fsb_sample& s = g_fsb.samples[r];
        wchar_t idxbuf[16]; std::swprintf(idxbuf, 16, L"%u", s.index);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)r; it.iSubItem = 0; it.lParam = (LPARAM)r;
        it.pszText = idxbuf;
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        auto setSub = [&](int col, const std::wstring& w) {
            LVITEMW sub{};
            sub.mask = LVIF_TEXT; sub.iItem = (int)r; sub.iSubItem = col;
            sub.pszText = const_cast<LPWSTR>(w.c_str());
            SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&sub);
        };
        setSub(1, utf8ToWide(s.name, (int)strlen(s.name)));
        setSub(2, utf8ToWide(fsb_format_name(s.format), (int)strlen(fsb_format_name(s.format))));
        wchar_t b[32];
        std::swprintf(b, 32, L"%d", s.channels);           setSub(3, b);
        std::swprintf(b, 32, L"%d Hz", s.frequency);        setSub(4, b);
        float secs = s.frequency > 0 ? (float)s.samples / (float)s.frequency : 0.0f;
        std::swprintf(b, 32, L"%.2fs", secs);               setSub(5, b);
    }
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, nullptr, TRUE);
    showMode(Mode::Table);
}

// Decode a single subsound and start (non-blocking) playback.
void playFsbIndex(int idx) {
    if (!g_fsbOpen || idx < 0 || idx >= (int)g_fsb.num_samples) return;
    const fsb_sample& s = g_fsb.samples[idx];
    int16_t* pcm = nullptr;
    uint64_t frames = 0; int ch = 0, rate = 0;
    int r = fsb_decode_pcm16(&g_fsb, &s, &pcm, &frames, &ch, &rate);
    std::wstring name = utf8ToWide(s.name, (int)strlen(s.name));
    if (r != 0 || !pcm) {
        wchar_t m[320];
        const char* fn = fsb_format_name(s.format);
        std::swprintf(m, 320, L"[%d] %ls   |   cannot decode (%ls, err %d) — codec not supported",
                      idx, name.c_str(), utf8ToWide(fn, (int)strlen(fn)).c_str(), r);
        setStatus(m);
        return;
    }
    int pr = fsb_player_start(pcm, frames, ch, rate, /*loop=*/0);
    free(pcm);
    float secs = rate > 0 ? (float)frames / (float)rate : 0.0f;
    wchar_t m[320];
    std::swprintf(m, 320, L"[%d] %ls   |   %dch %d Hz, %.2fs   %ls",
                  idx, name.c_str(), ch, rate, secs,
                  pr == 0 ? L"[playing]" : L"[audio device error]");
    setStatus(m);
}

// Resolve a particle texture (basename) through the effect texture folder and
// show it in the 2D image pane.
void previewPsbTexture(const std::string& base) {
    if (base.empty()) { setStatus(L"(emitter has no texture)"); return; }
    DecodedTex t = decodeTexture(base, {"effect/forkparticle/texture"});
    std::wstring wb = utf8ToWide(base.c_str(), (int)base.size());
    if (!t.ok) { setStatus((L"texture not found in VFS: " + wb).c_str()); return; }
    g_zoom = 1; g_panX = g_panY = 0; gw2gfx::set_view(1, 0, 0);
    gw2gfx::set_texture(t.fmt, t.w, t.h, t.pitch, t.bytes.data());
    showMode(Mode::Image);
    wchar_t m[256];
    std::swprintf(m, 256, L"%ls   |   %ux%u", wb.c_str(), t.w, t.h);
    setStatus(m);
}

// Populate the ListView with a parsed .psb's emitters and switch to it.
// Double-clicking a row (see WM_NOTIFY) previews that emitter's texture.
void showPsbList() {
    g_listIsMaps = false;
    g_listIsFsb = false;
    g_listIsPsb = true;

    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    HWND hHdr = (HWND)SendMessageW(g_hList, LVM_GETHEADER, 0, 0);
    int nCol = hHdr ? (int)SendMessageW(hHdr, HDM_GETITEMCOUNT, 0, 0) : 0;
    for (int i = nCol - 1; i >= 0; --i) SendMessageW(g_hList, LVM_DELETECOLUMN, i, 0);

    auto addCol = [&](int idx, const wchar_t* title, int width, int fmt) {
        LVCOLUMNW lc{};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        lc.pszText = const_cast<LPWSTR>(title);
        lc.cx = width; lc.iSubItem = idx; lc.fmt = fmt;
        SendMessageW(g_hList, LVM_INSERTCOLUMNW, idx, (LPARAM)&lc);
    };
    addCol(0, L"#",       50,  LVCFMT_RIGHT);
    addCol(1, L"Emitter", 220, LVCFMT_LEFT);
    addCol(2, L"#Tex",    50,  LVCFMT_RIGHT);
    addCol(3, L"Texture", 320, LVCFMT_LEFT);

    for (size_t r = 0; r < g_psb.emitters.size(); ++r) {
        const tosb::PsbEmitter& em = g_psb.emitters[r];
        wchar_t idxbuf[16]; std::swprintf(idxbuf, 16, L"%zu", r);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)r; it.iSubItem = 0; it.lParam = (LPARAM)r;
        it.pszText = idxbuf;
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        auto setSub = [&](int col, const std::wstring& w) {
            LVITEMW sub{};
            sub.mask = LVIF_TEXT; sub.iItem = (int)r; sub.iSubItem = col;
            sub.pszText = const_cast<LPWSTR>(w.c_str());
            SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&sub);
        };
        setSub(1, utf8ToWide(em.name.c_str(), (int)em.name.size()));
        wchar_t b[16]; std::swprintf(b, 16, L"%zu", em.textures.size()); setSub(2, b);
        // Join texture basenames (usually one; boom-style emitters animate several).
        std::string tex;
        for (const auto& tx : em.textures) { if (!tx.empty()) { if (!tex.empty()) tex += ", "; tex += tx; } }
        setSub(3, utf8ToWide(tex.c_str(), (int)tex.size()));
    }
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, nullptr, TRUE);
    showMode(Mode::Table);
}

// Build the Fork Particle name->def index once (from forkparticle.xml).
void ensureFpIndex() {
    if (g_fpLoaded) return;
    g_fpLoaded = true;
    if (const IpfEntry* e = g_vfs.find("effect/forkparticle/forkparticle.xml")) {
        try {
            auto d = IpfArchive::extract(*e);
            g_fpIndex = tosb::parseForkParticle(d.data(), d.size());
        } catch (...) {}
    }
}

// Load a map's sibling .3deffect and add its Fork Particle placements to the 3D
// scene as camera-facing billboards (each effect's first emitter texture). Runs
// after previewWorld has built the map mesh. Returns the number placed.
// Find a map sibling file (same basename, different extension). The map's side
// files (.3dprop/.3drender/.3deffect) often ship only under bg/hi_entity/ while a
// second copy of the .3dworld sits at bg/, so fall back to a basename lookup.
const IpfEntry* findSibling(const std::string& worldVpath, const char* newExt) {
    auto dot = worldVpath.find_last_of('.');
    if (dot == std::string::npos) return nullptr;
    std::string vp = worldVpath.substr(0, dot) + newExt;
    if (const IpfEntry* e = g_vfs.find(vp)) return e;
    auto sl = vp.find_last_of('/');
    std::string base = (sl == std::string::npos) ? vp : vp.substr(sl + 1);
    auto it = g_byBasename.find(base);
    return (it != g_byBasename.end()) ? it->second : nullptr;
}

int loadMapEffects(const std::string& worldVpath) {
    modelgfx::begin_particles();
    const IpfEntry* ee = findSibling(worldVpath, ".3deffect");
    if (!ee) return 0;
    std::vector<uint8_t> data;
    try { data = IpfArchive::extract(*ee); } catch (...) { return 0; }
    auto effs = tosb::parse3deffect(data.data(), data.size());
    if (effs.empty()) return 0;
    ensureFpIndex();

    std::unordered_map<std::string, tosb::PsbFile> psbCache;  // psb file -> parsed
    std::unordered_map<std::string, int> texCache;            // tex basename -> texId
    int placed = 0;
    for (const auto& ef : effs) {
        std::string key = ef.effect;
        for (char& c : key) c = (char)tolower((unsigned char)c);
        // The Effector name IS the .psb basename; forkparticle.xml only adds display
        // params (scale/color/render) or aliases name->file for a subset of effects.
        tosb::FpParticleDef def;
        auto it = g_fpIndex.find(key);
        if (it != g_fpIndex.end()) def = it->second;
        if (def.file.empty()) def.file = ef.effect;

        auto pit = psbCache.find(def.file);
        if (pit == psbCache.end()) {
            tosb::PsbFile pf;
            std::string pvp = "effect/forkparticle/psb/" + def.file + ".psb";
            for (char& c : pvp) c = (char)tolower((unsigned char)c);
            if (const IpfEntry* pe = g_vfs.find(pvp)) {
                try { auto pd = IpfArchive::extract(*pe); pf = tosb::parsePsb(pd.data(), pd.size()); }
                catch (...) {}
            }
            pit = psbCache.emplace(def.file, std::move(pf)).first;
        }
        const tosb::PsbFile& psb = pit->second;
        if (!psb.ok || psb.emitters.empty()) continue;

        // Render EVERY emitter of the system (a candle = warm glow + orange flame,
        // fire = layered flames), each with its own decoded tint, size and its own
        // sprite. Particle systems are additive glows, so the white radial glow
        // textures become warm light instead of milky blobs.
        bool any = false;
        for (const auto& em : psb.emitters) {
            std::string tex;
            for (const auto& t : em.textures) if (!t.empty()) { tex = t; break; }
            if (tex.empty()) continue;

            int texId;
            auto tcit = texCache.find(tex);
            if (tcit != texCache.end()) texId = tcit->second;
            else {
                DecodedTex dt = decodeTexture(tex, {"effect/forkparticle/texture"});
                texId = dt.ok ? modelgfx::add_particle_texture(dt.fmt, dt.w, dt.h, dt.pitch,
                                                               dt.bytes.data(), dt.mipCount)
                              : -1;
                texCache.emplace(tex, texId);
            }
            if (texId < 0) continue;

            // World sprite size ~ placement scale * emitter particle size. Capped
            // so a single stand-in billboard never blooms into a giant blob (real
            // systems emit many small particles; we draw one representative sprite).
            float size = ef.scale * em.particleSize * 4.0f;
            size = std::min(9.0f, std::max(1.2f, size));
            // Tint = emitter base colour * xml colour, heavily dimmed: one additive
            // stand-in billboard replaces many faint particles, so full brightness
            // blows out. Kept subtle to match the in-game candlelight.
            constexpr float kGlow = 0.22f;
            float r = em.color[0] * def.color[0] * kGlow;
            float g = em.color[1] * def.color[1] * kGlow;
            float b = em.color[2] * def.color[2] * kGlow;
            modelgfx::add_particle(ef.pos[0], ef.pos[1], ef.pos[2], size, r, g, b, 1.0f,
                                   texId, /*additive=*/true);
            any = true;
        }
        if (any) ++placed;
    }
    return placed;
}

void previewEntry(const IpfEntry* e) {
    // Selecting any file stops the currently playing sound (subsound playback is
    // driven separately from the FSB list, so switching files silences it).
    fsb_player_stop();

    std::vector<uint8_t> data;
    try {
        data = IpfArchive::extract(*e);
    } catch (const std::exception& ex) {
        gw2gfx::clear_texture();
        showMode(Mode::Text);
        std::wstring w = L"[extract error] ";
        w += utf8ToWide(ex.what(), (int)strlen(ex.what()));
        SetWindowTextW(g_hText, w.c_str());
        return;
    }

    std::string ext = extLower(e->path);
    std::wstring st = utf8ToWide(e->path.c_str(), (int)e->path.size());
    wchar_t tail[160];
    std::swprintf(tail, 160, L"   |   %u bytes   |   v%u%s",
                  (unsigned)data.size(), e->archiveVersion, e->fromPatch ? L" (patch)" : L"");
    setStatus(st + tail);

    if (ext == "xac") {
        try {
            auto actor = tos::emfx::parseXac(data.data(), data.size());
            auto geo = tosb::buildXacGeometry(actor);
            if (geo.ok) {
                std::string texDir = g_gameData.textureDir(entryVpath(e));
                int textured = uploadXacModel(geo, texDir);
                populateAnimDropdown(e->path);
                wchar_t m[256];
                std::swprintf(m, 256, L"%ls   |   %d meshes, %zu verts, %zu tris, %zu groups (%d textured)",
                              st.c_str(), geo.meshCount, geo.verts.size(), geo.indices.size() / 3,
                              geo.groups.size(), textured);
                setStatus(m);
                showMode(Mode::Model);
                return;
            }
        } catch (const std::exception&) {}
        // fall through to hex on parse failure
    } else if (ext == "3dworld") {
        std::string wvp = entryVpath(e);
        modelgfx::set_fog(false, 0, 0, 0, 0, 0);  // fog disabled (per request)
        int placed = previewWorld(data, st);      // .3dprop is metadata, not geometry
        if (placed > 0) {
            int fx = loadMapEffects(wvp);
            if (fx > 0) {
                wchar_t cur[512] = {0};
                GetWindowTextW(g_hStatus, cur, 512);
                wchar_t a[48]; std::swprintf(a, 48, L"   +  %d effects", fx);
                setStatus(std::wstring(cur) + a);
            }
            return;
        }
        // fall through to text on parse failure / empty map
    } else if (ext == "ies") {
        try {
            auto ies = tos::ies::IESRoot::fromBytes(data.data(), data.size());
            wchar_t m[256];
            std::swprintf(m, 256, L"%ls   |   IES: %u rows, %u cols (%u num, %u str)",
                          st.c_str(), ies.header.numField, ies.header.numColumn,
                          ies.header.numColumnNumber, ies.header.numColumnString);
            showIesTable(ies);
            setStatus(m);
            return;
        } catch (const std::exception&) {}
        // fall through to hex on parse failure
    } else if (ext == "xsm" && !g_bones.empty()) {
        if (loadAnimationEntry(e)) return;
        // fall through to hex on parse failure
    } else if (ext == "fsb") {
        closeFsb();
        g_fsbBuf.swap(data);                    // g_fsbBuf now owns the bytes
        if (fsb_open(g_fsbBuf.data(), g_fsbBuf.size(), &g_fsb) == 0) {
            g_fsbOpen = true;
            showFsbList();
            wchar_t m[256];
            const char* cf = fsb_format_name(g_fsb.format);
            std::swprintf(m, 256,
                          L"%ls   |   FSB%d: %u sounds (%ls)   |   double-click a row to play",
                          st.c_str(), g_fsb.container, g_fsb.num_samples,
                          utf8ToWide(cf, (int)strlen(cf)).c_str());
            setStatus(m);
            return;
        }
        g_fsbBuf.swap(data);                    // parse failed: restore for hex fallback
        g_fsbBuf.clear();
        // fall through to hex
    } else if (ext == "psb") {
        g_psb = tosb::parsePsb(data.data(), data.size());
        if (g_psb.ok) {
            showPsbList();
            size_t texTotal = 0;
            for (const auto& em : g_psb.emitters) texTotal += em.textures.size();
            wchar_t m[256];
            std::swprintf(m, 256,
                          L"%ls   |   ForkParticle .psb v%u: %zu emitters, %zu textures"
                          L"   |   double-click an emitter to preview its texture",
                          st.c_str(), g_psb.version, g_psb.emitters.size(), texTotal);
            setStatus(m);
            return;
        }
        // fall through to hex on parse failure
    } else if (ext == "dds") {
        auto info = gw2dds::parse_dds(data.data(), data.size());
        if (info) {
            g_zoom = 1; g_panX = g_panY = 0; gw2gfx::set_view(1, 0, 0);
            gw2gfx::set_texture(info->dxgi_format, info->width, info->height,
                                info->sys_mem_pitch, data.data() + info->data_offset);
            showMode(Mode::Image);
            return;
        }
    } else if (isImageExt(ext)) {
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &comp, 4);
        if (px) {
            g_zoom = 1; g_panX = g_panY = 0; gw2gfx::set_view(1, 0, 0);
            gw2gfx::set_texture(DXGI_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h, (uint32_t)(w * 4), px);
            stbi_image_free(px);
            showMode(Mode::Image);
            return;
        }
    }

    // Text or hex fallback
    gw2gfx::clear_texture();
    showMode(Mode::Text);
    if (isTextExt(ext))
        SetWindowTextW(g_hText, toEditText(data, 1u << 20).c_str());
    else
        SetWindowTextW(g_hText, hexDump(data, 64u * 1024).c_str());
}

HTREEITEM insertNode(HWND tree, HTREEITEM parent, Node* node) {
    TVINSERTSTRUCTA tvi{};
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvi.item.pszText = (LPSTR)node->name.c_str();
    tvi.item.lParam = (LPARAM)node;
    tvi.item.cChildren = node->isFile ? 0 : (node->kids.empty() ? 0 : 1);
    return (HTREEITEM)SendMessageA(tree, TVM_INSERTITEMA, 0, (LPARAM)&tvi);
}

void populateChildren(HWND tree, HTREEITEM parentItem, Node* parent) {
    if (parent->populated) return;
    parent->populated = true;
    for (auto& [k, n] : parent->kids) if (!n.isFile) insertNode(tree, parentItem, &n); // folders first
    for (auto& [k, n] : parent->kids) if (n.isFile)  insertNode(tree, parentItem, &n); // then files
}

// Refresh the window title with the current archive/file counts.
void updateTitle() {
    if (!g_hMain) return;
    char title[320];
    std::snprintf(title, sizeof(title),
                  "tosbrowser  -  %s  -  %zu archives, %zu files (%zu overridden)",
                  g_gameRoot.c_str(), g_vfs.archiveCount(), g_vfs.uniqueCount(),
                  g_vfs.overriddenCount());
    SetWindowTextA(g_hMain, title);
}

// (Re)load a game root: rescan every archive under data/ + patch/, rebuild the
// folder tree and lookup indexes, and reload the xac.ies / duplicates side
// tables. Safe to call after startup to switch folders.
void loadGameRoot(const std::string& root) {
    g_gameRoot = root;

    // Reset all state that depends on the previous root.
    if (g_hTree) TreeView_DeleteAllItems(g_hTree);
    g_root = Node{};
    g_byBasename.clear();
    g_allXsm.clear();
    g_vfs = IpfFileSystem{};
    // Map list + language dictionaries are tied to the game root; rebuild lazily.
    g_maps.clear(); g_mapsBuilt = false; g_listIsMaps = false;
    g_id2eng.clear(); g_kor2eng.clear(); g_kor2id.clear(); g_langLoaded = false;

    setStatus(L"Scanning archives...");
    g_vfs.scanGameRoot(root);
    buildTree();
    buildBasenameIndex();
    for (const auto& vf : g_vfs.files())
        if (extLower(vf.vpath) == "xsm") g_allXsm.push_back(vf.vpath);
    g_gameData.load(root, g_vfs);

    if (g_hTree) populateChildren(g_hTree, TVI_ROOT, &g_root);
    updateTitle();

    wchar_t st[256];
    std::swprintf(st, 256, L"Loaded %zu archives, %zu files   |   %zu mesh paths, %zu duplicate aliases",
                  g_vfs.archiveCount(), g_vfs.uniqueCount(),
                  g_gameData.meshMapCount(), g_gameData.duplicateCount());
    setStatus(st);
}

// Prompt for a game folder (SHBrowseForFolder) and switch to it.
void chooseGameFolder() {
    wchar_t path[MAX_PATH] = {0};
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hMain;
    bi.lpszTitle = L"Select the Tree of Savior game folder (contains data/ + patch/)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), n, nullptr, nullptr);
        for (char& c : utf8) if (c == '\\') c = '/';
        loadGameRoot(utf8);
    }
    CoTaskMemFree(pidl);
}

// ---------------------------------------------------------------------------
// Preview host window (D3D target + mouse zoom/pan)
// ---------------------------------------------------------------------------
LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_zoom *= (delta > 0) ? 1.1f : (1.0f / 1.1f);
        gw2gfx::set_view(g_zoom, g_panX, g_panY);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_dragging = true; g_dragStart.x = GET_X_LPARAM(lp); g_dragStart.y = GET_Y_LPARAM(lp);
        SetCapture(hwnd); return 0;
    case WM_LBUTTONUP:
        g_dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT rc; GetClientRect(hwnd, &rc);
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            g_panX -= (x - g_dragStart.x) / (float)std::max<LONG>(1, rc.right) / g_zoom;
            // Grab-style: dragging down moves the image down. Texture v grows
            // downward, so panning down must DECREASE panY (same sense as panX).
            g_panY -= (y - g_dragStart.y) / (float)std::max<LONG>(1, rc.bottom) / g_zoom;
            g_dragStart.x = x; g_dragStart.y = y;
            gw2gfx::set_view(g_zoom, g_panX, g_panY);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1; // D3D owns the surface; avoid flicker
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Model host window: left-drag orbits, wheel zooms.
LRESULT CALLBACK ModelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEWHEEL:
        modelgfx::zoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.9f : 1.1f);
        return 0;
    case WM_LBUTTONDOWN:
        g_dragging = true; g_dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd); return 0;
    case WM_LBUTTONUP:
        g_dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            // Grab-style orbit: dragging right rotates the model right; dragging
            // down tilts the view down (vertical follows the cursor).
            modelgfx::orbit((g_dragStart.x - x) * 0.01f, (y - g_dragStart.y) * 0.01f);
            g_dragStart = {x, y};
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void layout() {
    RECT rc; GetClientRect(g_hMain, &rc);
    int W = rc.right, H = rc.bottom;
    int previewX = kTreeWidth, previewW = std::max(1, W - kTreeWidth);
    int bodyH = std::max(1, H - kStatusHeight);
    // The model viewport leaves a strip at the top for the toolbar; image/text
    // panes use the full right area (their toolbar is hidden).
    int modelH = std::max(1, bodyH - kToolbarHeight);
    MoveWindow(g_hTree, 0, 0, kTreeWidth, bodyH, TRUE);
    MoveWindow(g_hPreview, previewX, 0, previewW, bodyH, TRUE);
    MoveWindow(g_hModel, previewX, kToolbarHeight, previewW, modelH, TRUE);
    MoveWindow(g_hText, previewX, 0, previewW, bodyH, TRUE);
    MoveWindow(g_hList, previewX, 0, previewW, bodyH, TRUE);
    MoveWindow(g_hStatus, 0, bodyH, W, kStatusHeight, TRUE);

    // Toolbar controls along the top strip of the right pane.
    int x = previewX + 8, y = 6, chkH = 22;
    auto place = [&](HWND hc, int w, int h) { if (hc) { MoveWindow(hc, x, y, w, h, TRUE); x += w + 6; } };
    place(g_hChkMesh, 64, chkH);
    place(g_hChkWire, 92, chkH);
    place(g_hChkTex, 78, chkH);
    place(g_hChkSkel, 84, chkH);
    place(g_hChkFx, 76, chkH);
    place(g_hAnimLabel, 42, chkH);
    int comboW = std::max(140, previewX + previewW - x - 8);
    if (g_hAnimCombo) MoveWindow(g_hAnimCombo, x, y - 2, comboW, 260, TRUE); // tall = dropdown list height

    gw2gfx::on_resize(previewW, bodyH);
    modelgfx::on_resize(previewW, modelH);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_hTree) layout();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_CHK_MESH:
            modelgfx::set_show_mesh(SendMessage(g_hChkMesh, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case IDC_CHK_WIRE:
            modelgfx::set_wireframe(SendMessage(g_hChkWire, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case IDC_CHK_TEX:
            modelgfx::set_use_texture(SendMessage(g_hChkTex, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case IDC_CHK_SKEL:
            modelgfx::set_show_skeleton(SendMessage(g_hChkSkel, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case IDC_CHK_FX: {
            g_effectsOn = SendMessage(g_hChkFx, BM_GETCHECK, 0, 0) == BST_CHECKED;
            modelgfx::set_show_particles(g_effectsOn);   // "Effects" gates particles + fog
            modelgfx::set_fog_enabled(g_effectsOn);
            return 0;
        }
        case IDC_CBO_ANIM:
            if (code == CBN_SELCHANGE) { onAnimSelect(); SetFocus(g_hModel); }
            return 0;
        case IDM_FILE_OPEN:
            chooseGameFolder();
            return 0;
        case IDM_MAPS_LIST:
            showMapList();
            return 0;
        case IDM_SOUND_STOP:
            fsb_player_stop();
            setStatus(L"Sound stopped.");
            return 0;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto nmh = (LPNMHDR)lp;
        if (nmh->hwndFrom == g_hList && g_listIsPsb &&
            (nmh->code == NM_DBLCLK || nmh->code == LVN_ITEMACTIVATE)) {
            auto ia = (LPNMITEMACTIVATE)lp;
            int item = ia->iItem;
            if (item < 0) item = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (item >= 0) {
                LVITEMW get{}; get.mask = LVIF_PARAM; get.iItem = item;
                SendMessageW(g_hList, LVM_GETITEMW, 0, (LPARAM)&get);
                size_t idx = (size_t)get.lParam;
                if (idx < g_psb.emitters.size()) {
                    const auto& em = g_psb.emitters[idx];
                    std::string tx;
                    for (const auto& t : em.textures) if (!t.empty()) { tx = t; break; }
                    previewPsbTexture(tx);
                }
            }
            return 0;
        }
        if (nmh->hwndFrom == g_hList && g_listIsFsb &&
            (nmh->code == NM_DBLCLK || nmh->code == LVN_ITEMACTIVATE)) {
            auto ia = (LPNMITEMACTIVATE)lp;
            int item = ia->iItem;
            if (item < 0) item = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (item >= 0) {
                LVITEMW get{}; get.mask = LVIF_PARAM; get.iItem = item;
                SendMessageW(g_hList, LVM_GETITEMW, 0, (LPARAM)&get);
                playFsbIndex((int)get.lParam);
            }
            return 0;
        }
        if (nmh->hwndFrom == g_hList && g_listIsMaps &&
            (nmh->code == NM_DBLCLK || nmh->code == LVN_ITEMACTIVATE)) {
            auto ia = (LPNMITEMACTIVATE)lp;
            int item = ia->iItem;
            if (item < 0) item = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (item >= 0) {
                LVITEMW get{}; get.mask = LVIF_PARAM; get.iItem = item;
                SendMessageW(g_hList, LVM_GETITEMW, 0, (LPARAM)&get);
                size_t idx = (size_t)get.lParam;
                if (idx < g_maps.size()) {
                    const MapEntry& m = g_maps[idx];
                    if (const IpfEntry* e = g_vfs.find(m.worldVpath)) previewEntry(e);
                    else setStatus(L"No .3dworld for this map (not renderable).");
                }
            }
            return 0;
        }
        if (nmh->hwndFrom == g_hTree) {
            if (nmh->code == TVN_ITEMEXPANDINGA) {
                auto tv = (LPNMTREEVIEWA)lp;
                if (tv->action == TVE_EXPAND) {
                    Node* node = (Node*)tv->itemNew.lParam;
                    if (node) populateChildren(g_hTree, tv->itemNew.hItem, node);
                }
            } else if (nmh->code == TVN_SELCHANGEDA) {
                auto tv = (LPNMTREEVIEWA)lp;
                Node* node = (Node*)tv->itemNew.lParam;
                if (node && node->isFile && node->entry) previewEntry(node->entry);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        fsb_player_shutdown();
        closeFsb();
        modelgfx::shutdown();
        gw2gfx::shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nShow) {
    std::string root = (lpCmdLine && *lpCmdLine) ? lpCmdLine
                     : "C:/Users/Ridwan Hidayatullah/Documents/TreeOfSaviorCN";

    OleInitialize(nullptr); // SHBrowseForFolder (new-style dialog) needs COM/OLE.

    INITCOMMONCONTROLSEX icc{sizeof(icc),
                             ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc{};
    wc.lpfnWndProc = MainProc; wc.hInstance = hInst; wc.lpszClassName = "TosBrowserMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    WNDCLASSA pc{};
    pc.lpfnWndProc = PreviewProc; pc.hInstance = hInst; pc.lpszClassName = "TosBrowserPreview";
    pc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassA(&pc);

    WNDCLASSA mc{};
    mc.lpfnWndProc = ModelProc; mc.hInstance = hInst; mc.lpszClassName = "TosBrowserModel";
    mc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    RegisterClassA(&mc);

    // Menu bar: File > Open Game Folder / Exit.
    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN, "Open Game Folder...\tCtrl+O");
    AppendMenuA(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXIT, "Exit");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFile, "File");

    // Maps menu: show the full map list (from map.ies, English names).
    HMENU hMaps = CreatePopupMenu();
    AppendMenuA(hMaps, MF_STRING, IDM_MAPS_LIST, "Map List...\tCtrl+M");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hMaps, "Maps");

    // Sound menu: stop any playing .fsb subsound.
    HMENU hSound = CreatePopupMenu();
    AppendMenuA(hSound, MF_STRING, IDM_SOUND_STOP, "Stop");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSound, "Sound");

    g_hMain = CreateWindowA(wc.lpszClassName, "tosbrowser", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, hMenu, hInst, nullptr);

    g_hTree = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TREEVIEWA, "",
                              WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                              0, 0, kTreeWidth, 700, g_hMain, (HMENU)1001, hInst, nullptr);
    g_hPreview = CreateWindowA(pc.lpszClassName, "", WS_CHILD | WS_VISIBLE,
                               kTreeWidth, 0, 800, 700, g_hMain, (HMENU)1002, hInst, nullptr);
    g_hModel = CreateWindowA(mc.lpszClassName, "", WS_CHILD,
                             kTreeWidth, 0, 800, 700, g_hMain, (HMENU)1005, hInst, nullptr);
    g_hText = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                              WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL,
                              kTreeWidth, 0, 800, 700, g_hMain, (HMENU)1003, hInst, nullptr);
    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                              WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
                              kTreeWidth, 0, 800, 700, g_hMain, (HMENU)1006, hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    g_hStatus = CreateWindowExA(0, STATUSCLASSNAMEA, "ready",
                                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_hMain, (HMENU)1004, hInst, nullptr);

    // Model-viewer toolbar: display toggles + animation dropdown (hidden until a
    // model is shown; showMode() controls visibility).
    g_hChkMesh = CreateWindowA("BUTTON", "Mesh", WS_CHILD | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hMain, (HMENU)IDC_CHK_MESH, hInst, nullptr);
    g_hChkWire = CreateWindowA("BUTTON", "Wireframe", WS_CHILD | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hMain, (HMENU)IDC_CHK_WIRE, hInst, nullptr);
    g_hChkTex  = CreateWindowA("BUTTON", "Texture", WS_CHILD | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hMain, (HMENU)IDC_CHK_TEX, hInst, nullptr);
    g_hChkSkel = CreateWindowA("BUTTON", "Skeleton", WS_CHILD | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hMain, (HMENU)IDC_CHK_SKEL, hInst, nullptr);
    g_hChkFx = CreateWindowA("BUTTON", "Effects", WS_CHILD | BS_AUTOCHECKBOX,
                             0, 0, 0, 0, g_hMain, (HMENU)IDC_CHK_FX, hInst, nullptr);
    g_hAnimLabel = CreateWindowA("STATIC", "Anim:", WS_CHILD | SS_CENTERIMAGE | SS_RIGHT,
                                 0, 0, 0, 0, g_hMain, (HMENU)0, hInst, nullptr);
    g_hAnimCombo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP,
                                 0, 0, 0, 0, g_hMain, (HMENU)IDC_CBO_ANIM, hInst, nullptr);
    SendMessage(g_hChkMesh, BM_SETCHECK, BST_CHECKED, 0);   // mesh + texture on by default
    SendMessage(g_hChkTex,  BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_hChkFx,   BM_SETCHECK, BST_CHECKED, 0);   // map effects on by default
    modelgfx::set_show_mesh(true);
    modelgfx::set_use_texture(true);
    modelgfx::set_wireframe(false);
    modelgfx::set_show_skeleton(false);
    modelgfx::set_show_particles(true);
    {
        HFONT mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                                 FIXED_PITCH | FF_MODERN, "Consolas");
        SendMessage(g_hText, WM_SETFONT, (WPARAM)mono, TRUE);
        HFONT gui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (HWND h : {g_hChkMesh, g_hChkWire, g_hChkTex, g_hChkSkel, g_hChkFx, g_hAnimLabel, g_hAnimCombo, g_hList})
            SendMessage(h, WM_SETFONT, (WPARAM)gui, TRUE);
    }

    if (!gw2gfx::initialize(g_hPreview)) {
        MessageBoxA(g_hMain, "D3D11 init failed", "tosbrowser", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!modelgfx::initialize(g_hModel)) {
        MessageBoxA(g_hMain, "D3D11 model renderer init failed", "tosbrowser", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Scan the game root and build the tree + side tables (populates the tree
    // control, indexes, xac.ies mesh map and duplicate alias tables).
    loadGameRoot(root);

    // Optional: auto-preview a vpath on startup (validation / deep-link).
    if (const char* pv = getenv("TOS_PREVIEW")) {
        if (const IpfEntry* e = g_vfs.find(pv)) previewEntry(e);
    }
    // Optional: auto-apply an animation to the just-loaded model.
    if (const char* av = getenv("TOS_ANIM")) {
        if (const IpfEntry* e = g_vfs.find(av)) previewEntry(e);
    }

    ShowWindow(g_hMain, nShow);
    layout();

    // Idle-render loop keeps zoom/pan responsive.
    MSG m{};
    bool running = true;
    while (running) {
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        if (!running) break;
        if (g_mode == Mode::Model) { tickAnimation(); modelgfx::render(); }
        else if (g_mode == Mode::Image) gw2gfx::render();
        else Sleep(1); // text/table mode: nothing to render
    }
    OleUninitialize();
    return 0;
}
