# Movement is server-authoritative for your own PC

**Status:** confirmed, fix shipped in `handlers.py`; speed formula recovered
**Evidence:** `capture_1785545696.bin`, zone connection (`conn=2`, port 17003),
`shared/script/calc_property_pc.lua`

## Finding

The client does **not** move its own character on local input. It sends the
intent and waits for the zone to echo the move back *carrying the PC's own
handle*. Until that echo arrives the actor stays rooted.

This is not client-side prediction with server correction. It is a hard gate.

## Evidence

Counting matched request/response pairs on our handle `0x000339CC`:

| client sends | count | server echoes | count on our handle |
|---|---|---|---|
| `CZ_KEYBOARD_MOVE` (3166) | 176 | `ZC_MOVE_DIR` (3110) | 179 |
| `CZ_MOVE_STOP` (3172) | 5 | `ZC_PC_MOVE_STOP` (3514) | 5 |
| `CZ_ROTATE` (3184) | 5 | `ZC_ROTATE` (3145) | 5 |
| `CZ_JUMP` (3168) | 2 | `ZC_JUMP` (3116) | 2 matched pairs |

Note `ZC_JUMP` appears 7 times s2c in total; only 2 of those pair with our own
`CZ_JUMP`, the rest are other PCs. Same for `ZC_MOVE_DIR`: 295 s2c overall,
179 of them addressed to us.

The 179-vs-176 gap on `ZC_MOVE_DIR` is the server also pushing moves we did not
ask for (position corrections / knockback).

## Why the replay-only emulator failed

`tosemu_server.py` replayed the world-entry burst and then went silent. The
client rendered the whole world correctly and accepted input, but every
keypress produced a `CZ_KEYBOARD_MOVE` that was never answered, so the
character never moved. Chat had the same shape (see `03-chat-packet-layouts.md`).

Both symptoms looked like a client-side input lock. They were not — the client
was working exactly as designed and waiting on us.

## Diagnostic that settles it quickly

If movement breaks again, check the server log for inbound `CZ_KEYBOARD_MOVE`:

- **arriving** → the client is fine, our echo is wrong or missing
- **not arriving** → genuine client-side input lock, different hunt entirely

## How fast: MSPD is 35, and it is units per second

The base is **35**, scaled by a job rate that is a percentage. Not 30, and the
divisor is 100, not 10.

The client computes it itself, in `shared/script/calc_property_pc.lua` inside
`data/bg.ipf`:

```lua
function SCR_Get_MSPD(self)
    ...
    local jobRate = SCR_GET_JOB_RATIO_STAT(self, "MOVE_SPEED")
    local value   = 35.0 * jobRate
    ...
    return math.floor(value)
end

function SCR_GET_JOB_RATIO_STAT(self, prop)
    local ctrlTypeClass = GetClass("Stat_PC", jobCtrlType)   -- statbase_pc.ies
    ctrlTypeRate = TryGetProp(ctrlTypeClass, prop, 100)
    ctrlTypeRate = ctrlTypeRate / 100                        -- <- the /100
    return ctrlTypeRate
end
```

`statbase_pc.ies` has six rows — `None`, `Warrior`, `Wizard`, `Archer`,
`Cleric`, `Scout` — and `MOVE_SPEED` is 100 for five of them and **110 for
Archer**. It is **not** indexed by character level.

That reproduces `capture_1785545696.bin` exactly:

| observed MSPD | count | why |
|---|---|---|
| 35 | 147 on our own handle | `floor(35 × 100/100)` |
| 45 | 32 on our own handle | DashRun adds a flat +10 |
| 38 | 46 on other handles | `floor(35 × 110/100)` = `floor(38.5)` — an Archer |
| 41, 44, 54, 61 | 68 | gear and buffs, all additive |

A second, independent capture settles it beyond argument.
`relay/dumps/capture_20260801_214001.bin` (63,008 records, a different session)
has only four `ZC_MOVE_DIR` speeds worth counting:

| speed | count | |
|---|---|---|
| 38 | 402 | Archer, `floor(35 × 110/100)` |
| 48 | 312 | the same Archer dashing, +10 |
| 35 | 260 | a 100-rate class |
| 45 | 174 | the same, dashing, +10 |

Two base values ten apart from two class rates, and nothing else above a count
of 1. Base 35, rate `/100`, dash `+10` — all three at once.

Other constants from the same script: `PC_MAX_MSPD = 60` is the cap, DashRun is
+10 and +13 for Scout, and an over-weight character outside a city gets
`value / 3`.

Monsters use `SCR_Get_MON_MSPD` instead: `WlkMSPD` by default, `RunMSPD` only
when `MOVE_TYPE_CURRENT == 2`, and **0 stays 0** — that is how statues,
signposts and crates are marked immobile. 46 of the 122 entities in the capture
carry 0, and every one whose id resolves carries its `WlkMSPD`, never its
`RunMSPD`.

### The unit

MSPD is **world units per second**. Melia divides a distance by it to get a
travel time (`TimeSpan.FromSeconds(distance / speed)`), and the client
extrapolates the same way: a `ZC_MOVE_DIR` with the movement flag set slides
the actor at MSPD until another packet clears it.

So a server step is `speed × elapsed`, never `speed` per tick. Stepping a whole
speed per tick ran every monster at 2× on the 2 Hz loop, and tied their speed
to the tick rate.

The live zone answered these with nothing, so silence is correct:
`CZ_HEARTBEAT` (3620), `CZ_ON_AIR` (3175), `CZ_ON_GROUND` (3176),
`CZ_MOVEMENT_INFO` (3177), `CZ_DASHRUN` (3623), `CZ_CHAT_LOG` (3190).
