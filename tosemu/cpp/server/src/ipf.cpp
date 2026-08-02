#include "ipf.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

#include "deflate.h"

namespace tos::ipf {
namespace {

constexpr uint32_t kMagic = 0x06054B50;      // "PK\5\6"
constexpr size_t kFooterSize = 24;

// The IPF password, used as the PKWARE key seed.
const uint8_t kPassword[20] = {0x6F, 0x66, 0x4F, 0x31, 0x61, 0x30, 0x75,
                               0x65, 0x58, 0x41, 0x3F, 0x20, 0x5B, 0xFF,
                               0x73, 0x20, 0x68, 0x20, 0x25, 0x3F};

const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            a[n] = c;
        }
        return a;
    }();
    return t;
}

inline uint32_t crc_step(uint32_t crc, uint8_t b) {
    return crc_table()[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

inline void update_keys(uint32_t keys[3], uint8_t byte) {
    keys[0] = crc_step(keys[0], byte);
    keys[1] = 0x08088405u * ((keys[0] & 0xFF) + keys[1]) + 1;
    keys[2] = crc_step(keys[2], uint8_t(keys[1] >> 24));
}

std::string ext_lower(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    return to_lower(p.substr(dot));
}

uint32_t zip_crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = crc_step(c, p[i]);
    return ~c;
}

// A minimal sequential reader over the file table.
struct Cursor {
    const uint8_t* p;
    size_t n, i = 0;
    bool room(size_t k) const { return i + k <= n; }
    uint16_t u16() { uint16_t v = rd16(p + i); i += 2; return v; }
    uint32_t u32() { uint32_t v = rd32(p + i); i += 4; return v; }
    std::string str(size_t k) {
        std::string s(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
};

}  // namespace

bool Entry::stored_plain() const {
    std::string e = ext_lower(path);
    return e == ".fsb" || e == ".jpg" || e == ".mp3";
}

void decrypt(uint8_t* buf, size_t len) {
    if (!len) return;
    uint32_t keys[3] = {0x12345678, 0x23456789, 0x34567890};
    for (uint8_t b : kPassword) update_keys(keys, b);

    // Only even byte indices are encrypted; the schedule advances per even byte.
    for (size_t idx = 0; idx < len; idx += 2) {
        uint32_t v = (keys[2] & 0xFFFD) | 2;
        buf[idx] ^= uint8_t((v * (v ^ 1)) >> 8);
        update_keys(keys, buf[idx]);
    }
}

bool Archive::open(const std::string& path, bool from_patch) {
    path_ = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < std::streamoff(kFooterSize)) return false;

    uint8_t footer[kFooterSize];
    f.seekg(size - std::streamoff(kFooterSize), std::ios::beg);
    f.read(reinterpret_cast<char*>(footer), kFooterSize);

    Cursor fr{footer, kFooterSize};
    uint16_t file_count = fr.u16();
    uint32_t table_ptr = fr.u32();
    fr.u16();                                    // padding
    fr.u32();                                    // headerPointer
    uint32_t magic = fr.u32();
    fr.u32();                                    // versionToPatch
    version_ = fr.u32();
    if (magic != kMagic) return false;
    if (std::streamoff(table_ptr) > size - std::streamoff(kFooterSize)) return false;

    size_t table_bytes = size_t(size - std::streamoff(kFooterSize)) - table_ptr;
    Bytes table(table_bytes);
    f.seekg(table_ptr, std::ios::beg);
    f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table_bytes));

    Cursor r{table.data(), table.size()};
    entries_.clear();
    entries_.reserve(file_count);
    for (uint16_t i = 0; i < file_count; ++i) {
        if (!r.room(20)) break;
        Entry e;
        uint16_t dir_len = r.u16();
        e.crc32 = r.u32();
        e.size_compressed = r.u32();
        e.size_uncompressed = r.u32();
        e.data_offset = r.u32();
        uint16_t container_len = r.u16();
        if (!r.room(size_t(container_len) + dir_len)) break;
        e.container = r.str(container_len);
        e.path = r.str(dir_len);
        e.archive_path = path_;
        e.archive_version = version_;
        e.from_patch = from_patch;
        entries_.push_back(std::move(e));
    }
    return true;
}

Bytes Archive::extract(const Entry& e) {
    std::ifstream f(e.archive_path, std::ios::binary);
    if (!f) return {};

    Bytes raw(e.size_compressed);
    f.seekg(e.data_offset, std::ios::beg);
    f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(e.size_compressed));
    if (!f) return {};

    if (e.stored_plain()) return raw;

    decrypt(raw.data(), raw.size());
    Bytes out;
    if (!inflate_raw(raw.data(), raw.size(), e.size_uncompressed, out)) return {};
    return out;
}

bool Archive::crc_ok(const Entry& e, const Bytes& data) {
    if (e.stored_plain()) return true;
    return zip_crc32(data.data(), data.size()) == e.crc32;
}

// ---- virtual file system -----------------------------------------------

std::string FileSystem::normalize(const std::string& p) {
    std::string s = to_lower(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

namespace {
std::string container_stem(const std::string& container) {
    std::string s = FileSystem::normalize(container);
    auto slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}
}  // namespace

void FileSystem::merge(const Entry& e) {
    std::string vpath = container_stem(e.container) + "/" + normalize(e.path);
    auto it = index_.find(vpath);
    if (it == index_.end()) {
        index_.emplace(std::move(vpath), &e);
        return;
    }
    ++overridden_;
    if (e.archive_version > it->second->archive_version) it->second = &e;
}

bool FileSystem::add_archive(const std::string& path, bool from_patch) {
    auto a = std::make_unique<Archive>();
    if (!a->open(path, from_patch)) return false;
    Archive* raw = a.get();
    archives_.push_back(std::move(a));
    for (const Entry& e : raw->entries()) merge(e);
    return true;
}

int FileSystem::scan_folder(const std::string& dir, bool from_patch) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    // Ascending version order, so the "highest wins" comparison in merge()
    // sees patches after the base data they replace.
    std::vector<std::string> paths;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file(ec)) continue;
        if (to_lower(de.path().extension().string()) != ".ipf") continue;
        paths.push_back(de.path().string());
    }
    std::sort(paths.begin(), paths.end());

    int n = 0;
    for (const std::string& p : paths)
        if (add_archive(p, from_patch)) ++n;
    return n;
}

int FileSystem::scan_game_root(const std::string& root) {
    int n = scan_folder(root + "/data", false);
    n += scan_folder(root + "/patch", true);
    return n;
}

const Entry* FileSystem::find(const std::string& vpath) const {
    auto it = index_.find(normalize(vpath));
    return it == index_.end() ? nullptr : it->second;
}

bool FileSystem::read(const std::string& vpath, Bytes& out) const {
    const Entry* e = find(vpath);
    if (!e) return false;
    out = Archive::extract(*e);
    return !out.empty();
}

std::vector<std::string> FileSystem::search(const std::string& needle,
                                            size_t limit) const {
    std::vector<std::string> hits;
    for (const auto& kv : index_) {
        if (kv.first.find(needle) == std::string::npos) continue;
        hits.push_back(kv.first);
        if (hits.size() >= limit) break;
    }
    std::sort(hits.begin(), hits.end());
    return hits;
}

}  // namespace tos::ipf
