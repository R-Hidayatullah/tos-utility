#include "zone_send.h"

#include <algorithm>

#include "op_gen.h"

namespace tos::send {
namespace {

using PW = PacketWriter;

PW make(const ServerContext& ctx, uint16_t op) { return PW(op, &ctx.table); }

// Broadcast packets are built once and written to many sockets, so they carry
// sequence 0 rather than any one connection's counter -- the client does not
// validate the server's sequence, and the replayed captures never had ours.
Bytes finish(const ServerContext& ctx, PW& w) {
    const_cast<ServerContext&>(ctx).report_verify(w);
    return w.build(0);
}

// ---- shared field blocks -----------------------------------------------
//
// These mirror the client's own composite writes. Their sizes are load-bearing
// -- the appearance block is exactly the 520 bytes ZC_ENTER_PC reserves at
// +0x5D, so a slot added or dropped here shifts every field after it.

// 520 bytes.
void appearance_pc(PW& w, const game::Character& c) {
    w.str(c.name, 65);
    w.str(c.team_name, 65);
    w.zeros(6);
    w.i64v(c.account_id);
    w.i32v(c.stance);
    w.i16v(int16_t(c.job_id));
    w.u8(c.gender);
    w.u8(0);
    w.i32v(c.level);
    w.i32v(1022);                       // display job
    w.u32v(c.skin_color);
    w.i32v(0);

    const std::vector<int32_t>& eq =
        c.equip.size() == 36 ? c.equip : game::Character::default_equip();
    for (int32_t id : eq) w.i32v(id);

    w.i32v(0);
    w.i32v(0);
    w.i32v(0);

    // Visual equipment ids: the client's own "empty" items again, so a
    // character with nothing equipped still has a silhouette.
    for (int32_t id : game::Character::default_equip()) w.i32v(id);

    w.i32v(0);
    w.i32v(0);
    w.i32v(0);
    w.i16v(int16_t(c.hair));

    // Headgear / wig visibility flags.
    w.u8(0);
    w.u8(1);
    w.u8(1);
    w.u8(1);
    w.u8(0);
    w.u8(1);

    w.u8(1);                            // sub-weapon visibility
    w.zeros(7);
    w.zeros(24);
}

// 608 bytes: the appearance block plus the barrack-only tail. BC_COMMANDER_CREATE
// declares 618 and the header is 10, so this closing exactly is the check.
void appearance_barrack_pc(PW& w, const game::Character& c) {
    appearance_pc(w, c);

    w.i64v(c.object_id);
    w.u8(c.slot);                       // list index
    w.u8(0);
    w.i16v(1);
    w.i16v(int16_t(c.map_id));
    w.i16v(int16_t(c.channel));
    w.i64v(0);                          // current xp
    w.i64v(8);                          // max xp
    w.i32v(0);
    w.i16v(0);
    w.i16v(0);
    w.i64v(c.silver);

    w.position(c.barrack_position.x, c.barrack_position.y,
               c.barrack_position.z);
    w.direction(c.barrack_direction.cos, c.barrack_direction.sin);
    w.position(c.barrack_position.x, c.barrack_position.y,
               c.barrack_position.z);
    w.direction(c.barrack_direction.cos, c.barrack_direction.sin);
}

void commander(PW& w, const game::Character& c) {
    w.u32v(c.handle);
    w.i32v(0);
    appearance_pc(w, c);

    w.position(c.position.x, c.position.y, c.position.z);
    w.i32v(0);
    w.i64v(c.exp);
    w.i64v(c.max_exp);
    w.i64v(c.total_exp);
    w.i64v(c.object_id);
    w.i64v(c.social_user_id);

    w.i32v(c.hp);
    w.i32v(c.max_hp);
    w.i32v(c.sp);
    w.i32v(c.max_sp);
    w.i32v(c.stamina);
    w.i32v(c.max_stamina);
    w.i32v(0);                          // shield
    w.i32v(0);                          // max shield
    w.zeros(32);
    w.zeros(8);
}

// The 20 bytes at +0x3E that the client's ZC_ENTER_MONSTER handler skips, plus
// the GenType it does read at +0x52.
void monster_appearance_base(PW& w, const game::Monster& m) {
    w.i32v(m.monster_id);
    w.i32v(0);
    w.i32v(m.max_hp);
    w.i32v(m.level);
    w.f32(1.0f);                        // SDR
    w.u8(0);
    w.zeros(3);
    // No fallback. 2 was a guess and it never appears on the wire; 0 does, on
    // 41 of the 162 captured spawns -- everything with no generator row behind
    // it. Sending 2 pointed those at some other map's generator.
    w.i32v(m.gen_type);                 // GenType
}

// Five fixed-width 256-byte names: exactly the 1280-byte run the client reads
// at +0x5E. docs/09 read this as a property blob, which is why spawning used
// to need a captured packet as a template.
void monster_appearance(PW& w, const game::Monster& m) {
    w.str(m.name, 256);
    w.str(m.unique_name.empty()
              ? (m.data ? m.data->class_name : std::string())
              : m.unique_name,
          256);
    // These come from the map's gentype_<map>.ies row. Sending them empty left
    // every NPC unclickable, because the dialog script the client runs is
    // named here and nowhere else.
    w.str(m.dialog, 256);
    w.str(m.enter_script, 256);
    w.str(m.leave_script, 256);
}

}  // namespace

// ---- barrack -----------------------------------------------------------

void BC_LOGIN_PACKET_RECEIVED(Session& s) {
    PW w = make(*s.ctx, op::BC_LOGIN_PACKET_RECEIVED);
    s.send(w);
}

void BC_DISCONNECT_PACKET_LOG_COUNT(Session& s) {
    PW w = make(*s.ctx, op::BC_DISCONNECT_PACKET_LOG_COUNT);
    w.i32v(0x1E);
    s.send(w);
}

void BC_IES_MODIFY_LIST(Session& s) {
    // The client expects the list of live IES overrides before it will leave
    // the loading screen. We publish none, which is a count of zero.
    PW w = make(*s.ctx, op::BC_IES_MODIFY_LIST);
    w.i16v(0);
    s.send(w);
}

void BC_LOGINOK(Session& s, const Account& acc) {
    PW w = make(*s.ctx, op::BC_LOGINOK);
    w.i16v(1001);                       // server group id
    w.i64v(acc.id);
    w.str(acc.name, 33);
    w.zeros(23);
    w.i32v(3);                          // account privileges
    w.str(acc.session_key, 64);
    w.i32v(4475);
    w.i64v(0);
    s.send(w);
}

void BC_MESSAGE(Session& s, const std::string& text) {
    PW w = make(*s.ctx, op::BC_MESSAGE);
    w.u8(0);
    w.i32v(0);
    w.lpstr(text);
    w.i32v(0);
    s.send(w);
}

void BC_COMMANDER_LIST(Session& s, const Account& acc) {
    PW w = make(*s.ctx, op::BC_COMMANDER_LIST);
    w.i64v(acc.id);
    w.u8(0);
    w.u8(0);
    w.str(acc.team_name, 64);

    // Account properties: size, then the block the client reads before them.
    w.i16v(0);                          // property bytes
    w.i16v(100);
    w.i16v(0);
    w.i32v(0);                          // selected barrack layer
    w.u8(1);
    w.u8(1);

    w.i16v(1);                          // available barrack themes
    w.i32v(11);                         // "barrack" map
    w.i16v(1);                          // selected

    w.i16v(0);                          // additional slots
    w.i32v(0);                          // team exp
    w.i16v(int16_t(acc.characters.size()));
    s.send(w);
}

// BC_NORMAL sub-opcodes, from the client's barrack normal-op table.
namespace normal {
constexpr uint32_t kTeamUI = 0x0C;
constexpr uint32_t kZoneTraffic = 0x0D;
constexpr uint32_t kCharacterInfo = 0x1C;
}  // namespace normal

void BC_NORMAL_ZoneTraffic(Session& s, const Account& acc) {
    // Without this the "Select Channel" dropdown stays empty and Start Game
    // does nothing: the client builds the channel list from here, keyed by the
    // maps its own characters are standing on.
    std::vector<int32_t> maps;
    for (const auto& c : acc.characters) {
        if (std::find(maps.begin(), maps.end(), c->map_id) == maps.end())
            maps.push_back(c->map_id);
    }

    PW w = make(*s.ctx, op::BC_NORMAL);
    w.u32v(normal::kZoneTraffic);
    w.zlib(true, [&](PW& z) {
        z.i16v(0);
        z.i16v(0);
        if (maps.empty()) {
            z.i16v(0);
            z.i32v(100);
            return;
        }
        z.i16v(100);                        // max players per map
        z.i16v(int16_t(maps.size()));
        for (int32_t map_id : maps) {
            z.i16v(int16_t(map_id));
            z.i16v(1);                      // one channel per map
            // The client turns this id into the channel NAME -- 0 becomes
            // "Ch 1" -- so it has to be a sequential index from 0, not our
            // channel number.
            z.i16v(0);
            z.i16v(int16_t(acc.characters.size()));   // current players
            z.i16v(100);                    // max players
        }
    });
    s.send(w);
}

void BC_NORMAL_TeamUI(Session& s, const Account& acc) {
    PW w = make(*s.ctx, op::BC_NORMAL);
    w.u32v(normal::kTeamUI);
    w.i64v(acc.id);
    w.i16v(0);                              // additional slots
    w.i32v(0);                              // team exp
    w.i16v(int16_t(acc.characters.size()));
    s.send(w);
}

void BC_NORMAL_CharacterInfo(Session& s, const Account& acc) {
    PW w = make(*s.ctx, op::BC_NORMAL);
    w.u32v(normal::kCharacterInfo);
    w.i64v(acc.id);
    w.i32v(int32_t(acc.characters.size()));
    for (const auto& c : acc.characters) {
        w.i64v(c->object_id);
        w.str(c->name, 64);
        w.zeros(24);
    }
    s.send(w);
}

void BC_NORMAL_SetPosition(Session& s, uint8_t index, const game::Position& p) {
    PW w = make(*s.ctx, op::BC_NORMAL);
    w.u32v(0x02);                           // Barrack.SetPosition
    w.i64v(s.account ? s.account->id : 0);
    w.u8(index);
    w.position(p.x, p.y, p.z);
    s.send(w);
}

void BC_BARRACKNAME_CHANGE(Session& s, int32_t result, const std::string& name) {
    PW w = make(*s.ctx, op::BC_BARRACKNAME_CHANGE);
    w.i32v(1);
    w.u8(uint8_t(result));
    w.str(name, 64);
    s.send(w);
}

void BC_BARRACKNAME_CHECK_RESULT(Session& s, int32_t result,
                                 const std::string& name,
                                 const std::string& message) {
    PW w = make(*s.ctx, op::BC_BARRACKNAME_CHECK_RESULT);
    w.i32v(result);
    w.str(message, 256);
    w.str(name, 64);
    s.send(w);
}

void BC_COMMANDER_CREATE_SLOTID(Session& s, uint8_t character_count) {
    // The slot the new character was given. The client waits for this before
    // it will place them in the lodge, so a creation without it hangs.
    PW w = make(*s.ctx, op::BC_COMMANDER_CREATE_SLOTID);
    w.u8(character_count);
    s.send(w);
}

void BC_COMMANDER_CREATE(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::BC_COMMANDER_CREATE);
    appearance_barrack_pc(w, c);
    s.send(w);
}

void BC_COMMANDER_DESTROY(Session& s, uint8_t slot) {
    PW w = make(*s.ctx, op::BC_COMMANDER_DESTROY);
    w.u8(slot);
    s.send(w);
}

void BC_START_GAMEOK(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::BC_START_GAMEOK);
    w.i32v(0);                          // zone id
    w.u32v(s.ctx->config.server_ip);
    w.i32v(s.ctx->config.zone_port);
    w.i32v(c.map_id);
    w.u8(1);                            // channel
    w.i64v(c.object_id);
    w.u8(0);                            // only connects when 0
    w.u8(1);
    s.send(w);
}

void BC_SERVER_ENTRY(Session& s) {
    PW w = make(*s.ctx, op::BC_SERVER_ENTRY);
    w.u32v(s.ctx->config.server_ip);    // chat server
    w.u32v(s.ctx->config.server_ip);    // relation server
    w.i16v(int16_t(s.ctx->config.chat_port));
    w.i16v(int16_t(s.ctx->config.chat_port + 1));
    s.send(w);
}

// ---- social / chat link ------------------------------------------------
//
// The client opens two plaintext links (chat and relation) and will not finish
// loading the world until both have answered its CS_LOGIN. Leaving them silent
// looks exactly like a hang on "loading world".

namespace social {
constexpr uint32_t kLoginSuccess = 0x00;
constexpr uint32_t kUnknown02 = 0x02;
constexpr uint32_t kLikedList = 0x7D01;
constexpr uint32_t kLikedMeList = 0x7D02;
}  // namespace social

void SC_LOGIN_OK(Session& s) {
    PW w = make(*s.ctx, op::SC_LOGIN_OK);
    s.send(w);
}

void SC_NORMAL_LoginSuccess(Session& s) {
    PW w = make(*s.ctx, op::SC_NORMAL);
    w.u32v(social::kLoginSuccess);
    s.send(w);
}

void SC_NORMAL_Unknown02(Session& s) {
    PW w = make(*s.ctx, op::SC_NORMAL);
    w.u32v(social::kUnknown02);
    s.send(w);
}

void SC_NORMAL_LikedList(Session& s) {
    PW w = make(*s.ctx, op::SC_NORMAL);
    w.u32v(social::kLikedList);
    w.i32v(0);                          // nobody liked yet
    s.send(w);
}

void SC_NORMAL_RelatedPcSession(Session& s) {
    PW w = make(*s.ctx, op::SC_NORMAL);
    w.u32v(0x7D00);                         // Social.RelationCount
    w.i32v(0);
    s.send(w);
}

void SC_NORMAL_LikedMeList(Session& s) {
    PW w = make(*s.ctx, op::SC_NORMAL);
    w.u32v(social::kLikedMeList);
    w.i32v(0);
    s.send(w);
}

// ---- zone: connection and entry ---------------------------------------

void ZC_CONNECT_OK(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_CONNECT_OK);
    w.u8(0);                            // 0 normal mode, 1 single mode
    w.i32v(1281523659);
    w.u8(3);                            // permission level
    w.zeros(10);
    w.i32v(0);
    w.i16v(0);
    w.i32v(40588976);
    w.zeros(10);
    w.u8(0);                            // integrated server
    w.u8(0);                            // integrated dungeon server
    w.u8(0);
    w.lpstr(s.account ? s.account->session_key : std::string());
    commander(w, c);
    s.send(w);
}

void ZC_START_GAME(Session& s) {
    // The second and third floats are the client's CLOCK BASE, and every
    // movement timestamp is on that same clock. Sending 1 here and a real
    // server time in ZC_MOVE_DIR makes the client believe that many seconds
    // have elapsed and extrapolate the character forward -- which reads as
    // running impossibly fast, or teleporting.
    //
    // The live server's numbers show the relationship: base 335542.5 with
    // moves at 335552..335924, i.e. a monotonic seconds clock shared by both.
    float now = s.world->server_time();

    PW w = make(*s.ctx, op::ZC_START_GAME);
    w.f32(1.0f);                        // client time scale
    w.f32(now);                         // serverAppTimeOffset
    w.f32(now);                         // globalAppTimeOffset
    w.u64v(now_filetime());
    w.u8(0);
    // The client wants the wall clock a second in the past, as a 19-character
    // string plus its NUL -- which is exactly the 20 bytes the recovered
    // layout reserves at +0x1F, closing the 51-byte packet.
    w.cstr(utc_string(-1));
    s.send(w);
}

void ZC_MYPC_ENTER(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_MYPC_ENTER);
    w.position(c.position.x, c.position.y, c.position.z);
    w.u8(0);
    w.u8(0);
    w.i32v(0);
    s.send(w);
}

Bytes make_ZC_ENTER_PC(const ServerContext& ctx, const game::Character& c) {
    PW w = make(ctx, op::ZC_ENTER_PC);
    w.u32v(c.handle);
    w.position(c.position.x, c.position.y, c.position.z);
    w.direction(c.direction.cos, c.direction.sin);
    w.i16v(0);
    w.i64v(c.social_user_id);
    w.u8(0);                            // pose
    w.f32(c.move_speed);
    w.f32(0);                           // moving shot
    w.i32v(c.hp);
    w.i32v(c.max_hp);
    w.i16v(int16_t(c.sp));
    w.i16v(int16_t(c.max_sp));
    w.i32v(0);
    w.i32v(0);
    w.i32v(c.stamina);
    w.i32v(c.max_stamina);
    w.u8(0);
    w.i16v(0);
    w.i32v(-1);                         // title achievement id
    w.i32v(0);
    w.u8(0);
    appearance_pc(w, c);
    w.i32v(0);
    w.f32(405494.3f);
    w.u8(0);
    w.u8(0);
    return finish(ctx, w);
}

void ZC_ENTER_PC(Session& s, const game::Character& c) {
    s.send_raw(make_ZC_ENTER_PC(*s.ctx, c));
}

Bytes make_ZC_ENTER_MONSTER(const ServerContext& ctx, const game::Monster& m,
                            bool from_ground) {
    PW w = make(ctx, op::ZC_ENTER_MONSTER);
    w.u32v(m.handle);
    w.position(m.position.x, m.position.y, m.position.z);
    w.direction(m.direction.cos, m.direction.sin);
    // 1 marks an enemy, 2 a friendly. This used to be hardcoded to 2 on the
    // strength of capture_1785545696.bin, where all 122 spawns carry 2 -- but
    // that capture is a town, so it has no hostile monsters in it at all.
    // relay/dumps/capture_20260801_214001.bin covers a field map and splits
    // 90 ones against 72 twos. Sending 2 for everything is what left field
    // monsters painted as friendly NPCs.
    w.u8(m.data && m.data->enemy() ? 1 : 2);
    // 1 only when the spawn appears while the player is already watching --
    // a respawn or a /spawn. Everything present at map entry carries 0.
    w.u8(from_ground ? 1 : 0);
    w.i32v(m.hp);
    w.i32v(m.max_hp);
    w.i32v(0);                          // shield
    w.i32v(0);                          // max shield
    w.f32(m.move_speed);

    monster_appearance_base(w, m);
    w.i32v(0);                          // attribute
    w.i32v(0);                          // race
    monster_appearance(w, m);

    w.i16v(int16_t(m.properties.byte_count()));
    w.i32v(0);
    w.i32v(0);
    w.i16v(0);
    w.u8(0);
    m.properties.write(w);
    return finish(ctx, w);
}

void ZC_ENTER_MONSTER(Session& s, const game::Monster& m, bool from_ground) {
    s.send_raw(make_ZC_ENTER_MONSTER(*s.ctx, m, from_ground));
}

Bytes make_ZC_LEAVE(const ServerContext& ctx, uint32_t handle) {
    PW w = make(ctx, op::ZC_LEAVE);
    w.u32v(handle);
    w.i16v(1);                          // 0 plays the blue vanish effect
    return finish(ctx, w);
}

void ZC_LEAVE(Session& s, uint32_t handle) {
    s.send_raw(make_ZC_LEAVE(*s.ctx, handle));
}

void ZC_OBJECT_PROPERTY(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_OBJECT_PROPERTY);
    w.i64v(c.object_id);
    w.i32v(0);                          // isTrickPacket
    c.properties.write(w);
    s.send(w);
}

namespace {

// Every item carries a property stream. The client crashes on an item with an
// empty one in ZC_ITEM_ADD, so there is always at least a cooldown.
//
// Filled in place rather than returned: Properties owns a mutex and so is
// neither copyable nor movable.
void fill_item_properties(game::Properties& p, const game::Item& it) {
    p.set("CoolDown", 0.0f);
    if (it.data) {
        p.set("Weight", it.data->weight);
        p.set("MaxStack", float(it.data->max_stack));
    }
}

}  // namespace

void ZC_ITEM_INVENTORY_LIST(Session& s, const game::Character& c) {
    // An empty inventory still has to be announced: without the list the
    // client leaves the inventory UI in its loading state.
    const ServerContext& ctx = *s.ctx;
    PW w = make(ctx, op::ZC_ITEM_INVENTORY_LIST);
    w.i32v(int32_t(c.inventory.size()));
    w.zlib(true, [&](PW& z) {
        for (const game::Item& it : c.inventory) {
            game::Properties props("Item", &ctx.gamedata.properties);
            fill_item_properties(props, it);
            z.i32v(it.id);
            z.i16v(int16_t(props.byte_count()));
            z.zeros(2);
            z.i64v(it.object_id);
            z.i32v(it.amount);
            z.i32v(it.price);
            z.i32v(it.slot);
            z.i32v(1);
            props.write(z);
        }
    });
    s.send(w);
}

void ZC_ITEM_EQUIP_LIST(Session& s, game::Character& c) {
    // The client wants this split into pages of five slots, with the first
    // page flagged. One packet with all 36 leaves the equipment window blank.
    const ServerContext& ctx = *s.ctx;
    const int kSlots = int(game::EquipSlot::Count);
    for (int page = 0; page * 5 < kSlots; ++page) {
        int lo = page * 5, hi = lo + 5;

        PW w = make(ctx, op::ZC_ITEM_EQUIP_LIST);
        w.boolean(page == 0);
        w.i32v(lo);
        w.i32v(hi);

        for (int slot = lo; slot < hi && slot < kSlots; ++slot) {
            game::Item* worn = c.equipped_in(game::EquipSlot(slot));
            game::Item blank;
            if (!worn) {
                // The client's own placeholder, so an empty slot still has an
                // item id to render.
                blank.id = game::Character::default_equip()[size_t(slot)];
                blank.slot = slot;
                worn = &blank;
            }
            game::Properties props("Item", &ctx.gamedata.properties);
            fill_item_properties(props, *worn);

            w.i32v(worn->id);
            w.i16v(int16_t(props.byte_count()));
            w.zeros(2);
            w.i64v(worn->object_id);
            w.u8(uint8_t(slot));
            w.zeros(3);
            w.i32v(0);
            w.i16v(0);
            props.write(w);

            if (worn->object_id != 0) {
                w.i16v(0);
                w.i64v(worn->object_id);
                w.i16v(0);
            }
        }
        s.send(w);
    }
}

void ZC_ITEM_ADD(Session& s, const game::Item& it, uint8_t add_type) {
    game::Properties props("Item", &s.ctx->gamedata.properties);
    fill_item_properties(props, it);

    PW w = make(*s.ctx, op::ZC_ITEM_ADD);
    w.i64v(it.object_id);
    w.i32v(it.amount);
    w.i32v(0);
    w.i32v(it.slot);
    w.i32v(it.id);
    w.i16v(int16_t(props.byte_count()));
    w.u8(add_type);
    w.f32(0);                           // notification delay, seconds
    w.u8(0);                            // inventory type
    w.u8(0);
    w.u8(0);
    props.write(w);
    s.send(w);
}

void ZC_ITEM_REMOVE(Session& s, int64_t object_id, int32_t amount) {
    PW w = make(*s.ctx, op::ZC_ITEM_REMOVE);
    w.i64v(object_id);
    w.i32v(amount);
    w.i32v(0);
    w.u8(0);                            // reason
    w.u8(0);                            // inventory type
    s.send(w);
}

Bytes make_ZC_UPDATED_PCAPPEARANCE(const ServerContext& ctx,
                                   const game::Character& c) {
    PW w = make(ctx, op::ZC_UPDATED_PCAPPEARANCE);
    w.u32v(c.handle);
    appearance_pc(w, c);
    return finish(ctx, w);
}

namespace {

// One skill entry, as the skill list and ZC_SKILL_ADD both carry it.
void add_skill(PW& w, const game::Skill& sk, const data::PropertyTable& pt) {
    game::Properties props("Skill", &pt);
    props.set("Level", float(sk.level));
    props.set("SkillFactor", 1.0f);
    props.set("SpendSP", 0.0f);

    w.i64v(sk.object_id);
    w.i32v(sk.id);
    w.i16v(int16_t(props.byte_count()));
    w.zeros(2);                         // alignment
    w.i32v(0);
    w.i16v(0);
    w.zeros(2);                         // alignment
    props.write(w);
}

}  // namespace

void ZC_SKILL_LIST(Session& s, const game::Character& c) {
    const data::PropertyTable& props = s.ctx->gamedata.properties;

    PW w = make(*s.ctx, op::ZC_SKILL_LIST);
    w.u32v(c.handle);
    w.i16v(int16_t(c.skills.size()));
    w.u8(0);
    w.zlib(true, [&](PW& z) {
        for (const game::Skill& sk : c.skills) add_skill(z, sk, props);
    });

    // Trailing block the client expects after the skills. The two property
    // ids are looked up rather than hard-coded so they follow the table.
    w.u8(0x00);
    w.u8(0x80);
    w.u8(0x3F);
    const data::PropertyTable& pt = s.ctx->gamedata.properties;
    w.i32v(pt.id("Skill", "SkillFactor"));
    w.f32(1.0f);
    w.i32v(pt.id("Skill", "CaptionTime"));
    w.f32(0.0f);
    s.send(w);
}

void ZC_SKILL_ADD(Session& s, const game::Character& c, const game::Skill& sk) {
    PW w = make(*s.ctx, op::ZC_SKILL_ADD);
    w.i64v(c.object_id);
    w.u8(1);                            // add to the quickbar
    w.u8(0);
    w.i64v(0);
    add_skill(w, sk, s.ctx->gamedata.properties);
    s.send(w);
}

void ZC_SKILL_CAST_CANCEL(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_SKILL_CAST_CANCEL);
    w.u32v(c.handle);
    s.send(w);
}

Bytes make_ZC_SKILL_MELEE_GROUND(const ServerContext& ctx,
                                 const game::Actor& caster, int32_t skill_id,
                                 const game::Position& target_pos) {
    PW w = make(ctx, op::ZC_SKILL_MELEE_GROUND);
    w.i32v(skill_id);
    w.u32v(caster.handle);
    w.f32(caster.direction.cos);
    w.f32(caster.direction.sin);
    w.i32v(1);
    w.f32(0.1f);                        // shoot time
    w.f32(1.0f);
    w.i32v(0);
    w.i32v(0);                          // force id
    w.f32(1.0f);                        // skill speed rate
    w.i32v(0);
    w.position(target_pos.x, target_pos.y, target_pos.z);
    w.i16v(0);                          // hit count, damage goes via ZC_HIT_INFO
    return finish(ctx, w);
}

void ZC_SET_CHATBALLOON_SKIN(Session& s) {
    static const uint8_t kBody[] = {0x71, 0x82, 0x01, 0x00, 0x01, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                    0x00, 0xD8, 0x6E};
    PW w = make(*s.ctx, op::ZC_SET_CHATBALLOON_SKIN);
    w.bin(kBody, sizeof kBody);
    s.send(w);
}

// Two normal-op blocks the live server sends around ZC_CONNECT_OK. The client
// does not send CZ_GAME_READY until it has them, so the map load simply never
// starts without them -- see the sequence in docs/10.
void ZC_NORMAL_AdventureBook(Session& s) {
    PW w = make(*s.ctx, op::ZC_NORMAL);
    w.u32v(0x199);                          // Zone.AdventureBook
    w.lpstr("AdventureBook");
    w.lpstr("Initialization_point");
    w.i32v(-1);
    w.i32v(0);
    w.i32v(0);
    w.u8(1);
    s.send(w);
}

void ZC_NORMAL_Unknown1B4(Session& s) {
    PW w = make(*s.ctx, op::ZC_NORMAL);
    w.u32v(0x1B9);
    w.i32v(0);
    s.send(w);
}

void ZC_STANCE_CHANGE(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_STANCE_CHANGE);
    w.u32v(c.handle);
    w.i32v(c.stance);
    s.send(w);
}

void ZC_QUICK_SLOT_LIST(Session& s) {
    // Nothing saved, and the client is happy with no packet at all -- sending
    // an empty one makes it parse 54 slots out of zero bytes.
    (void)s;
}

void ZC_LOGOUT_OK(Session& s) {
    PW w = make(*s.ctx, op::ZC_LOGOUT_OK);
    s.send(w);
}

void ZC_MOVE_BARRACK(Session& s) {
    PW w = make(*s.ctx, op::ZC_MOVE_BARRACK);
    s.send(w);
}

Bytes make_ZC_POSE(const ServerContext& ctx, const game::Actor& a,
                   int32_t pose) {
    PW w = make(ctx, op::ZC_POSE);
    w.u32v(a.handle);
    w.i32v(pose);
    w.position(a.position.x, a.position.y, a.position.z);
    w.direction(a.direction.cos, a.direction.sin);
    w.u8(1);
    return finish(ctx, w);
}

Bytes make_ZC_REST_SIT(const ServerContext& ctx, const game::Actor& a,
                       bool sitting) {
    PW w = make(ctx, op::ZC_REST_SIT);
    w.u32v(a.handle);
    w.u8(0);
    // Getting this wrong freezes the character: it stops animating while
    // running as well as while sitting.
    w.boolean(sitting);
    return finish(ctx, w);
}

// ---- world entry ------------------------------------------------------
//
// The client sends CZ_LOAD_COMPLETE once a second until it is answered, and
// stays in the loading state -- accepting no input -- until it has the rest of
// this set. Most are empty lists; being empty is fine, being absent is not.

void ZC_LOAD_COMPLETE(Session& s) {
    PW w = make(*s.ctx, op::ZC_LOAD_COMPLETE);
    s.send(w);
}

void ZC_IES_MODIFY_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_IES_MODIFY_LIST);
    w.i16v(0);
    s.send(w);
}

void ZC_OPTION_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_OPTION_LIST);
    w.cstr("");                             // no options changed from default
    s.send(w);
}

void ZC_SKILLMAP_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_SKILLMAP_LIST);
    w.i32v(0);
    s.send(w);
}

void ZC_CHAT_MACRO_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_CHAT_MACRO_LIST);
    w.i32v(0);
    s.send(w);
}

void ZC_MAP_REVEAL_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_MAP_REVEAL_LIST);
    w.i32v(0);
    s.send(w);
}

void ZC_NPC_STATE_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_NPC_STATE_LIST);
    w.i32v(0);
    w.zlib(true, [](PW&) {});
    s.send(w);
}

void ZC_HELP_LIST(Session& s) {
    PW w = make(*s.ctx, op::ZC_HELP_LIST);
    w.i32v(0);
    s.send(w);
}

void ZC_SESSION_OBJECTS(Session& s) {
    PW w = make(*s.ctx, op::ZC_SESSION_OBJECTS);
    w.i16v(0);                              // object count
    w.u8(0);
    w.f32(565831.1f);
    s.send(w);
}

void ZC_MYPAGE_MAP(Session& s) {
    PW w = make(*s.ctx, op::ZC_MYPAGE_MAP);
    w.i32v(1);
    w.u8(0);
    s.send(w);
}

void ZC_GUESTPAGE_MAP(Session& s) {
    PW w = make(*s.ctx, op::ZC_GUESTPAGE_MAP);
    w.i32v(1);
    w.u8(0);
    s.send(w);
}

void ZC_LOGIN_TIME(Session& s) {
    PW w = make(*s.ctx, op::ZC_LOGIN_TIME);
    w.u64v(now_filetime());
    s.send(w);
}

void ZC_START_INFO(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_START_INFO);
    w.i32v(1);                              // count
    w.i16v(int16_t(c.job_id));
    w.i32v(0);
    w.i32v(0);
    w.i16v(1);
    s.send(w);
}

void ZC_NORMAL_UpdateSkillUI(Session& s, const game::Character& c) {
    // The skill window is built from THIS, not from ZC_SKILL_LIST. The client
    // will only lay out skills belonging to a job it has data for here, so
    // without it the list arrives and nothing is drawn.
    PW w = make(*s.ctx, op::ZC_NORMAL);
    w.u32v(0x18B);                          // Zone.UpdateSkillUI
    w.i64v(c.object_id);

    w.i32v(1);                              // one job: the character's own
    w.i16v(int16_t(c.job_id));
    w.i16v(int16_t(c.level));
    w.i32v(0);
    w.i64v(c.total_exp);
    w.u8(10);                               // skill points
    w.i16v(41857);
    w.zeros(5);
    w.u64v(now_filetime());                 // job selection date
    w.i64v(0);
    s.send(w);
}

void ZC_QUICK_SLOT_LIST_EMPTY(Session& s) {
    // An empty payload rather than no packet: the client asks for this with
    // CZ_REQ_QUICKSLOT_LIST and waits on the answer.
    PW w = make(*s.ctx, op::ZC_QUICK_SLOT_LIST);
    w.i32v(0);                              // compressed length
    w.u8(0);
    s.send(w);
}

void ZC_RESPONSE_GUILD_INDEX(Session& s, const game::Character& c) {
    PW w = make(*s.ctx, op::ZC_RESPONSE_GUILD_INDEX);
    w.u32v(c.handle);
    w.i64v(c.object_id);
    w.i16v(1003);
    s.send(w);
}

void ZC_TRUST_INFO(Session& s) {
    PW w = make(*s.ctx, op::ZC_TRUST_INFO);
    w.zeros(20);
    w.i64v(1000000);
    w.i64v(30000000);
    w.i64v(15000000);
    w.i64v(0);
    s.send(w);
}

void ZC_MOVE_ZONE(Session& s, uint8_t state) {
    PW w = make(*s.ctx, op::ZC_MOVE_ZONE);
    w.u8(state);
    s.send(w);
}

void ZC_MOVE_ZONE_OK(Session& s, int32_t map_id) {
    PW w = make(*s.ctx, op::ZC_MOVE_ZONE_OK);
    // +0x0A is a zone id the client only stores for the reconnect, NOT the
    // destination map: the live server sent 113 here and 1021 at +0x16, and
    // the client passes +0x16 to its map-name lookup. Writing the map id at
    // +0x0A -- which is what docs/09's table said -- sends the client to the
    // wrong map with the right address.
    w.i32v(113);
    w.u32v(s.ctx->config.server_ip);
    w.i32v(s.ctx->config.zone_port);
    w.i32v(map_id);
    w.f32(45);                          // camera x
    w.f32(45);                          // camera y
    w.f32(0);                           // zoom min
    w.f32(0);                           // zoom max
    w.f32(0);                           // zoom start
    w.i32v(1630);
    w.i32v(210);
    w.i32v(-733);
    w.i16v(0);
    w.u8(1);                            // channel
    w.i64v(s.character ? s.character->object_id : 0);
    s.send(w);
}

// ---- movement, chat, combat --------------------------------------------

Bytes make_ZC_MOVE_DIR(const ServerContext& ctx, const game::Actor& a,
                       float unk_time) {
    PW w = make(ctx, op::ZC_MOVE_DIR);
    w.u32v(a.handle);
    w.position(a.position.x, a.position.y, a.position.z);
    w.direction(a.direction.cos, a.direction.sin);
    // The client mirrors its own movement flag back here: 1 while a movement
    // key is held, 0 on the frame it is released. Forcing it to 1 makes the
    // actor keep sliding after the key comes up.
    w.u8(a.moving ? 1 : 0);
    w.f32(a.move_speed);
    w.f32(unk_time);
    w.zeros(24);
    // Zone-side stance: 6 in 156 of the 164 captured request/response pairs,
    // and 0 exactly when the movement flag is clear.
    w.i32v(a.moving ? 6 : 0);
    w.i32v(0);
    w.u8(1);
    return finish(ctx, w);
}

Bytes make_ZC_PC_MOVE_STOP(const ServerContext& ctx, const game::Actor& a,
                           float unk_time) {
    PW w = make(ctx, op::ZC_PC_MOVE_STOP);
    w.u32v(a.handle);
    w.position(a.position.x, a.position.y, a.position.z);
    w.u8(1);
    w.direction(a.direction.cos, a.direction.sin);
    w.f32(unk_time);
    w.zeros(24);
    return finish(ctx, w);
}

// 26 bytes: handle, the speed, and a zero long. Confirmed against 464 live
// ZC_MSPD in capture_1785545696.bin, all of that length, all carrying the
// entity's WlkMSPD at +0x0E.
Bytes make_ZC_MSPD(const ServerContext& ctx, const game::Actor& a) {
    PW w = make(ctx, op::ZC_MSPD);
    w.u32v(a.handle);
    w.f32(a.move_speed);
    w.i64v(0);
    return finish(ctx, w);
}

Bytes make_ZC_ROTATE(const ServerContext& ctx, const game::Actor& a) {
    PW w = make(ctx, op::ZC_ROTATE);
    w.u32v(a.handle);
    w.f32(a.direction.cos);
    w.f32(a.direction.sin);
    w.u8(1);
    w.u8(1);
    w.i32v(0);
    return finish(ctx, w);
}

Bytes make_ZC_JUMP(const ServerContext& ctx, const game::Actor& a,
                   float jump_power, float unk_time) {
    PW w = make(ctx, op::ZC_JUMP);
    w.u32v(a.handle);
    w.f32(jump_power);
    w.i32v(0);                          // jump type
    w.u8(0);
    w.position(a.position.x, a.position.y, a.position.z);
    w.direction(a.direction.cos, a.direction.sin);
    w.f32(unk_time);
    w.zeros(13);
    w.i64v(0);
    w.i16v(0);
    w.u8(0);
    return finish(ctx, w);
}

Bytes make_ZC_CHAT(const ServerContext& ctx, const game::Actor& a,
                   const std::string& text) {
    const game::Character* c =
        a.is_character() ? static_cast<const game::Character*>(&a) : nullptr;

    PW w = make(ctx, op::ZC_CHAT);
    w.u32v(a.handle);
    w.str(c ? c->team_name : std::string(), 64);
    w.str(a.name, 65);
    w.u8(0);
    w.i16v(int16_t(c ? c->job_id : 1001));
    w.i32v(c ? c->job_id : 1001);
    w.u8(c ? c->gender : 0);
    w.u8(c ? c->hair : 0);
    w.zeros(2);
    w.i32v(0);
    w.i32v(1004);
    w.i32v(0);
    w.i32v(0);
    w.i32v(0);
    w.f32(0);                           // display seconds, floored at 5 client-side
    w.zeros(16);
    w.zeros(16);
    w.u8(1);
    w.str("GLOBAL", 64);                // balloon skin; puts the text at +0x115
    w.cstr(text);
    return finish(ctx, w);
}

Bytes make_ZC_UPDATE_ALL_STATUS(const ServerContext& ctx, const game::Actor& a) {
    PW w = make(ctx, op::ZC_UPDATE_ALL_STATUS);
    w.u32v(a.handle);
    w.i32v(a.hp);
    w.i32v(a.max_hp);
    w.i32v(a.sp);
    w.i32v(a.max_sp);
    w.i32v(0);                          // priority
    return finish(ctx, w);
}

Bytes make_ZC_HIT_INFO(const ServerContext& ctx, const game::Actor& attacker,
                       const game::Actor& target, int32_t damage) {
    PW w = make(ctx, op::ZC_HIT_INFO);
    w.u32v(target.handle);
    w.u32v(attacker.handle);
    w.i32v(0);                          // skill id, 0 == basic attack

    // Hit info block.
    w.i32v(damage);
    w.i32v(target.hp);
    w.i32v(0);                          // hit type
    w.i32v(0);                          // result
    w.i32v(0);
    w.i32v(0);

    w.u8(0);
    w.i32v(0);
    w.i32v(0);
    w.i32v(0);                          // force id
    w.u8(0);
    w.u8(0);
    w.f32(0);
    w.f32(0);
    w.i32v(1);                          // hit count
    w.u8(1);
    w.f32(0);
    w.i32v(0);                          // damage delay, ms
    return finish(ctx, w);
}

Bytes make_ZC_DEAD(const ServerContext& ctx, const game::Actor& a,
                   uint32_t killer) {
    PW w = make(ctx, op::ZC_DEAD);
    w.u32v(a.handle);
    w.u8(0);
    w.u8(0);                            // exp info count
    w.u8(0);                            // overkill
    w.u8(0);                            // special drop
    w.position(a.position.x, a.position.y, a.position.z);
    w.u32v(killer);
    return finish(ctx, w);
}

void ZC_CHAT(Session& s, const game::Actor& a, const std::string& text) {
    s.send_raw(make_ZC_CHAT(*s.ctx, a, text));
}

void notify(Session& s, const std::string& text) {
    if (!s.character) return;
    s.send_raw(make_ZC_CHAT(*s.ctx, *s.character, text));
}

}  // namespace tos::send
