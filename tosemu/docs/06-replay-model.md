# Replay model: grouping, one-shot, and where it runs out

> **SUPERSEDED.** The server no longer replays anything; every packet is
> constructed. This is kept for the capture-analysis technique and for the
> record of what the replay could and could not do. See `10`.

**Status:** confirmed
**Source:** `extract_replay.py`, `replay/index.json`, `capture_1785545696.bin`

## How grouping works

`extract_replay.py` walks each connection and files every run of server packets
under the client packet that triggered it. Only these client opcodes start a
new group; everything else the client sends is passed over so a burst does not
get split:

| opcode | group |
|---|---|
| 3, 4 | `CB_LOGIN` |
| 6 | `CB_START_BARRACK` |
| 12 | `CB_START_GAME` |
| 3001 | `CZ_CONNECT` |
| 3006 | `CZ_GAME_READY` |
| 3681 | `CZ_LOAD_COMPLETE` |

Resulting sizes: `CZ_GAME_READY` 611 packets, `CZ_LOAD_COMPLETE` 24,
`CHAT` 32, the rest under 10.

The zone is **not** request/reply — after `CZ_GAME_READY` the server pushes the
entire world-entry burst unprompted.

## Groups must be one-shot per connection

The client sends `CZ_LOAD_COMPLETE` **four times** during map entry
(capture indices 692, 767, 812, 864). The original `serve_replay` had no guard,
so each one re-dumped the group's 18 `ZC_MOVE_STOP` and yanked every visible PC
back to its captured position.

`handlers.py` now tracks served groups in `ctx.state["replayed"]` and returns
`True` without re-sending.

## The CZ_LOAD_COMPLETE cap is safe

`extract_replay.py` caps that group at 24 packets, because the client stays
locked in the loading state until the server answers and everything past it is
steady-state gameplay that would otherwise pull in the whole session.

Checked: `ZC_LOAD_COMPLETE` is packet **#1** of the group. The cap truncates
only `ZC_MSPD` / `ZC_PLAY_ANI` / `ZC_MOVE_SPEED` chatter from #25 on. Nothing
needed for world entry is lost.

## Address repointing

Captured packets carry the live server's addresses and must be rewritten or the
client dials the real zone. `handlers._repoint` patches:

- `BC_START_GAMEOK` (27): ip at +0x0E, u32 port at +0x12
- `BC_SERVER_ENTRY` (74): two ips at +0x0A, u16 ports at +0x12

## Where the replay model runs out

Replay gets the client into a rendered world; it cannot answer anything the
client asks afterwards. Everything in steady state has to be synthesised —
that is what `01-server-authoritative-movement.md` is about.

Client packets seen in steady state with no handler yet:
`CZ_CHANGE_CONFIG` (56×), `CZ_REQUEST_GUILD_INDEX` (36×), `CZ_CUSTOM_COMMAND`
(20×), `CZ_QUEST_NPC_STATE_CHECK` (17×), `CZ_SKILL_GROUND`, `CZ_SKILL_SELF`,
`CZ_SKILL_TARGET_ANI`, `CZ_DYNAMIC_CASTING_START` / `_END`, `CZ_MOVE_ZONE_OK`.

The live server *did* answer several of these (`ZC_RESPONSE_GUILD_INDEX`,
`ZC_SKILL_READY`, `ZC_UPDATE_SP`, `ZC_SYNC_START` / `ZC_SYNC_END`), so skills
are the obvious next thing to break and the capture already has the pairs.

## Debugging aid

Unhandled opcodes log one hexdump each per connection, then a count every 50.
Before that, steady-state chatter buried everything else in the log.
