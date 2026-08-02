#include "capture.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace view {

#pragma pack(push, 1)
struct V2File {
    char     magic[8];
    uint32_t version, file_header_size, record_header_size, flags;
    uint64_t start_us, start_mono_ns;
    char     region[16], note[32];
    uint32_t pid, reserved;
};
struct V2Rec {
    uint32_t magic, record_len;
    uint64_t time_us, mono_ns, index;
    uint32_t conn_id, src_ip, dst_ip;
    uint16_t src_port, dst_port, listen_port, direction, opcode, declared_size;
    uint32_t sequence, checksum, wire_len, body_len;
    uint8_t  link, checksum_ok, variable, encrypted, name_len, flags,
             reserved[2];
};
struct V2Trailer {
    uint32_t magic, clean;
    uint64_t end_us, records, bytes;
};
struct V1File {
    char     magic[8];
    uint32_t version, header_size;
    uint64_t epoch_us;
    char     region[16];
    uint64_t reserved;
};
struct V1Rec {
    uint32_t magic;
    uint64_t time_us;
    uint16_t direction, port;
    uint32_t conn_id;
    uint16_t opcode, declared_size;
    uint32_t sequence, checksum;
    uint8_t  checksum_ok, variable;
    uint16_t reserved;
    uint32_t body_len;
};
#pragma pack(pop)

static const uint32_t REC_MAGIC = 0x504B5452;   // 'PKTR'
static const uint32_t END_MAGIC = 0x444E4554;   // 'TEND'

// ------------------------------------------------------------- opcode names
//
// Only the legacy format needs these: TOSRLY records carry the name inline.

namespace {

struct OpNames {
    std::map<uint16_t, std::string> name;
    std::map<uint16_t, uint8_t> link;

    bool load(const std::wstring& csv) {
        std::ifstream f(csv.c_str());
        if (!f) return false;
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string dec, hex, nm, sz;
            std::getline(ss, dec, ',');
            std::getline(ss, hex, ',');
            std::getline(ss, nm, ',');
            if (dec.empty() || nm.empty()) continue;
            uint16_t op = uint16_t(std::stoul(dec));
            name[op] = nm;
            if (nm.rfind("CB_", 0) == 0 || nm.rfind("BC_", 0) == 0) link[op] = 1;
            else if (nm.rfind("CZ_", 0) == 0 || nm.rfind("ZC_", 0) == 0) link[op] = 2;
            else if (nm.rfind("CS_", 0) == 0 || nm.rfind("SC_", 0) == 0) link[op] = 3;
        }
        return !name.empty();
    }
};

// The csv lives next to the dump, next to the exe, or a couple of levels up
// from either -- dumps are written into out\dumps\ and the exe into a build
// tree, so neither is reliably beside it.
OpNames& op_names(const std::wstring& beside) {   // not `near`: windows.h owns it
    static OpNames tbl;
    static bool tried = false;
    if (tried) return tbl;
    tried = true;

    std::vector<std::wstring> roots;
    auto add_parents = [&](std::wstring d) {
        for (int i = 0; i < 4 && !d.empty(); ++i) {
            roots.push_back(d);
            size_t s = d.find_last_of(L"\\/");
            if (s == std::wstring::npos) break;
            d = d.substr(0, s);
        }
    };
    size_t s = beside.find_last_of(L"\\/");
    if (s != std::wstring::npos) add_parents(beside.substr(0, s));
    wchar_t exe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring e(exe);
        size_t p = e.find_last_of(L"\\/");
        if (p != std::wstring::npos) add_parents(e.substr(0, p));
    }
    for (const auto& r : roots)
        if (tbl.load(r + L"\\packet_opcodes.csv")) break;
    return tbl;
}

}  // namespace

// ------------------------------------------------------------------- loading

bool Capture::load(const std::wstring& path, std::wstring& err) {
    path_ = path;
    pkts_.clear();
    blob_.clear();
    meta_ = Meta();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Cannot open the file.";
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (1LL << 31)) {
        CloseHandle(h);
        err = sz.QuadPart <= 0 ? L"The file is empty."
                               : L"The file is larger than 2 GB.";
        return false;
    }
    blob_.resize(size_t(sz.QuadPart));
    size_t got = 0;
    while (got < blob_.size()) {
        DWORD n = 0;
        DWORD want = DWORD((blob_.size() - got) > (1u << 24) ? (1u << 24)
                                                             : (blob_.size() - got));
        if (!ReadFile(h, blob_.data() + got, want, &n, nullptr) || n == 0) break;
        got += n;
    }
    CloseHandle(h);
    blob_.resize(got);

    if (blob_.size() >= 8 && std::memcmp(blob_.data(), "TOSRLY", 6) == 0)
        return parse_v2(err);
    if (blob_.size() >= 8 && std::memcmp(blob_.data(), "TOSCAP", 6) == 0)
        return parse_v1(err);
    err = L"Not a packet dump: the file does not start with TOSRLY or TOSCAP.";
    return false;
}

bool Capture::parse_v2(std::wstring& err) {
    if (blob_.size() < sizeof(V2File)) {
        err = L"Truncated header.";
        return false;
    }
    V2File fh;
    std::memcpy(&fh, blob_.data(), sizeof(fh));
    meta_.version = int(fh.version);
    meta_.v2 = true;
    meta_.has_addresses = true;
    meta_.start_us = fh.start_us;
    meta_.pid = fh.pid;
    char r[17] = {0}, n[33] = {0};
    std::memcpy(r, fh.region, 16);
    std::memcpy(n, fh.note, 32);
    meta_.region = r;
    meta_.note = n;

    size_t end = blob_.size();
    if (end >= sizeof(V2Trailer)) {
        V2Trailer t;
        std::memcpy(&t, blob_.data() + end - sizeof(t), sizeof(t));
        if (t.magic == END_MAGIC) {
            meta_.clean = t.clean != 0;
            end -= sizeof(t);
        }
    }

    size_t off = fh.file_header_size ? fh.file_header_size : sizeof(V2File);
    const size_t rhsz = fh.record_header_size ? fh.record_header_size
                                              : sizeof(V2Rec);
    pkts_.reserve(1024);
    while (off + rhsz <= end) {
        V2Rec r2;
        std::memcpy(&r2, blob_.data() + off, sizeof(r2));
        if (r2.magic != REC_MAGIC || r2.record_len < rhsz ||
            off + r2.record_len > end) {
            ++off;                       // torn tail: find the next record
            ++meta_.resyncs;
            continue;
        }
        Packet p;
        p.time_us = r2.time_us;
        p.mono_ns = r2.mono_ns;
        p.index = r2.index;
        p.conn = r2.conn_id;
        p.src_ip = r2.src_ip;
        p.dst_ip = r2.dst_ip;
        p.src_port = r2.src_port;
        p.dst_port = r2.dst_port;
        p.listen_port = r2.listen_port;
        p.dir = r2.direction;
        p.opcode = r2.opcode;
        p.declared = r2.declared_size;
        p.seq = r2.sequence;
        p.checksum = r2.checksum;
        p.wire = r2.wire_len;
        p.link = r2.link;
        p.chk_ok = r2.checksum_ok;
        p.variable = r2.variable;
        p.encrypted = r2.encrypted;
        p.flags = r2.flags;
        p.name.assign(reinterpret_cast<const char*>(blob_.data()) + off + rhsz,
                      r2.name_len);
        p.body_off = uint32_t(off + rhsz + r2.name_len);
        p.body_len = r2.body_len;
        if (p.body_off + p.body_len <= blob_.size()) pkts_.push_back(std::move(p));
        off += r2.record_len;
    }
    // An empty capture is a valid capture -- a session where nothing connected
    // is a real answer, and refusing to open it hides the header that says so.
    (void)err;
    return true;
}

bool Capture::parse_v1(std::wstring& err) {
    if (blob_.size() < sizeof(V1File)) {
        err = L"Truncated header.";
        return false;
    }
    V1File fh;
    std::memcpy(&fh, blob_.data(), sizeof(fh));
    meta_.version = int(fh.version);
    meta_.v2 = false;
    meta_.has_addresses = false;
    meta_.start_us = fh.epoch_us;
    char r[17] = {0};
    std::memcpy(r, fh.region, 16);
    meta_.region = r;
    meta_.note = "legacy TOSCAP";

    OpNames& names = op_names(path_);
    const size_t rhsz = fh.header_size ? fh.header_size : sizeof(V1Rec);
    size_t off = sizeof(V1File);
    while (off + rhsz <= blob_.size()) {
        V1Rec r1;
        std::memcpy(&r1, blob_.data() + off, sizeof(r1));
        if (r1.magic != REC_MAGIC ||
            off + rhsz + r1.body_len > blob_.size()) {
            ++off;
            ++meta_.resyncs;
            continue;
        }
        Packet p;
        p.time_us = r1.time_us;
        p.index = pkts_.size();
        p.conn = r1.conn_id;
        p.listen_port = r1.port;
        p.dir = r1.direction;
        p.opcode = r1.opcode;
        p.declared = r1.declared_size;
        p.seq = r1.sequence;
        p.checksum = r1.checksum;
        p.chk_ok = r1.checksum_ok;
        p.variable = r1.variable;
        p.encrypted = r1.direction == 0 ? 1 : 0;
        p.body_off = uint32_t(off + rhsz);
        p.body_len = r1.body_len;
        p.wire = r1.body_len;
        auto it = names.name.find(r1.opcode);
        if (it != names.name.end()) {
            p.name = it->second;
            auto lk = names.link.find(r1.opcode);
            p.link = lk == names.link.end() ? 0 : lk->second;
        } else {
            char b[32];
            std::snprintf(b, sizeof(b), "UNKNOWN_%u", r1.opcode);
            p.name = b;
        }
        pkts_.push_back(std::move(p));
        off += rhsz + r1.body_len;
    }
    (void)err;
    return true;
}

size_t body_start(const Packet& p) {
    bool extra = p.dir == 0 && p.link != 3 && p.link != 0;
    return extra ? 0x16 : 0x0A;
}

std::wstring ip_to_w(uint32_t net_ip) {
    if (!net_ip) return L"";
    wchar_t b[32];
    const uint8_t* o = reinterpret_cast<const uint8_t*>(&net_ip);
    _snwprintf_s(b, 32, _TRUNCATE, L"%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    return b;
}

std::wstring format_clock(uint64_t time_us) {
    uint64_t ft100 = time_us * 10 + 116444736000000000ULL;
    FILETIME ft, lf;
    ft.dwLowDateTime = uint32_t(ft100 & 0xFFFFFFFFu);
    ft.dwHighDateTime = uint32_t(ft100 >> 32);
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lf);
    FileTimeToSystemTime(&lf, &st);
    wchar_t b[32];
    _snwprintf_s(b, 32, _TRUNCATE, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute,
                 st.wSecond, unsigned((time_us / 1000) % 1000));
    return b;
}

}  // namespace view
