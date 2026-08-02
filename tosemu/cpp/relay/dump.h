// Structured binary packet dump.
//
//   FileHeader
//   repeated { RecordHeader, char name[name_len], u8 body[body_len] }
//   FileTrailer                      (only present after a clean shutdown)
//
// Every record carries its own length and a magic, so a dump cut short by a
// crash still reads: a reader scans for the next magic and keeps going. The
// absence of the trailer is how a reader knows the capture was cut short.
//
// Bodies are the FULL plaintext packet including the 10-byte header, so
// opcode, sequence and checksum can be re-derived independently of the fields
// recorded beside them.
//
// Socket threads never touch the file. They append serialised records to a
// staging buffer under a short lock; one writer thread swaps the buffer out and
// does the I/O. A stall on disk costs latency in the dump, never in the relay.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace relay {

#pragma pack(push, 1)
struct FileHeader {
    char     magic[8];            // "TOSRLY\0\0"
    uint32_t version;             // 2
    uint32_t file_header_size;
    uint32_t record_header_size;
    uint32_t flags;
    uint64_t start_us;            // wall clock at capture start
    uint64_t start_mono_ns;
    char     region[16];
    char     note[32];
    uint32_t pid;
    uint32_t reserved;
};

struct RecordHeader {
    uint32_t magic;               // 'PKTR'
    uint32_t record_len;          // this header + name + body
    uint64_t time_us;             // wall clock, microseconds since UNIX epoch
    uint64_t mono_ns;             // nanoseconds since capture start
    uint64_t index;               // global record number
    uint32_t conn_id;
    uint32_t src_ip;              // IPv4, network byte order
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t listen_port;         // proxy port the connection arrived on
    uint16_t direction;           // 0 client->server, 1 server->client
    uint16_t opcode;
    uint16_t declared_size;       // from the table, or the inline field
    uint32_t sequence;
    uint32_t checksum;            // as carried in the packet
    uint32_t wire_len;            // bytes on the wire, incl. cipher padding
    uint32_t body_len;
    uint8_t  link;                // 0 unknown, 1 barrack, 2 zone, 3 social
    uint8_t  checksum_ok;         // 1 ok, 0 mismatch, 2 not verifiable
    uint8_t  variable;
    uint8_t  encrypted;           // 1 = arrived Blowfish-sealed
    uint8_t  name_len;
    uint8_t  flags;               // REC_F_*
    uint8_t  reserved[2];
};

// The opcode is not in packet_opcodes.csv, so its length could not be looked
// up. The record still holds the bytes; `declared_size` is 0 and `body_len` is
// what we could delimit.
static const uint8_t REC_F_UNKNOWN_OP = 1 << 0;
// The bytes could not be framed as a packet at all. They are stored verbatim
// from wherever framing was lost up to wherever it was recovered, so nothing
// is dropped even when the table cannot explain the stream.
static const uint8_t REC_F_UNFRAMED = 1 << 1;
// The length was inferred from where the next valid packet started, not read
// from the table or an inline field. Good enough to extend the table with,
// not good enough to trust blindly.
static const uint8_t REC_F_INFERRED_LEN = 1 << 2;

struct FileTrailer {
    uint32_t magic;               // 'TEND'
    uint32_t clean;               // 1 = shut down through the normal path
    uint64_t end_us;
    uint64_t records;
    uint64_t bytes;
};
#pragma pack(pop)

static const uint32_t REC_MAGIC = 0x504B5452;   // 'PKTR'
static const uint32_t END_MAGIC = 0x444E4554;   // 'TEND'

// Everything a record needs that is not derivable from the packet bytes.
struct RecordMeta {
    uint32_t conn_id = 0;
    uint32_t src_ip = 0;
    uint32_t dst_ip = 0;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint16_t listen_port = 0;
    uint16_t direction = 0;
    uint32_t wire_len = 0;
    uint8_t  link = 0;
    uint8_t  encrypted = 0;
    uint8_t  flags = 0;           // REC_F_*
};

class Dump {
public:
    bool open(const std::string& path, const std::string& region,
              const std::string& note);
    // Flushes what is queued, appends the trailer, closes. Safe to call twice.
    void close(bool clean);

    void write(const RecordMeta& meta, const uint8_t* pkt, uint32_t len,
               int declared, bool variable, const std::string& name);

    const std::string& path() const { return path_; }
    uint64_t records() const { return records_.load(); }
    uint64_t bytes() const { return bytes_.load(); }
    uint64_t dropped() const { return dropped_.load(); }
    size_t queued() const;

private:
    void writer_loop();
    void drain();

    std::string path_;
    std::FILE* f_ = nullptr;
    uint64_t start_mono_ = 0;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<uint8_t> staging_;
    std::vector<uint8_t> flushing_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> records_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace relay
