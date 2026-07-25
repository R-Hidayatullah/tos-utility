#include "app.h"
#include "client_gfx.h"
#include "tos/ipf/ipf_archive.h"

#include <unordered_map>

using namespace tos::ipf;

bool App::readAsset(const std::string& vpath, std::vector<uint8_t>& out) {
    std::string vp = IpfFileSystem::normalize(vpath);
    const IpfEntry* e = gameData.resolve(vfs, vp);
    if (!e) e = vfs.find(vp);
    if (!e) return false;
    try {
        out = IpfArchive::extract(*e);
    } catch (...) {
        return false;
    }
    return true;
}

int App::loadTexture(const std::string& vpath) {
    static std::unordered_map<std::string, int> cache;
    std::string vp = IpfFileSystem::normalize(vpath);
    auto it = cache.find(vp);
    if (it != cache.end()) return it->second;

    std::vector<uint8_t> bytes;
    int tex = -1;
    if (readAsset(vp, bytes) && !bytes.empty())
        tex = cgfx::load_image(bytes.data(), bytes.size());
    cache[vp] = tex;
    return tex;
}

const IpfEntry* App::resolveBasename(const std::string& base) {
    if (!basenameBuilt) {
        for (const auto& vf : vfs.files()) {
            const std::string& p = vf.vpath;
            auto slash = p.find_last_of('/');
            std::string b = (slash == std::string::npos) ? p : p.substr(slash + 1);
            auto it = basenameIdx.find(b);
            if (it == basenameIdx.end() || vf.entry->archiveVersion > it->second->archiveVersion)
                basenameIdx[b] = vf.entry;
        }
        basenameBuilt = true;
    }
    std::string b = base;
    for (char& c : b) c = (char)tolower((unsigned char)c);
    auto it = basenameIdx.find(b);
    return it == basenameIdx.end() ? nullptr : it->second;
}
