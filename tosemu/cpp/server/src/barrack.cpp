// Barrack link: login, character list, character creation, entering the game.
//
// Replaces the replay of a captured login burst. The old path worked only for
// the one account and one character the capture happened to contain; building
// the packets means any account name logs in and characters can be created and
// deleted for real.
#include <cstdio>

#include "op_gen.h"
#include "server.h"
#include "zone_send.h"

namespace tos {
namespace {

std::string make_session_key(const Account& acc) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "%s-%lld-%u", acc.name.c_str(),
                  static_cast<long long>(acc.id), uint32_t(now_sec() * 1000));
    return buf;
}

std::shared_ptr<game::Character> make_character(ServerContext& ctx,
                                                Account& acc,
                                                const std::string& name,
                                                int32_t job_id, uint8_t gender,
                                                uint8_t hair, uint8_t slot) {
    auto c = std::make_shared<game::Character>(&ctx.gamedata.properties);
    c->handle = ctx.world->handles().character();
    c->name = name;
    c->team_name = acc.team_name;
    c->account_id = acc.id;
    c->object_id = ctx.accounts.next_object_id();
    c->social_user_id = c->object_id;
    c->job_id = job_id;
    c->gender = gender;
    c->hair = hair;
    c->slot = slot;
    c->level = 1;
    c->map_id = ctx.config.start_map;
    c->equip = game::Character::default_equip();

    // Start where the client's own map table says the map starts, so the
    // character is on the ground rather than under it.
    if (const data::MapData* md = ctx.gamedata.map(c->map_id))
        c->position = {md->x, md->y, md->z};

    c->apply_base_stats(ctx.gamedata);

    // Starting skills come from the job's own tree in skilltree.ies, so a
    // Swordsman gets Thrust and Bash rather than an invented list.
    int64_t next = ctx.accounts.next_object_id();
    int learned = c->learn_job_skills(ctx.gamedata, next);

    // Starting gear. The weapon is chosen by the job's WEAPON CLASS rather
    // than by a hard-coded item id or class name: item_equip.ies tags every
    // piece with a ClassType, so asking for the lowest-requirement "Sword"
    // gets a sensible starter from whatever build is installed.
    static const struct { int32_t job; const char* weapon_class; } kStarters[] = {
        {1001, "Sword"},             // Swordsman
        {2001, "Staff"},             // Wizard
        {3001, "Bow"},               // Archer
        {4001, "Mace"},              // Cleric
        {5001, "Sword"},             // Scout
    };
    const char* weapon_class = "Sword";
    for (const auto& k : kStarters)
        if (k.job == job_id) weapon_class = k.weapon_class;

    int given = 0;
    if (const data::ItemData* d = ctx.gamedata.lowest_of_class_type(weapon_class)) {
        if (game::Item* it = c->add_item(ctx.gamedata, d->id, 1, next)) {
            int32_t dropped = 0;
            c->equip_item(it->object_id, game::EquipSlot::RightHand, dropped);
            ++given;
        }
    }
    // Drug_HP1 / Drug_SP1 are the client's own basic potions.
    for (const char* consumable : {"Drug_HP1", "Drug_SP1"})
        if (const data::ItemData* d = ctx.gamedata.item(consumable))
            if (c->add_item(ctx.gamedata, d->id, 10, next)) ++given;

    // Silver is an inventory item, not a character field -- the client reads
    // the wallet off the stack of item 900011.
    if (const data::ItemData* d = ctx.gamedata.item("Vis")) {
        if (c->add_item(ctx.gamedata, d->id, 1000, next)) ++given;
        c->silver = 1000;
    }

    c->rebuild_equip_ids();
    c->update_stance(ctx.gamedata);
    log_info("game", "new character " + name + ": " + std::to_string(learned) +
                         " skills, " + std::to_string(given) + " starting items, stance " +
                         std::to_string(c->stance));
    return c;
}

// The client's own rules, mirrored so its dialog and our answer agree:
// 2-16 characters and no whitespace.
constexpr int32_t kTeamNameExists = -1;
constexpr int32_t kTeamNameOk = 0;
constexpr int32_t kTeamNameInvalid = 1;

int32_t team_name_result(ServerContext& ctx, const std::string& name) {
    if (name.size() < 2 || name.size() > 16) return kTeamNameInvalid;
    for (char ch : name)
        if ((unsigned char)ch <= ' ') return kTeamNameInvalid;
    if (ctx.accounts.team_name_taken(name)) return kTeamNameExists;
    return kTeamNameOk;
}

void send_character_list(Session& s) {
    if (!s.account) return;
    send::BC_COMMANDER_LIST(s, *s.account);
    for (const auto& c : s.account->characters) send::BC_COMMANDER_CREATE(s, *c);
}

}  // namespace

// ---- accounts ----------------------------------------------------------

std::shared_ptr<game::Character> Account::character(int64_t object_id) const {
    for (const auto& c : characters)
        if (c->object_id == object_id) return c;
    return nullptr;
}

std::shared_ptr<game::Character> Account::character_in_slot(uint8_t slot) const {
    for (const auto& c : characters)
        if (c->slot == slot) return c;
    return nullptr;
}

uint8_t Account::free_slot() const {
    // 1-based: the client uses the index to address a character in
    // CB_START_GAME, and 0 there means "none selected".
    for (uint8_t i = 1; i < 32; ++i)
        if (!character_in_slot(i)) return i;
    return 1;
}

std::shared_ptr<Account> AccountStore::get_or_create(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_name_.find(name);
    if (it != by_name_.end()) return it->second;

    auto acc = std::make_shared<Account>();
    acc->id = next_id_++;
    acc->name = name;
    // Left empty on purpose: the client opens its team-name dialog when the
    // account has none, which is the only way to get that prompt.
    acc->team_name.clear();
    by_name_.emplace(name, acc);
    by_id_.emplace(acc->id, acc);
    return acc;
}

std::shared_ptr<Account> AccountStore::find(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

std::shared_ptr<Account> AccountStore::find(int64_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second;
}

bool AccountStore::team_name_taken(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : by_name_)
        if (kv.second->team_name == name) return true;
    return false;
}

// ---- handlers ----------------------------------------------------------

void handle_barrack(Session& s, PacketReader& r) {
    ServerContext& ctx = *s.ctx;

    switch (r.op()) {
        case op::CB_LOGIN:
        case op::CB_LOGIN_BY_PASSPORT: {
            bool passport = (r.op() == op::CB_LOGIN_BY_PASSPORT);
            std::string account, nation;
            if (!passport) {
                account = r.str(56);
                r.bin(16);                          // password hash
                r.u8(); r.u8(); r.u8();
                r.u32v();                           // client ip
                r.bin(405);
                nation = r.str(64);
            }

            // Always say what was parsed. A login that goes nowhere is the
            // single most confusing failure this server can have, and the
            // account name and service nation are the two fields that decide
            // whether the rest of the packet was read at the right offsets.
            s.log("CB_LOGIN" + std::string(passport ? "_BY_PASSPORT" : "") +
                  " account='" + account + "' nation='" + nation + "'");

            send::BC_LOGIN_PACKET_RECEIVED(s);

            // A client started without launcher credentials sends an empty
            // account name, and passport login carries no name at all -- it
            // authenticates against a token service we are not. Neither is a
            // reason to refuse: there is nothing to authenticate against here,
            // so both land on one well-known account whose characters persist
            // for the server's lifetime.
            if (account.empty()) {
                account = "player";
                s.log("no account name in the login packet; using '" + account +
                      "'");
            }

            // The TAIWAN service nation lays the login body out differently, so
            // everything above would be garbage. Log it and continue on the
            // default account rather than dropping the connection.
            if (nation == "TAIWAN") {
                s.log("service nation TAIWAN: login fields are laid out "
                      "differently and were not parsed");
                account = "player";
            }

            // No credential store to check against, so the first login for a
            // name creates it. See ServerContext's note on persistence.
            s.account = ctx.accounts.get_or_create(account);
            s.account->session_key = make_session_key(*s.account);
            s.logged_in = true;
            s.log("login as '" + account + "' (account " +
                  std::to_string(s.account->id) + ", " +
                  std::to_string(s.account->characters.size()) + " characters)");

            // The character list is NOT sent here. The client asks for it with
            // CB_START_BARRACK once it has finished loading the barrack scene;
            // sending it early means it arrives before anything can display it.
            send::BC_DISCONNECT_PACKET_LOG_COUNT(s);
            send::BC_LOGINOK(s, *s.account);
            return;
        }

        // The client asks for the channel list on its own whenever the lodge
        // refreshes. Silently ignoring this leaves the dropdown empty.
        case op::CB_REQ_CHANNEL_TRAFFIC: {
            if (!s.account) return;
            send::BC_NORMAL_ZoneTraffic(s, *s.account);
            return;
        }

        case op::CB_START_BARRACK: {
            if (!s.account) { s.log("CB_START_BARRACK before login"); return; }
            send::BC_IES_MODIFY_LIST(s);
            send::BC_SERVER_ENTRY(s);
            send_character_list(s);
            send::BC_NORMAL_CharacterInfo(s, *s.account);
            send::BC_NORMAL_TeamUI(s, *s.account);
            send::BC_NORMAL_ZoneTraffic(s, *s.account);
            return;
        }

        // The team name. An account with none set makes the client open its
        // naming dialog, which is where these two come from: CHECK asks whether
        // a name is free, CHANGE commits it.
        case op::CB_BARRACKNAME_CHECK: {
            if (!s.account) return;
            r.u16v();                               // server id
            std::string name = r.str(64);
            std::string message = r.str(256);
            send::BC_BARRACKNAME_CHECK_RESULT(s, team_name_result(ctx, name),
                                              name, message);
            return;
        }

        case op::CB_BARRACKNAME_CHANGE: {
            if (!s.account) return;
            std::string name = r.str(64);
            int32_t result = team_name_result(ctx, name);
            if (result == kTeamNameOk) {
                s.account->team_name = name;
                for (const auto& c : s.account->characters) c->team_name = name;
                s.log("team name set to '" + name + "'");
            }
            send::BC_BARRACKNAME_CHANGE(s, result, name);
            if (result == kTeamNameOk) send::BC_NORMAL_TeamUI(s, *s.account);
            return;
        }

        case op::CB_COMMANDER_CREATE: {
            if (!s.account) { s.log("CB_COMMANDER_CREATE before login"); return; }
            // Fields tile the declared 117 bytes exactly from the client body
            // start (+0x16), which is how this layout was confirmed.
            uint8_t slot = r.u8();
            std::string name = r.str(65);
            int32_t job_id = int32_t(r.u16v());
            uint8_t gender = r.u8();
            float bx = r.f32(), by = r.f32(), bz = r.f32();
            r.i32v();                               // lodge
            r.i32v();                               // start map preset
            uint8_t hair = uint8_t(r.u16v());
            uint32_t skin = r.u32v();

            if (name.empty()) name = "Nameless";
            if (job_id <= 0) job_id = 1001;
            if (gender != 1 && gender != 2) gender = 1;
            // 0 and 0xFF both mean "pick one" -- the client sends 0xFF when
            // the player has not dragged the new character to a bed.
            if (slot == 0 || slot == 0xFF) slot = s.account->free_slot();

            auto c = make_character(ctx, *s.account, name, job_id, gender, hair,
                                    slot);
            if (skin) c->skin_color = skin;
            c->barrack_position = {bx, by, bz};
            s.account->characters.push_back(c);
            s.log("created '" + name + "' job=" + std::to_string(job_id) +
                  " slot=" + std::to_string(slot) + " skills=" +
                  std::to_string(c->skills.size()));

            // Exactly two packets, in this order. Re-sending the whole
            // character list here instead makes the client see the new
            // character twice and stall in the creation screen; it asks for
            // the list, and for the channel table, on its own.
            send::BC_COMMANDER_CREATE_SLOTID(
                s, uint8_t(s.account->characters.size()));
            send::BC_COMMANDER_CREATE(s, *c);
            return;
        }

        case op::CB_COMMANDER_DESTROY: {
            if (!s.account) { s.log("CB_COMMANDER_DESTROY before login"); return; }
            int64_t object_id = int64_t(r.u64v());
            auto& v = s.account->characters;
            uint8_t index = 0;
            for (auto it = v.begin(); it != v.end(); ++it) {
                if ((*it)->object_id != object_id) continue;
                index = (*it)->slot;
                s.log("deleted character '" + (*it)->name + "'");
                v.erase(it);
                break;
            }
            send::BC_COMMANDER_DESTROY(s, index);
            send_character_list(s);
            return;
        }

        case op::CB_START_GAME: {
            if (!s.account) { s.log("CB_START_GAME before login"); return; }
            r.u16v();                               // channel
            uint8_t index = r.u8();
            // The client sends the character's list index, not its slot.
            auto& v = s.account->characters;
            std::shared_ptr<game::Character> c;
            if (index >= 1 && size_t(index) <= v.size()) c = v[index - 1];
            if (!c) c = s.account->character_in_slot(index);
            if (!c && !v.empty()) c = v.front();
            if (!c) {
                send::BC_MESSAGE(s, "No character in that slot.");
                return;
            }
            s.character = c;
            s.log("entering game as '" + c->name + "' on map " +
                  std::to_string(c->map_id));
            send::BC_START_GAMEOK(s, *c);
            return;
        }

        case op::CB_LOGOUT:
            s.log("logout");
            s.close();
            return;

        // Client bookkeeping the barrack sends constantly and does not wait on
        // a reply for: the IPF checksum, the addon list, the language, and the
        // camera position as you drag characters around the lodge. Answered by
        // being accepted.
        // Dragging a character around the lodge. Ignoring this leaves them
        // pinned to their bed -- the client waits for the position to come
        // back before it will let go.
        case op::CB_COMMANDER_MOVE: {
            if (!s.account) return;
            uint8_t index = r.u8();
            float x = r.f32(), y = r.f32(), z = r.f32();
            float cos_v = r.f32(), sin_v = r.f32();
            // 0xFF arrives once during character creation, before the
            // character has an index to move.
            if (index == 0xFF) return;

            auto c = s.account->character_in_slot(index);
            if (!c) {
                auto& v = s.account->characters;
                if (index >= 1 && size_t(index) <= v.size()) c = v[index - 1];
            }
            if (!c) return;

            c->barrack_position = {x, y, z};
            c->barrack_direction = {cos_v, sin_v};
            send::BC_NORMAL_SetPosition(s, index, c->barrack_position);
            return;
        }

        case op::CB_CHECK_CLIENT_INTEGRITY:
        case op::CB_ECHO:
        case op::CB_COMPANION_MOVE:
        case op::CB_NOT_AUTHORIZED_ADDON_LIST:
        case op::CB_SELECTED_LANGUAGE:
        case op::CB_CURRENT_BARRACK:
        case op::CB_SELECT_BARRACK_LAYER:
        case op::CB_CHANGE_BARRACK_TARGET_LAYER:
        case op::CB_OS_INFO:
            return;

        default:
            break;
    }

    uint32_t& seen = s.unhandled[r.op()];
    if (++seen == 1)
        s.log("no barrack handler for " + ctx.table.name_of(r.op()));
}

// The social link is plaintext both ways, and the client opens TWO of them --
// chat and relation, on consecutive ports. It blocks its map load until both
// have answered CS_LOGIN, so "stuck loading world" is the symptom of leaving
// them silent.
void handle_chat(Session& s, PacketReader& r) {
    ServerContext& ctx = *s.ctx;

    switch (r.op()) {
        case op::CS_LOGIN: {
            std::string account = r.str(56);
            r.bin(16);                              // password hash
            r.u8();
            int64_t account_id = int64_t(r.u64v());

            if (!account.empty()) s.account = ctx.accounts.find(account);
            if (!s.account) s.account = ctx.accounts.find(account_id);
            if (!s.account) s.account = ctx.accounts.find("player");
            s.logged_in = true;

            // The two links want different follow-ups, and the port is the
            // only thing that distinguishes them -- unlike the barrack and
            // zone links, they share an opcode range.
            bool relation = (s.port == ctx.config.chat_port + 1);
            s.log(std::string(relation ? "relation" : "chat") +
                  " login for '" + (s.account ? s.account->name : account) + "'");

            send::SC_NORMAL_LoginSuccess(s);
            if (relation) {
                send::SC_NORMAL_LikedList(s);
                send::SC_NORMAL_LikedMeList(s);
                send::SC_LOGIN_OK(s);
            } else {
                send::SC_LOGIN_OK(s);
                send::SC_NORMAL_Unknown02(s);
            }
            return;
        }

        case op::CS_REQ_RELATED_PC_SESSION:
            send::SC_NORMAL_RelatedPcSession(s);
            return;

        // Steady-state social chatter with no reply expected.
        case op::CS_NORMAL_GAME_START:
        case op::CS_REFRESH_GROUP_CHAT:
            return;

        default:
            break;
    }

    uint32_t& seen = s.unhandled[r.op()];
    if (++seen == 1) s.log("no social handler for " + ctx.table.name_of(r.op()));
}

}  // namespace tos
