# Reading the client's own data

**Status:** working; 543 maps, 4288 monsters, 22100 items, 1483 skills, 137 jobs
**Code:** `cpp/server/src/ipf.*`, `ies.*`, `gamedata.*`

Everything the world is made of is read from the installed game rather than
from a hand-maintained copy, so it tracks whatever build is on disk. Reference
implementations are a useful cross-check but they lag the live client; the
client cannot.

## IPF: patch resolution is the whole point

```
client: 1220 archives, 472768 files
```

Of 596,835 entries, **124,067 are overridden**. Every archive under `data/` and
`patch/` is indexed and each virtual path resolves to the highest
`newVersion` — the same latest-wins rule the runtime uses. Reading the base
`ies.ipf` alone gets data that is years stale.

Virtual path is `<container-stem>/<dirName>`, lowercased, so container
`ies.ipf` + entry `map.ies` resolves as `ies/map.ies`.

Entry data is ToS-partial-PKWARE-encrypted (even byte indices only) and then
raw-deflated; `.fsb`, `.jpg` and `.mp3` are stored plain. The reader is ported
from `tos-utility/tosmole-cpp` with zlib swapped for the in-tree inflater, so
the server keeps a dependency-free build.

**Caveat:** if the game client is running or mid-patch, some archives are
locked and the counts drop (one run indexed 760 archives / 22811 files instead
of 1220 / 472768). The startup line is the place to notice that.

## IES: row values are ordered by declIdx

The detail that is easy to get wrong and silent when you do. Row values are
**not** in file column order. Numeric values are ordered by the numeric
columns' `declIdx`, string values by the string columns' `declIdx`.

Verified against ground truth — `map.ies` row 1021:

| ordering | ClassID | DefGenX/Y/Z |
|---|---|---|
| file order | 5000 | 2, 0, 5 |
| by column name | 3400 | 1, -599, 260 |
| **by declIdx** | **1021** | **-599, 260, -1377** |

Only the last is right, and it matches `f_siauliai_west`'s known entry point.
The other two produce plausible-looking numbers, which is what makes this worth
stating: a wrong ordering does not throw, it silently reads every field one
column over.

## Integers are stored as floats, and some exceed int32

Every numeric column is an `f32`. Converting with a plain cast wraps on values
that do not fit — `Vis` (silver) has `MaxStack` **5,000,000,000**, which
overflowed to a negative and turned a stackable currency into a single coin.
Conversion clamps to the int32 range.

## Columns named like stats are often rates

`statbase_pc.ies`'s `MOVE_SPEED` reads 100 for every class and 110 for a fast
one. It is a **percentage** applied to a base of 30, not a speed. Sending it as
the absolute MSPD made characters run at 100 — better than three times too
fast. The live server sends **35** for an unbuffed character.

`ATK_SPEED`, `MHP` and the rest of that table behave the same way. Treat any
column that reads exactly 100 across most rows as a rate.

## Tables the server uses

| table | what for |
|---|---|
| `ies/map.ies` | 543 maps: id, class name, `DefGenX/Y/Z` entry point |
| `ies/monster.ies` | 4288 monsters and NPCs, split by `Faction` |
| `ies/item.ies` | consumables, materials, currency |
| `ies/item_equip.ies` + expansions | 6768 equipment pieces, with `ClassType` |
| `ies/skill.ies` | 1483 skills, `BasicCoolDown` in ms, `SklSR` range |
| `ies/skilltree.ies` | job to skill mapping, keyed by class name |
| `ies/job.ies` | 137 jobs, `BarrackStance` |
| `ies/stance.ies` + `stancecondition.ies` | animation sets (see below) |
| `ies/statbase_pc.ies` | base stats by class level |
| `ies_mongen/anchor_<map>.ies` | per-map spawn tables |

Joins are done by **class name**, not by id, because that is how the client's
own tables reference each other. `skilltree.ies` keys rows as `Char1_1_3`,
meaning slot 3 of job `Char1_1`, and names the skill `Swordman_Thrust` which
`skill.ies` resolves to id 10001.

The same principle picks starting gear: every equipment row carries a
`ClassType` (`Sword`, `Staff`, `Bow`, `Mace`), so a new Wizard is handed the
lowest-requirement `Staff` in whatever build is installed. An earlier attempt
guessed class names — `Wooden_Sword` and friends do not exist — and silently
produced empty bags.

## Stance is a two-table join

A character sent `stance = 0` renders in a **T-pose** and can stall the world
load outright. The stance is the client's animation set:

```
stancecondition.ies   (job class, right hand, left hand) -> stance NAME
stance.ies            name -> the id the packets carry
```

A Swordsman holding a Sword resolves to `OneHandSwordArtefact`, id **10020**; a
Wizard with a Staff to `TwoHandStaff`. `job.ies`'s `BarrackStance` is the
fallback when no condition row matches.

Rows with `Riding = TRUE` are skipped. Lookup falls back from the exact
(right, left) pair, to right hand alone, to the bare-handed row — an unknown
off-hand should not drop the character to no stance at all.

## Spawn anchors are correct; the entry point is not where they are

`ies_mongen/anchor_<map>.ies` gives `PosX/Y/Z`, `Direction`, `NPCID` and
`AnchorRange` per spawn. For `f_siauliai_west` the coordinate frame matches the
map — `PosY` spans 209..423 against an entry point at y=260 — so these are
directly usable.

The problem is *where the player lands*. That map has **one anchor within 300
units** of its default entry point, and that one carries no npc id; 160 of its
232 anchors have none. The player arrives in an empty corner of a 5000-unit
map, which looks exactly like broken spawning.

The default start map is therefore Klaipeda (1001), a town whose spawn table
puts NPCs where you arrive.

**Resolved.** This previously read "`anchor_c_klaipe.ies` reads `PosY` near -1
while everything in the capture sits at y=241, so the town anchors are in a
different frame". There is no frame difference — that was a mis-ordered column
read. Klaipeda anchors span y = -1.4 .. 248.2 and land on captured spawn
positions exactly. See `09`.

Note also that 160 of `f_siauliai_west`'s 232 anchors having "no npc id" is not
a defect in the data: `NPCID` is not the key. `GenType` is, and it resolves 99
of those 232. See `09`.

## The two client config files

`serverlist.xml` — `session::loginInfo::LoadServerList` reads
`Server%d_IP`/`Server%d_Port` starting at **0**, incrementing until an index is
missing or its IP is the literal `None`. Each index becomes its own entry under
the element's shared `NAME`/`GROUP_ID`. `Server0_Port` was advertising 7001,
which the server has never bound — it binds 2000 (barrack), 7002 (zone) and
9001/9002 (social). Login worked regardless because this client dials Server1
for `CB_LOGIN` (see `11`), but the dead entry is now 2000.

`static__Config.txt` — `gecfg::LoadStaticConfig` splits each line on `=` and
acts only on lines that yield **exactly two** tokens, so anything else is
silently skipped. The complete set of keys it recognises:

| key | effect |
|---|---|
| `ServiceNation` | stored |
| `SoundLanguage` | stored |
| `ForbiddenLanguages` | `/`-separated list |
| `NexonGLMCode` | stored string |
| `NPSCode` | `atoi` |
| `Dictionary` | **matched and then ignored** — a no-op branch |
| `UseNexonSSO` `UseNexonGLM` `UseNexonLauncher` `UseNGS` `UseNPS` | bool |
| `UseXigncode` `UseLiveXigncode` | bool |
| `UseSteamClient` `UseTaiwanClient` `UseChinaClient` | bool |
| `UseNISMS_TESTURL` `UseNISMS_ONLY_OFFER` | bool |
| `UseNonePakFile` | bool, **inverted** — the flag it sets is `!YES` |

`UseHackshield=NO` was in the shipped file and is **not a key** — the string
`hackshield` does not appear anywhere in the binary. It was removed, and the
switches the client does read (`UseLiveXigncode`, `UseNexonLauncher`, `UseNGS`,
`UseNPS`) are now set explicitly rather than left to their zero-initialised
defaults.

The booleans test for `YES` by substring, so any other value reads as off.

## Property ids: the one table not from the client

Property ids are assigned by the client at runtime and no shipped file carries
the mapping, so `properties.txt` is generated from a reference database by
`gen_properties.py`. It was then checked against real traffic — **16 of the 17
ids** in a captured `ZC_OBJECT_PROPERTY` resolve to `PCEtc` names:

```bash
python gen_properties.py --verify
```

32,956 properties across 13 namespaces. This is the first place to look if
properties start reading wrong after a client patch.
