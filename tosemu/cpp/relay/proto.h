// Tree of Savior wire protocol: Blowfish, the opcode table, the checksum.
//
//   client -> server : u16 padded_len | Blowfish-ECB(payload padded to 8)
//   server -> client : payload                    (plaintext, no framing)
//
//   payload = u16 opcode | u32 sequence | u32 checksum | body
//
// Total length comes from the opcode table; a 0 entry means the packet is
// variable and carries its total inline -- at +0x0A server->client, at +0x16
// client->server, because client packets on the barrack and zone links carry
// twelve further header bytes the server never reads. The social link has no
// extra header, so both its directions read +0x0A. (docs/11-packet-framing.md)
//
// Cipher is textbook Blowfish seeded with a CUSTOM 1042-dword init table --
// geCrypt::Init assembles it from four scattered chunks; bf_inittable.bin
// holds the assembled result and the key is 16 bytes.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace relay {

class Blowfish {
public:
    bool load(const std::string& table_path, const std::string& key);
    void decrypt_buffer(uint8_t* data, size_t len) const;

private:
    static uint32_t load_be(const uint8_t* p);
    static void store_be(uint8_t* p, uint32_t v);
    uint32_t F(uint32_t x) const;
    void encrypt(uint32_t& L, uint32_t& R) const;
    void decrypt(uint32_t& L, uint32_t& R) const;

    uint32_t P_[18]{};
    uint32_t S_[4][256]{};
};

enum class Link : uint8_t { Unknown = 0, Barrack = 1, Zone = 2, Social = 3 };

class Table {
public:
    bool load(const std::string& csv_path);

    const std::string& name_of(uint16_t op) const;
    Link link_of(uint16_t op) const;
    int size_of(uint16_t op) const;               // 0 variable, -1 unknown
    bool is_client_side(uint16_t op) const { return client_.count(op) != 0; }
    size_t count() const { return size_.size(); }

    // Total length of the packet at `p`, or 0 if it cannot be determined yet
    // (unknown opcode, or not enough bytes to reach the inline size field).
    // `variable` reports whether the length came from the inline field.
    int packet_size(const uint8_t* p, size_t avail, bool* variable) const;

    // Where a variable packet of this opcode carries its total length. Callers
    // need it to tell "the size field has not arrived yet" from "the size
    // field arrived and reads nonsense" -- the first waits, the second is a
    // framing loss.
    size_t size_field_offset(uint16_t op) const;

private:
    std::map<uint16_t, int> size_;
    std::map<uint16_t, std::string> name_;
    std::map<uint16_t, Link> link_;
    std::set<uint16_t> client_;
};

// CClientNet::Send: XOR on even byte index, ADD on odd.
uint32_t checksum(const uint8_t* p, size_t n);

}  // namespace relay
