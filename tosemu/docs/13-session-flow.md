# Session flow: what the client waits for

**Status:** driven end to end against the real client
**Source:** `capture_1785545696.bin` plus the client's own retry behaviour

The client rarely reports an error. When something is missing it either sits
still or retries forever, and the symptom appears far from the cause. This is
the order it actually wants things in, and what each stage is blocking on.

## Barrack

```
CB_LOGIN            -> BC_LOGIN_PACKET_RECEIVED
                       BC_DISCONNECT_PACKET_LOG_COUNT
                       BC_LOGINOK
CB_START_BARRACK    -> BC_IES_MODIFY_LIST, BC_SERVER_ENTRY,
                       BC_COMMANDER_LIST, one BC_COMMANDER_CREATE per
                       character, BC_NORMAL CharacterInfo/TeamUI/ZoneTraffic
CB_COMMANDER_CREATE -> BC_COMMANDER_CREATE_SLOTID, BC_COMMANDER_CREATE
CB_COMMANDER_MOVE   -> BC_NORMAL SetPosition (0x02)
CB_REQ_CHANNEL_TRAFFIC -> BC_NORMAL ZoneTraffic (0x0D)
CB_START_GAME       -> BC_START_GAMEOK
```

Four things that are not obvious:

**The character list follows `CB_START_BARRACK`, not `CB_LOGIN`.** Login only
answers the three packets above; the client asks for the list separately once
the barrack scene has loaded. Sending it early means it arrives before anything
can display it.

**Character creation is exactly two packets.**
`BC_COMMANDER_CREATE_SLOTID` carries one byte — the account's new character
count — and the client **waits for it** before placing the character in the
lodge. Without it, creation hangs on the last screen with no error. Re-sending
the whole character list here is also wrong: the client then sees the new
character announced twice.

**`CB_COMMANDER_MOVE` must be acknowledged** or the client will not let go of a
character being dragged around the lodge. Index `0xFF` arrives once during
creation, before the character has an index, and is ignored. The client also
sends `0xFF` as the requested slot on creation, meaning "pick one".

**The channel list is built from the maps the account's own characters are
standing on.** At first login there are none, so it comes back empty and
"Select Channel" stays blank — which makes Start Game do nothing. The client
re-asks with `CB_REQ_CHANNEL_TRAFFIC` after creating a character, so that
request has to be answered rather than ignored. The channel id in the table
must be a sequential index from 0, because the client turns it into the channel
*name*: 0 becomes "Ch 1".

## Team name

The client opens its naming dialog **only when the account has no team name**.
Filling the field in from the account name means the prompt never appears.
Accounts start blank and answer `CB_BARRACKNAME_CHECK` /
`CB_BARRACKNAME_CHANGE` with the client's own rules: 2-16 characters, no
whitespace.

## Login accepts what the client actually sends

There is no credential store, so there is nothing to authenticate against and
refusing a login only produces a dead end. Two cases a reference implementation
rejects are accepted here, both landing on a well-known `player` account:

- **An empty account name.** A client started without launcher credentials
  sends one. The client tested here also reports service nation `TAIWAN`, which
  lays the login body out differently, so those fields are not parsed at all.
- **`CB_LOGIN_BY_PASSPORT`.** Carries no plaintext name — the captures show
  only a token blob and a `Mode(2,0) Login` diagnostic string.

Every rejection path logs what it parsed. `account=` and `nation=` are the two
fields that reveal whether the body was read at the right offset.

## The social links block the map load

The client opens **two** plaintext links — chat and relation, on consecutive
ports — and will not finish loading the world until both have answered
`CS_LOGIN`. Leaving them silent looks exactly like a hang on "loading world"
with nothing in the log.

```
CS_LOGIN (chat)     -> SC_NORMAL LoginSuccess, SC_LOGIN_OK, SC_NORMAL 0x02
CS_LOGIN (relation) -> SC_NORMAL LoginSuccess, LikedList, LikedMeList,
                       SC_LOGIN_OK
CS_REQ_RELATED_PC_SESSION -> SC_NORMAL RelationCount
```

The port is the only thing distinguishing the two, since they share an opcode
range.

## Zone

Order taken from the live server, and it is not the obvious one:

```
CZ_CONNECT    -> ZC_STANCE_CHANGE
                 ZC_CONNECT_OK
                 ZC_NORMAL AdventureBook (0x199)
                 ZC_SET_CHATBALLOON_SKIN
                 ZC_NORMAL 0x1B9

CZ_GAME_READY -> ZC_IES_MODIFY_LIST, ZC_ITEM_INVENTORY_LIST,
                 ZC_SESSION_OBJECTS, ZC_OPTION_LIST, ZC_SKILLMAP_LIST,
                 ZC_CHAT_MACRO_LIST, ZC_MAP_REVEAL_LIST, ZC_NPC_STATE_LIST,
                 ZC_HELP_LIST, ZC_MYPAGE_MAP, ZC_GUESTPAGE_MAP,
                 ZC_NORMAL UpdateSkillUI (0x18B),
                 ZC_ITEM_EQUIP_LIST, ZC_SKILL_LIST, ZC_OBJECT_PROPERTY,
                 ZC_START_INFO, ZC_LOGIN_TIME, ZC_START_GAME, ZC_MYPC_ENTER

CZ_LOAD_COMPLETE -> ZC_LOAD_COMPLETE
```

**`ZC_STANCE_CHANGE` precedes `ZC_CONNECT_OK`.** The client builds its model
from that stance; sending it late gives a T-pose, and stance 0 can stall the
load outright.

**The client will not send `CZ_GAME_READY`** until it has both `ZC_NORMAL`
blocks. Sending only `ZC_CONNECT_OK` leaves it on "loading world" forever with
nothing further on the wire — the load never starts, so there is no error.

**`CZ_LOAD_COMPLETE` is sent once a second until answered**, and the client
stays in the loading state, refusing all input, while it waits. That reads as
"I am in the world but cannot move or use skills".

Most of the `CZ_GAME_READY` set are empty lists. Being empty is fine; being
absent is not.

## Becoming visible

`CZ_LOAD_COMPLETE` **never appears** on the zone link in the capture, so gating
visibility on it alone leaves the player invisible to everyone and everyone
invisible to them. Any packet that only a loaded client sends — a heartbeat, a
movement — triggers world entry as well.

## Requests the client retries until answered

| request | answer |
|---|---|
| `CZ_LOAD_COMPLETE` | `ZC_LOAD_COMPLETE` |
| `CZ_REQ_QUICKSLOT_LIST` | `ZC_QUICK_SLOT_LIST` |
| `CZ_REQUEST_GUILD_INDEX` | `ZC_RESPONSE_GUILD_INDEX` |
| `CZ_REQ_COMMANDER_INFO` | `ZC_TRUST_INFO` |

Ignored deliberately, because the client expects no reply and sends them
constantly: `CZ_CHANGE_CONFIG` (one per setting, dozens in a burst),
`CZ_MAP_REVEAL_INFO`, `CZ_CUSTOM_COMMAND`, `CZ_DO_CLIENT_MOVE_CHECK`,
`CZ_CAMPINFO`, `CZ_FIXED_NOTICE_SHOW`, `CZ_DISCONNECT_REASON_FOR_LOG`,
`CZ_REQ_NORMAL_TX`, `CZ_REQ_FIELD_BOSS_EXIST`, `CZ_RUN_GAMEEXIT_TIMER`, the
event probes, and on the social link `CS_NORMAL_GAME_START` /
`CS_REFRESH_GROUP_CHAT`.

## Properties the client gates its own input on

The client refuses some inputs unless it holds the matching property, and it
fails silently — the key simply does nothing:

- **`JumpPower`.** Unset means no jump. The reference value is 350.
- **The skill window** is built from `ZC_NORMAL` sub-op `0x18B`
  (UpdateSkillUI), which carries the character's job data. `ZC_SKILL_LIST` on
  its own leaves the window empty, and UpdateSkillUI must arrive **before** the
  list — the client ignores skills belonging to a job it has not been told
  about.

## The clock, and why the character teleports

`ZC_START_GAME`'s second and third floats are the client's **clock base**, and
every movement timestamp (`ZC_MOVE_DIR+0x27`) is on that same clock:

```
ZC_START_GAME  (1.0, 335542.5, 335557.3)
ZC_MOVE_DIR    335552 .. 335924        same clock, 372s of play
```

Sending `1` as the base while movement carries a real server time makes the
client compute the difference as elapsed time and extrapolate the character
that far forward. At 45 seconds of uptime it reads as an instant jump across
the map — which looks like a movement-speed problem and is not. Both must come
from one monotonic seconds clock.

## Leaving

```
CZ_LOGOUT        -> ZC_LOGOUT_OK
CZ_MOVE_BARRACK  -> ZC_MOVE_BARRACK
```

Both need their acknowledgement; the client does not simply disconnect, it
waits on a dead menu.

## Map change is a reconnect

```
/map <id>        -> ZC_MOVE_ZONE          client tears down the socket
CZ_MOVE_ZONE_OK  -> ZC_MOVE_ZONE_OK       destination and address
                    client reconnects and reloads
```

The destination map id sits at **+0x16**, not +0x0A — see the correction in
`09`.
