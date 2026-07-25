// IPF archive reader — Tree of Savior asset archive (PKZIP-derived).
//
// Layout (all little-endian):
//   * 24-byte footer at EOF-24 (ZIP EOCD signature 0x06054B50):
//       uint16 fileCount; uint32 fileTablePointer; uint16 padding;
//       uint32 headerPointer; uint32 magic; uint32 versionToPatch; uint32 newVersion;
//   * File table at fileTablePointer, fileCount entries, each:
//       uint16 dirNameLen; uint32 crc32; uint32 sizeCompressed; uint32 sizeUncompressed;
//       uint32 dataOffset; uint16 containerNameLen;
//       char container[containerNameLen]; char dirName[dirNameLen];
//
// Data is ToS-partial-PKWARE-encrypted (only even byte indices) then raw-deflated.
// Files with extension .fsb/.jpg/.mp3 are stored plain (no encrypt/deflate).
// newVersion is the archive's build/patch revision — the key for latest-wins
// resolution across the data/ + patch/ folders.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tos::ipf {

struct IpfEntry {
    std::string container;        // logical container name, e.g. "bg.ipf"
    std::string path;             // file path inside the archive (dirName)
    uint32_t crc32 = 0;           // ZIP CRC32 of the UNCOMPRESSED data (oracle)
    uint32_t sizeCompressed = 0;
    uint32_t sizeUncompressed = 0;
    uint32_t dataOffset = 0;      // byte offset of the raw data within the .ipf

    // Owner archive info (filled by IpfArchive::open):
    std::string archivePath;      // .ipf file on disk
    uint32_t archiveVersion = 0;  // owning archive's newVersion (latest-wins key)
    bool fromPatch = false;       // came from the patch/ folder

    bool storedPlain() const;     // true for .fsb/.jpg/.mp3 (no decrypt/deflate)
};

struct IpfHeader {
    uint16_t fileCount = 0;
    uint32_t fileTablePointer = 0;
    uint16_t padding = 0;
    uint32_t headerPointer = 0;
    uint32_t magic = 0;
    uint32_t versionToPatch = 0;
    uint32_t newVersion = 0;
};

class IpfArchive {
public:
    // Parse header + file table. Returns false on I/O error or bad magic.
    bool open(const std::string& path, bool fromPatch = false);

    const IpfHeader& header() const { return header_; }
    const std::vector<IpfEntry>& entries() const { return entries_; }
    const std::string& path() const { return path_; }
    uint32_t newVersion() const { return header_.newVersion; }

    // Extract one entry: read raw bytes, ToS-decrypt (even bytes), raw-inflate.
    // Throws std::runtime_error on failure.
    static std::vector<uint8_t> extract(const IpfEntry& e);

    // Verify an extracted buffer against the entry's stored ZIP CRC32.
    static bool verifyCrc(const IpfEntry& e, const std::vector<uint8_t>& data);

private:
    std::string path_;
    IpfHeader header_;
    std::vector<IpfEntry> entries_;
};

// Raw DEFLATE (no zlib/gzip header) via zlib inflate(windowBits=-15).
std::vector<uint8_t> inflateRaw(const uint8_t* in, size_t inLen, size_t expectedOut);

// ToS partial PKWARE decryption (in place; only even-indexed bytes).
void ipfDecrypt(uint8_t* buf, size_t len);

// ZIP CRC32 (zlib polynomial) of a buffer.
uint32_t zipCrc32(const uint8_t* data, size_t len);

} // namespace tos::ipf
