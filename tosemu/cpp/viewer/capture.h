// Loads a packet dump into memory.
//
// Two formats are read. TOSRLY (cpp/relay) carries addresses, ports and the
// packet name in every record. TOSCAP (cpp/tos_capture) is the older one and
// carries neither, so names come from packet_opcodes.csv and the address
// fields stay empty -- the viewer shows both through the same struct rather
// than growing two code paths.
//
// The whole file is held as one blob and packet bodies are offsets into it.
// A 2 MB capture is 7887 records; copying each body would be pointless work
// and would double the memory for nothing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace view {

struct Packet {
    uint64_t time_us = 0;
    uint64_t mono_ns = 0;
    uint64_t index = 0;
    uint32_t conn = 0;
    uint32_t src_ip = 0, dst_ip = 0;
    uint32_t seq = 0, checksum = 0;
    uint32_t wire = 0;
    uint32_t body_off = 0, body_len = 0;   // into Capture::blob
    uint16_t src_port = 0, dst_port = 0, listen_port = 0;
    uint16_t dir = 0;                      // 0 c2s, 1 s2c
    uint16_t opcode = 0, declared = 0;
    uint8_t  link = 0;                     // 0 ?, 1 barrack, 2 zone, 3 social
    uint8_t  chk_ok = 2;                   // 1 ok, 0 bad, 2 unverifiable
    uint8_t  variable = 0;
    uint8_t  encrypted = 0;
    uint8_t  flags = 0;                    // PF_*
    std::string name;
};

// Mirrors REC_F_* in cpp/relay/dump.h.
static const uint8_t PF_UNKNOWN_OP = 1 << 0;   // not in packet_opcodes.csv
static const uint8_t PF_UNFRAMED = 1 << 1;     // bytes that could not be framed
static const uint8_t PF_INFERRED_LEN = 1 << 2; // length from the next boundary

struct Meta {
    int version = 0;
    bool v2 = false;              // TOSRLY; false means legacy TOSCAP
    bool clean = false;           // trailer present (v2 only)
    bool has_addresses = false;
    uint64_t start_us = 0;
    uint32_t pid = 0;
    uint64_t resyncs = 0;         // bytes skipped past damage
    std::string region, note;
};

class Capture {
public:
    // `err` gets a message the UI can show as-is. `csv_hint` is a directory to
    // look in for packet_opcodes.csv, used only by the legacy format.
    bool load(const std::wstring& path, std::wstring& err);

    const std::vector<Packet>& packets() const { return pkts_; }
    const Meta& meta() const { return meta_; }
    const std::wstring& path() const { return path_; }
    const uint8_t* body(const Packet& p) const { return blob_.data() + p.body_off; }
    bool empty() const { return pkts_.empty(); }

private:
    bool parse_v2(std::wstring& err);
    bool parse_v1(std::wstring& err);

    std::wstring path_;
    std::vector<uint8_t> blob_;
    std::vector<Packet> pkts_;
    Meta meta_;
};

// Where a packet's body starts, given its direction and link. Client packets
// on the barrack and zone links carry twelve further header bytes the server
// never reads (docs/11-packet-framing.md), which the hex panel shades
// separately -- reading them as body is the classic way to be off by twelve.
size_t body_start(const Packet& p);

std::wstring ip_to_w(uint32_t net_ip);
std::wstring format_clock(uint64_t time_us);

}  // namespace view
