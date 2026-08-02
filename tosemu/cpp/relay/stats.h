// Live counters and the console dashboard.
//
// The dashboard owns the top of the window and the log scrolls underneath it,
// so the two never fight: the running totals stay visible while individual
// events keep streaming past. If the console will not do VT sequences the
// dashboard degrades to printing the same block every few seconds.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace relay {

class Dump;
class Guard;

struct OpStat {
    std::string name;
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t last_us = 0;
    uint16_t declared = 0;
    uint8_t  link = 0;
};

class Stats {
public:
    void start();

    // dir: 0 = client->server, 1 = server->client.
    void packet(int dir, uint16_t op, const std::string& name, uint32_t len,
                uint32_t wire, int declared, uint8_t link, bool checksum_bad);
    void conn_open()  { conns_total_.fetch_add(1); conns_active_.fetch_add(1); }
    void conn_close() { conns_active_.fetch_sub(1); }
    void resync(int dir) { resync_[dir & 1].fetch_add(1); }
    void passthrough() { passthrough_.fetch_add(1); }
    // Bytes captured that could not be framed, and opcodes the table does not
    // have. Both are the interesting kind of unknown: they are what a capture
    // is for when the opcode table is out of date.
    void unframed(uint16_t op, uint32_t len, bool inferred);
    void unknown_c2s(uint16_t op, uint32_t len);
    void http_hit()  { http_.fetch_add(1); }
    void listener(uint16_t local, const std::string& upstream);

    uint64_t packets(int d) const { return pkt_[d & 1].load(); }
    uint64_t bytes(int d) const { return bytes_[d & 1].load(); }
    uint64_t wire(int d) const { return wire_[d & 1].load(); }
    uint64_t conns_total() const { return conns_total_.load(); }
    uint64_t conns_active() const { return conns_active_.load(); }
    uint64_t bad_checksum() const { return bad_.load(); }
    uint64_t resyncs() const { return resync_[0].load() + resync_[1].load(); }
    uint64_t passthroughs() const { return passthrough_.load(); }
    uint64_t http_hits() const { return http_.load(); }
    uint64_t distinct() const;
    uint64_t start_us() const { return start_us_; }
    uint64_t unframed_bytes() const { return unframed_bytes_.load(); }
    uint64_t unframed_records() const { return unframed_recs_.load(); }

    // Opcode -> what we saw of it, for the shutdown summary. These are the
    // rows that packet_opcodes.csv is missing.
    struct Unknown {
        uint64_t count = 0;
        uint32_t min_len = 0xFFFFFFFF, max_len = 0;
        bool from_client = false;
        bool length_inferred = false;
    };
    std::vector<std::pair<uint16_t, Unknown>> unknowns() const;

    std::vector<std::pair<int, OpStat>> top(size_t n) const;
    std::vector<std::string> listeners() const;

    // Rate since the previous call, for the dashboard.
    struct Rate { double pps, bps; };
    Rate sample();

private:
    std::atomic<uint64_t> pkt_[2]{}, bytes_[2]{}, wire_[2]{}, resync_[2]{};
    std::atomic<uint64_t> conns_total_{0}, conns_active_{0}, bad_{0};
    std::atomic<uint64_t> passthrough_{0}, http_{0};
    std::atomic<uint64_t> unframed_bytes_{0}, unframed_recs_{0};
    std::map<uint16_t, Unknown> unknown_;
    uint64_t start_us_ = 0, start_mono_ = 0;

    mutable std::mutex m_;
    std::map<std::pair<int, uint16_t>, OpStat> ops_;
    std::vector<std::string> listeners_;
    uint64_t last_mono_ = 0, last_pkt_ = 0, last_bytes_ = 0;
};

class Dashboard {
public:
    // Reserves the top of the window and starts the repaint thread.
    void start(Stats* s, Dump* d, Guard* g, const std::string& title);
    void stop();
    bool vt() const { return vt_; }

private:
    void loop();
    std::string frame();

    Stats* s_ = nullptr;
    Dump* d_ = nullptr;
    Guard* g_ = nullptr;
    std::string title_;
    std::thread th_;
    std::atomic<bool> run_{false};
    bool vt_ = false;
    int height_ = 16;
};

}  // namespace relay
