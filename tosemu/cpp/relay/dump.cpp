#include "dump.h"

#include <windows.h>

#include <chrono>
#include <cstring>

#include "common.h"
#include "log.h"
#include "proto.h"

namespace relay {

// A capture that outgrows this is a bug or an attack, not a session. Records
// past the cap are counted as dropped rather than allowed to eat all of RAM
// while the disk falls behind.
static const size_t kStagingCap = 64u << 20;   // 64 MB

bool Dump::open(const std::string& path, const std::string& region,
                const std::string& note) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    path_ = path;
    start_mono_ = mono_ns();

    FileHeader h{};
    std::memcpy(h.magic, "TOSRLY\0\0", 8);
    h.version = 2;
    h.file_header_size = sizeof(FileHeader);
    h.record_header_size = sizeof(RecordHeader);
    h.flags = 1;                                // names stored inline
    h.start_us = now_us();
    h.start_mono_ns = start_mono_;
    std::strncpy(h.region, region.c_str(), sizeof(h.region) - 1);
    std::strncpy(h.note, note.c_str(), sizeof(h.note) - 1);
    h.pid = uint32_t(GetCurrentProcessId());
    std::fwrite(&h, sizeof(h), 1, f_);
    std::fflush(f_);

    staging_.reserve(1u << 20);
    run_ = true;
    th_ = std::thread(&Dump::writer_loop, this);
    return true;
}

void Dump::close(bool clean) {
    if (closed_.exchange(true)) return;
    run_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    drain();                                    // whatever landed after the join
    if (f_) {
        FileTrailer t{};
        t.magic = END_MAGIC;
        t.clean = clean ? 1 : 0;
        t.end_us = now_us();
        t.records = records_.load();
        t.bytes = bytes_.load();
        std::fwrite(&t, sizeof(t), 1, f_);
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
    }
}

void Dump::write(const RecordMeta& meta, const uint8_t* pkt, uint32_t len,
                 int declared, bool variable, const std::string& name) {
    RecordHeader r{};
    r.magic = REC_MAGIC;
    r.time_us = now_us();
    r.mono_ns = mono_ns() - start_mono_;
    r.conn_id = meta.conn_id;
    r.src_ip = meta.src_ip;
    r.dst_ip = meta.dst_ip;
    r.src_port = meta.src_port;
    r.dst_port = meta.dst_port;
    r.listen_port = meta.listen_port;
    r.direction = meta.direction;
    r.wire_len = meta.wire_len;
    r.link = meta.link;
    r.encrypted = meta.encrypted;
    r.flags = meta.flags;
    r.declared_size = uint16_t(declared > 0 ? declared : 0);
    r.variable = variable ? 1 : 0;
    r.body_len = len;
    if (len >= 10) {
        std::memcpy(&r.opcode, pkt, 2);
        std::memcpy(&r.sequence, pkt + 2, 4);
        std::memcpy(&r.checksum, pkt + 6, 4);
    }

    // The built-in correctness check: a client->server record that decodes
    // with a bad checksum means the Blowfish key or init table is wrong.
    if (len >= 10 && declared > 0 && uint32_t(declared) <= len) {
        std::vector<uint8_t> v(pkt, pkt + declared);
        std::memset(v.data() + 6, 0, 4);
        r.checksum_ok = checksum(v.data(), v.size()) == r.checksum ? 1 : 0;
    } else {
        r.checksum_ok = 2;
    }

    if (closed_.load()) { dropped_.fetch_add(1); return; }

    uint8_t nlen = uint8_t(name.size() > 255 ? 255 : name.size());
    r.name_len = nlen;
    r.record_len = uint32_t(sizeof(r)) + nlen + len;

    std::unique_lock<std::mutex> lk(m_);
    if (staging_.size() + r.record_len > kStagingCap) {
        lk.unlock();
        dropped_.fetch_add(1);
        return;
    }
    r.index = records_.fetch_add(1);
    const uint8_t* rh = reinterpret_cast<const uint8_t*>(&r);
    staging_.insert(staging_.end(), rh, rh + sizeof(r));
    staging_.insert(staging_.end(), name.begin(), name.begin() + nlen);
    staging_.insert(staging_.end(), pkt, pkt + len);
    bytes_.fetch_add(r.record_len);
    lk.unlock();
    cv_.notify_one();
}

size_t Dump::queued() const {
    std::lock_guard<std::mutex> lk(m_);
    return staging_.size();
}

void Dump::drain() {
    {
        std::lock_guard<std::mutex> lk(m_);
        flushing_.swap(staging_);
        staging_.clear();
    }
    if (flushing_.empty()) return;
    if (f_) {
        std::fwrite(flushing_.data(), 1, flushing_.size(), f_);
        std::fflush(f_);
    }
    flushing_.clear();
}

void Dump::writer_loop() {
    while (run_.load()) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(200),
                         [&] { return !staging_.empty() || !run_.load(); });
        }
        drain();
    }
    drain();
}

}  // namespace relay
