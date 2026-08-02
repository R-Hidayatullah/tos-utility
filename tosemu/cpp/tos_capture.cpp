// tos_capture.cpp -- Tree of Savior transparent capture proxy.
//
// Relays the client to the real server byte-for-byte untouched, decrypts the
// client->server direction, and writes every packet to a structured .bin for
// offline static analysis against the reversed opcode table.
//
// Protocol (reversed from Client_tos_x64.exe):
//
//   client -> server : u16 padded_len | Blowfish-ECB(payload padded to 8)
//   server -> client : payload                    (plaintext, no framing)
//
//   payload = u16 opcode | u32 sequence | u32 checksum | body
//
//   Total packet length comes from the packet-size table; when that entry is
//   0 the packet is variable and carries its TOTAL length as a u16 at +0x0A.
//   (gePacketTable::GetPacketSize / CClientNet::MoveToRecvQueue.)
//
//   Checksum, from CClientNet::Send, over the whole packet with the checksum
//   field zeroed: even byte index XORs, odd index ADDs.
//
//   Cipher is textbook Blowfish (16 rounds, big-endian blocks, standard F)
//   seeded with a CUSTOM 1042-dword init table -- NOT the usual pi constants.
//   geCrypt::Init assembles it from 4 scattered chunks of a source table,
//   parts {16,2,256,768} at seeks {4,1056,24,284}; the standard-looking
//   P-arrays elsewhere in .data are decoys. bf_inittable.bin holds the
//   assembled table. Key is 16 bytes: row 11 of the table at 0x141A70680.
//
// Build (MSVC):   cl /std:c++17 /O2 /EHsc tos_capture.cpp /link ws2_32.lib
// Build (MinGW):  g++ -std=c++17 -O2 -o tos_capture.exe tos_capture.cpp -lws2_32
//
// Run:            tos_capture.exe asia            (or: na)
//
// Needs bf_inittable.bin and packet_opcodes.csv in the working directory.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------- container
//
// File layout:
//
//   FileHeader
//   repeated { RecordHeader, uint8 body[body_len] }
//
// Every record's body is the FULL plaintext packet including its 10-byte
// header, so a reader can re-derive opcode/sequence/checksum independently.
// Records are fixed-size-prefixed and carry a magic, so a truncated capture
// (crash, Ctrl+C) can still be scanned and resynced.

#pragma pack(push, 1)
struct FileHeader {
    char     magic[8];      // "TOSCAP\0\0"
    uint32_t version;       // 1
    uint32_t header_size;   // sizeof(RecordHeader), for forward compat
    uint64_t epoch_us;      // capture start, microseconds since UNIX epoch
    char     region[16];    // "asia" / "na"
    uint64_t reserved;
};

struct RecordHeader {
    uint32_t magic;         // 'PKTR' = 0x504B5452
    uint64_t time_us;       // microseconds since UNIX epoch
    uint16_t direction;     // 0 = client->server, 1 = server->client
    uint16_t port;          // local port the connection arrived on
    uint32_t conn_id;       // distinguishes concurrent connections
    uint16_t opcode;
    uint16_t declared_size; // from the size table, or +0x0A when variable
    uint32_t sequence;
    uint32_t checksum;      // as carried in the packet
    uint8_t  checksum_ok;   // 1 ok, 0 mismatch, 2 not verifiable
    uint8_t  variable;      // 1 if the size table said 0 (length at +0x0A)
    uint16_t reserved;
    uint32_t body_len;      // bytes of packet that follow
};
#pragma pack(pop)

static const uint32_t REC_MAGIC = 0x504B5452;  // 'PKTR'

// ---------------------------------------------------------------- blowfish

class Blowfish {
public:
    bool load(const char* table_path, const std::string& key) {
        std::ifstream f(table_path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        if (raw.size() < 1042 * 4) return false;

        auto be32 = [&](size_t i) {
            return (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) |
                   (uint32_t(raw[i * 4 + 2]) << 8) | uint32_t(raw[i * 4 + 3]);
        };
        for (int i = 0; i < 18; ++i) P_[i] = be32(i);
        for (int b = 0; b < 4; ++b)
            for (int i = 0; i < 256; ++i) S_[b][i] = be32(18 + b * 256 + i);

        size_t j = 0;
        for (int i = 0; i < 18; ++i) {
            uint32_t k = 0;
            for (int n = 0; n < 4; ++n) { k = (k << 8) | uint8_t(key[j % key.size()]); ++j; }
            P_[i] ^= k;
        }
        uint32_t L = 0, R = 0;
        for (int i = 0; i < 18; i += 2) { encrypt(L, R); P_[i] = L; P_[i + 1] = R; }
        for (int b = 0; b < 4; ++b)
            for (int i = 0; i < 256; i += 2) { encrypt(L, R); S_[b][i] = L; S_[b][i + 1] = R; }
        return true;
    }

    void decrypt_buffer(uint8_t* data, size_t len) const {
        for (size_t off = 0; off + 8 <= len; off += 8) {
            uint32_t L = load_be(data + off), R = load_be(data + off + 4);
            decrypt(L, R);
            store_be(data + off, L); store_be(data + off + 4, R);
        }
    }

private:
    uint32_t P_[18]{};
    uint32_t S_[4][256]{};

    static uint32_t load_be(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    static void store_be(uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
    }
    uint32_t F(uint32_t x) const {
        return ((S_[0][(x >> 24) & 0xFF] + S_[1][(x >> 16) & 0xFF]) ^
                S_[2][(x >> 8) & 0xFF]) + S_[3][x & 0xFF];
    }
    void encrypt(uint32_t& L, uint32_t& R) const {
        for (int i = 0; i < 16; ++i) { L ^= P_[i]; R ^= F(L); std::swap(L, R); }
        std::swap(L, R);
        R ^= P_[16]; L ^= P_[17];
    }
    void decrypt(uint32_t& L, uint32_t& R) const {
        for (int i = 17; i > 1; --i) { L ^= P_[i]; R ^= F(L); std::swap(L, R); }
        std::swap(L, R);
        R ^= P_[1]; L ^= P_[0];
    }
};

// ---------------------------------------------------------------- packet table

struct PacketTable {
    std::map<uint16_t, int>         size;   // 0 == variable
    std::map<uint16_t, std::string> name;

    bool load(const char* csv_path) {
        std::ifstream f(csv_path);
        if (!f) return false;
        std::string line;
        std::getline(f, line);                       // header row
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::string dec, hex, nm, sz;
            if (!std::getline(ss, dec, ',')) continue;
            std::getline(ss, hex, ','); std::getline(ss, nm, ','); std::getline(ss, sz, ',');
            if (dec.empty() || sz.empty()) continue;
            uint16_t op = uint16_t(std::stoul(dec));
            size[op] = std::stoi(sz);
            name[op] = nm;
        }
        return !size.empty();
    }

    const char* name_of(uint16_t op) const {
        auto it = name.find(op);
        return it == name.end() ? "UNKNOWN" : it->second.c_str();
    }

    // Returns total packet length, 0 if unknown. `variable` reports whether
    // the length came from +0x0A rather than the table.
    int packet_size(const uint8_t* p, size_t avail, bool* variable) const {
        *variable = false;
        if (avail < 10) return 0;
        uint16_t op; std::memcpy(&op, p, 2);
        auto it = size.find(op);
        if (it == size.end()) return 0;
        if (it->second > 0) return it->second;
        *variable = true;
        if (avail < 12) return 0;
        uint16_t len; std::memcpy(&len, p + 10, 2);
        return len;
    }
};

static uint32_t tos_checksum(const uint8_t* p, size_t len) {
    uint32_t s = 0;
    for (size_t j = 0; j < len; ++j)
        s = (j & 1) == 0 ? (uint32_t(p[j]) ^ s) : (s + p[j]);
    return s;
}

// ---------------------------------------------------------------- writer

class Writer {
public:
    bool open(const char* path, const char* region) {
        f_.open(path, std::ios::binary);
        if (!f_) return false;
        FileHeader h{};
        std::memcpy(h.magic, "TOSCAP\0\0", 8);
        h.version = 1;
        h.header_size = sizeof(RecordHeader);
        h.epoch_us = now_us();
        std::strncpy(h.region, region, sizeof(h.region) - 1);
        f_.write(reinterpret_cast<char*>(&h), sizeof(h));
        f_.flush();
        return true;
    }

    void write(uint16_t dir, uint16_t port, uint32_t conn,
               const uint8_t* pkt, uint32_t len, int declared, bool variable,
               const PacketTable& tbl) {
        RecordHeader r{};
        r.magic = REC_MAGIC;
        r.time_us = now_us();
        r.direction = dir;
        r.port = port;
        r.conn_id = conn;
        std::memcpy(&r.opcode, pkt, 2);
        std::memcpy(&r.sequence, pkt + 2, 4);
        std::memcpy(&r.checksum, pkt + 6, 4);
        r.declared_size = uint16_t(declared);
        r.variable = variable ? 1 : 0;
        r.body_len = len;

        if (declared > 0 && uint32_t(declared) <= len) {
            std::vector<uint8_t> v(pkt, pkt + declared);
            std::memset(v.data() + 6, 0, 4);
            r.checksum_ok = (tos_checksum(v.data(), v.size()) == r.checksum) ? 1 : 0;
        } else {
            r.checksum_ok = 2;
        }

        std::lock_guard<std::mutex> lk(m_);
        f_.write(reinterpret_cast<char*>(&r), sizeof(r));
        f_.write(reinterpret_cast<const char*>(pkt), len);
        f_.flush();

        auto key = std::make_pair(dir, r.opcode);
        if (seen_.insert(key).second) {
            std::printf("%s %-32s op=%-5u size=%-5d %s\n",
                        dir ? "s2c" : "c2s", tbl.name_of(r.opcode),
                        r.opcode, declared,
                        r.checksum_ok == 1 ? "chk=ok" :
                        r.checksum_ok == 0 ? "chk=BAD" : "");
            std::fflush(stdout);
        }
    }

    size_t distinct() const { return seen_.size(); }

private:
    static uint64_t now_us() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return (t - 116444736000000000ULL) / 10;     // FILETIME -> UNIX us
    }
    std::ofstream f_;
    std::mutex m_;
    std::set<std::pair<uint16_t, uint16_t>> seen_;
};

// ---------------------------------------------------------------- proxy

static Blowfish    g_bf;
static PacketTable g_tbl;
static Writer      g_out;
static std::atomic<uint32_t> g_conn_id{0};

// Client -> server: u16 padded length, then that many encrypted bytes.
static void split_c2s(std::vector<uint8_t>& buf, uint16_t port, uint32_t conn) {
    while (buf.size() >= 2) {
        uint16_t padded; std::memcpy(&padded, buf.data(), 2);
        if (padded == 0 || (padded % 8) || padded > 0x8000) {
            buf.erase(buf.begin(), buf.begin() + 2);      // resync
            continue;
        }
        if (buf.size() < size_t(2) + padded) return;
        std::vector<uint8_t> plain(buf.begin() + 2, buf.begin() + 2 + padded);
        g_bf.decrypt_buffer(plain.data(), plain.size());
        buf.erase(buf.begin(), buf.begin() + 2 + padded);

        bool var = false;
        int sz = g_tbl.packet_size(plain.data(), plain.size(), &var);
        uint32_t body = (sz > 0 && uint32_t(sz) <= plain.size())
                            ? uint32_t(sz) : uint32_t(plain.size());
        g_out.write(0, port, conn, plain.data(), body, sz, var, g_tbl);
    }
}

// ------------------------------------------------------------ zone redirect
//
// The barrack hands the client the real zone address and the client dials it
// directly, so a purely passive proxy never sees zone traffic. We rewrite
// those addresses to 127.0.0.1:<relay> and stand up a relay per destination.
//
//   BC_START_GAMEOK (27, 37B)  ip bytes @+0x0E, u32 port @+0x12
//   BC_SERVER_ENTRY (74, 22B)  2 x ip bytes @+0x0A step 4, u16 ports @+0x12
//
// (Process case 74 loops twice reading 4 IP bytes from a3+10 and a u16 port
// from a3+18, formatting "%d.%d.%d.%d" -- hence the byte-order handling.)
//
// Server->client packets carry checksum 0, so rewriting needs no fixup.

static void listen_port(uint16_t port, std::string host, uint16_t rport);

static std::mutex g_relay_m;
static std::map<std::string, uint16_t> g_relays;   // "ip:port" -> local port
static uint16_t g_next_relay = 17001;

static uint16_t ensure_relay(const std::string& ip, uint16_t rport) {
    char key[64];
    std::snprintf(key, sizeof(key), "%s:%u", ip.c_str(), rport);
    std::lock_guard<std::mutex> lk(g_relay_m);
    auto it = g_relays.find(key);
    if (it != g_relays.end()) return it->second;
    uint16_t local = g_next_relay++;
    g_relays[key] = local;
    std::printf("[>] zone relay %u -> %s:%u\n", local, ip.c_str(), rport);
    std::thread(listen_port, local, ip, rport).detach();
    Sleep(120);                         // let the listener bind before we advertise it
    return local;
}

static void rewrite_addr(uint8_t* ipb, uint16_t realport, uint16_t* portslot,
                         uint32_t* portslot32) {
    char ip[32];
    std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", ipb[0], ipb[1], ipb[2], ipb[3]);
    if (ipb[0] == 0 || ipb[0] == 127) return;            // unset or already ours
    uint16_t local = ensure_relay(ip, realport);
    ipb[0] = 127; ipb[1] = 0; ipb[2] = 0; ipb[3] = 1;
    if (portslot)   *portslot = local;
    if (portslot32) *portslot32 = local;
}

// Returns true if the packet was modified.
static bool redirect_zone(uint8_t* p, int sz) {
    uint16_t op; std::memcpy(&op, p, 2);
    if (op == 27 && sz >= 37) {                          // BC_START_GAMEOK
        uint32_t port; std::memcpy(&port, p + 0x12, 4);
        rewrite_addr(p + 0x0E, uint16_t(port), nullptr,
                     reinterpret_cast<uint32_t*>(p + 0x12));
        return true;
    }
    if (op == 74 && sz >= 22) {                          // BC_SERVER_ENTRY
        for (int i = 0; i < 2; ++i) {
            uint16_t port;
            std::memcpy(&port, p + 0x12 + i * 2, 2);
            rewrite_addr(p + 0x0A + i * 4, port,
                         reinterpret_cast<uint16_t*>(p + 0x12 + i * 2), nullptr);
        }
        return true;
    }
    return false;
}

// Server -> client: raw packets back to back, length from the table or +0x0A.
// Complete packets are appended to `out` (possibly rewritten) for forwarding;
// a partial tail stays in `buf`. On an unparseable opcode we fall back to raw
// passthrough for the rest of the connection rather than dropping bytes.
static void split_s2c(std::vector<uint8_t>& buf, uint16_t port, uint32_t conn,
                      std::vector<uint8_t>& out, bool* passthrough) {
    if (*passthrough) {
        out.insert(out.end(), buf.begin(), buf.end());
        buf.clear();
        return;
    }
    while (buf.size() >= 10) {
        bool var = false;
        int sz = g_tbl.packet_size(buf.data(), buf.size(), &var);
        if (sz < 10 || sz > 0x8000) {
            uint16_t op; std::memcpy(&op, buf.data(), 2);
            std::printf("[!] s2c unparseable op=%u size=%d -- passthrough\n", op, sz);
            *passthrough = true;
            out.insert(out.end(), buf.begin(), buf.end());
            buf.clear();
            return;
        }
        if (buf.size() < size_t(sz)) return;
        g_out.write(1, port, conn, buf.data(), uint32_t(sz), sz, var, g_tbl);
        redirect_zone(buf.data(), sz);                   // rewrite before forward
        out.insert(out.end(), buf.begin(), buf.begin() + sz);
        buf.erase(buf.begin(), buf.begin() + sz);
    }
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

static void pump(SOCKET src, SOCKET dst, uint16_t dir, uint16_t port,
                 uint32_t conn) {
    std::vector<uint8_t> buf, out;
    std::vector<uint8_t> chunk(65536);
    bool passthrough = false;
    for (;;) {
        int n = recv(src, reinterpret_cast<char*>(chunk.data()),
                     int(chunk.size()), 0);
        if (n <= 0) break;
        if (dir == 0) {
            // client -> server: nothing to rewrite, relay immediately.
            if (!send_all(dst, chunk.data(), size_t(n))) break;
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
            split_c2s(buf, port, conn);
        } else {
            // server -> client: must decode before forwarding so zone
            // addresses can be redirected. Only whole packets go out; a
            // partial tail waits for the rest.
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
            out.clear();
            split_s2c(buf, port, conn, out, &passthrough);
            if (!out.empty() && !send_all(dst, out.data(), out.size())) break;
        }
    }
    shutdown(src, SD_BOTH);
    shutdown(dst, SD_BOTH);
}

static void serve(SOCKET client, uint16_t port, const char* host, uint16_t rport) {
    uint32_t conn = ++g_conn_id;
    SOCKET up = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(rport);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(up, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        std::printf("[!] upstream %s:%u connect failed\n", host, rport);
        closesocket(client);
        return;
    }
    std::printf("[+] conn %u  local:%u -> %s:%u\n", conn, port, host, rport);
    std::fflush(stdout);
    std::thread a(pump, client, up, uint16_t(0), port, conn);
    std::thread b(pump, up, client, uint16_t(1), port, conn);
    a.join(); b.join();
    closesocket(client); closesocket(up);
    std::printf("[-] conn %u closed\n", conn);
}

static void listen_port(uint16_t port, std::string host, uint16_t rport) {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));
    sockaddr_in sa{}; sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY; sa.sin_port = htons(port);
    if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        std::printf("[!] bind %u failed\n", port);
        return;
    }
    listen(srv, 8);
    std::printf("[*] listening %u -> %s:%u\n", port, host.c_str(), rport);
    for (;;) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread(serve, c, port, host.c_str(), rport).detach();
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: survive a crash
    std::string region = argc > 1 ? argv[1] : "asia";
    std::printf("[.] start region=%s\n", region.c_str());
    struct Up { uint16_t local; const char* host; uint16_t port; };
    std::vector<Up> ups;
    if (region == "na")
        ups = {{7001, "52.5.58.238", 7001}, {7002, "52.4.126.228", 7002}};
    else if (region == "asia")
        ups = {{7001, "54.180.190.119", 7001}, {7002, "54.180.208.135", 7002}};
    else { std::printf("usage: tos_capture [asia|na]\n"); return 1; }

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    std::printf("[.] loading blowfish table\n");
    if (!g_bf.load("bf_inittable.bin", "hsunffalqyrqewes")) {
        std::printf("[!] bf_inittable.bin missing or short\n"); return 1;
    }
    std::printf("[.] loading packet table\n");
    if (!g_tbl.load("packet_opcodes.csv")) {
        std::printf("[!] packet_opcodes.csv missing\n"); return 1;
    }
    char path[128];
    std::snprintf(path, sizeof(path), "capture_%llu.bin",
                  (unsigned long long)time(nullptr));
    std::printf("[.] opening %s\n", path);
    if (!g_out.open(path, region.c_str())) {
        std::printf("[!] cannot open %s\n", path); return 1;
    }
    std::printf("[*] region=%s  opcodes=%u  capture=%s\n",
                region.c_str(), unsigned(g_tbl.size.size()), path);

    std::vector<std::thread> ts;
    for (auto& u : ups) ts.emplace_back(listen_port, u.local, u.host, u.port);
    for (auto& t : ts) t.join();
    WSACleanup();
    return 0;
}
