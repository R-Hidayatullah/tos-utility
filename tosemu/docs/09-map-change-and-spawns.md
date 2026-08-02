# Map change, warp arrows, and monster spawns

**Status:** map change confirmed; spawn pipeline confirmed; warp arrow **resolved**
**Source:** `capture_1785545696.bin` (a live map change at the tail), `CNormalNet::MoveZoneOk`

## Map change

The transition is a four-packet handshake, captured at indices 6092-6095:

```
s2c  ZC_LEAVE_HOOK      14 B
s2c  ZC_MOVE_ZONE       11 B    "prepare to leave"
c2s  CZ_MOVE_ZONE_OK    22 B    client acks
s2c  ZC_MOVE_ZONE_OK    69 B    destination
```

`ZC_MOVE_ZONE` (3003) carries a single byte at +0x0A, `0` in the capture.

`ZC_MOVE_ZONE_OK` (3007) is decoded from `CNormalNet::MoveZoneOk`
(`GENormalNet.cpp:1333`), which reads:

```c
sub_140B2C2B0(*(_DWORD *)(a2 + 10),   // +0x0A  destination map id
              *(_DWORD *)(a2 + 14),   // +0x0E  zone server IP
              *(_DWORD *)(a2 + 18),   // +0x12  zone server port
              *(_DWORD *)(a2 + 22),
              *(unsigned __int8 *)(a2 + 60));
...
v29 = HIBYTE(*(_DWORD *)(a2 + 14));      // the IP is formatted byte by byte
sub_1400DABF0(Buffer, 0x20, "%d.%d.%d.%d");
connect(v7, Buffer, *(unsigned __int16 *)(a2 + 18), ...);
```

| offset | field |
|---|---|
| +0x0A | u32 zone id (stored for the reconnect; **not** the map) |
| +0x0E | u32 zone server IP (formatted `%d.%d.%d.%d` from its bytes) |
| +0x12 | u16 zone server port |
| +0x16 | u32 **destination map id** |
| +0x1A | 5 floats: camera x/y, zoom min/max/start |
| +0x2E | 3 ints |
| +0x3C | u16 direction, u8 channel |
| +0x3F | u64 character object id |

**Corrected.** This table originally read +0x0A as the destination map id. It
is not. The capture has `113` at +0x0A and `1021` (`f_siauliai_west`) at +0x16,
and `sub_140B2C2B0` passes its fourth argument -- `*(_DWORD *)(a2 + 22)`, i.e.
+0x16 -- to the map-name lookup, while +0x0A is only stored in a global
alongside the address. Writing the map id at +0x0A sends the client to the
right server and the wrong map.

In the capture: zone `0x71`, IP `36 B4 BE 77` = `54.180.190.119`, port `0x1B61`
= 7009, map `1021`.

**So a map change is a reconnect.** The client tears down the zone socket and
replays `CZ_CONNECT` → `CZ_GAME_READY` → `CZ_LOAD_COMPLETE` against the address
in the packet. Pointing it at our own IP and port makes the transition local,
which is what the C++ server does.

## The green arrow: it is an ordinary spawn

**Answered.** The arrow is a `ZC_ENTER_MONSTER` entity like any other. Its
class is `warp_arrow`, monster id **40001**, and it reaches the map through the
GenType pipeline below rather than through the anchor's `NPCID` column — which
is exactly why it was invisible to the earlier reading.

Three of them are in `capture_1785545696.bin`, at Klaipeda anchor positions
`(-201, -1182)`, `(223, -95)` and `(-2, -290)`. Resolving those anchors by
`GenType` yields 40001 for all three; resolving them by `NPCID` yields 40070,
which is a different NPC entirely.

The last note in this section previously guessed that the empty-`Name`
GenType rows were "candidates for non-NPC markers". That was the right hunch
for the wrong reason: the marker is not a special row type, it is a normal
generator row whose `ClassType` happens to be `warp_arrow`.

The earlier ruling-out was sound as far as it went — `Warp_arrow` at
`0x1416BF518` really is the client-side quest guidance effect, and it is a
different thing from this.

## Monster and NPC spawns

### Where the data lives

`data/ies_mongen.ipf` — 711 files, one `anchor_<map>.ies` per map. Extracted
with the tools in `tos-utility/tosmole-cpp`:

```bash
ipf_tool extract .../ies_mongen.ipf 3 anchor_c_Klaipe.ies
```

```bash
ies_dump anchor_c_Klaipe.ies
```

Schema (46 rows for Klaipeda, 9 columns):

| column | decl | meaning |
|---|---|---|
| `ClassID` | 0 | spawn entry id |
| `Name` | 0 | display name (Korean) |
| `GenType` | 1 | generator type |
| `PosX` `PosY` `PosZ` | 2,3,4 | spawn position |
| `Direction` | 5 | facing |
| `NPCID` | 6 | which NPC/monster |
| `AnchorRange` | 7 | wander radius |

### The anchor is only half of it

An anchor says **where**, not **what**. The `what` lives in a second table,
`ies_mongen/gentype_<map>.ies`, keyed by `ClassID` — the same number the
anchor's `GenType` column carries:

| column | meaning |
|---|---|
| `ClassID` | joins to the anchor's `GenType` |
| `ClassType` | the monster/NPC **ClassName**, e.g. `npc_illanai` |
| `Name` | the display name this spawn shows |
| `Dialog` `Enter` `Leave` | the scripts the client runs on click/enter/leave |
| `RespawnTime` | milliseconds |
| `MaxPop` `GenRange` `Range` `Lv` `Faction` | population and behaviour |

**`GenType` is the key, not `NPCID`.** Where both resolve they disagree more
often than they agree (70 vs 31 across five maps), and the capture settles it:
of the Klaipeda anchors whose position matches a captured spawn, GenType names
the right monster **20 times out of 20**, NPCID **8 out of 19**. Whole maps have
no usable NPCID at all — every one of `c_Orsha`'s 91 anchors carries 0.

Resolving through GenType takes the five maps sampled from 107 usable anchors
to 388 of 510.

The `Dialog`/`Enter`/`Leave` strings are the last three of the five 256-byte
names in `ZC_ENTER_MONSTER`. Sending them empty leaves every NPC unclickable,
because the dialog script the client runs is named here and nowhere else.

### NPCs are in a different file

`ies/monster.ies` (4288 rows) is the hostile world. Town NPCs, props and
session objects are in **`ies/monster_npc.ies`** (2016 rows), and no id appears
in both. Reading only the first left every anchored NPC unresolvable: 96 of the
122 live spawns in the Klaipeda capture come from the NPC table.

The `Faction` column does not identify them — `monster_npc.ies` is 93%
`Neutral` and `monster.ies` has 137 `Neutral` rows of its own. Which file a row
came from is the reliable signal; `Faction == "Monster"` is what marks
something that actually takes aggro.

**Corrected.** This section previously warned that anchor coordinates "have not
been reconciled with the capture" because Klaipeda anchors read `PosY` around
149-156 against a captured y of 241.1. There is no frame mismatch — that was
the mis-ordered column read the `ies_dump` tool produced. Read with the columns
sorted by declaration index, Klaipeda anchors span y = -1.4 .. 248.2 against a
captured -1.3 .. 241.2, and 33 of 99 anchor XZ pairs land exactly on a captured
spawn. The anchor positions are packet-ready.

Related client strings: `gentype_%s.xml`, `GEN_POINT_LIST`, `GEN_BY_NAME`,
`GEN_MONSTER_LIFE_TIME`.

### The wire format

`ZC_ENTER_MONSTER` (3102, variable), from `EnterMonster`
(`geCommonPacket_World.cpp`):

| offset | field |
|---|---|
| +0x0C | u32 handle |
| +0x10 | float x, y, z |
| +0x1C | float dir cos |
| +0x20 | float dir sin |
| +0x24 | u8 relation — **always 2**, u8 from-ground |
| +0x26 | u32 hp, u32 max hp, u64 shield |
| +0x36 | float move speed |
| +0x3A | u32 monster id, then level / SDR / GenType |
| +0x5E | **five 256-byte names**: name, unique, dialog, enter, leave |
| +0x55E | u16 property bytes, then the property stream |

Positions read straight out of the capture and match what was on screen:

```
handle=00000667 pos=( -585.0, 241.1,  812.0)  KLAPEDA_FISHING_CAT
handle=000334D1 pos=( -764.0, 217.9,  700.0)  FISHING_BAG
handle=00033062 pos=( -560.0, 241.1,  548.0)
```

Every live `ZC_ENTER_MONSTER` in the capture is exactly **1403 bytes**, all 128
of them.

**Corrected.** The block at +0x5E is not a property blob -- it is five
fixed-width 256-byte name strings, which is exactly the 1280 bytes the client
reads. That misreading is why spawning originally needed a captured packet as a
template. The C++ server now builds `ZC_ENTER_MONSTER` from the client tables
directly, and the whole packet closes cleanly against the recovered layout.

The relation byte at +0x24 is **2 for everything**, town NPCs included: all 122
unique spawns in the capture carry 2, 96 of them NPC-table entries. Sending 0
for NPCs was a guess that never appeared on the wire.

## Driving it

The C++ server claims two chat commands (see `10-cpp-server.md`):

- `/map <id>` — runs the `ZC_MOVE_ZONE` handshake
- `/spawn` — places monster templates around the player
