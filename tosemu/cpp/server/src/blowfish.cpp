#include "blowfish.h"

#include <stdexcept>

namespace tos {

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

Blowfish::Blowfish(const std::vector<uint8_t>& init_table,
                   const std::string& key) {
    if (init_table.size() < 1042 * 4)
        throw std::runtime_error("bf_inittable.bin too small");
    if (key.empty()) throw std::runtime_error("empty blowfish key");

    for (int i = 0; i < 18; ++i) P_[i] = be32(&init_table[i * 4]);
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 256; ++i)
            S_[b][i] = be32(&init_table[(18 + b * 256 + i) * 4]);

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
    for (int i = 0; i < 18; i += 2) {
        encrypt_block(L, R);
        P_[i] = L;
        P_[i + 1] = R;
    }
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 256; i += 2) {
            encrypt_block(L, R);
            S_[b][i] = L;
            S_[b][i + 1] = R;
        }
    }
}

uint32_t Blowfish::f(uint32_t x) const {
    uint32_t a = S_[0][(x >> 24) & 0xFF] + S_[1][(x >> 16) & 0xFF];
    return (a ^ S_[2][(x >> 8) & 0xFF]) + S_[3][x & 0xFF];
}

void Blowfish::encrypt_block(uint32_t& L, uint32_t& R) const {
    for (int i = 0; i < 16; ++i) {
        L ^= P_[i];
        R ^= f(L);
        uint32_t t = L; L = R; R = t;
    }
    uint32_t t = L; L = R; R = t;
    L ^= P_[17];
    R ^= P_[16];
}

void Blowfish::decrypt_block(uint32_t& L, uint32_t& R) const {
    for (int i = 17; i > 1; --i) {
        L ^= P_[i];
        R ^= f(L);
        uint32_t t = L; L = R; R = t;
    }
    uint32_t t = L; L = R; R = t;
    L ^= P_[0];
    R ^= P_[1];
}

std::vector<uint8_t> Blowfish::decrypt(const uint8_t* data, size_t len) const {
    std::vector<uint8_t> out;
    if (len < 8) return out;
    out.resize((len / 8) * 8);
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint32_t L = be32(data + off), R = be32(data + off + 4);
        decrypt_block(L, R);
        put_be32(&out[off], L);
        put_be32(&out[off + 4], R);
    }
    return out;
}

std::vector<uint8_t> Blowfish::encrypt(const uint8_t* data, size_t len) const {
    std::vector<uint8_t> out;
    if (len < 8) return out;
    out.resize((len / 8) * 8);
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint32_t L = be32(data + off), R = be32(data + off + 4);
        encrypt_block(L, R);
        put_be32(&out[off], L);
        put_be32(&out[off + 4], R);
    }
    return out;
}

}  // namespace tos
