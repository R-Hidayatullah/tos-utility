#include "tos/ipf/ipf_fs.h"

#include <algorithm>
#include <filesystem>

namespace tos::ipf {

namespace fs = std::filesystem;

std::string IpfFileSystem::normalize(const std::string& p) {
    std::string s = p;
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

std::string IpfFileSystem::stem(const std::string& container) {
    // strip directory and the .ipf extension
    std::string s = container;
    auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

void IpfFileSystem::mergeEntry(const IpfEntry& e) {
    ++totalEntries_;
    std::string vpath = normalize(stem(e.container) + "/" + e.path);
    auto it = index_.find(vpath);
    if (it == index_.end()) {
        index_.emplace(std::move(vpath), &e);
        return;
    }
    // Latest-wins: higher archiveVersion overrides; tie-break patch over data.
    const IpfEntry* cur = it->second;
    bool replace = e.archiveVersion > cur->archiveVersion ||
                   (e.archiveVersion == cur->archiveVersion && e.fromPatch && !cur->fromPatch);
    if (replace) { it->second = &e; ++overridden_; }
    else ++overridden_;
}

bool IpfFileSystem::addArchive(const std::string& path, bool fromPatch) {
    auto arc = std::make_unique<IpfArchive>();
    if (!arc->open(path, fromPatch)) return false;
    // Entries live in the archive object; keep it alive and index pointers.
    const auto& entries = arc->entries();
    archives_.push_back(std::move(arc));
    for (const auto& e : entries) mergeEntry(e);
    return true;
}

int IpfFileSystem::scanFolder(const std::string& dir, bool fromPatch) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        auto p = de.path();
        if (p.extension() != ".ipf" && p.extension() != ".IPF") continue;
        if (addArchive(p.string(), fromPatch)) ++n;
    }
    return n;
}

void IpfFileSystem::scanGameRoot(const std::string& root) {
    scanFolder((fs::path(root) / "data").string(), /*fromPatch=*/false);
    scanFolder((fs::path(root) / "patch").string(), /*fromPatch=*/true);
}

const IpfEntry* IpfFileSystem::find(const std::string& vpath) const {
    auto it = index_.find(normalize(vpath));
    return it == index_.end() ? nullptr : it->second;
}

std::vector<VfsFile> IpfFileSystem::files() const {
    std::vector<VfsFile> out;
    out.reserve(index_.size());
    for (const auto& [k, v] : index_) out.push_back({k, v});
    std::sort(out.begin(), out.end(),
              [](const VfsFile& a, const VfsFile& b){ return a.vpath < b.vpath; });
    return out;
}

} // namespace tos::ipf
