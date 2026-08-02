// Packet layouts recovered from the client, and the check that our own
// builders agree with them.
//
// packet_schema.json is written by gen_packets_h.py from the 121 server->client
// structs ida_dump_structs.py recovered out of the client's parsing handlers
// (docs/08). Each entry lists the offsets the client actually reads and how
// wide each read is, which makes it the authority on layout -- ahead of any
// reference implementation, whose layouts were largely inferred.
//
// `verify` turns that into a test: PacketWriter records a boundary after every
// field, and a packet is correct only if every offset the client reads from is
// also a boundary in what we wrote. A field inserted, dropped or sized wrong
// shifts everything after it, so the mismatch surfaces at startup instead of
// as a mis-rendered character.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace tos {

class PacketWriter;

struct Field {
    uint32_t off = 0;
    uint32_t width = 0;
    std::string type;    // "u8" "u16" "u32" "u64" "f32" "f32[n]" "bytes" "str"
    std::string name;    // only the built-in client->server entries carry one
};

struct SchemaEntry {
    std::string name;
    int size = 0;        // client's declared sizeof; 0 == variable
    std::vector<Field> fields;
};

class PacketSchema {
public:
    // Loads the JSON schema; also installs the built-in client->server table.
    // Missing file is not an error -- the server runs, just without checks.
    bool load(const std::string& path);

    const SchemaEntry* find(uint16_t op) const;
    const std::vector<Field>* fields_for(uint16_t op) const;
    size_t count() const { return entries_.size(); }

    // Compare a built packet's field boundaries with the client's layout.
    // Returns the number of misaligned fields and appends one line per problem
    // to `problems`. Opcodes with no recovered layout verify trivially.
    int verify(const PacketWriter& w, std::vector<std::string>& problems) const;

private:
    std::unordered_map<uint16_t, SchemaEntry> entries_;
};

}  // namespace tos
