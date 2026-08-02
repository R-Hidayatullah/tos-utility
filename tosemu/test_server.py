"""Drive full sessions against a running server and check what comes back.

    python test_server.py [host]

Nothing here is replayed from a capture: every client packet is built from the
opcode table and the layouts in docs/, which is the point -- a server that only
answered recorded bytes would not be a server. The run covers what a player
actually does:

    barrack   log in, ask for the lodge, create a character, enter the game
    social    the two plaintext links the client blocks its map load on
    zone      connect, load, walk, stop, turn, jump
    chat      speak, and be heard by a second session
    skills    the skill list arrives, and a skill can be used
    items     starting gear, and silver that stacks
    combat    spawn monsters, hit one, kill it, and get hit back
    options   sit, emote, give
    maps      change map and re-enter through the new address
    leaving   back to the lodge, and logout

Exit code is non-zero if any check fails.

Two framing rules, worth stating once rather than rediscovering per packet:

  * barrack and zone packets carry 12 bytes past the 10-byte header, so their
    body starts at +0x16, and a variable one puts its size there and starts at
    +0x18.
  * social packets have no such extra header: body and size both at +0x0A.
"""

import os
import socket
import struct
import sys
import time
import zlib

import tos_proto as P

NM = P.NAMES
BY_NAME = {v: k for k, v in NM.items()}

C2S_BODY = 22


def op(name):
    if name not in BY_NAME:
        raise SystemExit("unknown opcode name: " + name)
    return BY_NAME[name]


def build(name, body=b"", variable=False):
    """A barrack/zone client packet: header, 12 ignored bytes, then the body."""
    total = 10 + 12 + (2 if variable else 0) + len(body)
    pkt = bytearray(total)
    struct.pack_into("<H", pkt, 0, op(name))
    if variable:
        struct.pack_into("<H", pkt, 0x16, total)
        pkt[0x18:] = body
    else:
        pkt[C2S_BODY:] = body
    struct.pack_into("<I", pkt, 6, 0)
    struct.pack_into("<I", pkt, 6, P.checksum(pkt))
    return bytes(pkt)


def pad_to(body, size):
    """Pad a body out to the size the client's own table declares."""
    return body + bytes(max(0, size - C2S_BODY - len(body)))


def fixed(name, body=b""):
    size = P.SIZES.get(op(name), 0)
    return build(name, pad_to(body, size) if size else body)


def frame(plain):
    pad = (-len(plain)) % 8
    enc = P.bf_encrypt(plain + bytes(pad))
    return struct.pack("<H", len(enc)) + enc


class Client:
    def __init__(self, host, port, tag):
        self.tag = tag
        self.s = socket.create_connection((host, port), timeout=5)
        self.buf = b""
        self.seen = []

    def send(self, plain):
        self.s.sendall(frame(plain))

    def pump(self, seconds=0.7):
        self.s.settimeout(0.2)
        end = time.time() + seconds
        while time.time() < end:
            try:
                b = self.s.recv(65535)
                if not b:
                    break
                self.buf += b
            except socket.timeout:
                pass
            except OSError:
                break
            while len(self.buf) >= 10:
                size = P.packet_size(self.buf[:12])
                if not size or size > len(self.buf):
                    break
                self.seen.append(self.buf[:size])
                self.buf = self.buf[size:]
        return self.seen

    def of(self, name):
        want = op(name)
        return [p for p in self.seen
                if struct.unpack_from("<H", p, 0)[0] == want]

    def clear(self):
        self.seen = []

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


FAILS = []


def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        FAILS.append(msg)


def silver_amount(inventory_packet):
    """Pull the Vis (900011) stack size out of a ZC_ITEM_INVENTORY_LIST.

    Silver is the regression test for integer clamping: its MaxStack is 5e9,
    which overflows int32 and used to collapse the stack to a single coin.
    """
    marker = bytes((0x8D, 0xFA))
    i = inventory_packet.find(marker)
    if i < 0:
        return 0
    n = struct.unpack_from("<I", inventory_packet, i + 2)[0]
    body = zlib.decompress(inventory_packet[i + 6:i + 6 + n], -15)
    off = 0
    while off + 32 <= len(body):
        item_id = struct.unpack_from("<i", body, off)[0]
        psize = struct.unpack_from("<H", body, off + 4)[0]
        amount = struct.unpack_from("<i", body, off + 16)[0]
        if item_id == 900011:
            return amount
        off += 32 + psize
    return 0


# ---- client packet builders --------------------------------------------

def cb_login(account):
    body = account.encode().ljust(56, bytes(1))
    body += bytes(16)                        # password hash
    body += bytes(3)
    body += struct.pack("<I", 0x0100007F)
    body += bytes(405)
    body += b"GLOBAL".ljust(64, bytes(1))
    return build("CB_LOGIN", body)


def cb_start_barrack():
    # byte origin + char[64] service nation, closing the declared 87 bytes.
    return build("CB_START_BARRACK", bytes(1) + b"GLOBAL".ljust(64, bytes(1)))


def cb_commander_create(name, job=1001, gender=1, hair=1):
    body = struct.pack("<B", 1)
    body += name.encode().ljust(65, bytes(1))
    body += struct.pack("<H", job)
    body += struct.pack("<B", gender)
    body += struct.pack("<fff", 0, 0, 0)     # barrack position
    body += struct.pack("<i", 0)             # lodge
    body += struct.pack("<i", 1)             # start map preset
    body += struct.pack("<H", hair)
    body += struct.pack("<I", 0xFFFFFFFF)    # skin colour
    return build("CB_COMMANDER_CREATE", body)


def cb_start_game(index=1):
    return build("CB_START_GAME", struct.pack("<HBB", 1, index, 0))


def cs_login(account):
    """Social link: no extra header, and padded to the declared size or the
    server waits for bytes that never arrive."""
    body = account.encode().ljust(56, bytes(1))
    body += bytes(16)                        # password hash
    body += bytes(1)
    body += struct.pack("<q", 1)             # account id
    body += bytes(256)                       # session key
    size = P.SIZES.get(op("CS_LOGIN"), 0)
    total = max(size, 10 + len(body))
    pkt = bytearray(total)
    struct.pack_into("<H", pkt, 0, op("CS_LOGIN"))
    pkt[10:10 + len(body)] = body
    struct.pack_into("<I", pkt, 6, 0)
    struct.pack_into("<I", pkt, 6, P.checksum(pkt))
    return bytes(pkt)


def cz_connect(account, object_id):
    body = bytes(1024)
    body += b"key".ljust(64, bytes(1))
    body += account.encode().ljust(56, bytes(1))
    body += bytes(48)                        # mac
    body += struct.pack("<qq", 0, 0)
    body += struct.pack("<q", 1)             # account id
    body += struct.pack("<q", object_id)     # character object id
    body += struct.pack("<iii", 0, 0, 0)
    body += struct.pack("<hhh", 0, 0, 0)
    body += bytes((1, 0, 0, 0, 0))
    return build("CZ_CONNECT", body)


def cz_move(x, y, z, dx=1.0, dz=0.0, moving=1):
    body = bytearray(pad_to(struct.pack("<fffff", x, y, z, dx, dz), 73))
    body[0x48 - C2S_BODY] = moving
    return build("CZ_KEYBOARD_MOVE", bytes(body))


def cz_move_stop(x, y, z, dx=1.0, dz=0.0):
    body = bytes(1) + struct.pack("<fffff", x, y, z, dx, dz)
    return build("CZ_MOVE_STOP", pad_to(body, 71))


def cz_rotate(dx, dz):
    return build("CZ_ROTATE", pad_to(bytes(4) + struct.pack("<ff", dx, dz), 34))


def cz_jump(x, y, z):
    body = bytes(1) + struct.pack("<fffff", x, y, z, 1, 0)
    return build("CZ_JUMP", pad_to(body, 71))


def cz_chat(text):
    return build("CZ_CHAT", text.encode() + bytes(1), variable=True)


def cz_skill_target(skill_id, target_handle):
    body = struct.pack("<BiiB", 0, skill_id, target_handle, 0)
    return build("CZ_SKILL_TARGET", pad_to(body, 33))


def cz_empty(name):
    return fixed(name)


def chat_text(packet):
    """ZC_CHAT puts the message at +0x115, past the balloon-skin char[64]."""
    return packet[0x115:].split(bytes(1))[0]


# ---- flow ---------------------------------------------------------------

def barrack_login(host, port, account, char_name, job=1001):
    c = Client(host, port, "barrack")
    c.send(cb_login(account))
    c.pump(0.8)
    check(len(c.of("BC_LOGINOK")) > 0, "barrack: BC_LOGINOK for '%s'" % account)

    # The client asks for the character list separately, once the barrack
    # scene has loaded -- BC_LOGINOK alone does not carry it.
    c.clear()
    c.send(cb_start_barrack())
    c.pump(0.8)
    check(len(c.of("BC_COMMANDER_LIST")) > 0, "barrack: character list arrived")
    # Without the channel table "Select Channel" is empty and Start Game does
    # nothing.
    check(len(c.of("BC_NORMAL")) >= 3, "barrack: channel and team blocks sent")

    c.clear()
    c.send(cb_commander_create(char_name, job=job))
    c.pump(0.8)
    check(len(c.of("BC_COMMANDER_CREATE")) > 0,
          "barrack: character '%s' created" % char_name)
    # The client waits for its slot assignment before placing the new
    # character in the lodge; without it creation hangs on the last screen.
    check(len(c.of("BC_COMMANDER_CREATE_SLOTID")) > 0,
          "barrack: slot id returned for the new character")
    check(len(c.of("BC_COMMANDER_CREATE")) == 1,
          "barrack: the new character is announced exactly once")

    # Coming back to the lodge re-lists, and now has a map to build the
    # channel table from.
    c.clear()
    c.send(cb_start_barrack())
    c.pump(0.8)
    check(len(c.of("BC_COMMANDER_LIST")) > 0, "barrack: lodge re-lists")
    check(len(c.of("BC_NORMAL")) >= 3, "barrack: channel table after creation")

    c.clear()
    c.send(cb_start_game(1))
    c.pump(0.8)
    ok = c.of("BC_START_GAMEOK")
    check(len(ok) > 0, "barrack: BC_START_GAMEOK returned a zone address")

    object_id = None
    if ok:
        p = ok[0]
        ip, zport, map_id = struct.unpack_from("<III", p, 0x0E)
        object_id = struct.unpack_from("<q", p, 0x1B)[0]
        print("        zone %d.%d.%d.%d:%d map=%d objectId=%d"
              % (ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, (ip >> 24) & 255,
                 zport, map_id, object_id))
    c.close()
    return object_id


def social_login(host, port, account, label):
    """Plaintext and unframed, so this is a raw socket rather than a Client."""
    sock = socket.create_connection((host, port), timeout=5)
    sock.sendall(cs_login(account))
    sock.settimeout(1.5)
    got = b""
    end = time.time() + 1.5
    while time.time() < end:
        try:
            b = sock.recv(65535)
        except OSError:
            break
        if not b:
            break
        got += b
    sock.close()

    ops, off = [], 0
    while off + 10 <= len(got):
        o = struct.unpack_from("<H", got, off)[0]
        size = P.SIZES.get(o, 0) or struct.unpack_from("<H", got, off + 10)[0]
        if size < 10 or off + size > len(got):
            break
        ops.append(NM.get(o, str(o)))
        off += size
    print("        %s(%d) -> %s" % (label, port, ops))
    check("SC_LOGIN_OK" in ops, "social: %s link answered CS_LOGIN" % label)


def enter_zone(host, port, account, object_id, tag):
    c = Client(host, port, tag)
    c.send(cz_connect(account, object_id))
    c.pump(1.0)
    check(len(c.of("ZC_CONNECT_OK")) > 0, "%s: ZC_CONNECT_OK" % tag)
    # Stance precedes ZC_CONNECT_OK on the live server; stance 0 is a T-pose.
    check(len(c.of("ZC_STANCE_CHANGE")) > 0, "%s: stance sent on connect" % tag)
    # The client will not send CZ_GAME_READY until these arrive, so without
    # them the map load never starts.
    check(len(c.of("ZC_NORMAL")) >= 2, "%s: connect normal-ops sent" % tag)

    # The commander block's offset depends entirely on the session key's
    # length prefix, so decoding the handle and name back out of it is a
    # direct test that lpstr counts the terminating NUL. Get the handle from
    # here rather than by scanning for a plausible value.
    handle = None
    for p in c.of("ZC_CONNECT_OK"):
        off = 10 + 2 + 1 + 4 + 1 + 10 + 4 + 2 + 4 + 10 + 3
        key_len = struct.unpack_from("<H", p, off)[0]
        off += 2 + key_len
        handle = struct.unpack_from("<I", p, off)[0]
        name = p[off + 8:off + 8 + 65].split(bytes(1))[0]
        print("        %s: commander at +%d handle=0x%X name=%s"
              % (tag, off, handle, name.decode(errors="replace")))
        check(0x00600000 <= handle < 0x00700000 and name != b"",
              "%s: commander block lands at the right offset" % tag)
        break
    check(handle is not None, "%s: got a handle" % tag)

    c.clear()
    c.send(cz_empty("CZ_GAME_READY"))
    c.pump(1.2)
    starts = c.of("ZC_START_GAME")
    check(len(starts) > 0, "%s: ZC_START_GAME" % tag)
    if starts:
        # +0x0E and +0x12 are the client's CLOCK BASE, and movement timestamps
        # are on the same clock. Sending 1 here while moves carry the real
        # server time makes the client think minutes have elapsed and
        # extrapolate the character across the map -- it reads as teleporting.
        scale, base, gbase = struct.unpack_from("<fff", starts[0], 0x0A)
        print("        clock: scale=%.1f base=%.1f" % (scale, base))
        check(scale == 1.0, "%s: client time scale is 1" % tag)
        check(base > 1.0, "%s: clock base is a real server time" % tag)
    check(len(c.of("ZC_MYPC_ENTER")) > 0, "%s: ZC_MYPC_ENTER" % tag)
    check(len(c.of("ZC_OBJECT_PROPERTY")) > 0, "%s: stats sent" % tag)

    # ZC_SKILL_LIST alone leaves the skill window empty: the client builds it
    # from the job data in ZC_NORMAL UpdateSkillUI, which must arrive first.
    check(len(c.of("ZC_NORMAL")) >= 1, "%s: skill UI job data sent" % tag)

    skills = c.of("ZC_SKILL_LIST")
    check(len(skills) > 0, "%s: skill list sent" % tag)
    if skills:
        count = struct.unpack_from("<H", skills[0], 16)[0]
        print("        %s: %d skills" % (tag, count))
        check(count > 0, "%s: skill list is not empty" % tag)

    inv = c.of("ZC_ITEM_INVENTORY_LIST")
    check(len(inv) > 0, "%s: inventory sent" % tag)
    if inv:
        n = struct.unpack_from("<i", inv[0], 12)[0]
        silver = silver_amount(inv[0])
        print("        %s: %d items carried, %d silver" % (tag, n, silver))
        check(n > 0, "%s: starting items present" % tag)
        check(silver >= 1000,
              "%s: silver stacks (MaxStack 5e9 must not wrap)" % tag)
    check(len(c.of("ZC_ITEM_EQUIP_LIST")) > 0, "%s: equipment sent" % tag)

    c.clear()
    c.send(cz_empty("CZ_LOAD_COMPLETE"))
    c.pump(1.2)
    # A real client may never send CZ_LOAD_COMPLETE, so the fallback trigger
    # has to work too: a heartbeat must be enough to become visible.
    c.send(cz_empty("CZ_HEARTBEAT"))
    c.pump(0.5)
    return c, handle


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    barrack_port, zone_port = 2000, 7002

    # Accounts live in memory for the server's lifetime, and the map-change
    # check leaves its character somewhere else. Fresh names per run keep the
    # test independent of whatever a previous run left behind.
    tag = "%04x" % (os.getpid() & 0xFFFF)
    acc_a, acc_b = "alice" + tag, "bob" + tag

    print("== barrack ==")
    oid_a = barrack_login(host, barrack_port, acc_a, "Alice")
    oid_b = barrack_login(host, barrack_port, acc_b, "Bob")
    if oid_a is None or oid_b is None:
        print("\nbarrack login failed, stopping")
        return 1

    # The client opens both of these and will not finish loading the world
    # until each has answered.
    print("\n== social ==")
    social_login(host, 9001, acc_a, "chat")
    social_login(host, 9002, acc_a, "relation")

    print("\n== zone entry ==")
    a, ha = enter_zone(host, zone_port, acc_a, oid_a, "A")
    b, hb = enter_zone(host, zone_port, acc_b, oid_b, "B")
    print("        A handle=%s  B handle=%s"
          % (hex(ha) if ha else "?", hex(hb) if hb else "?"))
    check(ha is not None and hb is not None and ha != hb,
          "each session got its own handle")

    a.pump(0.8)
    check(len(a.of("ZC_ENTER_PC")) > 0, "A was told about B entering")

    print("\n== movement ==")
    a.clear(); b.clear()
    a.send(cz_move(-599, 260, -1377))
    a.pump(0.6); b.pump(0.6)
    own = [p for p in a.of("ZC_MOVE_DIR")
           if struct.unpack_from("<I", p, 10)[0] == ha]
    check(len(own) > 0, "walk: echoed back on A's own handle")
    if own:
        # MSPD sits at +0x23. statbase_pc's MOVE_SPEED is a PERCENTAGE (100,
        # or 110 for a fast class), so sending it raw put this at 100 against
        # a base of 30 and the character sprinted. The live server sends 35
        # for an unbuffed character.
        mspd = struct.unpack_from("<f", own[0], 0x23)[0]
        # The move timestamp must sit on the same clock as ZC_START_GAME's
        # base, or the client extrapolates the gap as elapsed movement.
        stamp = struct.unpack_from("<f", own[0], 0x27)[0]
        print("        MSPD = %.1f  (live sends 35), move stamp = %.1f"
              % (mspd, stamp))
        check(20 <= mspd <= 60, "walk: move speed is in the live range")
        check(stamp > 1.0, "walk: move timestamp is on the server clock")
    if own:
        # MSPD sits at +0x23. statbase_pc's MOVE_SPEED is a percentage, so
        # sending it raw put this at 100 against a base of 30 and the
        # character sprinted. The live server sends 35 unbuffed.
        mspd = struct.unpack_from("<f", own[0], 0x23)[0]
        print("        MSPD = %.1f" % mspd)
        check(20 <= mspd <= 60, "walk: move speed is in the live range")
    check(any(struct.unpack_from("<I", p, 10)[0] == ha
              for p in b.of("ZC_MOVE_DIR")), "walk: B saw A move")

    a.clear()
    a.send(cz_move_stop(-590, 260, -1370))
    a.pump(0.5)
    check(len(a.of("ZC_PC_MOVE_STOP")) > 0, "stop: echoed back")

    a.clear()
    a.send(cz_rotate(0.0, 1.0))
    a.pump(0.5)
    check(len(a.of("ZC_ROTATE")) > 0, "turn: echoed back")

    a.clear()
    a.send(cz_jump(-590, 260, -1370))
    a.pump(0.5)
    jumps = a.of("ZC_JUMP")
    check(len(jumps) > 0, "jump: echoed back")
    if jumps:
        # The client gates its own jump on the JumpPower property, so an echo
        # with 0 here means the key does nothing on screen.
        power = struct.unpack_from("<f", jumps[0], 0x0E)[0]
        print("        jump power = %.0f" % power)
        check(power > 0, "jump: jump power is set")

    print("\n== chat ==")
    a.clear(); b.clear()
    a.send(cz_chat("hello from Alice"))
    a.pump(0.6); b.pump(0.6)
    check(len(a.of("ZC_CHAT")) > 0, "chat: echoed back to the speaker")
    check(len(b.of("ZC_CHAT")) > 0, "chat: B heard it")

    print("\n== skills ==")
    a.clear()
    a.send(cz_chat("/skills"))
    a.pump(0.6)
    line = b""
    for p in a.of("ZC_CHAT"):
        line = chat_text(p)
    print("        %s" % line.decode(errors="replace"))
    check(b"skills:" in line and b"none" not in line,
          "skills: the character knows some")


    skill_id = 0
    if b"skills:" in line:
        for tok in line.decode(errors="replace").split():
            digits = tok.split("(")[0]
            if digits.isdigit():
                skill_id = int(digits)
                break

    print("\n== combat ==")
    a.clear(); b.clear()
    a.send(cz_chat("/spawn 57001 4"))
    a.pump(1.0); b.pump(0.5)
    mobs = a.of("ZC_ENTER_MONSTER")
    check(len(mobs) > 0, "spawn: monsters announced")
    check(len(b.of("ZC_ENTER_MONSTER")) > 0, "spawn: B saw them too")

    target = None
    if mobs:
        target = struct.unpack_from("<I", mobs[0], 12)[0]
        print("        target handle = 0x%X, skill = %d" % (target, skill_id))

    if target:
        a.clear()
        for _ in range(30):
            a.send(cz_skill_target(skill_id, target))
            a.pump(0.35)
            if a.of("ZC_DEAD"):
                break
        check(len(a.of("ZC_SKILL_MELEE_GROUND")) > 0, "combat: skill effect sent")
        check(len(a.of("ZC_HIT_INFO")) > 0, "combat: damage dealt")
        check(len(a.of("ZC_UPDATE_ALL_STATUS")) > 0, "combat: target hp updated")
        check(len(a.of("ZC_DEAD")) > 0, "combat: target died")

    print("\n== aggro ==")
    a.clear()
    a.pump(6.0)
    check(len(a.of("ZC_HIT_INFO")) > 0, "aggro: a monster attacked the player")

    print("\n== world tick ==")
    a.clear()
    a.pump(3.0)
    check(len(a.of("ZC_MOVE_DIR")) > 0, "tick: monsters wander or chase")

    print("\n== options ==")
    a.clear()
    a.send(build("CZ_REST_SIT", pad_to(b"", 22)))
    a.pump(0.6)
    check(len(a.of("ZC_REST_SIT")) > 0, "sit toggles")

    a.clear()
    a.send(build("CZ_POSE", pad_to(struct.pack("<ifff", 1, 0, 0, 0), 47)))
    a.pump(0.6)
    check(len(a.of("ZC_POSE")) > 0, "emote broadcast")

    # Dragging a character around the lodge has to be acknowledged or the
    # client will not let go of it.
    lodge = Client(host, barrack_port, "lodge")
    lodge.send(cb_login(acc_a)); lodge.pump(0.7)
    lodge.send(cb_start_barrack()); lodge.pump(0.7)
    lodge.clear()
    body = struct.pack("<Bfffff", 1, 10, 0, -20, 1, 0)
    lodge.send(build("CB_COMMANDER_MOVE", pad_to(body, 43)))
    lodge.pump(0.7)
    check(len(lodge.of("BC_NORMAL")) > 0, "lodge: character move acknowledged")
    lodge.close()

    a.clear()
    a.send(cz_chat("/give Drug_HP2 5"))
    a.pump(0.6)
    check(len(a.of("ZC_ITEM_ADD")) > 0, "items: /give adds an item")

    print("\n== map change ==")
    a.clear()
    a.send(cz_chat("/map 1001"))
    a.pump(0.8)
    check(len(a.of("ZC_MOVE_ZONE")) > 0, "map: server asked the client to leave")

    a.clear()
    a.send(cz_empty("CZ_MOVE_ZONE_OK"))
    a.pump(0.8)
    ok = a.of("ZC_MOVE_ZONE_OK")
    check(len(ok) > 0, "map: destination returned")
    if ok:
        map_id = struct.unpack_from("<I", ok[0], 0x16)[0]
        print("        destination map id at +0x16 = %d" % map_id)
        check(map_id == 1001, "map: destination is the map we asked for")

    # The real client reconnects here, so do the same.
    a.close()
    c, hc = enter_zone(host, zone_port, acc_a, oid_a, "A2")
    c.pump(0.8)
    check(hc is not None, "map: re-entered on the new map")

    print("\n== leaving ==")
    c.clear()
    c.send(build("CZ_MOVE_BARRACK", pad_to(bytes(1), 31)))
    c.pump(0.7)
    check(len(c.of("ZC_MOVE_BARRACK")) > 0, "back to lodge acknowledged")
    c.close()

    d, _ = enter_zone(host, zone_port, acc_a, oid_a, "A3")
    d.clear()
    d.send(build("CZ_LOGOUT", pad_to(bytes(1), 23)))
    d.pump(0.7)
    check(len(d.of("ZC_LOGOUT_OK")) > 0, "logout acknowledged")
    d.close()
    b.close()

    print("\n%d checks failed" % len(FAILS))
    for f in FAILS:
        print("  - " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
