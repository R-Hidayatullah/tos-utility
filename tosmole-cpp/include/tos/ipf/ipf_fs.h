// IpfFileSystem — a virtual file system that merges many IPF archives the way
// the ToS runtime resolves assets: every archive under data/ and patch/ is
// indexed, and when the same virtual path appears in several archives the entry
// from the highest newVersion wins (patches override base data).
//
// Virtual path = "<container-stem>/<dirName>" (matching tosmole), normalized to
// lowercase with forward slashes.
#pragma once

#include "tos/ipf/ipf_archive.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tos::ipf {

struct VfsFile {
    std::string vpath;      // normalized virtual path
    const IpfEntry* entry;  // winning entry (highest archiveVersion)
};

class IpfFileSystem {
public:
    // Parse one archive and merge its entries. Returns false on failure.
    bool addArchive(const std::string& path, bool fromPatch);
    // Parse every *.ipf in a folder. Returns number of archives added.
    int scanFolder(const std::string& dir, bool fromPatch);
    // Scan <root>/data and <root>/patch.
    void scanGameRoot(const std::string& root);

    // Look up a virtual path (case-insensitive). nullptr if absent.
    const IpfEntry* find(const std::string& vpath) const;

    size_t uniqueCount() const { return index_.size(); }
    size_t archiveCount() const { return archives_.size(); }
    size_t totalEntryCount() const { return totalEntries_; }
    size_t overriddenCount() const { return overridden_; }

    // All winning files (sorted by vpath).
    std::vector<VfsFile> files() const;

    static std::string normalize(const std::string& p);
    static std::string stem(const std::string& container);

private:
    void mergeEntry(const IpfEntry& e);

    std::vector<std::unique_ptr<IpfArchive>> archives_;
    std::unordered_map<std::string, const IpfEntry*> index_;
    size_t totalEntries_ = 0;
    size_t overridden_ = 0;
};

} // namespace tos::ipf
