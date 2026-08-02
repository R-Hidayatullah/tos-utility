// Tree of Savior emulator.
//
//     tosemu_server [options]
//
// One process serves the barrack, zone and chat links. Framing is decided per
// connection, not globally: barrack and zone are Blowfish-framed with a u16
// padded-length prefix, the chat link is plaintext both ways.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "op_gen.h"
#include "server.h"

namespace tos {

static std::atomic<uint32_t> g_next_session{1};

size_t body_start(const Table& t, uint16_t op) {
    // Client packets carry 12 bytes past the 10-byte header that the server
    // never reads, so their body starts at +0x16 rather than +0x0A. That is
    // also why their inline size field sits at +0x16 (docs/07) and it is what
    // makes the declared sizes tile: CZ_CONNECT is 1269 bytes, and its fields
    // only add up from 22.
    // Social packets are the exception: they carry no extra header, so their
    // body starts right after the standard 10 bytes in both directions.
    bool extra_header = t.is_client_side(op) && t.link_of(op) != Link::Social;
    size_t base = extra_header ? 22 : 10;
    return t.is_variable(op) ? base + 2 : base;
}

// ---- session i/o -------------------------------------------------------

void Session::log(const std::string& msg) const {
    log_info(peer, msg);
}

void ServerContext::report_verify(const PacketWriter& w) {
    if (!config.verify_packets) return;

    std::vector<std::string> problems;
    std::string why;
    if (!w.size_ok(why)) problems.push_back(why);
    schema.verify(w, problems);
    if (problems.empty()) return;

    std::lock_guard<std::mutex> lk(verify_mu);
    for (const std::string& p : problems) {
        // Once per distinct problem: a mis-sized packet is usually sent on
        // every tick, and one line is as informative as ten thousand.
        if (std::find(verify_problems.begin(), verify_problems.end(), p) !=
            verify_problems.end())
            continue;
        verify_problems.push_back(p);
        log_warn("layout", p);
    }
}

void Session::send(PacketWriter& w) {
    ctx->report_verify(w);
    Bytes pkt = w.build(seq++);
    ctx->pktlog.write(Dir::S2C, ctx->table, id, pkt.data(), pkt.size());
    log_debug(peer, "SEND " + ctx->table.name_of(w.op()) + " size=" +
                        std::to_string(pkt.size()));
    send_raw(pkt);
}

void Session::send_raw(const Bytes& pkt) {
    if (pkt.empty()) return;
    std::lock_guard<std::mutex> lk(send_mu);
    ::send(SOCKET(sock), reinterpret_cast<const char*>(pkt.data()),
           int(pkt.size()), 0);
}

void Session::close() { ::shutdown(SOCKET(sock), SD_BOTH); }

// ---- connection loop ---------------------------------------------------

namespace {

void dispatch(Session& s, const uint8_t* plain, size_t n) {
    uint16_t op = rd16(plain);
    s.ctx->pktlog.write(Dir::C2S, s.ctx->table, s.id, plain, n);
    log_debug(s.peer, "RECV " + s.ctx->table.name_of(op) + " size=" +
                          std::to_string(n));

    // Route on the opcode, not on the listen port. Which port a client dials
    // for login versus for the zone is a matter of its own configuration --
    // this client sends CB_LOGIN to 7002 -- but the opcode says which link the
    // packet belongs to and cannot be configured wrong.
    Role role = s.role;
    switch (s.ctx->table.link_of(op)) {
        case Link::Barrack: role = Role::Barrack; break;
        case Link::Zone:    role = Role::Zone; break;
        case Link::Social:  role = Role::Chat; break;
        case Link::Unknown: break;
    }
    if (role != s.role) {
        s.log(std::string("link=") +
              (role == Role::Barrack ? "barrack"
                                     : role == Role::Chat ? "social" : "zone"));
        s.role = role;
    }

    PacketReader r(plain, n, body_start(s.ctx->table, op));
    switch (s.role) {
        case Role::Barrack: handle_barrack(s, r); break;
        case Role::Chat:    handle_chat(s, r); break;
        default:            handle_zone(s, r); break;
    }
}

void serve(SOCKET c, std::string peer, uint16_t port, Role role,
           ServerContext* ctx) {
    auto sp = std::make_shared<Session>();
    Session& s = *sp;
    s.id = g_next_session++;
    s.sock = uintptr_t(c);
    s.peer = std::move(peer);
    s.port = port;
    s.role = role;
    s.ctx = ctx;
    s.world = ctx->world.get();
    ctx->world->add_session(sp);
    s.log("connected");

    Bytes buf;
    std::vector<char> chunk(65536);
    bool running = true;
    while (running) {
        int got = ::recv(c, chunk.data(), int(chunk.size()), 0);
        if (got <= 0) break;
        buf.insert(buf.end(), chunk.begin(), chunk.begin() + got);

        // Barrack and zone links prefix each message with a padded length,
        // always a multiple of 8. The chat link is plaintext, so its leading
        // u16 is an opcode (CS_LOGIN = 15901) and will not be.
        if (s.framing == Framing::Unknown && buf.size() >= 2) {
            uint16_t lead = rd16(buf.data());
            s.framing = (lead && lead % 8 == 0) ? Framing::Encrypted
                                                : Framing::Plain;
            if (s.framing == Framing::Plain) s.role = Role::Chat;
            s.log(std::string("framing=") +
                  (s.framing == Framing::Encrypted ? "encrypted" : "plain"));
        }

        if (s.framing == Framing::Plain) {
            while (buf.size() >= 10) {
                size_t size = ctx->table.packet_size(buf.data(), buf.size());
                if (size < 10 || size > 0x8000) {
                    s.log("plain: cannot size op=" +
                          std::to_string(rd16(buf.data())) + ", dropping");
                    running = false;
                    break;
                }
                if (buf.size() < size) break;
                dispatch(s, buf.data(), size);
                buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(size));
            }
            continue;
        }

        while (buf.size() >= 2) {
            uint16_t padded = rd16(buf.data());
            if (padded == 0 || padded % 8) {
                s.log("bad frame length " + std::to_string(padded) +
                      ", dropping");
                running = false;
                break;
            }
            if (buf.size() < size_t(2 + padded)) break;
            Bytes plain = ctx->blowfish->decrypt(buf.data() + 2, padded);
            if (plain.size() >= 10) dispatch(s, plain.data(), plain.size());
            buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(2 + padded));
        }
    }

    on_disconnect(s);
    ctx->world->remove_session(s.id);
    ::closesocket(c);
    s.log("closed");
}

void listen_on(uint16_t port, Role role, ServerContext* ctx) {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
                 sizeof yes);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a) ||
        ::listen(srv, 16)) {
        log_error("net", "cannot listen on port " + std::to_string(port));
        return;
    }
    log_info("net", "listening on 0.0.0.0:" + std::to_string(port));

    for (;;) {
        sockaddr_in ca{};
        int cl = sizeof ca;
        SOCKET c = ::accept(srv, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c == INVALID_SOCKET) break;
        char ip[64] = {0};
        ::inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
        std::thread(serve, c,
                    std::string(ip) + ":" + std::to_string(ntohs(ca.sin_port)),
                    port, role, ctx)
            .detach();
    }
}

// 2 Hz: enough for wander broadcasts, and cheap enough to leave running.
void tick_loop(ServerContext* ctx) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ctx->world->tick();
    }
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* flag) -> std::string {
            size_t n = std::strlen(flag);
            return a.compare(0, n, flag) == 0 ? a.substr(n) : std::string();
        };
        if (a == "-h" || a == "--help") return false;
        else if (!value("--data=").empty())      cfg.data_root = value("--data=");
        else if (!value("--game=").empty())      cfg.game_root = value("--game=");
        else if (!value("--log-json=").empty())  cfg.json_log = value("--log-json=");
        else if (!value("--zone-port=").empty()) cfg.zone_port = uint16_t(std::stoi(value("--zone-port=")));
        else if (!value("--barrack-port=").empty()) cfg.barrack_port = uint16_t(std::stoi(value("--barrack-port=")));
        else if (!value("--map=").empty())       cfg.start_map = std::stoi(value("--map="));
        else if (a == "--no-spawns")             cfg.spawn_monsters = false;
        else if (a == "--no-verify")             cfg.verify_packets = false;
        else if (a == "--debug")                 log_set_min_level(LogLevel::Debug);
        else if (a[0] != '-')                    cfg.data_root = a;
        else {
            log_error("args", "unknown option " + a);
            return false;
        }
    }
    return true;
}

void usage() {
    std::printf(
        "tosemu_server [options]\n"
        "\n"
        "  --data=DIR          server data (packet_opcodes.csv, properties.txt,\n"
        "                      bf_inittable.bin, packet_schema.json)   [.]\n"
        "  --game=DIR          Tree of Savior install, for the client's own\n"
        "                      .ipf data. Without it maps, monsters and spawns\n"
        "                      are unavailable and the server runs bare.\n"
        "  --barrack-port=N    [2000]\n"
        "  --zone-port=N       [7002]\n"
        "  --map=N             starting map id                        [1001]\n"
        "  --log-json=FILE     write a decoded packet log\n"
        "  --no-spawns         do not populate maps from the spawn tables\n"
        "  --no-verify         skip checking built packets against the\n"
        "                      client-extracted layouts\n"
        "  --debug             log every packet\n");
}

}  // namespace
}  // namespace tos

int main(int argc, char** argv) {
    using namespace tos;

    ServerContext ctx;
    if (!parse_args(argc, argv, ctx.config)) {
        usage();
        return 1;
    }
    const Config& cfg = ctx.config;

    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa)) {
        log_error("net", "WSAStartup failed");
        return 1;
    }

    // ---- wire layer
    try {
        ctx.table.load_csv(cfg.data_root + "/packet_opcodes.csv");
    } catch (const std::exception& e) {
        log_error("data", e.what());
        return 1;
    }
    log_info("data", std::to_string(ctx.table.count()) + " opcodes");

    Bytes init;
    if (!read_file(cfg.data_root + "/bf_inittable.bin", init)) {
        log_error("data", "missing " + cfg.data_root + "/bf_inittable.bin");
        return 1;
    }
    ctx.blowfish = std::make_unique<Blowfish>(init, kDefaultKey());

    if (ctx.schema.load(cfg.data_root + "/packet_schema.json"))
        log_info("data", std::to_string(ctx.schema.count()) +
                             " recovered packet layouts");
    else
        log_warn("data", "no packet_schema.json -- built packets will only be "
                         "checked against their declared size");

    // ---- game data
    if (!cfg.game_root.empty()) {
        if (ctx.gamedata.open_client(cfg.game_root)) {
            log_info("data", "client: " +
                                 std::to_string(ctx.gamedata.fs()->archive_count()) +
                                 " archives, " +
                                 std::to_string(ctx.gamedata.fs()->unique_count()) +
                                 " files");
            ctx.gamedata.load_tables();
            log_info("data", std::to_string(ctx.gamedata.maps().size()) +
                                 " maps, " +
                                 std::to_string(ctx.gamedata.monster_count()) +
                                 " monsters, " +
                                 std::to_string(ctx.gamedata.item_count()) +
                                 " items, " +
                                 std::to_string(ctx.gamedata.skill_count()) +
                                 " skills, " +
                                 std::to_string(ctx.gamedata.job_count()) +
                                 " jobs");
        } else {
            log_warn("data", "cannot read the client at " + cfg.game_root);
        }
    } else {
        log_warn("data", "no --game= given; running without client data");
    }

    if (ctx.gamedata.properties.load(cfg.data_root + "/properties.txt"))
        log_info("data", std::to_string(ctx.gamedata.properties.count()) +
                             " property ids");
    else
        log_warn("data", "no properties.txt -- object properties cannot be "
                         "sent, so stats will not appear");

    ctx.world = std::make_unique<game::World>(ctx.table, ctx.gamedata);

    ctx.pktlog.load(cfg.data_root + "/packet_schema.json");
    if (!cfg.json_log.empty() && ctx.pktlog.open(cfg.json_log))
        log_info("log", "packet log -> " + cfg.json_log);

    if (const data::MapData* md = ctx.gamedata.map(cfg.start_map))
        log_info("world", "start map " + std::to_string(cfg.start_map) + " " +
                              md->class_name);

    log_info("chat", "commands: /where /map /spawn /who /heal /level "
                     "/skills /items /give");

    // The role passed here is only a starting guess -- dispatch() re-routes
    // every packet by its opcode, so a client that dials its login port for
    // the barrack and the same port for the zone works either way.
    std::thread(tick_loop, &ctx).detach();
    std::thread(listen_on, cfg.barrack_port, Role::Barrack, &ctx).detach();
    std::thread(listen_on, cfg.chat_port, Role::Chat, &ctx).detach();
    std::thread(listen_on, uint16_t(cfg.chat_port + 1), Role::Chat, &ctx).detach();
    listen_on(cfg.zone_port, Role::Zone, &ctx);

    ::WSACleanup();
    return 0;
}
