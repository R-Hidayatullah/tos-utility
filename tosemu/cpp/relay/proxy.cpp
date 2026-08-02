#include "proxy.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#include "common.h"
#include "dump.h"
#include "log.h"
#include "proto.h"
#include "stats.h"

namespace relay {

struct Proxy::Conn {
    uint32_t id = 0;
    uint16_t listen_port = 0;
    SOCKET client = INVALID_SOCKET;
    SOCKET up = INVALID_SOCKET;
    uint32_t client_ip = 0, up_ip = 0;
    uint16_t client_port = 0, up_port = 0;

    // s2c framing was lost -- an opcode the table does not know, so its length
    // cannot be looked up. Forwarding switches to immediate and unconditional
    // (the client must never be held up by our decoding), while decoding keeps
    // running on the same bytes looking for a boundary it recognises again.
    bool degraded = false;
    uint16_t degraded_op = 0;     // the opcode that lost us the framing
    uint64_t degraded_bytes = 0;  // how much has gone by unframed since

    // Set the first time framing is lost and never cleared: from then on the
    // pump forwards each chunk the moment it arrives. Decoding may recover
    // and go on producing proper records, but forwarding must not be routed
    // through the decoder again -- those bytes are already on their way to the
    // client, and sending them twice would corrupt the stream.
    bool raw_forward = false;
};

static std::string ip_str(uint32_t net_ip) {
    in_addr a;
    a.s_addr = net_ip;
    char b[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, b, sizeof(b));
    return b;
}

static bool send_all(SOCKET s, const uint8_t* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = send(s, reinterpret_cast<const char*>(p) + off, int(n - off), 0);
        if (w <= 0) return false;
        off += size_t(w);
    }
    return true;
}

bool Proxy::start(const std::vector<Upstream>& ups, const Config& cfg,
                  const Blowfish* bf, const Table* tbl, Dump* dump,
                  Stats* stats) {
    cfg_ = cfg;
    bf_ = bf;
    tbl_ = tbl;
    dump_ = dump;
    stats_ = stats;
    next_relay_ = cfg.relay_base;
    run_ = true;
    for (const auto& u : ups) listeners_.emplace_back(&Proxy::listen_loop, this, u);
    return true;
}

void Proxy::stop() {
    if (!run_.exchange(false)) return;
    // Closing the sockets is what wakes the blocked accept()/recv() calls.
    std::set<uintptr_t> socks;
    std::vector<std::thread> mine;
    {
        std::lock_guard<std::mutex> lk(m_);
        socks = sockets_;
        mine.swap(listeners_);   // a zone relay can still be adding to it
    }
    for (uintptr_t s : socks) {
        shutdown(SOCKET(s), SD_BOTH);
        closesocket(SOCKET(s));
    }
    for (auto& t : mine)
        if (t.joinable()) t.join();

    // Per-connection threads are detached; their sockets are shut down above,
    // so this is a short wait. Bounded, because a wedged connection must not
    // hold the restore hostage.
    for (int i = 0; i < 100 && live_.load() > 0; ++i) Sleep(50);
    if (live_.load() > 0)
        LOGW("%d connection thread(s) still running at shutdown", live_.load());
}

void Proxy::listen_loop(Upstream up) {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        LOGE("socket() failed for port %u", up.local);
        return;
    }
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
               sizeof(yes));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(up.local);
    if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
        listen(srv, 16) != 0) {
        LOGE("cannot listen on %u (WSA %d) -- port already in use?", up.local,
             WSAGetLastError());
        closesocket(srv);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        sockets_.insert(uintptr_t(srv));
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%s:%u", up.host.c_str(), up.port);
    if (stats_) stats_->listener(up.local, b);
    LOGI("listening on 0.0.0.0:%u -> %s", up.local, b);

    while (run_.load()) {
        sockaddr_in ca{};
        int cl = sizeof(ca);
        SOCKET c = accept(srv, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c == INVALID_SOCKET) break;
        std::thread(&Proxy::serve, this, uintptr_t(c), up).detach();
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        sockets_.erase(uintptr_t(srv));
    }
    closesocket(srv);
}

void Proxy::serve(uintptr_t client_sock, Upstream up) {
    struct Live {
        std::atomic<int>* n;
        explicit Live(std::atomic<int>* p) : n(p) { n->fetch_add(1); }
        ~Live() { n->fetch_sub(1); }
    } live(&live_);

    Conn c;
    c.id = ++next_conn_;
    c.listen_port = up.local;
    c.client = SOCKET(client_sock);

    sockaddr_in ca{};
    int cl = sizeof(ca);
    if (getpeername(c.client, reinterpret_cast<sockaddr*>(&ca), &cl) == 0) {
        c.client_ip = ca.sin_addr.s_addr;
        c.client_port = ntohs(ca.sin_port);
    }

    c.up = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(up.port);
    inet_pton(AF_INET, up.host.c_str(), &sa.sin_addr);
    c.up_ip = sa.sin_addr.s_addr;
    c.up_port = up.port;

    if (c.up == INVALID_SOCKET ||
        connect(c.up, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        LOGE("conn %u: upstream %s:%u unreachable (WSA %d)", c.id,
             up.host.c_str(), up.port, WSAGetLastError());
        closesocket(c.client);
        if (c.up != INVALID_SOCKET) closesocket(c.up);
        return;
    }

    BOOL nodelay = TRUE;
    setsockopt(c.client, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&nodelay), sizeof(nodelay));
    setsockopt(c.up, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&nodelay), sizeof(nodelay));
    {
        std::lock_guard<std::mutex> lk(m_);
        sockets_.insert(uintptr_t(c.client));
        sockets_.insert(uintptr_t(c.up));
    }
    if (stats_) stats_->conn_open();
    LOGI("conn %u open: %s:%u -> :%u -> %s:%u", c.id,
         ip_str(c.client_ip).c_str(), c.client_port, c.listen_port,
         up.host.c_str(), up.port);

    std::thread a(&Proxy::pump, this, &c, 0);
    std::thread b(&Proxy::pump, this, &c, 1);
    a.join();
    b.join();

    {
        std::lock_guard<std::mutex> lk(m_);
        sockets_.erase(uintptr_t(c.client));
        sockets_.erase(uintptr_t(c.up));
    }
    closesocket(c.client);
    closesocket(c.up);
    if (stats_) stats_->conn_close();
    LOGI("conn %u closed", c.id);
}

void Proxy::pump(Conn* c, int dir) {
    SOCKET src = dir == 0 ? c->client : c->up;
    SOCKET dst = dir == 0 ? c->up : c->client;
    std::vector<uint8_t> buf, out;
    std::vector<uint8_t> chunk(65536);

    for (;;) {
        int n = recv(src, reinterpret_cast<char*>(chunk.data()),
                     int(chunk.size()), 0);
        if (n <= 0) break;
        if (dir == 0) {
            // Relay first, always: the decode is a copy and must never be able
            // to delay or drop the client's own bytes.
            if (!send_all(dst, chunk.data(), size_t(n))) break;
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
            split_c2s(c, buf);
        } else if (c->raw_forward) {
            // Framing was lost earlier on this connection. Forward first and
            // unconditionally, then decode what we can from a copy: an opcode
            // the table cannot name must never delay the client.
            if (!send_all(dst, chunk.data(), size_t(n))) break;
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
            out.clear();
            split_s2c(c, buf, out);
        } else {
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
            out.clear();
            split_s2c(c, buf, out);
            if (!out.empty() && !send_all(dst, out.data(), out.size())) break;
        }
    }
    shutdown(src, SD_BOTH);
    shutdown(dst, SD_BOTH);
}

void Proxy::record(Conn* c, int dir, const uint8_t* pkt, uint32_t len,
                   int declared, bool variable, uint32_t wire, uint8_t flags) {
    uint16_t op = 0;
    if (len >= 2) std::memcpy(&op, pkt, 2);
    std::string name = (flags & REC_F_UNFRAMED) ? "UNFRAMED" : tbl_->name_of(op);
    uint8_t link = uint8_t(tbl_->link_of(op));
    if (tbl_->size_of(op) < 0) flags |= REC_F_UNKNOWN_OP;

    RecordMeta m;
    m.conn_id = c->id;
    m.listen_port = c->listen_port;
    m.direction = uint16_t(dir);
    m.wire_len = wire;
    m.link = link;
    m.encrypted = dir == 0 ? 1 : 0;
    m.flags = flags;
    if (dir == 0) {
        m.src_ip = c->client_ip; m.src_port = c->client_port;
        m.dst_ip = c->up_ip;     m.dst_port = c->up_port;
    } else {
        m.src_ip = c->up_ip;     m.src_port = c->up_port;
        m.dst_ip = c->client_ip; m.dst_port = c->client_port;
    }

    bool bad = false;
    if (len >= 10 && declared > 0 && uint32_t(declared) <= len) {
        std::vector<uint8_t> v(pkt, pkt + declared);
        uint32_t carried;
        std::memcpy(&carried, pkt + 6, 4);
        std::memset(v.data() + 6, 0, 4);
        bad = checksum(v.data(), v.size()) != carried;
    }
    if (dump_) dump_->write(m, pkt, len, declared, variable, name);
    if (stats_) stats_->packet(dir, op, name, len, wire, declared, link, bad);
}

void Proxy::record_unframed(Conn* c, const uint8_t* p, uint32_t len,
                            bool inferred) {
    if (!len) return;
    uint8_t flags = REC_F_UNFRAMED | REC_F_UNKNOWN_OP;
    if (inferred) flags |= REC_F_INFERRED_LEN;
    // The opcode is a guess -- the first two bytes of whatever we could not
    // parse -- but when the resync landed cleanly it is the real opcode, and
    // the record length is then its real length. That pair is exactly what is
    // needed to add a row to packet_opcodes.csv.
    uint16_t op = 0;
    if (len >= 2) std::memcpy(&op, p, 2);
    if (stats_) stats_->unframed(op, len, inferred);
    record(c, 1, p, len, 0, false, len, flags);
}

// A byte offset that plausibly starts a packet. Requires the candidate to be
// followed by another header the table also recognises, because a lone valid
// opcode turns up in payload data constantly and resyncing onto one would
// produce confident nonsense.
size_t Proxy::resync_offset(const uint8_t* p, size_t n) const {
    for (size_t i = 0; i + 10 <= n; ++i) {
        uint16_t op;
        std::memcpy(&op, p + i, 2);
        bool var = false;
        int sz = tbl_->packet_size(p + i, n - i, &var);
        if (sz < 10 || sz > 0x8000) continue;
        if (tbl_->size_of(op) < 0) continue;

        size_t next = i + size_t(sz);
        if (next + 2 > n) {
            // Cannot chain-check yet. Only accept a candidate that ends
            // exactly on the data we have; otherwise wait for more bytes.
            if (next == n) return i;
            continue;
        }
        uint16_t op2;
        std::memcpy(&op2, p + next, 2);
        if (tbl_->size_of(op2) >= 0) return i;
    }
    return std::string::npos;
}

void Proxy::split_c2s(Conn* c, std::vector<uint8_t>& buf) {
    while (buf.size() >= 2) {
        uint16_t padded;
        std::memcpy(&padded, buf.data(), 2);
        if (padded == 0 || (padded % 8) || padded > 0x8000) {
            buf.erase(buf.begin(), buf.begin() + 2);       // resync
            if (stats_) stats_->resync(0);
            continue;
        }
        if (buf.size() < size_t(2) + padded) return;
        std::vector<uint8_t> plain(buf.begin() + 2, buf.begin() + 2 + padded);
        bf_->decrypt_buffer(plain.data(), plain.size());
        buf.erase(buf.begin(), buf.begin() + 2 + padded);

        // An unknown opcode costs nothing here: the frame carries its own
        // length, so the packet is captured whole either way. Only its
        // declared size is unavailable, and the trailing bytes are the
        // cipher's padding to a multiple of 8.
        bool var = false;
        int sz = tbl_->packet_size(plain.data(), plain.size(), &var);
        uint32_t body = (sz > 0 && uint32_t(sz) <= plain.size())
                            ? uint32_t(sz)
                            : uint32_t(plain.size());
        uint16_t op = 0;
        std::memcpy(&op, plain.data(), 2);
        uint8_t flags = 0;
        if (tbl_->size_of(op) < 0) {
            flags |= REC_F_UNKNOWN_OP;
            if (stats_) stats_->unknown_c2s(op, body);
        }
        record(c, 0, plain.data(), body, sz, var, uint32_t(padded) + 2, flags);
    }
}

// While degraded, the caller has already forwarded these bytes, so `out` is
// left alone and this is pure decoding. The buffer is never held back waiting
// for a resync -- an opcode we cannot name must not become a stall.
void Proxy::split_s2c(Conn* c, std::vector<uint8_t>& buf,
                      std::vector<uint8_t>& out) {
    while (buf.size() >= 10) {
        if (c->degraded) {
            size_t at = resync_offset(buf.data(), buf.size());
            if (at == std::string::npos) {
                // Keep a short tail: a packet header could be split across
                // this chunk and the next, and cutting through one would lose
                // the boundary we are looking for.
                const size_t kTail = 64;
                if (buf.size() > kTail) {
                    uint32_t n = uint32_t(buf.size() - kTail);
                    record_unframed(c, buf.data(), n, false);
                    c->degraded_bytes += n;
                    buf.erase(buf.begin(), buf.begin() + n);
                }
                return;
            }
            if (at > 0) {
                record_unframed(c, buf.data(), uint32_t(at), true);
                c->degraded_bytes += at;
                buf.erase(buf.begin(), buf.begin() + at);
            }
            c->degraded = false;
            LOGI("conn %u: s2c framing recovered after %llu unframed byte(s) "
                 "from op=%u", c->id, (unsigned long long)c->degraded_bytes,
                 c->degraded_op);
            c->degraded_bytes = 0;
            continue;
        }

        bool var = false;
        int sz = tbl_->packet_size(buf.data(), buf.size(), &var);
        uint16_t op;
        std::memcpy(&op, buf.data(), 2);
        if (sz == 0 && var) {
            // Nothing is wrong yet if the inline size field simply has not
            // arrived; wait for it rather than declaring a framing loss.
            if (buf.size() < tbl_->size_field_offset(op) + 2) return;
        }
        if (sz < 10 || sz > 0x8000) {
            // An opcode that is not in packet_opcodes.csv, or a length that
            // cannot be true. Its bytes are still captured, as UNFRAMED
            // records, and decoding resumes at the next boundary the table
            // recognises -- one unknown packet no longer costs the rest of
            // the connection.
            LOGW("conn %u: s2c opcode %u is not in the table (size=%d) -- "
                 "capturing unframed until the stream can be read again%s",
                 c->id, op, sz,
                 cfg_.redirect_zone && !c->raw_forward
                     ? "; zone redirect is off for the rest of this connection"
                     : "");
            c->degraded = true;
            c->degraded_op = op;
            c->degraded_bytes = 0;
            if (stats_) stats_->passthrough();
            if (!c->raw_forward) {
                // Not yet forwarded; hand them over once, and from here the
                // pump does the forwarding.
                out.insert(out.end(), buf.begin(), buf.end());
                c->raw_forward = true;
            }
            continue;
        }
        if (buf.size() < size_t(sz)) return;
        record(c, 1, buf.data(), uint32_t(sz), sz, var, uint32_t(sz), 0);
        if (!c->raw_forward) {
            if (cfg_.redirect_zone) redirect(c, buf.data(), sz);
            out.insert(out.end(), buf.begin(), buf.begin() + sz);
        }
        buf.erase(buf.begin(), buf.begin() + sz);
    }
}

uint16_t Proxy::ensure_relay(const std::string& ip, uint16_t port) {
    char key[64];
    std::snprintf(key, sizeof(key), "%s:%u", ip.c_str(), port);
    uint16_t local;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!run_.load()) return 0;          // shutting down; do not spawn more
        auto it = relays_.find(key);
        if (it != relays_.end()) return it->second;
        local = next_relay_++;
        relays_[key] = local;
        listeners_.emplace_back(&Proxy::listen_loop, this,
                                Upstream{local, ip, port});
    }
    LOGI("zone relay: 127.0.0.1:%u -> %s", local, key);
    Sleep(120);        // let the listener bind before the client is told about it
    return local;
}

// The barrack hands the client the real zone address and the client dials it
// directly, so a passive proxy never sees zone traffic. Rewrite those to
// 127.0.0.1:<relay> and stand a relay up per destination.
//
//   BC_START_GAMEOK (27, 37B)  ip bytes @+0x0E, u32 port @+0x12
//   BC_SERVER_ENTRY (74, 22B)  2 x ip bytes @+0x0A step 4, u16 ports @+0x12
//
// Server->client packets carry checksum 0, so no fixup is needed after the
// edit.
bool Proxy::redirect(Conn* c, uint8_t* p, int sz) {
    auto rewrite = [&](uint8_t* ipb, uint16_t realport, uint16_t* slot16,
                       uint32_t* slot32) {
        if (ipb[0] == 0 || ipb[0] == 127) return;    // unset, or already ours
        char ip[32];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", ipb[0], ipb[1], ipb[2],
                      ipb[3]);
        uint16_t local = ensure_relay(ip, realport);
        if (local == 0) return;                      // shutting down
        ipb[0] = 127; ipb[1] = 0; ipb[2] = 0; ipb[3] = 1;
        if (slot16) *slot16 = local;
        if (slot32) *slot32 = local;
        LOGI("conn %u: redirected %s:%u -> 127.0.0.1:%u", c->id, ip, realport,
             local);
    };

    uint16_t op;
    std::memcpy(&op, p, 2);
    if (op == 27 && sz >= 37) {                        // BC_START_GAMEOK
        uint32_t port;
        std::memcpy(&port, p + 0x12, 4);
        rewrite(p + 0x0E, uint16_t(port), nullptr,
                reinterpret_cast<uint32_t*>(p + 0x12));
        return true;
    }
    if (op == 74 && sz >= 22) {                        // BC_SERVER_ENTRY
        for (int i = 0; i < 2; ++i) {
            uint16_t port;
            std::memcpy(&port, p + 0x12 + i * 2, 2);
            rewrite(p + 0x0A + i * 4, port,
                    reinterpret_cast<uint16_t*>(p + 0x12 + i * 2), nullptr);
        }
        return true;
    }
    return false;
}

}  // namespace relay
