#include "proto.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace relay {

// ---------------------------------------------------------------- blowfish

bool Blowfish::load(const std::string& table_path, const std::string& key) {
    std::ifstream f(table_path, std::ios::binary);
    if (!f || key.empty()) return false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (raw.size() < 1042 * 4) return false;

    auto be32 = [&](size_t i) {
        return (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) |
               (uint32_t(raw[i * 4 + 2]) << 8) | uint32_t(raw[i * 4 + 3]);
    };
    for (int i = 0; i < 18; ++i) P_[i] = be32(i);
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 256; ++i) S_[b][i] = be32(18 + b * 256 + i);

    size_t j = 0;
    for (int i = 0; i < 18; ++i) {
        uint32_t k = 0;
        for (int n = 0; n < 4; ++n) {
            k = (k << 8) | uint8_t(key[j % key.size()]);
            ++j;
        }
        P_[i] ^= k;
    }
    uint32_t L = 0, R = 0;
    for (int i = 0; i < 18; i += 2) { encrypt(L, R); P_[i] = L; P_[i + 1] = R; }
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 256; i += 2) {
            encrypt(L, R);
            S_[b][i] = L;
            S_[b][i + 1] = R;
        }
    return true;
}

void Blowfish::decrypt_buffer(uint8_t* data, size_t len) const {
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint32_t L = load_be(data + off), R = load_be(data + off + 4);
        decrypt(L, R);
        store_be(data + off, L);
        store_be(data + off + 4, R);
    }
}

uint32_t Blowfish::load_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void Blowfish::store_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t Blowfish::F(uint32_t x) const {
    return ((S_[0][(x >> 24) & 0xFF] + S_[1][(x >> 16) & 0xFF]) ^
            S_[2][(x >> 8) & 0xFF]) + S_[3][x & 0xFF];
}

void Blowfish::encrypt(uint32_t& L, uint32_t& R) const {
    for (int i = 0; i < 16; ++i) { L ^= P_[i]; R ^= F(L); std::swap(L, R); }
    std::swap(L, R);
    R ^= P_[16];
    L ^= P_[17];
}

void Blowfish::decrypt(uint32_t& L, uint32_t& R) const {
    for (int i = 17; i > 1; --i) { L ^= P_[i]; R ^= F(L); std::swap(L, R); }
    std::swap(L, R);
    R ^= P_[1];
    L ^= P_[0];
}

// ------------------------------------------------------------ opcode table

bool Table::load(const std::string& csv_path) {
    std::ifstream f(csv_path);
    if (!f) return false;
    std::string line;
    std::getline(f, line);                       // header row
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string dec, hex, nm, sz;
        if (!std::getline(ss, dec, ',')) continue;
        std::getline(ss, hex, ',');
        std::getline(ss, nm, ',');
        std::getline(ss, sz, ',');
        if (dec.empty() || sz.empty()) continue;
        uint16_t op = uint16_t(std::stoul(dec));
        size_[op] = std::stoi(sz);
        name_[op] = nm;

        // The name carries both the direction and the link, and unlike the
        // listen port neither can be configured wrong.
        if (nm.rfind("CZ_", 0) == 0 || nm.rfind("CB_", 0) == 0 ||
            nm.rfind("CS_", 0) == 0)
            client_.insert(op);
        if (nm.rfind("CB_", 0) == 0 || nm.rfind("BC_", 0) == 0)
            link_[op] = Link::Barrack;
        else if (nm.rfind("CZ_", 0) == 0 || nm.rfind("ZC_", 0) == 0)
            link_[op] = Link::Zone;
        else if (nm.rfind("CS_", 0) == 0 || nm.rfind("SC_", 0) == 0)
            link_[op] = Link::Social;
    }
    // The extractor could not size this one; the client's own table supplies it.
    if (auto it = size_.find(21002); it != size_.end() && it->second == 0)
        it->second = 64;
    return !size_.empty();
}

const std::string& Table::name_of(uint16_t op) const {
    static thread_local std::string tmp;
    auto it = name_.find(op);
    if (it != name_.end()) return it->second;
    tmp = "UNKNOWN_" + std::to_string(op);
    return tmp;
}

Link Table::link_of(uint16_t op) const {
    auto it = link_.find(op);
    return it == link_.end() ? Link::Unknown : it->second;
}

int Table::size_of(uint16_t op) const {
    auto it = size_.find(op);
    return it == size_.end() ? -1 : it->second;
}

size_t Table::size_field_offset(uint16_t op) const {
    bool extra_header = is_client_side(op) && link_of(op) != Link::Social;
    return extra_header ? 0x16 : 0x0A;
}

int Table::packet_size(const uint8_t* p, size_t avail, bool* variable) const {
    *variable = false;
    if (avail < 10) return 0;
    uint16_t op;
    std::memcpy(&op, p, 2);
    int s = size_of(op);
    if (s < 0) return 0;
    if (s > 0) return s;

    *variable = true;
    size_t at = size_field_offset(op);
    if (avail < at + 2) return 0;
    uint16_t len;
    std::memcpy(&len, p + at, 2);
    return len;
}

uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t s = 0;
    for (size_t j = 0; j < n; ++j)
        s = (j & 1) == 0 ? (uint32_t(p[j]) ^ s) : (s + p[j]);
    return s;
}

}  // namespace relay
