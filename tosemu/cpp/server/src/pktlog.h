// JSON packet log: direction, opcode, and decoded fields, one object per line.
//
// Layouts come from PacketSchema, so the log and the send-side verification
// read the same definitions -- a field the log names wrong is a field the
// verifier is checking wrong too, and both get fixed at once.
//
// Output is newline-delimited JSON so it can be tailed, grepped or piped into
// jq without a parser.
#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "packet.h"
#include "schema.h"

namespace tos {

enum class Dir { C2S, S2C };

class PacketLog {
public:
    // The schema path may be absent -- the log still records direction,
    // opcode, name and size, just without decoded fields.
    void load(const std::string& schema_path);
    bool open(const std::string& out_path);
    bool enabled() const { return out_.is_open(); }

    void write(Dir d, const Table& t, uint32_t session, const uint8_t* p,
               size_t n);

    // Exposed for tests: decode one packet to a JSON object string.
    std::string to_json(Dir d, const Table& t, uint32_t session,
                        const uint8_t* p, size_t n) const;

private:
    PacketSchema schema_;
    mutable std::mutex mu_;
    std::ofstream out_;
};

}  // namespace tos
