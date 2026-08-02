// Connections, accounts and the shared server context.
//
// One process serves all three links the client opens, distinguished by the
// port it arrived on and by how the first bytes are framed:
//
//   barrack  Blowfish-framed, u16 padded length prefix
//   zone     Blowfish-framed, same
//   chat     plaintext both ways, the leading u16 is the opcode
//
// Accounts and characters live in memory only. That is a deliberate limit, not
// an oversight: it keeps the whole server startable from a game install with
// no other setup, and every packet path is exercised the same way it would be
// with storage behind it.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "actor.h"
#include "blowfish.h"
#include "gamedata.h"
#include "packet.h"
#include "pktlog.h"
#include "schema.h"
#include "world.h"

namespace tos {

// ---- accounts ----------------------------------------------------------

struct Account {
    int64_t id = 0;
    std::string name;
    std::string team_name;
    std::string session_key;
    std::vector<std::shared_ptr<game::Character>> characters;

    std::shared_ptr<game::Character> character(int64_t object_id) const;
    std::shared_ptr<game::Character> character_in_slot(uint8_t slot) const;
    uint8_t free_slot() const;
};

// First login creates the account. There is nothing to authenticate against
// and no way to persist a registration, so rejecting unknown names would only
// make the server unusable.
class AccountStore {
public:
    std::shared_ptr<Account> get_or_create(const std::string& name);
    std::shared_ptr<Account> find(const std::string& name) const;
    std::shared_ptr<Account> find(int64_t id) const;
    bool team_name_taken(const std::string& name) const;
    int64_t next_object_id() { return next_object_id_++; }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Account>> by_name_;
    std::unordered_map<int64_t, std::shared_ptr<Account>> by_id_;
    int64_t next_id_ = 1;
    int64_t next_object_id_ = 0x0000004000000000LL;
};

// ---- configuration -----------------------------------------------------

struct Config {
    std::string data_root = ".";
    std::string game_root;             // Tree of Savior install, for .ipf data
    std::string json_log;

    uint32_t server_ip = 0x0100007F;   // 127.0.0.1, byte order as sent
    uint16_t barrack_port = 2000;
    uint16_t zone_port = 7002;
    uint16_t chat_port = 9001;

    // Klaipeda. Its spawn table has NPCs right where the player lands; the
    // field maps put you in an empty corner -- f_siauliai_west has exactly one
    // anchor within 300 units of its default entry point, and that one has no
    // npc id. An empty world looks like broken spawning, so the default is
    // somewhere populated.
    int32_t start_map = 1001;
    bool spawn_monsters = true;
    bool verify_packets = true;
};

// ---- shared context ----------------------------------------------------

struct ServerContext {
    Config config;
    Table table;
    PacketSchema schema;
    data::GameData gamedata;
    AccountStore accounts;
    std::unique_ptr<Blowfish> blowfish;
    std::unique_ptr<game::World> world;
    PacketLog pktlog;

    // Counts of layout problems found while verifying built packets, so the
    // startup summary can say plainly whether anything disagrees with the
    // client's own definitions.
    std::mutex verify_mu;
    std::vector<std::string> verify_problems;
    void report_verify(const PacketWriter& w);
};

// ---- sessions ----------------------------------------------------------

enum class Framing { Unknown, Encrypted, Plain };
enum class Role { Barrack, Zone, Chat };

struct Session : std::enable_shared_from_this<Session> {
    uint32_t id = 0;
    uintptr_t sock = 0;                // SOCKET, kept opaque so this header
                                       // stays free of <winsock2.h>
    std::string peer;
    uint16_t port = 0;
    Role role = Role::Zone;
    Framing framing = Framing::Unknown;

    ServerContext* ctx = nullptr;
    game::World* world = nullptr;

    std::shared_ptr<Account> account;
    std::shared_ptr<game::Character> character;

    bool logged_in = false;
    bool game_ready = false;           // CZ_GAME_READY answered
    bool in_world = false;             // visible to, and seeing, other players
    uint32_t seq = 0;

    std::unordered_map<uint16_t, uint32_t> unhandled;
    std::mutex send_mu;                // the tick thread and this connection's
                                       // own thread both write to the socket

    void log(const std::string& msg) const;
    // Finishes the packet (inline size, sequence, checksum), verifies it
    // against the client's layout, and writes it.
    void send(PacketWriter& w);
    void send_raw(const Bytes& pkt);
    void close();

    game::Map* map() const {
        return character ? character->map : nullptr;
    }
};

// Dispatch, split by which link the packet arrived on.
void handle_barrack(Session& s, PacketReader& r);
void handle_zone(Session& s, PacketReader& r);
void handle_chat(Session& s, PacketReader& r);

void on_disconnect(Session& s);

// Where a packet's body starts: past the 10-byte header, plus the inline size
// field that variable-size packets carry.
size_t body_start(const Table& t, uint16_t op);

}  // namespace tos
