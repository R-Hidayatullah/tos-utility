# Server time base

**Status:** anchor confirmed, unit inferred
**Source:** replayed `ZC_START_GAME`, `capture_1785545696.bin`

## Finding

Movement echoes carry a float timestamp on a server clock that
`ZC_START_GAME` (3014) publishes to the client at world entry. Synthesised
echoes must sit on that same clock or the client has no consistent basis for
interpolating movement.

## ZC_START_GAME (3014, 51 B)

| offset | value in the current replay | reading |
|---|---|---|
| 0x0A | `1.000` | time factor |
| **0x0E** | **`335542.531`** | **game-start clock — our anchor** |
| 0x12 | `335557.344` | unknown, ~15 s later |
| 0x16 | FILETIME | wall clock |
| 0x1E | `"2026-08-01 09:57:..."` | wall clock as text |

## Why +0x0E is the anchor

The first `ZC_MOVE_DIR` timestamp in the capture is `335552.469`. That is:

- **~10 s after** +0x0E (`335542.531`)
- **~5 s before** +0x12 (`335557.344`)

The session had the player entering the world and moving a few seconds later,
so +0x0E is the plausible "game started" reading and +0x12 is something else
(possibly an expiry or a scheduled future event — not established).

Elapsed ≈ 10 units for ≈ 10 s of session, so **the unit is seconds**. This is
inferred from one interval, not proven.

## Implementation

`handlers.py` reads the float at +0x0E out of the replayed `ZC_START_GAME` at
load time and adds `time.monotonic()` elapsed:

```python
TIME_BASE = _time_base()          # 335542.5 for the current replay
_T0 = time.monotonic()

def server_time():
    return TIME_BASE + (time.monotonic() - _T0)
```

Reading it from the replay rather than hard-coding means re-extracting
`replay/` from a different capture keeps the clock consistent automatically —
unlike `MY_HANDLE`, which is still pinned (see `04-pc-handle.md`).

## Other constants on this clock

- `ZC_MOVE_DIR+0x27`, `ZC_PC_MOVE_STOP+0x23`, `ZC_JUMP+0x2B` — all the same clock
- speed `35.0` and jump power `350.0` are **not** time-based, just constants
