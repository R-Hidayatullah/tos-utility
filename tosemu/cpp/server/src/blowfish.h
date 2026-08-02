// Blowfish as Tree of Savior uses it.
//
// Textbook Blowfish -- 16 rounds, big-endian blocks, standard F -- but seeded
// from a CUSTOM init table instead of the usual pi constants. geCrypt::Init
// assembles that table at runtime from four scattered chunks of a source
// table (parts [16,2,256,768] from seeks [4,1056,24,284]), which is why the
// standard-looking P-arrays elsewhere in .data are decoys.
//
// bf_inittable.bin holds the assembled 1042 dwords, extracted from the IDB.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tos {

class Blowfish {
public:
    // init_table: 1042 big-endian dwords (4168 bytes) from bf_inittable.bin
    Blowfish(const std::vector<uint8_t>& init_table, const std::string& key);

    void encrypt_block(uint32_t& L, uint32_t& R) const;
    void decrypt_block(uint32_t& L, uint32_t& R) const;

    // Whole-buffer ECB. Trailing bytes past the last full block are dropped,
    // matching the Python reference.
    std::vector<uint8_t> decrypt(const uint8_t* data, size_t len) const;
    std::vector<uint8_t> encrypt(const uint8_t* data, size_t len) const;

private:
    uint32_t f(uint32_t x) const;

    uint32_t P_[18];
    uint32_t S_[4][256];
};

// Row 11 of the 16x16 table at 0x141A70680 (geCrypt::MixKey picks a row when
// the mode byte is set, a column otherwise).
inline const char* kDefaultKey() { return "hsunffalqyrqewes"; }

}  // namespace tos
