// IPF archive reader and the merged virtual file system the client resolves
// assets through.
//
// Ported from tos-utility/tosmole-cpp, with zlib swapped for the in-tree
// inflater so the server keeps its dependency-free build.
//
// Layout (little-endian):
//   * 24-byte footer at EOF-24, starting with the ZIP EOCD signature:
//       u16 fileCount; u32 fileTablePointer; u16 padding;
//       u32 headerPointer; u32 magic; u32 versionToPatch; u32 newVersion
//   * file table at fileTablePointer, fileCount entries of:
//       u16 dirNameLen; u32 crc32; u32 sizeCompressed; u32 sizeUncompressed;
//       u32 dataOffset; u16 containerNameLen;
//       char container[containerNameLen]; char dirName[dirNameLen]
//
// Entry data is ToS-partial-PKWARE-encrypted (even byte indices only) and then
// raw-deflated. .fsb/.jpg/.mp3 are stored plain.
//
// `newVersion` is the archive's patch revision, and it is what makes the VFS
// correct: the game ships ~1200 archives where patch/ overrides data/, so the
// same virtual path appears many times and only the highest revision is live.
// Reading the base ies.ipf alone gets you data that is years stale.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace tos::ipf {

struct Entry {
    std::string container;        // logical container, e.g. "ies.ipf"
    std::string path;             // path inside that container
    uint32_t crc32 = 0;           // ZIP CRC32 of the uncompressed bytes
    uint32_t size_compressed = 0;
    uint32_t size_uncompressed = 0;
    uint32_t data_offset = 0;

    std::string archive_path;     // .ipf file on disk
    uint32_t archive_version = 0; // owning archive's newVersion
    bool from_patch = false;

    bool stored_plain() const;    // .fsb/.jpg/.mp3
};

class Archive {
public:
    bool open(const std::string& path, bool from_patch);

    const std::vector<Entry>& entries() const { return entries_; }
    uint32_t version() const { return version_; }

    // Read, decrypt and inflate one entry. Empty on failure.
    static Bytes extract(const Entry& e);
    static bool crc_ok(const Entry& e, const Bytes& data);

private:
    std::string path_;
    uint32_t version_ = 0;
    std::vector<Entry> entries_;
};

// ToS partial-PKWARE decryption, in place; only even-indexed bytes.
void decrypt(uint8_t* buf, size_t len);

// The merged view: every archive under <root>/data and <root>/patch, with the
// highest archive_version winning per virtual path.
//
// Virtual path is "<container-stem>/<dirName>", lowercased with forward
// slashes -- so "ies.ipf" + "map.ies" resolves as "ies/map.ies".
class FileSystem {
public:
    bool add_archive(const std::string& path, bool from_patch);
    int scan_folder(const std::string& dir, bool from_patch);
    // Scans <root>/data then <root>/patch. Returns archives indexed.
    int scan_game_root(const std::string& root);

    const Entry* find(const std::string& vpath) const;
    // Convenience: find + extract in one step.
    bool read(const std::string& vpath, Bytes& out) const;

    // Every winning path whose name contains `needle` (already lowercased).
    std::vector<std::string> search(const std::string& needle,
                                    size_t limit = 64) const;

    size_t unique_count() const { return index_.size(); }
    size_t archive_count() const { return archives_.size(); }
    size_t overridden_count() const { return overridden_; }

    static std::string normalize(const std::string& p);

private:
    void merge(const Entry& e);

    std::vector<std::unique_ptr<Archive>> archives_;
    std::unordered_map<std::string, const Entry*> index_;
    size_t overridden_ = 0;
};

}  // namespace tos::ipf
