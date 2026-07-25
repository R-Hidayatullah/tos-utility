#include "tos/ipf/ipf_archive.h"
#include "tos/io/byte_reader.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tos::ipf {

namespace {

constexpr uint32_t kMagic = 0x06054B50;   // "PK\5\6" ZIP EOCD signature
constexpr int64_t  kFooterSize = 24;

// ToS IPF password (PKWARE key seed).
const uint8_t kPassword[20] = {
    0x6F, 0x66, 0x4F, 0x31, 0x61, 0x30, 0x75, 0x65, 0x58, 0x41,
    0x3F, 0x20, 0x5B, 0xFF, 0x73, 0x20, 0x68, 0x20, 0x25, 0x3F,
};

// Standard ZIP/zlib CRC32 table (used for the PKWARE key schedule).
uint32_t gCrcTable[256];
bool gCrcInit = false;
void initCrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        gCrcTable[n] = c;
    }
    gCrcInit = true;
}
inline uint32_t crcStep(uint32_t crc, uint8_t b) {
    return gCrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

inline void updateKeys(uint32_t keys[3], uint8_t byte) {
    keys[0] = crcStep(keys[0], byte);
    keys[1] = 0x08088405u * ((keys[0] & 0xFF) + keys[1]) + 1;
    keys[2] = crcStep(keys[2], (uint8_t)(keys[1] >> 24));
}

void generateKeys(uint32_t keys[3]) {
    keys[0] = 0x12345678; keys[1] = 0x23456789; keys[2] = 0x34567890;
    for (uint8_t b : kPassword) updateKeys(keys, b);
}

std::string extLower(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = p.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return e;
}

} // namespace

bool IpfEntry::storedPlain() const {
    std::string e = extLower(path);
    return e == ".fsb" || e == ".jpg" || e == ".mp3";
}

void ipfDecrypt(uint8_t* buf, size_t len) {
    if (!len) return;
    if (!gCrcInit) initCrcTable();
    uint32_t keys[3];
    generateKeys(keys);
    // Only even byte indices are encrypted; key schedule advances per even byte.
    size_t steps = (len - 1) / 2 + 1;
    for (size_t i = 0; i < steps; ++i) {
        size_t idx = i * 2;
        if (idx >= len) break;
        uint32_t v = (keys[2] & 0xFFFD) | 2;
        buf[idx] ^= (uint8_t)((v * (v ^ 1)) >> 8);
        updateKeys(keys, buf[idx]);
    }
}

std::vector<uint8_t> inflateRaw(const uint8_t* in, size_t inLen, size_t expectedOut) {
    std::vector<uint8_t> out(expectedOut);
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK)  // -15 = raw deflate, no header
        throw std::runtime_error("inflateInit2 failed");
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(inLen);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(expectedOut);
    int rc = inflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK)
        throw std::runtime_error("inflate failed rc=" + std::to_string(rc));
    out.resize(produced);
    return out;
}

uint32_t zipCrc32(const uint8_t* data, size_t len) {
    return (uint32_t)crc32(crc32(0L, Z_NULL, 0), data, (uInt)len);
}

bool IpfArchive::open(const std::string& path, bool fromPatch) {
    path_ = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    int64_t size = f.tellg();
    if (size < kFooterSize) return false;

    // Footer / EOCD header.
    uint8_t footer[kFooterSize];
    f.seekg(size - kFooterSize, std::ios::beg);
    f.read(reinterpret_cast<char*>(footer), kFooterSize);
    tos::io::ByteReader fr(footer, kFooterSize);
    header_.fileCount        = fr.read_u16();
    header_.fileTablePointer = fr.read_u32();
    header_.padding          = fr.read_u16();
    header_.headerPointer    = fr.read_u32();
    header_.magic            = fr.read_u32();
    header_.versionToPatch   = fr.read_u32();
    header_.newVersion       = fr.read_u32();
    if (header_.magic != kMagic) return false;

    // File table.
    if (header_.fileTablePointer > (uint64_t)size) return false;
    size_t tableBytes = (size_t)(size - kFooterSize) - header_.fileTablePointer;
    std::vector<uint8_t> table(tableBytes);
    f.seekg(header_.fileTablePointer, std::ios::beg);
    f.read(reinterpret_cast<char*>(table.data()), tableBytes);
    tos::io::ByteReader r(table.data(), table.size());

    entries_.clear();
    entries_.reserve(header_.fileCount);
    for (uint16_t i = 0; i < header_.fileCount; ++i) {
        if (!r.can_read(20)) break;
        IpfEntry e;
        uint16_t dirLen        = r.read_u16();
        e.crc32                = r.read_u32();
        e.sizeCompressed       = r.read_u32();
        e.sizeUncompressed     = r.read_u32();
        e.dataOffset           = r.read_u32();
        uint16_t containerLen  = r.read_u16();
        if (!r.can_read((size_t)containerLen + dirLen)) break;
        auto cbytes = r.read_bytes(containerLen);
        e.container.assign(cbytes.begin(), cbytes.end());
        auto dbytes = r.read_bytes(dirLen);
        e.path.assign(dbytes.begin(), dbytes.end());
        e.archivePath = path_;
        e.archiveVersion = header_.newVersion;
        e.fromPatch = fromPatch;
        entries_.push_back(std::move(e));
    }
    return true;
}

std::vector<uint8_t> IpfArchive::extract(const IpfEntry& e) {
    std::ifstream f(e.archivePath, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open archive: " + e.archivePath);
    f.seekg(e.dataOffset, std::ios::beg);
    std::vector<uint8_t> raw(e.sizeCompressed);
    f.read(reinterpret_cast<char*>(raw.data()), e.sizeCompressed);
    if (!f) throw std::runtime_error("short read for entry: " + e.path);

    if (e.storedPlain())
        return raw;  // .fsb/.jpg/.mp3: stored as-is

    ipfDecrypt(raw.data(), raw.size());
    return inflateRaw(raw.data(), raw.size(), e.sizeUncompressed);
}

bool IpfArchive::verifyCrc(const IpfEntry& e, const std::vector<uint8_t>& data) {
    if (e.storedPlain()) return true; // CRC in table is of stored (plain) bytes anyway
    return zipCrc32(data.data(), data.size()) == e.crc32;
}

} // namespace tos::ipf
