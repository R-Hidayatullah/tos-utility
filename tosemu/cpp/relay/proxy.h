// The TCP relay itself.
//
// One thread accepts on each configured port; every accepted connection gets
// two more, one per direction, so a slow upstream in one direction can never
// stall the other. Decoding happens on the pump thread that owns the stream;
// the only shared state on the hot path is the dump's staging buffer and the
// counters.
//
// Client->server bytes are relayed the instant they arrive and decoded from a
// copy, so nothing we do can add latency to the client's input. Server->client
// bytes have to be decoded before forwarding, because the barrack hands out
// the zone address inside a packet and that address has to be rewritten to
// point back here -- otherwise the client dials the real zone directly and the
// capture stops at the barrack.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace relay {

class Blowfish;
class Table;
class Dump;
class Stats;

struct Upstream {
    uint16_t local = 0;
    std::string host;
    uint16_t port = 0;
};

class Proxy {
public:
    struct Config {
        bool redirect_zone = true;    // rewrite zone addresses back to us
        uint16_t relay_base = 17001;  // first port handed out to zone relays
    };

    bool start(const std::vector<Upstream>& ups, const Config& cfg,
               const Blowfish* bf, const Table* tbl, Dump* dump, Stats* stats);
    void stop();

private:
    struct Conn;

    void listen_loop(Upstream up);
    void serve(uintptr_t client, Upstream up);
    void pump(Conn* c, int dir);
    void split_c2s(Conn* c, std::vector<uint8_t>& buf);
    void split_s2c(Conn* c, std::vector<uint8_t>& buf, std::vector<uint8_t>& out);
    bool redirect(Conn* c, uint8_t* p, int sz);
    uint16_t ensure_relay(const std::string& ip, uint16_t port);
    void record(Conn* c, int dir, const uint8_t* pkt, uint32_t len,
                int declared, bool variable, uint32_t wire, uint8_t flags);
    // Bytes we could not frame, stored verbatim rather than dropped.
    void record_unframed(Conn* c, const uint8_t* p, uint32_t len, bool inferred);
    // Offset of the next byte that plausibly starts a packet, or npos.
    size_t resync_offset(const uint8_t* p, size_t n) const;

    Config cfg_;
    const Blowfish* bf_ = nullptr;
    const Table* tbl_ = nullptr;
    Dump* dump_ = nullptr;
    Stats* stats_ = nullptr;

    std::atomic<bool> run_{false};
    std::atomic<uint32_t> next_conn_{0};
    // Connection threads are detached, so stop() waits on this rather than on
    // a join. It has to reach zero before the dump may be closed.
    std::atomic<int> live_{0};

    std::mutex m_;
    std::vector<std::thread> listeners_;
    std::set<uintptr_t> sockets_;               // closed on stop to unblock
    std::map<std::string, uint16_t> relays_;    // "ip:port" -> local port
    uint16_t next_relay_ = 17001;
};

}  // namespace relay
