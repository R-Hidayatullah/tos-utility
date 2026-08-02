"""Extract server responses from a live capture into replay/.

Walks every connection in the capture and groups each run of server packets
under the client packet that triggered it, keyed by opcode name:

    CB_LOGIN          -> BC_LOGIN_PACKET_RECEIVED, BC_LOGINOK, ...
    CB_START_BARRACK  -> BC_SERVER_ENTRY, BC_COMMANDER_LIST, ...
    CZ_CONNECT        -> ZC_CONNECT_OK, ZC_SESSION_OBJECTS, ZC_SKILL_LIST, ...

Only the first occurrence of each group is kept. Packets are stored verbatim,
including sequence 0xFFFFFFFF and checksum 0 -- what the live server sends and
the client accepts.

The barrack response and the world-entry burst are largely generic IES
property streams rather than structs, so replaying real bytes gets a working
client far faster than reconstructing them field by field.

    python extract_replay.py capture_*.bin
"""

import json
import os
import shutil
import sys

from read_capture import read, names

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "replay")

# Only these client packets start a new group. The zone is not request/reply
# -- after CZ_GAME_READY the server pushes the entire world-entry burst
# unprompted, so intermediate client packets (CZ_HEARTBEAT, CZ_REQUEST_*)
# must NOT reset the group or the burst gets split across them.
#
# CB_LOGIN_BY_PASSPORT is the Steam variant of CB_LOGIN; our emulator only
# ever sees CB_LOGIN, so the captured Steam login folds into that group.
TRIGGERS = {
    3: "CB_LOGIN",
    4: "CB_LOGIN",
    6: "CB_START_BARRACK",
    12: "CB_START_GAME",
    3001: "CZ_CONNECT",
    3006: "CZ_GAME_READY",
    3681: "CZ_LOAD_COMPLETE",
}

# CZ_LOAD_COMPLETE is a trigger, not a stop: the client stays locked in the
# loading state until the server answers with ZC_LOAD_COMPLETE. Everything
# after that is steady-state gameplay, so cap the group rather than pulling
# in the rest of the session.
CAP = {"CZ_LOAD_COMPLETE": 24}
STOP_AT = None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    _, recs = read(sys.argv[1])
    NM = names()

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    conns = {}
    for r in recs:
        conns.setdefault(r.conn, []).append(r)

    groups, n = {}, 0
    for conn in sorted(conns):
        cur = None
        for r in conns[conn]:
            nm = NM.get(r.opcode, "op%d" % r.opcode)
            if r.direction == 0:                       # client -> server
                if STOP_AT and r.opcode == STOP_AT:
                    break                              # handshake over
                g = TRIGGERS.get(r.opcode)
                if g is not None:
                    cur = None if g in groups else g   # keep first only
                    if cur:
                        groups[cur] = []
                continue                               # non-trigger: keep cur
            if cur is None:
                continue
            if len(groups[cur]) >= CAP.get(cur, 1 << 30):
                cur = None
                continue
            fn = "%03d_%s_%s_%d.bin" % (n, cur, nm, r.opcode)
            with open(os.path.join(OUT, fn), "wb") as f:
                f.write(r.body)
            groups[cur].append({"file": fn, "opcode": r.opcode,
                                "name": nm, "len": len(r.body)})
            n += 1

    # Chat/social connections are plaintext in BOTH directions, so our
    # capture proxy (which parses client->server as Blowfish) never decoded
    # their CS_* packets -- those connections show up as server-only. Grab
    # the first one wholesale as the CHAT group.
    for conn in sorted(conns):
        v = conns[conn]
        if "CHAT" in groups or not v or v[0].direction != 1:
            continue
        if not any(x.opcode >= 15901 for x in v):
            continue
        groups["CHAT"] = []
        for r in v:
            if r.direction != 1:
                continue
            nm = NM.get(r.opcode, "op%d" % r.opcode)
            fn = "%03d_CHAT_%s_%d.bin" % (n, nm, r.opcode)
            with open(os.path.join(OUT, fn), "wb") as f:
                f.write(r.body)
            groups["CHAT"].append({"file": fn, "opcode": r.opcode,
                                   "name": nm, "len": len(r.body)})
            n += 1
        break

    groups = {k: v for k, v in groups.items() if v}    # drop empty groups
    with open(os.path.join(OUT, "index.json"), "w", encoding="utf-8") as f:
        json.dump(groups, f, indent=1)

    print("extracted %d packets into %d groups -> %s\n" % (n, len(groups), OUT))
    for g, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        total = sum(i["len"] for i in items)
        print("%-34s %3d packets  %7d B" % (g, len(items), total))
        for it in items[:6]:
            print("      %-34s %6d B" % (it["name"], it["len"]))
        if len(items) > 6:
            print("      ... %d more" % (len(items) - 6))


if __name__ == "__main__":
    main()
