# Identifying the player's own handle

> **SUPERSEDED.** Handles are allocated per session now, so nothing depends
> on the captured `0x000339CC`. The identification method is still how you
> read a capture. See `10`.

**Status:** confirmed
**Value in the current replay:** `0x000339CC` — "Akaichi" / team "akaichi"

## Why it matters

Every echo we synthesise has to be addressed to the PC's own handle or the
client applies it to somebody else — the character stays put while some
unrelated NPC slides across the map.

## How it was found

The handle is not in `CZ_KEYBOARD_MOVE` (the zone knows who is asking). It was
recovered from the reply side: take a `ZC_MOVE_DIR` whose position floats match
a `CZ_KEYBOARD_MOVE` we sent, and read the handle at +0x0A. Cross-checked
against `ZC_CHAT`, where the handle at +0x0C sits next to the name field
containing our character name.

## Where it appears in the replay

Searching `replay/` for the little-endian bytes `CC 39 03 00`:

| group | packet | offset |
|---|---|---|
| `CZ_CONNECT` | `ZC_CONNECT_OK` | 95 |
| `CZ_CONNECT` | `ZC_STANCE_CHANGE` | 10 |
| `CZ_CONNECT` | `ZC_SET_CHATBALLOON_SKIN` | 10 |
| `CZ_GAME_READY` | `ZC_ENTER_PC` | 10 |
| `CZ_GAME_READY` | `ZC_UPDATED_PCAPPEARANCE` | 10 |
| `CZ_GAME_READY` | `ZC_SKILL_LIST` | 12 |
| `CZ_GAME_READY` | `ZC_MOVE_SPEED` / `ZC_CASTING_SPEED` / `ZC_UPDATE_ALL_STATUS` | 10 |
| `CZ_GAME_READY` | `ZC_BUFF_LIST` | 12, 67, 135, … |

That spread also confirms the replayed world data is self-consistent — the PC
genuinely exists in it, so the client had a valid actor to control all along.

Note `ZC_MYPC_ENTER` (28 B) carries **only a position** (x, y, z floats at
+0x0A), no handle. It is not where the client learns which actor is his.

## Known-thin in our implementation

`handlers.py` hard-codes `MY_HANDLE = 0x000339CC`. This works only because the
replayed world *is* that session. It breaks the moment you:

- re-extract `replay/` from a different capture
- serve more than one client

The fix is to read the handle out of the replayed `ZC_CONNECT_OK` at +95 when
`REPLAY_SET` loads, instead of pinning the constant. Same for `PC_NAME` /
`TEAM_NAME`, which can come from `ZC_ENTER_PC`.
