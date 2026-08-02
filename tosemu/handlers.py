"""Packet handlers -- HOT-RELOADED on every inbound packet.

Edit this file while the client is connected; changes take effect on the next
packet with no server restart and no client relaunch. A syntax error here is
caught by the server, logged, and the previous version keeps serving.

`ctx` is owned by the server and survives reloads, so ctx.seq, ctx.team_name
and ctx.account_id persist. Adding a NEW ctx attribute only applies to the
next connection -- use ctx.state (a dict) for anything you add here.

Everything below is derived from CBarrackNet::Process / CModeNet::LoginOK.
"""

import json
import os
import struct
import time

from tos_proto import first_string, hexdump, name_of

# ---------------------------------------------------------------- replay
#
# Byte-exact server packets captured from a live session (extract_replay.py).
# The barrack response is a generic IES property stream, not a struct, so
# replaying real bytes beats reconstructing them field by field. Decode
# incrementally afterwards, against a barrack that actually renders.
#
# Set REPLAY = False to fall back to the synthesised packets below.

REPLAY = True
REPLAY_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "replay")

# Our own zone endpoint -- captured packets carry the live server's addresses
# and must be repointed or the client will dial the real zone.
ZONE_IP, ZONE_PORT = (127, 0, 0, 1), 7002


def _repoint(op, b):
    """Rewrite zone addresses in captured BC_SERVER_ENTRY / BC_START_GAMEOK."""
    b = bytearray(b)
    if op == 27 and len(b) >= 37:            # ip @+0x0E, u32 port @+0x12
        b[0x0E:0x12] = bytes(ZONE_IP)
        struct.pack_into("<I", b, 0x12, ZONE_PORT)
    elif op == 74 and len(b) >= 22:          # 2x ip @+0x0A, u16 ports @+0x12
        for i in range(2):
            b[0x0A + i * 4:0x0E + i * 4] = bytes(ZONE_IP)
            struct.pack_into("<H", b, 0x12 + i * 2, ZONE_PORT)
    return bytes(b)


def load_replay():
    path = os.path.join(REPLAY_DIR, "index.json")
    if not os.path.exists(path):
        return {}
    groups = json.load(open(path, encoding="utf-8"))
    out = {}
    for group, items in groups.items():
        seq = []
        for it in items:
            with open(os.path.join(REPLAY_DIR, it["file"]), "rb") as f:
                seq.append((it["opcode"], _repoint(it["opcode"], f.read())))
        out[group] = seq
    return out


REPLAY_SET = load_replay() if REPLAY else {}


def serve_replay(ctx, group):
    """Send a captured response group once. Returns True if it was ours.

    Groups are one-shot per connection: the client sends CZ_LOAD_COMPLETE four
    times during map entry, and re-dumping its 18 ZC_MOVE_STOP each time would
    keep yanking every visible PC back to its captured position.
    """
    seq = REPLAY_SET.get(group)
    if not seq:
        return False
    done = ctx.state.setdefault("replayed", set())
    if group in done:
        return True
    done.add(group)
    total = sum(len(b) for _, b in seq)
    if len(seq) > 8:
        ctx.log("RPLY [%s] %d packets, %d bytes" % (group, len(seq), total))
        for _, body in seq:
            ctx.send_raw(body, quiet=True)
    else:
        for _, body in seq:
            ctx.send_raw(body, "[replay %s]" % group)
    return True

# ---- client -> server
CB_LOGIN, CB_LOGOUT, CB_START_BARRACK = 3, 5, 6
CB_START_GAME, CB_CHECK_CLIENT_INTEGRITY, CB_ECHO = 12, 10, 18
CB_BARRACKNAME_CHECK, CB_BARRACKNAME_CHANGE = 13, 15
CB_NOT_AUTHORIZED_ADDON_LIST, CB_OS_INFO = 84, 98
CB_SELECTED_LANGUAGE, CB_CURRENT_BARRACK = 104, 59
CZ_CONNECT, CZ_GAME_READY = 3001, 3006
CB_COMMANDER_CREATE, CB_COMMANDER_MOVE = 7, 16
CB_COMMANDER_DESTROY, CB_REQ_CHANNEL_TRAFFIC = 9, 55

# ---- server -> client
BC_LOGINOK, BC_LOGOUTOK, BC_COMMANDER_LIST = 19, 22, 23
BC_START_GAMEOK, BC_BARRACKNAME_CHANGE = 27, 32
BC_COMMANDER_CREATE_SLOTID, BC_COMMANDER_CREATE = 8, 25
ZC_CONNECT_OK, ZC_CONNECT_FAILED = 3002, 3005

# ---- steady state, after the world-entry burst
#
# The PC's own movement is server-authoritative: the client sends its intent
# and does NOT move the actor until the zone echoes the move back carrying the
# PC's own handle. The capture is unambiguous -- 176 CZ_KEYBOARD_MOVE in, 179
# ZC_MOVE_DIR out on our handle, and 5:5 for both stop and rotate. A server
# that only replays the entry burst and then goes quiet leaves the character
# rooted in place, which is exactly what a replay-only emulator does.
#
# Chat is the same shape: the text the client types only appears in the window
# once ZC_CHAT comes back from the zone.
CZ_KEYBOARD_MOVE, CZ_MOVE_STOP, CZ_JUMP = 3166, 3172, 3168
CZ_ROTATE, CZ_CHAT, CZ_CHAT_LOG = 3184, 3188, 3190
CZ_ON_AIR, CZ_ON_GROUND, CZ_MOVEMENT_INFO = 3175, 3176, 3177
CZ_HEARTBEAT, CZ_LOAD_COMPLETE, CZ_DASHRUN = 3620, 3681, 3623
ZC_MOVE_DIR, ZC_JUMP, ZC_CHAT = 3110, 3116, 3124
ZC_ROTATE, ZC_PC_MOVE_STOP, ZC_START_GAME = 3145, 3514, 3014

# The replayed world is one specific session, so our PC is its PC: handle
# 0x000339CC ("Akaichi"), which appears in ZC_CONNECT_OK+95 and ZC_ENTER_PC+10.
# Everything we echo has to be addressed to that handle or the client will
# apply it to somebody else.
MY_HANDLE = 0x000339CC
PC_NAME, TEAM_NAME = b"Akaichi", b"akaichi"

MSPD = 35.0          # ZC_MOVE_DIR+0x22 throughout the capture
JUMP_POWER = 350.0   # ZC_JUMP+0x0E, constant across both captured jumps


def _time_base():
    """Server clock the replayed ZC_START_GAME hands the client (+0x0E).

    Every movement echo carries a timestamp on this clock. In the capture the
    first move sits ~10s past the value ZC_START_GAME published, so we anchor
    to that same float and add wall time rather than inventing a scale.
    """
    for op, body in REPLAY_SET.get("CZ_GAME_READY", []):
        if op == ZC_START_GAME and len(body) >= 18:
            return struct.unpack_from("<f", body, 14)[0]
    return 0.0


TIME_BASE = _time_base()
_T0 = time.monotonic()


def server_time():
    return TIME_BASE + (time.monotonic() - _T0)

# packets that are informational only -- receive and ignore
SILENT = {CB_ECHO, CB_CHECK_CLIENT_INTEGRITY, CZ_GAME_READY,
          CB_NOT_AUTHORIZED_ADDON_LIST, CB_OS_INFO, CB_SELECTED_LANGUAGE,
          CB_CURRENT_BARRACK, CB_COMMANDER_MOVE, CB_REQ_CHANNEL_TRAFFIC,
          # steady-state chatter the live zone also answered with nothing
          CZ_HEARTBEAT, CZ_ON_AIR, CZ_ON_GROUND, CZ_MOVEMENT_INFO,
          CZ_DASHRUN, CZ_CHAT_LOG}

# CB_COMMANDER_CREATE (7, 117 bytes) field offsets, from the live capture
CREATE_SLOT, CREATE_NAME, CREATE_JOB = 0x16, 0x17, 0x58


# --------------------------------------------------------------- builders

def bc_loginok(ctx):
    p = bytearray(156)
    struct.pack_into("<H", p, 0, BC_LOGINOK)
    struct.pack_into("<H", p, 10, 0)                    # channel
    struct.pack_into("<Q", p, 12, ctx.account_id)
    p[20:20 + len(ctx.team_name)] = ctx.team_name       # team_name[56]
    struct.pack_into("<I", p, 76, 0)
    struct.pack_into("<i", p, 144, 0)
    struct.pack_into("<Q", p, 148, 0)
    return bytes(p)


def bc_commander_list(ctx):
    """Empty list: 98-byte fixed part, m=0, then trailing u16/u32/u16."""
    size = 108
    p = bytearray(size)
    struct.pack_into("<H", p, 0, BC_COMMANDER_LIST)
    struct.pack_into("<H", p, 10, size)                 # TOTAL length
    struct.pack_into("<Q", p, 12, ctx.account_id)
    p[20] = 0
    p[21] = 0                                           # char_count
    struct.pack_into("<H", p, 86, 0)                    # prop_blob_len
    struct.pack_into("<H", p, 88, 0)
    struct.pack_into("<H", p, 90, 0)
    struct.pack_into("<I", p, 92, 0)
    p[96] = 0
    p[97] = 1                                           # gates Barrack vs Visit
    struct.pack_into("<H", p, 98, 0)                    # m = 0
    struct.pack_into("<H", p, 100, 0)
    struct.pack_into("<I", p, 102, 0)
    struct.pack_into("<H", p, 106, 0)
    return bytes(p)


def bc_barrackname_change(ctx, name, ok=True, error=0):
    """Commits the team name -- Process case 32."""
    p = bytearray(79)
    struct.pack_into("<H", p, 0, BC_BARRACKNAME_CHANGE)
    p[10] = 1 if ok else 0
    struct.pack_into("<I", p, 11, error)
    p[15:15 + min(len(name), 63)] = name[:63]
    return bytes(p)


def bc_commander_create_slotid(ctx, slot):
    """Assigns a real slot to the client's pending (slot -1) commander.

    Process case 8: finds the commander whose slot is -1 and writes the byte
    at +10 into it. The client's CB_COMMANDER_CREATE carries 0xFF as its
    placeholder slot, which is that -1.
    """
    p = bytearray(11)
    struct.pack_into("<H", p, 0, BC_COMMANDER_CREATE_SLOTID)
    p[10] = slot & 0xFF
    return bytes(p)


def bc_commander_create(ctx, slot, extra=0):
    """CBarrackNet::CommanderCreate looks the commander up by slot at +538.

    The client already holds the name/class it just sent, so this packet only
    has to name the slot -- most of the 608-byte record can stay zero until we
    need it for BC_COMMANDER_LIST on a re-login.
    """
    p = bytearray(618)
    struct.pack_into("<H", p, 0, BC_COMMANDER_CREATE)
    p[538] = slot & 0xFF
    struct.pack_into("<H", p, 542, extra)
    return bytes(p)


def bc_start_gameok(ctx, server_index=1):
    p = bytearray(37)
    struct.pack_into("<H", p, 0, BC_START_GAMEOK)
    struct.pack_into("<I", p, 22, 0)                    # map id
    struct.pack_into("<Q", p, 27, ctx.account_id)
    p[35] = 0                                           # 0 -> do connect
    p[36] = server_index & 0xFF
    return bytes(p)


def zc_connect_ok(ctx):
    size = 64
    p = bytearray(size)
    struct.pack_into("<H", p, 0, ZC_CONNECT_OK)
    struct.pack_into("<H", p, 10, size)
    p[17] = 1                                           # zone id
    p[50] = 1                                           # full init
    return bytes(p)


def bc_logoutok(ctx):
    p = bytearray(10)
    struct.pack_into("<H", p, 0, BC_LOGOUTOK)
    return bytes(p)


# ------------------------------------------------------- movement echoes
#
# Field offsets below are read straight off matched request/response pairs in
# capture_1785545696.bin. The client packets carry no handle (the zone knows
# who is asking); the echoes insert ours and otherwise pass the same floats
# back, which is why these are slices rather than unpack/repack.

def zc_move_dir(ctx, cz):
    """CZ_KEYBOARD_MOVE (73) -> ZC_MOVE_DIR (76).

    x,y,z,dx,dz sit at CZ+0x16 and come back at ZC+0x0E. The client's own
    +0x2A float is dropped; the zone substitutes the authoritative speed,
    which lands at +0x23 after a single pad byte.
    """
    p = bytearray(76)
    struct.pack_into("<H", p, 0, ZC_MOVE_DIR)
    struct.pack_into("<I", p, 10, MY_HANDLE)
    p[0x0E:0x22] = cz[0x16:0x2A]
    p[0x22] = cz[0x48]                           # moving flag, mirrored 1:1
    struct.pack_into("<f", p, 0x23, MSPD)
    struct.pack_into("<f", p, 0x27, server_time())
    # +0x43 is zone-side stance, not a function of the request: 6 for 156 of
    # the 164 captured pairs, 1 for the rest, 0 only when the moving flag is
    # clear. Take the common case.
    p[0x43] = 6 if cz[0x48] else 0
    p[75] = 1
    return bytes(p)


def zc_pc_move_stop(ctx, cz):
    """CZ_MOVE_STOP (71) -> ZC_PC_MOVE_STOP (63).

    CZ_MOVE_STOP carries one more leading pad byte than CZ_KEYBOARD_MOVE, so
    its position starts at +0x17, not +0x16.
    """
    p = bytearray(63)
    struct.pack_into("<H", p, 0, ZC_PC_MOVE_STOP)
    struct.pack_into("<I", p, 10, MY_HANDLE)
    p[0x0E:0x1A] = cz[0x17:0x23]                 # x,y,z
    p[0x1A] = 1
    p[0x1B:0x23] = cz[0x23:0x2B]                 # dx,dz
    struct.pack_into("<f", p, 0x23, server_time())
    return bytes(p)


def zc_rotate(ctx, cz):
    """CZ_ROTATE (34) -> ZC_ROTATE (28). Both direction floats pass through."""
    p = bytearray(28)
    struct.pack_into("<H", p, 0, ZC_ROTATE)
    struct.pack_into("<I", p, 10, MY_HANDLE)
    p[0x0E:0x16] = cz[0x1A:0x22]
    p[0x16] = 1
    return bytes(p)


def zc_jump(ctx, cz):
    """CZ_JUMP (71) -> ZC_JUMP (71). Body offsets line up; only the head differs."""
    p = bytearray(71)
    struct.pack_into("<H", p, 0, ZC_JUMP)
    struct.pack_into("<I", p, 10, MY_HANDLE)
    struct.pack_into("<f", p, 0x0E, JUMP_POWER)
    p[0x17:0x2B] = cz[0x17:0x2B]                 # x,y,z,dx,dz
    struct.pack_into("<f", p, 0x2B, server_time())
    return bytes(p)


# ------------------------------------------------------------------ chat
#
# ZC_CHAT is a 277-byte fixed part followed by NUL-terminated text; the u16 at
# +10 is the resulting total. The trailing balloon-skin field is char[64] at
# +0xD5, which is what puts the text at 277.

CHAT_TEXT_AT = 0xD5 + 64


def zc_chat(ctx, text):
    size = CHAT_TEXT_AT + len(text) + 1
    p = bytearray(size)
    struct.pack_into("<H", p, 0, ZC_CHAT)
    struct.pack_into("<H", p, 10, size)
    struct.pack_into("<I", p, 12, MY_HANDLE)
    p[16:16 + len(PC_NAME)] = PC_NAME            # name[64]
    p[80:80 + len(TEAM_NAME)] = TEAM_NAME        # team name[64]
    struct.pack_into("<H", p, 0x92, 0x07D3)      # class / job ids
    struct.pack_into("<H", p, 0x94, 0x07D1)
    p[0x98] = 8                                  # chat channel: say
    p[0x9C] = 1
    p[0x9E] = 0x1A
    struct.pack_into("<I", p, 0xB4, 0x03E9)
    struct.pack_into("<I", p, 0xB8, 0x01CC)
    p[0xBC:0xC0] = b"\x80\x80\x80\xFF"           # name colour RGBA
    struct.pack_into("<H", p, 0xC0, 0x07D1)
    struct.pack_into("<H", p, 0xC2, 0x07D2)
    struct.pack_into("<H", p, 0xC4, 0x07D3)
    p[0xD4] = 1
    p[0xD5:0xD5 + 6] = b"GLOBAL"                 # balloon skin[64]
    p[CHAT_TEXT_AT:CHAT_TEXT_AT + len(text)] = text
    return bytes(p)


# --------------------------------------------------------------- dispatch

def handle(ctx, op, plain):
    if op == CB_LOGIN:
        ident = plain[0x16:0x16 + 56].split(b"\x00")[0]
        ctx.log("  login id=%r" % ident)

    # Chat/social link: plaintext both ways, and the client blocks its map
    # load until it completes. Serve the captured chat burst once, on the
    # first packet of the connection.
    if getattr(ctx, "framing", None) == "plain":
        if not ctx.state.get("chat_done"):
            ctx.state["chat_done"] = True
            serve_replay(ctx, "CHAT")
        return

    # Generic replay: if we captured a response group for this opcode, serve
    # it verbatim. Covers CB_LOGIN, CB_START_BARRACK, CB_START_GAME,
    # CZ_CONNECT and CZ_GAME_READY (the world-entry burst).
    if serve_replay(ctx, name_of(op)):
        return

    if op == CB_LOGIN:
        ctx.send(bc_loginok(ctx))

    elif op == CB_START_BARRACK:
        ctx.send(bc_commander_list(ctx))

    elif op in (CB_BARRACKNAME_CHECK, CB_BARRACKNAME_CHANGE):
        name, at = first_string(plain)
        ctx.log("  team name %r (at +%d)" % (name, at))
        ctx.log("\n" + hexdump(plain, 80))
        if name:
            ctx.team_name = name
        ctx.send(bc_barrackname_change(ctx, name))
        # Do NOT re-send BC_COMMANDER_LIST here: CommanderList clears the
        # account name (sub_1400B3D30 on account+16) and our empty property
        # blob leaves it blank, so the client re-opens the name prompt.

    elif op == CB_COMMANDER_CREATE:
        name = plain[CREATE_NAME:CREATE_NAME + 64].split(b"\x00")[0]
        job = struct.unpack_from("<H", plain, CREATE_JOB)[0]
        slot = ctx.state.get("next_slot", 1)
        ctx.state["next_slot"] = slot + 1
        ctx.state.setdefault("chars", []).append(
            {"slot": slot, "name": name, "job": job})
        ctx.log("  create %r job=%d -> slot %d" % (name, job, slot))
        ctx.send(bc_commander_create_slotid(ctx, slot))
        ctx.send(bc_commander_create(ctx, slot))

    elif op == CB_START_GAME:
        if not serve_replay(ctx, "CB_START_GAME"):
            ctx.send(bc_start_gameok(ctx))

    elif op == CB_LOGOUT:
        ctx.send(bc_logoutok(ctx))

    elif op == CZ_CONNECT:
        ctx.send(zc_connect_ok(ctx))

    elif op == CZ_KEYBOARD_MOVE:
        ctx.send(zc_move_dir(ctx, plain))

    elif op == CZ_MOVE_STOP:
        ctx.send(zc_pc_move_stop(ctx, plain))

    elif op == CZ_ROTATE:
        ctx.send(zc_rotate(ctx, plain))

    elif op == CZ_JUMP:
        ctx.send(zc_jump(ctx, plain))

    elif op == CZ_CHAT:
        text = plain[24:].split(b"\x00")[0]
        # Slash commands are addon RPC, not speech -- the live zone answered
        # 21 CZ_CHAT with only 4 ZC_CHAT, and every echoed one was plain text.
        if text and not text.startswith(b"/"):
            ctx.log("  chat %r" % text)
            ctx.send(zc_chat(ctx, text))

    elif op in SILENT:
        pass

    else:
        # One dump per opcode per connection: the steady-state client repeats
        # CZ_HEARTBEAT and friends forever and would bury everything else.
        seen = ctx.state.setdefault("seen_ops", {})
        seen[op] = seen.get(op, 0) + 1
        if seen[op] == 1:
            ctx.log("  no handler")
            ctx.log("\n" + hexdump(plain))
        elif seen[op] % 50 == 0:
            ctx.log("  no handler (x%d)" % seen[op])
