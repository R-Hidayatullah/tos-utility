#!/usr/bin/env python3
"""
fsb2audio.py - Konverter FMOD FSB5 -> audio (ogg/wav/mp3)

Dibuat berdasarkan analisis parser FMOD::CodecFSB5 di fmodexD.dll (IDA Pro):
  - Header FSB5 64/60 byte (struct FSB5_HEADER_0)
  - Sample header bitfield 64-bit  (CodecFSB5::getWaveFormatInternal)
  - dataFormat 0x0F = FMOD_SOUND_FORMAT_VORBIS  (kasus Tree of Savior)

Mesin decode memakai:
  - python-fsb5  (pip install fsb5)  -> parsing + rebuild Ogg Vorbis
  - pyogg        (pip install pyogg) -> menyediakan libogg.dll & libvorbis.dll
                                        (libvorbis pyogg = build gabungan enc+dec)

FSB5 Vorbis tidak menyimpan setup-header (codebook) Vorbis, hanya crc32-nya.
python-fsb5 merekonstruksi setup-header dari tabel crc32 bawaannya. Sample
dengan crc32 yang tidak dikenal tidak bisa direbuild dan akan dilewati (dicatat).

Pemakaian:
  python fsb2audio.py                     # konversi semua *.fsb di folder ini
  python fsb2audio.py SE.fsb              # satu file
  python fsb2audio.py skilvoice_F1.fsb -o out --limit 20
  python fsb2audio.py --list SE.fsb       # hanya daftar isi, tanpa konversi
"""
import argparse
import ctypes
import glob
import os
import re
import sys


# ---------------------------------------------------------------------------
# Setup library native (libogg / libvorbis) dari paket pyogg, lalu tambal
# fsb5.utils.load_lib supaya menemukannya. Harus dilakukan SEBELUM
# fsb5.vorbis diimpor (modul itu memanggil load_lib saat import).
# ---------------------------------------------------------------------------
def setup_native_libs():
    try:
        import pyogg
    except ImportError:
        sys.exit("ERROR: paket 'pyogg' belum terpasang. Jalankan: pip install pyogg")

    libdir = os.path.dirname(pyogg.__file__)
    if hasattr(os, "add_dll_directory") and os.path.isdir(libdir):
        os.add_dll_directory(libdir)  # agar libvorbis menemukan dependensinya (libogg)

    # nama file di pyogg: libogg.dll, libvorbis.dll (tanpa libvorbisenc terpisah)
    name_map = {
        "ogg": "libogg.dll",
        "vorbis": "libvorbis.dll",
        "vorbisenc": "libvorbis.dll",  # simbol encoder ada di dalam libvorbis pyogg
    }

    import fsb5.utils as _utils

    def _load_lib(*names):
        last = None
        for name in names:
            fname = name_map.get(name, "lib%s.dll" % name)
            path = os.path.join(libdir, fname)
            if os.path.exists(path):
                try:
                    return ctypes.CDLL(path)
                except OSError as e:
                    last = e
        raise _utils.LibraryNotFoundException(
            "Tidak dapat memuat library %r (%s)" % (names[0], last)
        )

    _utils.load_lib = _load_lib


_INVALID = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def sanitize(name):
    name = _INVALID.sub("_", name).strip().rstrip(".")
    return name or "sample"


def convert_file(path, outroot, do_list=False, limit=0):
    import fsb5
    from fsb5 import MetadataChunkType

    with open(path, "rb") as fh:
        data = fh.read()

    fsb = fsb5.load(data)
    h = fsb.header
    fmt = h.mode.name
    ext = fsb.get_sample_extension()
    n = h.numSamples
    print("\n=== %s ===" % os.path.basename(path))
    print("  format=%s  subsounds=%d  ukuran_file=%d  (rebuild -> .%s)"
          % (fmt, n, len(data), ext))

    if do_list:
        for i, s in enumerate(fsb.samples):
            if limit and i >= limit:
                print("  ... (%d lagi)" % (n - limit)); break
            vd = s.metadata.get(MetadataChunkType.VORBISDATA)
            crc = vd.crc32 if vd else "-"
            print("  [%4d] %-40s %dch %6dHz samples=%-8d crc32=%s"
                  % (i, s.name, s.channels, s.frequency, s.samples, crc))
        return (0, 0, 0)

    outdir = os.path.join(outroot, os.path.splitext(os.path.basename(path))[0])
    os.makedirs(outdir, exist_ok=True)

    ok = fail = 0
    unknown_crc = set()
    used = {}
    for i, s in enumerate(fsb.samples):
        if limit and i >= limit:
            print("  ... berhenti di limit %d dari %d subsound" % (limit, n)); break

        base = sanitize(s.name)
        # cegah tabrakan nama
        cnt = used.get(base, 0); used[base] = cnt + 1
        fname = "%s.%s" % (base if cnt == 0 else "%s_%d" % (base, cnt), ext)
        dest = os.path.join(outdir, fname)

        try:
            audio = fsb.rebuild_sample(s)
            with open(dest, "wb") as out:
                out.write(audio)
            ok += 1
        except Exception as e:
            fail += 1
            vd = s.metadata.get(MetadataChunkType.VORBISDATA)
            if vd:
                unknown_crc.add(vd.crc32)
            if fail <= 10:
                print("  ! gagal [%d] %s: %s" % (i, s.name, e))

    print("  selesai: %d berhasil, %d gagal -> %s" % (ok, fail, outdir))
    if unknown_crc:
        print("  crc32 setup-header tak dikenal (%d): %s"
              % (len(unknown_crc), ", ".join(str(c) for c in sorted(unknown_crc))))
    return (ok, fail, len(unknown_crc))


def main():
    ap = argparse.ArgumentParser(description="Konverter FSB5 -> audio")
    ap.add_argument("inputs", nargs="*", help="file .fsb (default: semua *.fsb di folder ini)")
    ap.add_argument("-o", "--outdir", default="fsb_out", help="folder output (default: fsb_out)")
    ap.add_argument("--list", action="store_true", help="hanya tampilkan isi, tanpa konversi")
    ap.add_argument("--limit", type=int, default=0, help="batasi jumlah subsound diproses")
    args = ap.parse_args()

    inputs = args.inputs or sorted(glob.glob("*.fsb"))
    if not inputs:
        sys.exit("Tidak ada file .fsb ditemukan.")

    setup_native_libs()

    tot_ok = tot_fail = 0
    for path in inputs:
        if not os.path.isfile(path):
            print("lewati (tidak ada):", path); continue
        try:
            ok, fail, _ = convert_file(path, args.outdir, args.list, args.limit)
            tot_ok += ok; tot_fail += fail
        except Exception as e:
            print("ERROR memproses %s: %s" % (path, e))

    if not args.list:
        print("\nTOTAL: %d berhasil, %d gagal" % (tot_ok, tot_fail))


if __name__ == "__main__":
    main()
