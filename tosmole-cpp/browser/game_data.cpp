#include "game_data.h"

#include "tos/ies/ies.h"
#include "tos/ipf/ipf_archive.h"
#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tosb {

using tos::ipf::IpfEntry;
using tos::ipf::IpfFileSystem;

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Lowercase, forward slashes, no leading/trailing slash — the vpath form used by
// IpfFileSystem and the duplicates tables alike.
std::string normVpath(const std::string& in) {
    std::string s = in;
    for (char& c : s) {
        if (c == '\\') c = '/';
        c = (char)std::tolower((unsigned char)c);
    }
    size_t a = s.find_first_not_of('/');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of('/');
    return s.substr(a, b - a + 1);
}

std::string extOf(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    return toLower(p.substr(dot + 1));
}

// Load one <duplicates> table from disk into target->source.
void loadDupFile(const std::string& path,
                 std::unordered_map<std::string, std::string>& out) {
    out.clear();
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) return;
    auto* root = doc.FirstChildElement("duplicates");
    if (!root) return;
    for (auto* src = root->FirstChildElement("source"); src;
         src = src->NextSiblingElement("source")) {
        const char* sfile = src->Attribute("file");
        if (!sfile) continue;
        std::string source = normVpath(sfile);
        for (auto* tgt = src->FirstChildElement("target"); tgt;
             tgt = tgt->NextSiblingElement("target")) {
            const char* tfile = tgt->Attribute("file");
            if (!tfile) continue;
            out[normVpath(tfile)] = source;
        }
    }
}

// Parse "a b c" into up to n floats. Returns how many were read.
int parseFloats(const char* s, float* out, int n) {
    if (!s) return 0;
    int got = 0;
    while (got < n && *s) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        char* end = nullptr;
        float v = std::strtof(s, &end);
        if (end == s) break;
        out[got++] = v;
        s = end;
    }
    return got;
}

} // namespace

void GameData::clear() {
    dupXac_.clear(); dupXsm_.clear(); dupXsmtime_.clear();
    dupXpm_.clear(); dupDds_.clear();
    meshPathMap_.clear();
}

void GameData::load(const std::string& root, const IpfFileSystem& vfs) {
    clear();

    // xac.ies: Mesh -> texture Path map.
    if (const IpfEntry* e = vfs.find("ies_client/xac.ies")) {
        try {
            auto data = tos::ipf::IpfArchive::extract(*e);
            auto ies = tos::ies::IESRoot::fromBytes(data.data(), data.size());
            for (auto& [mesh, path] : ies.extractMeshPathMap())
                meshPathMap_[normVpath(mesh)] = normVpath(path);
        } catch (...) {}
    }

    // release/<kind>_duplicates.xml alias tables.
    std::string rel = root;
    if (!rel.empty() && rel.back() != '/' && rel.back() != '\\') rel += '/';
    rel += "release/";
    loadDupFile(rel + "xac_duplicates.xml", dupXac_);
    loadDupFile(rel + "xsm_duplicates.xml", dupXsm_);
    loadDupFile(rel + "xsmtime_duplicates.xml", dupXsmtime_);
    loadDupFile(rel + "xpm_duplicates.xml", dupXpm_);
    loadDupFile(rel + "dds_duplicates.xml", dupDds_);
}

const std::unordered_map<std::string, std::string>*
GameData::dupFor(const std::string& ext) const {
    if (ext == "xac") return &dupXac_;
    if (ext == "xsm") return &dupXsm_;
    if (ext == "xsmtime") return &dupXsmtime_;
    if (ext == "xpm") return &dupXpm_;
    if (ext == "dds") return &dupDds_;
    return nullptr;
}

std::string GameData::resolveDuplicate(const std::string& vpath) const {
    const auto* m = dupFor(extOf(vpath));
    if (!m) return vpath;
    auto it = m->find(vpath);
    return it == m->end() ? vpath : it->second;
}

const IpfEntry* GameData::resolve(const IpfFileSystem& vfs,
                                  const std::string& vpath) const {
    std::string key = normVpath(vpath);
    if (const IpfEntry* e = vfs.find(key)) return e;
    std::string src = resolveDuplicate(key);
    if (src != key) return vfs.find(src);
    return nullptr;
}

std::string GameData::textureDir(const std::string& xacVpath) const {
    std::string key = normVpath(xacVpath);
    auto it = meshPathMap_.find(key);
    if (it != meshPathMap_.end()) return it->second;
    // xac.ies "Mesh" values often omit the archive-stem prefix; retry without the
    // leading path segment (the ipf stem) and by bare basename.
    auto slash = key.find('/');
    if (slash != std::string::npos) {
        it = meshPathMap_.find(key.substr(slash + 1));
        if (it != meshPathMap_.end()) return it->second;
    }
    auto last = key.find_last_of('/');
    if (last != std::string::npos) {
        it = meshPathMap_.find(key.substr(last + 1));
        if (it != meshPathMap_.end()) return it->second;
    }
    return "";
}

size_t GameData::duplicateCount() const {
    return dupXac_.size() + dupXsm_.size() + dupXsmtime_.size() +
           dupXpm_.size() + dupDds_.size();
}

World3D parse3dworld(const uint8_t* data, size_t size) {
    World3D w;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(data), size) != tinyxml2::XML_SUCCESS)
        return w;
    auto* world = doc.FirstChildElement("World");
    if (!world) return w;

    if (auto* md = world->FirstChildElement("ModelDir")) {
        if (const char* n = md->Attribute("IpfName")) w.modelIpf = normVpath(n);
        if (const char* p = md->Attribute("Path")) w.modelPath = normVpath(p);
    }

    // Texture directories: <TexDir>/<SubTexDir>/<ShaTexDir> give "ipf/path" roots
    // where this map's textures live (map textures aren't in ies_client/xac.ies).
    auto addTexDir = [&](const char* tag) {
        for (auto* d = world->FirstChildElement(tag); d; d = d->NextSiblingElement(tag)) {
            std::string ipf = d->Attribute("IpfName") ? normVpath(d->Attribute("IpfName")) : "";
            std::string path = d->Attribute("Path") ? normVpath(d->Attribute("Path")) : "";
            std::string dir = ipf;
            if (!dir.empty() && !path.empty()) dir += "/";
            dir += path;
            if (!dir.empty() &&
                std::find(w.texDirs.begin(), w.texDirs.end(), dir) == w.texDirs.end())
                w.texDirs.push_back(dir);
        }
    };
    addTexDir("TexDir");
    addTexDir("SubTexDir");
    addTexDir("ShaTexDir");

    for (auto* m = world->FirstChildElement("Model"); m;
         m = m->NextSiblingElement("Model")) {
        WorldModel wm;
        if (const char* f = m->Attribute("File")) wm.file = f;
        if (const char* mm = m->Attribute("Model")) wm.model = mm;
        if (const char* p = m->Attribute("pos"))
            wm.hasPos = parseFloats(p, wm.pos, 3) == 3;
        if (const char* r = m->Attribute("rot")) {
            int n = parseFloats(r, wm.rot, 4);
            if (n == 4) {
                wm.hasRot = true;
            } else if (n == 3) {
                // Compressed quaternion: only x,y,z are stored, reconstruct w.
                float x = wm.rot[0], y = wm.rot[1], z = wm.rot[2];
                float t = 1.0f - x * x - y * y - z * z;
                wm.rot[3] = t > 0.0f ? std::sqrt(t) : 0.0f;
                wm.hasRot = true;
            }
        }
        if (const char* s = m->Attribute("scale"))
            wm.hasScale = parseFloats(s, wm.scale, 3) == 3;
        if (!wm.file.empty()) w.models.push_back(std::move(wm));
    }
    w.ok = true;
    return w;
}

std::vector<MapEffector> parse3deffect(const uint8_t* data, size_t size) {
    std::vector<MapEffector> out;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(data), size) != tinyxml2::XML_SUCCESS)
        return out;
    auto* zone = doc.FirstChildElement("Zone");
    if (!zone) return out;
    for (auto* ef = zone->FirstChildElement("Effector"); ef;
         ef = ef->NextSiblingElement("Effector")) {
        MapEffector m;
        if (const char* e = ef->Attribute("Effect")) m.effect = e;
        if (const char* p = ef->Attribute("pos")) parseFloats(p, m.pos, 3);
        if (const char* s = ef->Attribute("scale")) m.scale = (float)std::atof(s);
        if (const char* v = ef->Attribute("ViewDist")) m.viewDist = (float)std::atof(v);
        if (!m.effect.empty()) out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::string> parse3dprop(const uint8_t* data, size_t size) {
    std::vector<std::string> out;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(data), size) != tinyxml2::XML_SUCCESS)
        return out;
    auto* prop = doc.FirstChildElement("Prop");
    if (!prop) return out;
    for (auto* m = prop->FirstChildElement("Model"); m; m = m->NextSiblingElement("Model")) {
        if (const char* p = m->Attribute("Path")) {
            if (*p) out.push_back(p);
        }
    }
    return out;
}

RenderSettings parse3drender(const uint8_t* data, size_t size) {
    RenderSettings rs;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(data), size) != tinyxml2::XML_SUCCESS)
        return rs;
    auto* root = doc.FirstChildElement("Render");
    if (!root) return rs;
    rs.ok = true;
    if (auto* f = root->FirstChildElement("LinearFog")) {
        int en = 0; f->QueryIntAttribute("Enable", &en);
        rs.fogEnable = en != 0;
        if (const char* c = f->Attribute("FogColor")) parseFloats(c, rs.fogColor, 3);
        if (const char* s = f->Attribute("FogStart")) rs.fogStart = (float)std::atof(s);
        if (const char* e = f->Attribute("FogEnd")) rs.fogEnd = (float)std::atof(e);
    }
    return rs;
}

std::unordered_map<std::string, FpParticleDef> parseForkParticle(const uint8_t* data, size_t size) {
    std::unordered_map<std::string, FpParticleDef> out;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(data), size) != tinyxml2::XML_SUCCESS)
        return out;
    auto* root = doc.FirstChildElement("ForkParticle");
    if (!root) return out;
    for (auto* p = root->FirstChildElement("Particle"); p; p = p->NextSiblingElement("Particle")) {
        const char* name = p->Attribute("name");
        const char* file = p->Attribute("file");
        if (!name || !file) continue;
        FpParticleDef d;
        d.file = file;
        if (const char* s = p->Attribute("scale")) d.scale = (float)std::atof(s);
        if (const char* c = p->Attribute("color")) {  // "r;g;b;a" (0..255), ';' or ' '
            int cc[4] = {255, 255, 255, 255};
            std::sscanf(c, "%d%*[;, ]%d%*[;, ]%d%*[;, ]%d", &cc[0], &cc[1], &cc[2], &cc[3]);
            for (int i = 0; i < 4; ++i) d.color[i] = std::min(1.0f, std::max(0.0f, cc[i] / 255.0f));
        }
        if (const char* r = p->Attribute("render"))
            d.additive = (toLower(r) == "add");
        out.emplace(toLower(name), std::move(d));
    }
    return out;
}

} // namespace tosb
