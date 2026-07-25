# Tree of Savior — Analisis Sistem Pathing

> Binary: `Client_tos_x64.exe` (imagebase `0x140000000`)
> Sumber: reverse engineering via IDA Pro. Simbol fungsi di-strip; nama diambil dari RTTI, log string, dan assert.

## Ringkasan

ToS **tidak** memakai Recast/Detour atau navmesh buatan sendiri. Seluruh pathfinding ditangani oleh **imcPathEngine** — wrapper tipis IMC Games di atas SDK komersial **[PathEngine](https://www.pathengine.com)**.

Bukti: source tree `F:\live\Source\imcPathEngine\` dan class RTTI yang khas PathEngine.

| Class | Peran |
|---|---|
| `imcPathEngine::PathEngine` | Engine utama |
| `IGround` / `CGround` | **Navigation mesh** (ground, tersusun dari *cell*) |
| `IAgent` / `CAgent` | Aktor yang bergerak & mencari jalan |
| `CPath` | Hasil path yang dihitung |
| `cPosition` | Posisi di mesh = `{ x, y, cell }` (cell `-1` = di luar mesh / invalid) |
| `cSolidObjects` / `MeshWrapper` | Obstacle & wrapper mesh |
| `CResource` | Resource internal engine |

Model konseptual PathEngine:
- **ground** = navigation mesh berbentuk kumpulan *cell*.
- **cPosition** = titik pada mesh, disimpan sebagai `{x, y, cell}`; `cell == -1` berarti off-mesh / invalid.
- **agent** = entitas yang bergerak di ground, menghitung & mengikuti path.

---

## 1. Parser Ground — `PathEngine::CreateGround(const char*, bool)`

**Alamat:** `sub_141366290`

Alur parsing file mesh:

1. Pastikan nama file berekstensi **`.pathengine`** (di-append otomatis kalau belum ada — di kode terlihat konstanta `'.'` = `46` ditambahkan ke path).
2. Buka file, ekstrak blob bertag **`"tok"`** — format *tokenised ground* native PathEngine — lalu diserahkan ke engine via panggilan vtable `loadFromBuffer("tok", data, size)` (`*(*(a1+24)+72)`).
3. Diikuti 2 stream data tambahan lewat vtable `+440` / `+448` (persistent / preprocessing data).
4. Bungkus hasil menjadi `shared_ptr<CGround>` (`_Ref_count_obj2<imcPathEngine::CGround>`, alokasi `0x15920` byte).
5. Kalau gagal → lempar exception `FATAL_FILE_LOAD` dengan pesan `"cannot load TOK data"`.

**Kesimpulan parser:** container `.pathengine` → section `tok` → PathEngine men-decode mesh cell → objek `CGround`.

### 1a. Format Container `.pathengine` (byte-level)

Loader sesungguhnya = **`imcPathEngine::CResource::Load(const char*)`** @ `sub_141368AA0` (source `Resource.cpp`, error `"BG Data [%s] Can't load pathengine File"`). File dibaca mentah ke memory (`sub_1402A84D0`), lalu di-parse sebagai **3 section length-prefixed** yang berurutan. Setiap section = `[uint32 length][payload]`:

```
Offset               Field                     Keterangan
------               -----                     ----------
0x00      uint32     len0                      panjang section TOK
0x04      byte[len0] tokData                   PathEngine tokenised ground mesh ("tok")
                                               
len0+0x04 uint32     len1                      panjang preprocess stream #1
len0+0x08 byte[len1] preproc1                  persistent data → ground vtable +440

len0+len1+0x08  uint32     len2                panjang preprocess stream #2
len0+len1+0x0C  byte[len2] preproc2            persistent data → ground vtable +448
```

Accessor per-section (dipakai `CreateGround`):

| Fungsi | Section | Pointer | Panjang | Disimpan di |
|---|---|---|---|---|
| `sub_141368A80` | 0 (tok) | `base + 4` | `*(this+8)` = len0 | `this+8` |
| `sub_141368A20` | 1 | `base + len0 + 8` | `*(this+12)` = len1 | `this+12` |
| `sub_141368A50` | 2 | `base + len0 + len1 + 12` | `*(this+16)` = len2 | `this+16` |

Di `CreateGround`, ketiganya dipakai begini:

```c
mesh = engine->loadMeshFromBuffer("tok", tokData, len0);   // vtable +72 → iMesh*
mesh->vt_440(errHandler, preproc1, len1);                  // muat preprocess persistent #1
mesh->vt_448(errHandler, preproc2, len2);                  // muat preprocess persistent #2
```

- **Section 0 (`tok`)** = mesh ground utama dalam format *tokenised* native PathEngine (walkable faces/cells + konektivitas + vertex koordinat integer). "tok" adalah nama format yang diteruskan ke `loadMeshFromBuffer` (alternatifnya "xml").
- **Section 1 & 2** = dua blob **preprocess persistent data** yang di-attach ke ground setelah mesh dimuat. Berdasarkan API standar PathEngine, kemungkinan besar = *pathfind preprocess* (akselerasi `findShortestPath` / connected-region) dan *collision preprocess* (akselerasi query obstacle/collision). Keduanya di-generate offline oleh tool preprocessing PathEngine, bukan dihitung saat runtime.

Struktur internal blob `tok` di-decode di dalam `loadMeshFromBuffer` PathEngine — lihat **§1c** untuk detail token-per-token (sudah dibongkar).

> Catatan: class `Tokenizer` (`ParseFloat`/`ParseStringAppend`) di binary adalah milik **Google Protobuf** (`ExternLib\protobuf\...\io\tokenizer.cc`) untuk parsing protobuf text-format di modul lain — **bukan** parser `tok`.

### 1c. Format Internal Blob `tok` (tokenised XML tree)

Referensi implementasi tervalidasi: `tosmole-master/src/tok.rs` (punya test terhadap `barrack_noble.tok`). Blob `tok` = **pohon dokumen ala-XML yang di-tokenise biner**. Semua integer **little-endian**. Terdiri dari 3 bagian berurutan:

**(1) Tabel nama elemen** — deretan C-string null-terminated, di-index mulai `1`, diakhiri string kosong (`0x00` tunggal):
```
"mesh3D\0" "verts\0" "vert\0" "mappingTo2D\0" ... "\0"
   idx=1      idx=2    idx=3       idx=4         (akhir)
```

**(2) Tabel tipe atribut** — deretan `[u8 type][cstring name]`, diakhiri byte type `0`. Kode tipe:

| Kode | Tipe | Ukuran |
|---|---|---|
| 1 | CString | null-terminated |
| 2 | SInt32 | 4 (LE) |
| 3 | SInt16 | 2 (LE) |
| 4 | SInt8 | 1 |
| 5 | UInt32 | 4 (LE) |
| 6 | UInt16 | 2 (LE) |
| 7 | UInt8 | 1 |

**(3) Pohon node** (rekursif):
```
u8  element_index        ; 0 = akhir daftar node (node null / tutup anak)
repeat:
    u8 attr_index        ; 0 = akhir atribut
    <value sesuai tipe atribut di tabel (2)>
repeat:
    <child node>         ; ulang parse_node sampai element_index == 0
```

Tiap node = `{ element_index → nama elemen, daftar atribut bertipe, anak-anak bersarang }`. Tidak ada magic header di awal section — langsung mulai tabel nama elemen.

**Semantik navmesh** (dari exporter SVG di `tok.rs`):
- `mesh3D` → `verts` → banyak `vert{ x, y, z }` = **geometri 3D** ground.
- `mappingTo2D` → banyak `polygon` → tiap `polygon` berisi `edge{ startVert }` (index ke `verts`) = **cell/face walkable 2D** + konektivitasnya.

Vertices + polygon walkable + edge inilah navigation mesh yang dipakai agent PathEngine untuk `findShortestPath`.

### 1d. Sisi generate mesh (dev/offline) — `sub_141367070`

Ada juga jalur pembuatan navmesh (bukan runtime). Fungsi ini mem-build ground dari `imcPathEngine::cSolidObjects` (geometri map) lewat **voxelisasi**, dengan set parameter:
`method`, `voxels`, `maxSlopeOnTerrain`, `minimumFragmentSize`, `voxelSize`, `optimiseWithThreshold`, `subdivisionSize` (-1), `stripTerrainHeightDetail` (false), `excludeDownwardFacingFromGroundResult` (false).
Hasilnya di-serialize ke format `"tok"` (via `cVectorBuildingOutputStream`) lalu ditulis ke file dengan `_wfopen_s(…, L"wb")` + `fwrite`. Rantai lengkap:

```
solid objects (geometri map) --voxelize(params)--> buffer "tok" --tulis--> .pathengine
        ↑ dev/offline (sub_141367070)                                        ↓ runtime
                     CResource::Load → loadMeshFromBuffer("tok") → CGround → agent
```

### 1b. Konstruksi `CGround` — `sub_141369230`

Setelah mesh dimuat, `CGround` dibangun (`_Ref_count_obj2<CGround>`):
- `this+32` = objek mesh/ground engine.
- `this+48` = `mesh->vt_472()` (context, mis. collision context).
- `this+40` = `mesh->vt_464()`, lalu `obj->vt_48(this+48)` — mengikat context ke mesh.
- Alokasi cache: array 3× (size 0x18) di `this+64`, dan array **0x4C9 (1225)** elemen (size 0x48) di `this+136` — kemungkinan cache per-cell/grid.

---

## 2. State Machine Gerakan — `CFSMActor::CMS_MOVE_PATH_ProcessState(imcFSM::State)`

**Alamat:** `sub_140D6DC00`

Pergerakan aktor berjalan lewat FSM dengan *blackboard* berbasis **string key yang di-hash CRC** (fungsi hash `sub_1400E0AA0`, tabel CRC `dword_141694500`).

State enum `{ Enter = 0, Update = 1, Exit = 2 }`:

- **Enter (0):** baca variabel blackboard `PathEngineTargetPos` → `m_moveTo` (`this+5092` = pos, `this+5100` = cell) dan `MoveType` (`this+5056`). Ada assert `m_moveTo.cell != -1`.
- **Update (1)** (`UPDATE_CMS_MOVE_PATH`): baca `ElapsedTime`, majukan agent sepanjang path, lakukan **repath** kalau target cell/pos berubah, lalu terapkan gerak ke aktor.
- **Exit (2):** cleanup state.

Agent (`CAgent` @ `this+3552`) dibuat lazily oleh `sub_140AC6CE0` dari ground di `this+3536`.

Pembuatan agent eksplisit: **`CFSMActor::CreatePathAgent(cPosition&, float2&)`** @ `sub_140AC53D0` (error path: `ERROR_CREATE_PATH_AGENT_FAIL`, log `"x,y : %d %d"`).

---

## 3. Fungsi Terkait Lain

| Fungsi | Alamat | Peran |
|---|---|---|
| `CWorld::FindRayNavMeshColPos()` | — | Picking klik mouse → titik pada ground |
| `CGround::GetFrontUnobstructedPos` | `sub_14136A9B0` | Cek obstacle / line-of-sight |
| `GetGroundHeight` | — | Tinggi ground di titik tertentu |
| `IsIgnoreNavMesh` | — | Flag entitas mengabaikan navmesh |
| `CNaviEffect::MakeEffect` | — | Visualisasi navmesh debug ("Visible NaviMesh") |
| `CKnockDownRTCalc::CalcKDMoveXY / CalcKBMoveXY` | — | Kalkulasi gerak knockdown/knockback di atas ground |

---

## Anchor IDA (untuk referensi cepat)

```
sub_141366290   PathEngine::CreateGround(const char*, bool)   -- parser .pathengine/tok
sub_140D6DC00   CFSMActor::CMS_MOVE_PATH_ProcessState         -- FSM gerak
sub_140AC53D0   CFSMActor::CreatePathAgent                    -- buat agent
sub_140AC6CE0   getter/lazy-create CAgent (this+3552)
sub_14136A9B0   CGround::GetFrontUnobstructedPos              -- obstacle query
sub_1400E0AA0   hash string blackboard (+ tabel dword_141694500)
```

Blackboard keys: `PathEngineTargetPos`, `MoveType`, `ElapsedTime`.
