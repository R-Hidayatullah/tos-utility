// Zone link: world entry, movement, chat, combat.
//
// The load-bearing fact throughout is that movement is server-authoritative
// for the player's own character (docs/01): the client sends its intent and
// does NOT move the actor until the zone echoes the move back on that handle.
// Every movement handler therefore echoes to the sender as well as
// broadcasting, and dropping the echo looks exactly like a client-side input
// lock.
#include <cmath>
#include <cstdlib>
#include <string>

#include "op_gen.h"
#include "server.h"
#include "zone_send.h"

namespace tos {
namespace {

// Both jump packets in the capture carried this, and the client uses it for
// the arc rather than deriving one.
constexpr float kJumpPower = 350.0f;

// Becoming visible. The obvious trigger is CZ_LOAD_COMPLETE, but the live
// capture never contains one on the zone link -- so gating visibility on it
// alone leaves the player invisible to everyone, and everyone invisible to
// them. Any traffic after the world is loaded works just as well, so the first
// packet that can only come from a loaded client triggers it too.
void enter_world(Session& s);

// Show this player everyone already on the map, and everyone else this player.
void exchange_presence(Session& s) {
    game::Map* map = s.map();
    if (!map || !s.character) return;

    int seen = 0;
    for (const auto& a : map->actors()) {
        if (a->handle == s.character->handle) continue;
        if (a->is_character()) {
            s.send_raw(send::make_ZC_ENTER_PC(
                *s.ctx, static_cast<const game::Character&>(*a)));
        } else {
            s.send_raw(send::make_ZC_ENTER_MONSTER(
                *s.ctx, static_cast<const game::Monster&>(*a)));
        }
        ++seen;
    }

    map->broadcast(send::make_ZC_ENTER_PC(*s.ctx, *s.character),
                   s.character->handle);
    s.log("presence: " + std::to_string(seen) + " actors on " +
          map->class_name());
}

void enter_world(Session& s) {
    if (!s.character || !s.game_ready || s.in_world) return;
    s.in_world = true;
    exchange_presence(s);
}

void enter_map(Session& s, int32_t map_id) {
    game::Map* map = s.world->map(map_id);
    if (s.character->map) s.character->map->remove(s.character->handle);
    s.character->map_id = map_id;
    map->add(s.character);

    if (s.ctx->config.spawn_monsters) {
        int n = map->spawn_from_anchors();
        if (n) s.log("spawned " + std::to_string(n) + " actors from " +
                     map->class_name() + " anchors");
    }
}

// A basic attack: flat damage scaled off the attacker's level, since neither
// weapons nor the stat formulas are modelled yet.
int32_t basic_damage(const game::Actor& attacker) {
    return 8 + attacker.level * 3;
}

void kill(Session& s, game::Actor& target, const game::Actor& killer) {
    game::Map* map = s.map();
    if (!map) return;

    target.hp = 0;
    map->broadcast(send::make_ZC_DEAD(*s.ctx, target, killer.handle));

    if (!target.is_character()) {
        auto& m = static_cast<game::Monster&>(target);
        m.despawned = true;
        // RespawnTime from the map's generator row, in seconds. 20 is only the
        // fallback for a spawn that came from /spawn rather than an anchor.
        m.respawn_at = now_sec() + (m.respawn_delay > 0 ? m.respawn_delay : 20.0);
        m.moving = false;
        map->broadcast(send::make_ZC_LEAVE(*s.ctx, m.handle));
    }
}

// Damage one target. Returns false when the handle names something that
// cannot be attacked, which is common: the client sends a target handle for
// friendlies and client-side actors too.
bool damage_target(Session& s, uint32_t target_handle, int32_t dmg) {
    game::Map* map = s.map();
    if (!map || !s.character) return false;

    auto target = map->get(target_handle);
    if (!target || target->dead() || target->is_character()) return false;

    auto& mon = static_cast<game::Monster&>(*target);
    if (mon.despawned) return false;

    // HP reaches the client as ZC_UPDATE_ALL_STATUS below; the property blob
    // on ZC_ENTER_MONSTER carries only Range and Scale, so writing HP into it
    // would have made a respawned mob's re-entry packet differ from the live
    // one for no gain.
    mon.hp = mon.hp > dmg ? mon.hp - dmg : 0;

    map->broadcast(send::make_ZC_HIT_INFO(*s.ctx, *s.character, mon, dmg));
    map->broadcast(send::make_ZC_UPDATE_ALL_STATUS(*s.ctx, mon));

    if (mon.hp <= 0) kill(s, mon, *s.character);
    return true;
}

// Everything within `radius` of a point, so a skill hits a group the way the
// client's own effect implies it should.
int damage_area(Session& s, const game::Position& at, float radius,
                int32_t dmg) {
    game::Map* map = s.map();
    if (!map) return 0;
    float r2 = radius * radius;
    int hit = 0;
    for (const auto& a : map->actors()) {
        if (a->is_character() || a->dead()) continue;
        if (a->position.dist_sq(at) > r2) continue;
        if (damage_target(s, a->handle, dmg)) ++hit;
    }
    return hit;
}

// Use a learned skill. Falls back to treating an unknown skill id as a basic
// attack rather than refusing: the client sends id 0 for the default attack
// animation, and every job has skills we have no handler for.
void use_skill(Session& s, int32_t skill_id, uint32_t target_handle,
               const game::Position& target_pos) {
    if (!s.character) return;
    game::Map* map = s.map();
    if (!map) return;

    double now = now_sec();
    game::Skill* sk = s.character->find_skill(skill_id);
    if (sk && !sk->ready(now)) {
        // Silently. The client fires attack requests far faster than the
        // cooldown -- holding the button sends several a second against a
        // 1000ms BasicCoolDown -- and it already draws the cooldown itself,
        // so answering each one with a chat line just spams the screen.
        return;
    }

    int32_t dmg = basic_damage(*s.character);
    float radius = 0;
    if (sk) {
        // Skill level scales the damage, and the client's own range column
        // decides whether it lands on one target or a group.
        dmg = dmg * (2 + sk->level) / 2;
        radius = sk->data ? sk->data->range : 0;
        float cd = sk->data ? sk->data->cooldown : 0;
        sk->cooldown_until = now + (cd > 0 ? cd / 1000.0 : 1.0);
    }

    map->broadcast(send::make_ZC_SKILL_MELEE_GROUND(
        *s.ctx, *s.character, skill_id,
        target_pos.x || target_pos.z ? target_pos : s.character->position));

    int hits = 0;
    if (target_handle && damage_target(s, target_handle, dmg)) ++hits;
    if (!hits && radius > 0)
        hits = damage_area(s, target_pos.x || target_pos.z
                                  ? target_pos
                                  : s.character->position,
                           radius, dmg);
    // Nothing named and nothing in range: still show the swing, which is what
    // the client does for an attack into empty air.
}

// ---- chat commands -----------------------------------------------------
//
// Slash commands are addon RPC as far as the live zone is concerned: 21 of the
// captured CZ_CHAT produced only 4 ZC_CHAT, and every echo was plain text. We
// claim a few for driving the emulator and swallow the rest.

bool run_command(Session& s, const std::string& text) {
    auto arg = [&](size_t n) {
        size_t start = 0, count = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == ' ') {
                if (i > start) {
                    if (count == n) return text.substr(start, i - start);
                    ++count;
                }
                start = i + 1;
            }
        }
        return std::string();
    };
    std::string cmd = arg(0);

    if (cmd == "/where") {
        game::Map* m = s.map();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s (%d) at %.0f, %.0f, %.0f",
                      m ? m->class_name().c_str() : "?",
                      s.character ? s.character->map_id : 0,
                      s.character->position.x, s.character->position.y,
                      s.character->position.z);
        send::notify(s, buf);
        return true;
    }

    if (cmd == "/map") {
        std::string dest = arg(1);
        if (dest.empty()) {
            send::notify(s, "usage: /map <id|classname>");
            return true;
        }
        int32_t id = 0;
        if (dest.find_first_not_of("0123456789") == std::string::npos) {
            id = std::atoi(dest.c_str());
        } else if (const data::MapData* md = s.ctx->gamedata.map(dest)) {
            id = md->id;
        }
        if (!id || !s.ctx->gamedata.map(id)) {
            send::notify(s, "unknown map: " + dest);
            return true;
        }
        s.character->map_id = id;
        if (const data::MapData* md = s.ctx->gamedata.map(id))
            s.character->position = {md->x, md->y, md->z};
        s.log("map change -> " + std::to_string(id));
        // The client tears the socket down and reconnects, so state is reset
        // on the way out; ZC_MOVE_ZONE_OK carries the address to dial.
        s.in_world = false;
        send::ZC_MOVE_ZONE(s, 0);
        return true;
    }

    if (cmd == "/spawn") {
        std::string what = arg(1);
        int count = arg(2).empty() ? 5 : std::atoi(arg(2).c_str());
        int32_t id = 0;
        if (!what.empty() &&
            what.find_first_not_of("0123456789") == std::string::npos) {
            id = std::atoi(what.c_str());
        } else if (const data::MonsterData* md = s.ctx->gamedata.monster(what)) {
            id = md->id;
        }
        if (!id) {
            send::notify(s, "usage: /spawn <monster id|classname> [count]");
            return true;
        }
        game::Map* map = s.map();
        if (!map) return true;
        auto spawned = map->spawn(id, s.character->position, 120.0f,
                                  count > 0 && count < 50 ? count : 5);
        for (const auto& m : spawned)
            map->broadcast(send::make_ZC_ENTER_MONSTER(*s.ctx, *m, true));
        send::notify(s, "spawned " + std::to_string(spawned.size()));
        return true;
    }

    if (cmd == "/skills") {
        std::string line = "skills:";
        for (const game::Skill& sk : s.character->skills) {
            line += " " + std::to_string(sk.id);
            if (sk.data && !sk.data->class_name.empty())
                line += "(" + sk.data->class_name + ")";
        }
        if (s.character->skills.empty()) line += " none";
        send::notify(s, line);
        return true;
    }

    if (cmd == "/give") {
        std::string what = arg(1);
        int count = arg(2).empty() ? 1 : std::atoi(arg(2).c_str());
        int32_t id = 0;
        if (!what.empty() &&
            what.find_first_not_of("0123456789") == std::string::npos) {
            id = std::atoi(what.c_str());
        } else if (const data::ItemData* d = s.ctx->gamedata.item(what)) {
            id = d->id;
        }
        if (!id) {
            send::notify(s, "usage: /give <item id|classname> [count]");
            return true;
        }
        int64_t next = s.ctx->accounts.next_object_id();
        game::Item* it = s.character->add_item(s.ctx->gamedata, id,
                                               count > 0 ? count : 1, next);
        if (!it) {
            send::notify(s, "unknown item: " + what);
            return true;
        }
        send::ZC_ITEM_ADD(s, *it, 0);
        send::ZC_OBJECT_PROPERTY(s, *s.character);
        send::notify(s, "gave " + std::to_string(it->amount) + "x " +
                            (it->data ? it->data->class_name : what));
        return true;
    }

    if (cmd == "/items") {
        std::string line = "carrying:";
        for (const game::Item& i : s.character->inventory) {
            line += " " + (i.data ? i.data->class_name : std::to_string(i.id));
            if (i.amount > 1) line += "x" + std::to_string(i.amount);
        }
        if (s.character->inventory.empty()) line += " nothing";
        send::notify(s, line);
        return true;
    }

    if (cmd == "/who") {
        game::Map* map = s.map();
        std::string line = "online here:";
        if (map)
            for (const auto& c : map->characters()) line += " " + c->name;
        send::notify(s, line);
        return true;
    }

    if (cmd == "/heal") {
        s.character->hp = s.character->max_hp;
        s.character->sp = s.character->max_sp;
        s.character->properties.set("HP", float(s.character->hp));
        s.character->properties.set("SP", float(s.character->sp));
        s.send_raw(send::make_ZC_UPDATE_ALL_STATUS(*s.ctx, *s.character));
        send::notify(s, "healed");
        return true;
    }

    if (cmd == "/level") {
        int lv = std::atoi(arg(1).c_str());
        if (lv < 1 || lv > 500) {
            send::notify(s, "usage: /level <1-500>");
            return true;
        }
        s.character->level = lv;
        s.character->apply_base_stats(s.ctx->gamedata);
        send::ZC_OBJECT_PROPERTY(s, *s.character);
        s.send_raw(send::make_ZC_UPDATE_ALL_STATUS(*s.ctx, *s.character));

        // A higher level unlocks more of the job tree; announce anything new
        // so it appears in the skill window without a relog.
        int64_t next = s.ctx->accounts.next_object_id();
        size_t before = s.character->skills.size();
        s.character->learn_job_skills(s.ctx->gamedata, next);
        for (size_t i = before; i < s.character->skills.size(); ++i)
            send::ZC_SKILL_ADD(s, *s.character, s.character->skills[i]);

        send::notify(s, "level " + std::to_string(lv) + ", " +
                            std::to_string(s.character->skills.size()) +
                            " skills");
        return true;
    }

    return cmd.size() > 1 && cmd[0] == '/';   // swallow other addon RPC
}

}  // namespace

void handle_zone(Session& s, PacketReader& r) {
    ServerContext& ctx = *s.ctx;

    switch (r.op()) {
        case op::CZ_CONNECT: {
            r.bin(1024);
            std::string session_key = r.str(64);
            std::string account_name = r.str(56);
            r.str(48);                              // mac
            r.u64v();
            r.u64v();
            int64_t account_id = int64_t(r.u64v());
            int64_t character_id = int64_t(r.u64v());

            s.account = ctx.accounts.find(account_name);
            if (!s.account) s.account = ctx.accounts.find(account_id);
            if (!s.account) {
                s.log("zone login for unknown account '" + account_name + "'");
                s.close();
                return;
            }

            s.character = s.account->character(character_id);
            if (!s.character && !s.account->characters.empty())
                s.character = s.account->characters.front();
            if (!s.character) {
                s.log("zone login with no character");
                s.close();
                return;
            }

            s.character->session = s.shared_from_this();
            s.logged_in = true;
            enter_map(s, s.character->map_id);

            s.log("zone entry: '" + s.character->name + "' handle=" +
                  std::to_string(s.character->handle));

            // Order taken from the live server (capture_1785545696.bin, zone
            // link): ZC_STANCE_CHANGE lands BEFORE ZC_CONNECT_OK. The client
            // builds its own model from that stance, and without it the
            // character comes up in a T-pose and the load can stall outright.
            send::ZC_STANCE_CHANGE(s, *s.character);
            send::ZC_CONNECT_OK(s, *s.character);
            send::ZC_NORMAL_AdventureBook(s);
            send::ZC_SET_CHATBALLOON_SKIN(s);
            send::ZC_NORMAL_Unknown1B4(s);
            return;
        }

        case op::CZ_GAME_READY: {
            if (!s.character) { s.log("CZ_GAME_READY with no character"); return; }
            // Same source as above: the inventory, equipment and skill lists
            // all precede ZC_START_GAME, and ZC_MYPC_ENTER follows it.
            // The client stays in the loading state until it has this whole
            // set. Most are empty lists -- empty is fine, absent is not.
            send::ZC_IES_MODIFY_LIST(s);
            send::ZC_ITEM_INVENTORY_LIST(s, *s.character);
            send::ZC_SESSION_OBJECTS(s);
            send::ZC_OPTION_LIST(s);
            send::ZC_SKILLMAP_LIST(s);
            send::ZC_CHAT_MACRO_LIST(s);
            send::ZC_MAP_REVEAL_LIST(s);
            send::ZC_NPC_STATE_LIST(s);
            send::ZC_HELP_LIST(s);
            send::ZC_MYPAGE_MAP(s);
            send::ZC_GUESTPAGE_MAP(s);
            // Before the equipment and skill lists, not after: the client
            // builds the skill window from the job data in here and ignores
            // skills belonging to a job it has not been told about yet.
            send::ZC_NORMAL_UpdateSkillUI(s, *s.character);
            send::ZC_ITEM_EQUIP_LIST(s, *s.character);
            send::ZC_SKILL_LIST(s, *s.character);
            send::ZC_OBJECT_PROPERTY(s, *s.character);
            send::ZC_START_INFO(s, *s.character);
            send::ZC_LOGIN_TIME(s);
            send::ZC_START_GAME(s);
            send::ZC_MYPC_ENTER(s, *s.character);
            s.game_ready = true;
            return;
        }

        // Sent once a second until answered, and the client stays in the
        // loading state -- refusing all input -- while it waits.
        case op::CZ_LOAD_COMPLETE:
            send::ZC_LOAD_COMPLETE(s);
            enter_world(s);
            return;

        case op::CZ_REQ_QUICKSLOT_LIST:
            send::ZC_QUICK_SLOT_LIST_EMPTY(s);
            return;

        case op::CZ_REQUEST_GUILD_INDEX:
            if (s.character) send::ZC_RESPONSE_GUILD_INDEX(s, *s.character);
            return;

        case op::CZ_REQ_COMMANDER_INFO:
            send::ZC_TRUST_INFO(s);
            return;

        // Client-side bookkeeping that expects no reply: settings being saved
        // one at a time, map exploration progress, addon chatter, and the
        // various "is there an event on" probes.
        case op::CZ_CHANGE_CONFIG:
        case op::CZ_MAP_REVEAL_INFO:
        case op::CZ_CUSTOM_COMMAND:
        case op::CZ_DO_CLIENT_MOVE_CHECK:
        case op::CZ_CAMPINFO:
        case op::CZ_FIXED_NOTICE_SHOW:
        case op::CZ_DISCONNECT_REASON_FOR_LOG:
        case op::CZ_REQ_NORMAL_TX:
        case op::CZ_REQ_FIELD_BOSS_EXIST:
        case op::CZ_RUN_GAMEEXIT_TIMER:
        case op::CZ_MYTHIC_DUNGEON_REQUEST_CURRENT_SEASON:
        case op::CZ_REQUEST_RANK_SYSTEM_TIME_TABLE:
        case op::CZ_REQUEST_DRAW_TOSHERO_EMBLEM:
            return;

        case op::CZ_KEYBOARD_MOVE: {
            if (!s.character) return;
            enter_world(s);
            // Offsets from matched request/response pairs in the capture
            // (docs/02); the client packet carries no handle because the zone
            // knows who is asking.
            r.seek(0x16);
            float x = r.f32(), y = r.f32(), z = r.f32();
            float dx = r.f32(), dz = r.f32();
            r.seek(0x48);
            bool moving = r.u8() != 0;

            s.character->position = {x, y, z};
            s.character->direction = {dx, dz};
            s.character->moving = moving;

            Bytes echo = send::make_ZC_MOVE_DIR(ctx, *s.character,
                                                s.world->server_time());
            s.send_raw(echo);                       // the client waits on this
            if (game::Map* m = s.map()) m->broadcast(echo, s.character->handle);
            return;
        }

        case op::CZ_MOVE_STOP: {
            if (!s.character) return;
            // One more leading pad byte than CZ_KEYBOARD_MOVE, so the position
            // starts at +0x17 rather than +0x16.
            r.seek(0x17);
            float x = r.f32(), y = r.f32(), z = r.f32();
            float dx = r.f32(), dz = r.f32();

            s.character->position = {x, y, z};
            s.character->direction = {dx, dz};
            s.character->moving = false;

            Bytes echo = send::make_ZC_PC_MOVE_STOP(ctx, *s.character,
                                                    s.world->server_time());
            s.send_raw(echo);
            if (game::Map* m = s.map()) m->broadcast(echo, s.character->handle);
            return;
        }

        case op::CZ_ROTATE: {
            if (!s.character) return;
            r.seek(0x1A);
            s.character->direction = {r.f32(), r.f32()};
            Bytes echo = send::make_ZC_ROTATE(ctx, *s.character);
            s.send_raw(echo);
            if (game::Map* m = s.map()) m->broadcast(echo, s.character->handle);
            return;
        }

        case op::CZ_JUMP: {
            if (!s.character) return;
            r.seek(0x17);
            float x = r.f32(), y = r.f32(), z = r.f32();
            s.character->position = {x, y, z};
            Bytes echo = send::make_ZC_JUMP(ctx, *s.character, kJumpPower,
                                            s.world->server_time());
            s.send_raw(echo);
            if (game::Map* m = s.map()) m->broadcast(echo, s.character->handle);
            return;
        }

        case op::CZ_CHAT: {
            if (!s.character) return;
            r.seek(0x18);
            std::string text = r.cstr(512);
            if (text.empty()) return;
            if (text[0] == '/') {
                run_command(s, text);
                return;
            }
            s.log("chat: " + text);
            Bytes c = send::make_ZC_CHAT(ctx, *s.character, text);
            s.send_raw(c);                          // the client waits on this
            if (game::Map* m = s.map()) m->broadcast(c, s.character->handle);
            return;
        }

        case op::CZ_ITEM_EQUIP: {
            if (!s.character) return;
            int64_t object_id = int64_t(r.u64v());
            uint8_t slot = r.u8();
            if (slot >= uint8_t(game::EquipSlot::Count)) {
                s.log("equip into slot " + std::to_string(slot) + " ignored");
                return;
            }
            int32_t removed = 0;
            if (!s.character->equip_item(object_id, game::EquipSlot(slot),
                                         removed)) {
                s.log("equip of an item not in the bag");
                return;
            }
            send::ZC_ITEM_INVENTORY_LIST(s, *s.character);
            send::ZC_ITEM_EQUIP_LIST(s, *s.character);
            send::ZC_OBJECT_PROPERTY(s, *s.character);
            // Everyone else has to see the change too, or the character keeps
            // wearing the old item on their screens.
            if (game::Map* m = s.map())
                m->broadcast(send::make_ZC_UPDATED_PCAPPEARANCE(ctx, *s.character),
                             s.character->handle);
            return;
        }

        case op::CZ_ITEM_UNEQUIP: {
            if (!s.character) return;
            uint8_t slot = r.u8();
            if (slot >= uint8_t(game::EquipSlot::Count)) return;

            auto& worn = s.character->equipped;
            for (auto it = worn.begin(); it != worn.end(); ++it) {
                if (it->slot != int32_t(slot)) continue;
                game::Item back = *it;
                back.slot = int32_t(s.character->inventory.size());
                worn.erase(it);
                s.character->inventory.push_back(back);
                s.character->rebuild_equip_ids();
                break;
            }
            send::ZC_ITEM_INVENTORY_LIST(s, *s.character);
            send::ZC_ITEM_EQUIP_LIST(s, *s.character);
            if (game::Map* m = s.map())
                m->broadcast(send::make_ZC_UPDATED_PCAPPEARANCE(ctx, *s.character),
                             s.character->handle);
            return;
        }

        case op::CZ_ITEM_DELETE:
        case op::CZ_ITEM_DROP: {
            if (!s.character) return;
            int64_t object_id = int64_t(r.u64v());
            int32_t amount = r.i32v();
            if (amount <= 0) amount = 1;
            if (!s.character->remove_item(object_id, amount)) return;
            send::ZC_ITEM_REMOVE(s, object_id, amount);
            s.character->rebuild_equip_ids();
            send::ZC_OBJECT_PROPERTY(s, *s.character);
            return;
        }

        case op::CZ_MOVE_ZONE_OK:
            if (s.character) send::ZC_MOVE_ZONE_OK(s, s.character->map_id);
            return;

        case op::CZ_SKILL_TARGET: {
            if (!s.character) return;
            r.u8();
            int32_t skill_id = r.i32v();
            uint32_t target = r.u32v();
            use_skill(s, skill_id, target, {});
            return;
        }

        // Sent after the default attack animation when nothing is targeted,
        // with skill id 0. It still deserves a swing so the animation is not
        // left hanging, but there is nothing to damage.
        case op::CZ_SKILL_TARGET_ANI: {
            if (!s.character) return;
            r.u8();
            int32_t skill_id = r.i32v();
            s.character->direction = {r.f32(), r.f32()};
            if (skill_id) use_skill(s, skill_id, 0, {});
            return;
        }

        case op::CZ_SKILL_GROUND: {
            if (!s.character) return;
            r.u8();
            int32_t skill_id = r.i32v();
            r.i32v();
            game::Position origin{r.f32(), r.f32(), r.f32()};
            game::Position far_pos{r.f32(), r.f32(), r.f32()};
            s.character->direction = {r.f32(), r.f32()};
            uint32_t target = r.u32v();
            (void)origin;
            use_skill(s, skill_id, target, far_pos);
            return;
        }

        // Leaving. Logout closes the client down; move-barrack sends the
        // player back to the lodge. Both need their acknowledgement or the
        // client sits on a dead menu -- it does not just disconnect.
        case op::CZ_LOGOUT:
            s.log("logout requested");
            on_disconnect(s);
            send::ZC_LOGOUT_OK(s);
            return;

        case op::CZ_MOVE_BARRACK:
            s.log("returning to the lodge");
            on_disconnect(s);
            send::ZC_MOVE_BARRACK(s);
            return;

        case op::CZ_POSE: {
            if (!s.character) return;
            int32_t pose = r.i32v();
            float x = r.f32(), y = r.f32(), z = r.f32();
            s.character->position = {x, y, z};
            Bytes p = send::make_ZC_POSE(ctx, *s.character, pose);
            s.send_raw(p);
            if (game::Map* m = s.map()) m->broadcast(p, s.character->handle);
            return;
        }

        case op::CZ_REST_SIT: {
            if (!s.character) return;
            s.character->sitting = !s.character->sitting;
            Bytes p = send::make_ZC_REST_SIT(ctx, *s.character,
                                             s.character->sitting);
            s.send_raw(p);
            if (game::Map* m = s.map()) m->broadcast(p, s.character->handle);
            return;
        }

        // Steady-state chatter: acknowledged by being ignored, and noisy
        // enough that logging it would bury everything else.
        case op::CZ_HEARTBEAT:
        case op::CZ_ON_AIR:
        case op::CZ_ON_GROUND:
        case op::CZ_MOVEMENT_INFO:
        case op::CZ_DASHRUN:
        case op::CZ_CHAT_LOG:
            // Only a loaded client sends these, so they are a safe fallback
            // trigger for becoming visible.
            enter_world(s);
            return;

        default:
            break;
    }

    uint32_t& seen = s.unhandled[r.op()];
    ++seen;
    if (seen == 1)
        s.log("no zone handler for " + ctx.table.name_of(r.op()));
    else if (seen % 200 == 0)
        s.log("no zone handler for " + ctx.table.name_of(r.op()) + " (x" +
              std::to_string(seen) + ")");
}

void on_disconnect(Session& s) {
    if (!s.character) return;
    if (game::Map* m = s.map()) {
        m->broadcast(send::make_ZC_LEAVE(*s.ctx, s.character->handle),
                     s.character->handle);
        m->remove(s.character->handle);
    }
    s.character->session.reset();
    s.character->map = nullptr;
    s.character.reset();
}

}  // namespace tos
