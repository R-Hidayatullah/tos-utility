# fsbtool

Portable (Linux / Windows / macOS) FMOD **FSB** inspector, exporter, and player.
Handles **FSB5** and legacy **FSB1 / FSB2 / FSB3 / FSB4**. Written in C.

FSB5 and FSB3/FSB4 (container + Vorbis codebook mechanism) were reverse-engineered
directly from `fmodexD.dll` (`FMOD::CodecFSB5`, `FMOD::CodecFSB`, `FMOD::CodecVorbis`)
via IDA Pro — authoritative. **FSB1/FSB2 are not present in that DLL** (FMOD Ex
dropped them), so their layout follows the public format spec (best-effort, not
IDA-verified); FSB2 shares FSB3's structure, FSB1 uses fixed 0x40 sample headers.

FSB5 carries one codec for the whole file; FSB1–FSB4 store a **codec per sample**
(the tool reports and exports each accordingly).

## Build

Dependencies are the C libraries under `sdk audio/` (libogg, libvorbis, miniaudio).
No system packages required.

```bash
# Linux / macOS / Windows (Git Bash / MSYS)
./build.sh

# or CMake anywhere
cmake -B build -DSDK_AUDIO="/path/to/sdk audio" && cmake --build build
```

Set `SDK_AUDIO` (env var for build.sh, `-D` for CMake) if the libraries live
elsewhere.

## Usage

```
fsbtool info   <file.fsb>
fsbtool list   <file.fsb> [--limit N]
fsbtool export <file.fsb> [-o outdir] [--ogg|--wav] [--index N] [--limit N]
fsbtool play   <file.fsb> [--index N]
```

* `export` default per format: Vorbis → lossless `.ogg` (repacked, not re-encoded),
  MPEG → lossless `.mp3` (passthrough), everything decodable → `.wav`.
  `--wav` forces decode-to-WAV. Proprietary codecs with no portable decoder
  (XMA/CELT/AT9/XWMA) dump their raw encoded stream for external tools.
* `play` decodes a subsound and plays it through the system audio device (miniaudio).

### Format support
| Format | Decode → WAV/play | Export |
|--------|-------------------|--------|
| PCM8/16/24/32/float | ✅ | wav |
| Vorbis | ✅ | **.ogg** lossless repack |
| MPEG | ✅ (minimp3) | **.mp3** passthrough |
| IMA ADPCM | ✅ (exact FMOD algorithm from IDA) | wav |
| GC ADPCM | ⚠️ best-effort (DSP spec + DSPCOEFF meta) | wav |
| VAG / HEVAG | ⚠️ best-effort (PS-ADPCM) | wav |
| XMA / CELT / AT9 / XWMA | ❌ no portable decoder | raw dump |

⚠️ = implemented but not verifiable here (no non-Vorbis test samples); the IMA
transfer function and block layout are transcribed verbatim from `fmodexD.dll`
and unit-tested against an independent reference decoder.

Examples:
```
fsbtool list   SE.fsb --limit 20
fsbtool export skilvoice_jap.fsb            # all 1036 subsounds -> fsb_out/skilvoice_jap/
fsbtool export SE.fsb --index 3 -o dump
fsbtool play   skilvoice_jap.fsb --index 0
```

## Encrypted FSB

FMOD's FSB codec reads **plaintext** (verified in IDA: `CodecFSB5::headerReadCheck`
just checks the `FSB5` magic — no decrypt step). Encryption is a game-specific XOR
layer applied on top, with a key that is **not** stored in the file. Supply it with
`--key`:

```
fsbtool list bank.fsb --key MySecretKey     # ASCII key
fsbtool list bank.fsb --key 0xDEADBEEF       # hex key
```

The tool decrypts in place and auto-detects the scheme (plain XOR, XOR+nibble-swap,
nibble-swap+XOR) by checking for a valid FSB magic afterwards; a wrong key is
reported rather than silently producing garbage. Games using a different cipher
would need that scheme added to `src/fsbcrypt.c`.

## FSB1 / FSB2 / FSB3 / FSB4 format notes

FSB3/FSB4 are from IDA (`FMOD::CodecFSB`); FSB1/FSB2 from the public spec.
Header base (`id, numsamples@4, shdrsize@8, datasize@c` [`, version@10, mode@14`]):
FSB1 = 0x10, FSB2 = FSB3 = 0x18, FSB4 = 0x30 bytes.
FSB1 sample headers are a fixed 0x40 with `name[0x20]@0` and no size field;
FSB2/3/4 headers carry a `u16 size@0` + `name[0x1e]@2`. In every case the sample
fields live at `lengthsamples@0x20, lengthcompressedbytes@0x24, loopstart@0x28,
loopend@0x2c, mode@0x30, deffreq@0x34, numchannels@0x3e`.

### From IDA, `FMOD::CodecFSB` (FSB3/FSB4 authoritative)

`FMOD_FSB_HEADER`: FSB3 = 24 bytes, FSB4 = 48 bytes
(`id, numsamples, shdrsize, datasize, version, mode` [+ `hash, guid` for FSB4]).
Data starts at `headerSize + shdrsize`; each sample's data is the running sum of
`lengthcompressedbytes`.

`FMOD_FSB_SAMPLE_HEADER` (>= 80 bytes, variable via the `size` field @0):
`name[30]@2, lengthsamples@0x20, lengthcompressedbytes@0x24, loopstart@0x28,
loopend@0x2c, mode@0x30, deffreq@0x34, numchannels@0x3e`. When header `mode & 2`
(BASICHEADERS) only sample 0 has a full header and the rest are 8-byte
`{lengthsamples, lengthcompressedbytes}` basics sharing sample 0's format.

Per-sample codec comes from the FSOUND `mode` bits: `0x8`=PCM8, `0x10`=PCM16,
`0x200000`=float, `0x400000`=IMA ADPCM, `0x800000`=VAG, `0x2000000`=GCADPCM,
`0x1000000`=XMA, `0x200`=MPEG, `0x8000000`=CELT.

## FSB5 format notes (from IDA)

### Header (60 bytes; 64 when `version == 0`)
| off | field |
|-----|-------|
| 0x00 | `"FSB5"` |
| 0x04 | version |
| 0x08 | numSamples |
| 0x0C | sampleHeadersSize |
| 0x10 | nameTableSize |
| 0x14 | dataSize |
| 0x18 | dataFormat (1=PCM8 … 11=MPEG 15=VORBIS) |

Data chunk starts at `headerSize + sampleHeadersSize + nameTableSize`.

### Per-sample 64-bit bitfield (`CodecFSB5::getWaveFormatInternal`)
| bits | meaning |
|------|---------|
| 0 | has metadata chunks |
| 1–4 | frequency code → {4000,8000,11000,12000,16000,22050,24000,32000,44100,48000,96000} |
| 5–6 | channel code → {1,2,6,8} |
| **7–33 (27 bits)** | data offset in 32-byte units (`dataStart + 32*field`) |
| 34–63 | PCM sample count |

> The commonly-copied "28-bit offset ×16" layout is **wrong**: the offset is a
> **27-bit** field scaled by **32**, and channels is a 2-bit code. Using 28 bits
> leaks the low sample-count bit into the offset (breaks any subsound whose
> sample count is odd in bit 34).

### Metadata chunk (`hdr>>25` type, `(hdr>>1)&0xFFFFFF` size, `hdr&1` next)
`1`=channels `2`=frequency `3`=loop `6`=xmaseek `7`=dspcoeff `11`=vorbisdata
(crc32 + seek table).

### Vorbis (`CodecVorbis::addCodecSetup`)
FSB5 stores only raw Vorbis audio packets (each `u16 length`-prefixed) plus a
**crc32** identifying the shared codec setup (codebooks). The setup packet is
either embedded in-file or looked up in FMOD's built-in table of **161** setups.
`gen/fsb_vorbis_setup_table.c` is that table, extracted verbatim from
`fmodexD.dll` (`FMOD::CodecVorbis::VorbisCodecSetups`). Each entry is a full
`\x05vorbis`-prefixed setup packet. Block sizes are **256 / 2048**.

To decode/repack we rebuild the three canonical Vorbis headers
(identification / comment / setup) and feed the stored packets to libvorbis.

## Layout
```
src/fsb5.*             FSB5 parser + fsb_open() version sniffing dispatch
src/fsb4.c             FSB3 / FSB4 parser (fills the same struct)
src/fsb_vorbis.*       codebook lookup, Ogg rebuild, low-level libvorbis decode
src/adpcm.*            IMA (from IDA) / GC DSP / VAG ADPCM decoders
src/mp3.*              minimp3 wrapper (MPEG decode)
src/decode.*           unified "any sample -> int16 PCM" dispatch
src/wav.*              WAV writer
src/player.*           miniaudio playback
gen/fsb_vorbis_setup_table.c   161 codebooks extracted from fmodexD.dll
```

## Status
Validated on real FMOD files (FSB1/FSB3/FSB4 + FSB5):
- **PCM16** (FSB3) — healthy decode
- **IMA ADPCM** mono / stereo / 5.1 (FSB3/FSB4) — block sizes match to the byte
  (36 bytes/ch/block, 64 samples/block, contiguous per-channel sub-blocks)
- **GC DSP ADPCM** mono / stereo (FSB3/FSB4) — coeffs read big-endian from the
  header extra past 0x50; exact sample counts
- **Vorbis** (FSB5) — lossless `.ogg` repack, 1036/1036 + SE.fsb

Implemented, not yet seen on a real sample: VAG/HEVAG, MPEG-in-FSB, FSB2, and
FSB1's codec (FSB1 uses FMOD3 flags that aren't decodable reliably → raw dump;
its metadata name/freq/channels/length/offset are parsed correctly).
XMA/CELT/AT9/XWMA need a proprietary decoder library (none portable).
