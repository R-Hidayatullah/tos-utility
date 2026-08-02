# tosemu research notes

Protocol findings reversed from `Client_tos_x64.exe` and live captures, written
up as they were established. Each file states what was **confirmed** versus
**inferred**, and what evidence backs it.

| # | finding | status |
|---|---|---|
| [01](01-server-authoritative-movement.md) | Movement is server-authoritative for your own PC | confirmed |
| [02](02-movement-packet-layouts.md) | Movement packet layouts (move / stop / rotate / jump) | verified byte-exact |
| [03](03-chat-packet-layouts.md) | Chat packet layouts (`CZ_CHAT`, `ZC_CHAT`) | verified byte-exact |
| [04](04-pc-handle.md) | Identifying the player's own handle | confirmed; obsolete now handles are allocated |
| [05](05-server-time-base.md) | Server time base from `ZC_START_GAME` | confirmed, and `13` explains what it breaks |
| [06](06-replay-model.md) | Replay grouping, one-shot rule, limits | obsolete — the replay is gone |
| [07](07-variable-packet-size-field.md) | Variable-size field is not always at +0x0A | confirmed; cause explained in `11` |
| [08](08-struct-extraction.md) | Recovering packet structs from the client | pipeline built, 121 structs, server→client only |
| [09](09-map-change-and-spawns.md) | Map change, warp arrows, monster spawns | spawn pipeline **corrected**; warp arrow resolved |
| [10](10-cpp-server.md) | The C++ server | a real server: barrack, characters, zone, movement, skills, items, combat, maps |
| [11](11-packet-framing.md) | Headers, length-prefixed strings, sizes, routing | four rules, all confirmed against real bytes |
| [12](12-client-data.md) | Reading the client's own IPF/IES data | working: maps, monsters, items, skills, stances, spawns |
| [13](13-session-flow.md) | What the client waits for, in order | driven end to end against the real client |
| [14](14-field-names.md) | Naming packet fields, and the fields panel | 228 of 504 named; the rest stay `f_XX` |
| [15](15-running-the-proxy-and-server.md) | **Running the proxy and the server** | operational guide: capture, run, read |

## The headline

A replay-only emulator got the client into a fully rendered world where the
character could not move and chat did nothing. That looked like a client-side
input lock. It was not — ToS gates the PC's own movement on a server echo
(`01`) and chat text on `ZC_CHAT` coming back (`03`). The client was working
correctly and waiting on us the whole time.

That turned out to be the pattern for the whole protocol, and it is what `13`
is about: **the client almost never reports an error.** When something is
missing it sits still or retries forever, and the symptom shows up nowhere near
the cause.

| what you see | what is actually missing |
|---|---|
| stuck on "loading world" | the two social links' `CS_LOGIN` reply, or `ZC_NORMAL` after `ZC_CONNECT_OK` |
| in the world, cannot move or use skills | `ZC_LOAD_COMPLETE` |
| walking is far too fast, like teleporting | `ZC_START_GAME`'s clock base |
| character stands in a T-pose | stance 0 — the animation set was never resolved |
| skill window empty | `ZC_NORMAL` UpdateSkillUI, which carries the job data |
| jump key does nothing | the `JumpPower` property |
| character creation hangs | `BC_COMMANDER_CREATE_SLOTID` |
| "Select Channel" empty, Start Game does nothing | `BC_NORMAL` ZoneTraffic |
| no monsters or NPCs anywhere | nothing — you landed in an empty corner of the map |

## The replay is gone

The old "known-thin" list was mostly consequences of one thing: the server
replayed captured bursts, so the captured session's identity was baked into
everything. The C++ server (`10`) constructs every packet instead and reads the
world from the installed client's own `.ipf` data, so the hard-coded handle,
the single-sender chat echo and the missing skills are all resolved.

## Findings that turned out to be wrong

Building against these notes is how the errors surfaced. Each is corrected in
place, with the evidence:

1. **`ZC_MOVE_ZONE_OK` carries the map id at +0x16**, not +0x0A (`09`). The old
   reading sent the client to the right server and the wrong map.
2. **Client packets have a 22-byte header** (`11`). `07` found the symptom —
   the inline size at +0x16 — without the cause.
3. **`ZC_ENTER_MONSTER+0x5E` is five 256-byte name strings**, not "a 1280-byte
   property blob" (`09`). That misreading is why spawning originally needed a
   captured packet as a template.
4. **Spawns resolve through `GenType`, not the anchor's `NPCID`** (`09`).
   Against the capture, GenType is right 20 times out of 20 and NPCID 8 out of
   19 — and NPCs live in a second table the server was not reading at all.
5. **The town anchor coordinate frame was never wrong** (`09`, `12`). The
   apparent mismatch was a mis-ordered column read in the dump tool.

A fourth was never written down because it was never suspected: **length-
prefixed strings count their terminating NUL** (`11`). It produces packets of
exactly the right total length with every following field shifted by one byte,
so both the size check and the field-offset check pass.

## Still thin

- **No `CZ_*` structs** (`08`). Struct extraction covers server→client only;
  the client builds outbound packets rather than parsing them, so client
  layouts still come from capture diffing.
- **No persistence** (`10`). Accounts and characters are in memory; everything
  is gone on restart.
- **Items have no effect, combat is a stub, no experience, no quests, no
  warps** (`10`).
- **Spawns are resolved but not watched land in-game.** The GenType pipeline
  (`09`) is verified against the capture, not against a running client.

## Primary sources

- `capture_1785545696.bin` — full session, 7887 records. Zone is `conn=2`
  (port 17003, 6096 records); chat is `conn=6` (port 17004).
- `packet_opcodes.csv` — 1259 opcodes with sizes; 0 means variable.
- `packet_schema.json` — 121 server→client layouts recovered from the client's
  own parsing handlers.
- The installed game's `.ipf` archives — 1220 of them, 472,768 files.

## Running any of it

See [`15`](15-running-the-proxy-and-server.md) — capturing live traffic with the
relay or the proxy, running the emulator, and reading the dumps back.

Useful one-liners:

```bash
python read_capture.py capture_1785545696.bin ZC_MOVE_DIR
```

```bash
python gen_properties.py --verify
```

```bash
python test_server.py
```
