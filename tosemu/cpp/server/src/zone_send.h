// Server -> client packet builders.
//
// Every one of these writes its fields in the order the client reads them, and
// PacketWriter checks the finished length against the size the client declares
// for that opcode. Where packet_schema.json has a recovered layout, the field
// boundaries are checked too -- so a packet that assembles at the right total
// length but with a field split wrong still gets caught.
//
// Layouts were taken from a reference implementation and then verified against
// the client-extracted schema; the comments record where the two disagreed.
#pragma once

#include <string>
#include <vector>

#include "server.h"

namespace tos::send {

// ---- barrack -----------------------------------------------------------

void BC_LOGIN_PACKET_RECEIVED(Session& s);
void BC_DISCONNECT_PACKET_LOG_COUNT(Session& s);
void BC_IES_MODIFY_LIST(Session& s);
void BC_LOGINOK(Session& s, const Account& acc);
void BC_MESSAGE(Session& s, const std::string& text);
void BC_COMMANDER_LIST(Session& s, const Account& acc);
// BC_NORMAL sub-packets. The client keys these off a leading u32 sub-opcode.
void BC_NORMAL_ZoneTraffic(Session& s, const Account& acc);
void BC_NORMAL_TeamUI(Session& s, const Account& acc);
void BC_NORMAL_CharacterInfo(Session& s, const Account& acc);
void BC_NORMAL_SetPosition(Session& s, uint8_t index, const game::Position& p);
void BC_BARRACKNAME_CHANGE(Session& s, int32_t result, const std::string& name);
void BC_BARRACKNAME_CHECK_RESULT(Session& s, int32_t result,
                                 const std::string& name,
                                 const std::string& message);
void BC_COMMANDER_CREATE_SLOTID(Session& s, uint8_t character_count);
void BC_COMMANDER_CREATE(Session& s, const game::Character& c);
void BC_COMMANDER_DESTROY(Session& s, uint8_t slot);
void BC_START_GAMEOK(Session& s, const game::Character& c);
void BC_SERVER_ENTRY(Session& s);

// ---- social / chat link ------------------------------------------------

void SC_LOGIN_OK(Session& s);
void SC_NORMAL_LoginSuccess(Session& s);
void SC_NORMAL_Unknown02(Session& s);
void SC_NORMAL_LikedList(Session& s);
void SC_NORMAL_LikedMeList(Session& s);
void SC_NORMAL_RelatedPcSession(Session& s);

// ---- zone: connection and entry ---------------------------------------

void ZC_CONNECT_OK(Session& s, const game::Character& c);
void ZC_START_GAME(Session& s);
void ZC_MYPC_ENTER(Session& s, const game::Character& c);
void ZC_ENTER_PC(Session& s, const game::Character& c);
void ZC_ENTER_MONSTER(Session& s, const game::Monster& m,
                      bool from_ground = false);
void ZC_LEAVE(Session& s, uint32_t handle);
void ZC_OBJECT_PROPERTY(Session& s, const game::Character& c);
void ZC_ITEM_INVENTORY_LIST(Session& s, const game::Character& c);
void ZC_ITEM_EQUIP_LIST(Session& s, game::Character& c);
void ZC_ITEM_ADD(Session& s, const game::Item& it, uint8_t add_type);
void ZC_ITEM_REMOVE(Session& s, int64_t object_id, int32_t amount);
Bytes make_ZC_UPDATED_PCAPPEARANCE(const ServerContext& ctx,
                                   const game::Character& c);
void ZC_SKILL_LIST(Session& s, const game::Character& c);
void ZC_SKILL_ADD(Session& s, const game::Character& c, const game::Skill& sk);
void ZC_SKILL_CAST_CANCEL(Session& s, const game::Character& c);
void ZC_SET_CHATBALLOON_SKIN(Session& s);
void ZC_NORMAL_AdventureBook(Session& s);
void ZC_NORMAL_Unknown1B4(Session& s);
void ZC_STANCE_CHANGE(Session& s, const game::Character& c);
void ZC_QUICK_SLOT_LIST(Session& s);
void ZC_LOGOUT_OK(Session& s);
void ZC_MOVE_BARRACK(Session& s);
Bytes make_ZC_POSE(const ServerContext& ctx, const game::Actor& a, int32_t pose);
Bytes make_ZC_REST_SIT(const ServerContext& ctx, const game::Actor& a,
                       bool sitting);
// The world-entry burst the client expects after CZ_GAME_READY. Several of
// these are empty lists, but the client waits on them before it will leave the
// loading screen and start accepting input.
void ZC_LOAD_COMPLETE(Session& s);
void ZC_IES_MODIFY_LIST(Session& s);
void ZC_OPTION_LIST(Session& s);
void ZC_SKILLMAP_LIST(Session& s);
void ZC_CHAT_MACRO_LIST(Session& s);
void ZC_MAP_REVEAL_LIST(Session& s);
void ZC_NPC_STATE_LIST(Session& s);
void ZC_HELP_LIST(Session& s);
void ZC_SESSION_OBJECTS(Session& s);
void ZC_MYPAGE_MAP(Session& s);
void ZC_GUESTPAGE_MAP(Session& s);
void ZC_LOGIN_TIME(Session& s);
void ZC_START_INFO(Session& s, const game::Character& c);
// Without this the skill window stays empty no matter what ZC_SKILL_LIST says:
// it carries the job data the client uses to decide which skills to show.
void ZC_NORMAL_UpdateSkillUI(Session& s, const game::Character& c);
void ZC_QUICK_SLOT_LIST_EMPTY(Session& s);
void ZC_RESPONSE_GUILD_INDEX(Session& s, const game::Character& c);
void ZC_TRUST_INFO(Session& s);
void ZC_MOVE_ZONE(Session& s, uint8_t state);
void ZC_MOVE_ZONE_OK(Session& s, int32_t map_id);

// ---- zone: movement and chat -------------------------------------------

// These broadcast rather than target one session, because everyone on the map
// has to see them; the moving player included, since the client does not move
// its own actor until the zone echoes the move back (docs/01).
Bytes make_ZC_MOVE_DIR(const ServerContext& ctx, const game::Actor& a,
                       float unk_time);
Bytes make_ZC_PC_MOVE_STOP(const ServerContext& ctx, const game::Actor& a,
                           float unk_time);
Bytes make_ZC_ROTATE(const ServerContext& ctx, const game::Actor& a);
// Announces a changed MSPD. The client keeps its own copy and extrapolates
// movement from it, so a speed that changes server-side without this desyncs.
Bytes make_ZC_MSPD(const ServerContext& ctx, const game::Actor& a);
Bytes make_ZC_JUMP(const ServerContext& ctx, const game::Actor& a,
                   float jump_power, float unk_time);
Bytes make_ZC_LEAVE(const ServerContext& ctx, uint32_t handle);
// `from_ground` is the +0x25 byte: 1 for a spawn that appears while the player
// is already on the map (a respawn, a /spawn), 0 for everything handed over in
// the map-entry presence exchange.
Bytes make_ZC_ENTER_MONSTER(const ServerContext& ctx, const game::Monster& m,
                            bool from_ground = false);
Bytes make_ZC_ENTER_PC(const ServerContext& ctx, const game::Character& c);
Bytes make_ZC_CHAT(const ServerContext& ctx, const game::Actor& a,
                   const std::string& text);
Bytes make_ZC_UPDATE_ALL_STATUS(const ServerContext& ctx, const game::Actor& a);
Bytes make_ZC_HIT_INFO(const ServerContext& ctx, const game::Actor& attacker,
                       const game::Actor& target, int32_t damage);
Bytes make_ZC_DEAD(const ServerContext& ctx, const game::Actor& a,
                   uint32_t killer);
// The visible skill effect. `hits` is written as a count only; the damage
// numbers travel in their own ZC_HIT_INFO, which is how the live server does
// it for the single-target case.
Bytes make_ZC_SKILL_MELEE_GROUND(const ServerContext& ctx,
                                 const game::Actor& caster, int32_t skill_id,
                                 const game::Position& target_pos);

void ZC_CHAT(Session& s, const game::Actor& a, const std::string& text);
// Command feedback. Goes out as chat from the player's own actor rather than
// as ZC_MESSAGE, because the message packet's body is a 36-byte parameter
// block the client parses against a string table we have not recovered.
void notify(Session& s, const std::string& text);

}  // namespace tos::send
