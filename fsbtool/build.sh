#!/usr/bin/env bash
# Portable build for fsbtool (Linux & Windows/MinGW via Git Bash).
# Uses arrays + per-file compile so paths containing spaces work.
set -e
cd "$(dirname "$0")"

SDK_AUDIO="${SDK_AUDIO:-C:/Users/Ridwan Hidayatullah/Documents/sdk audio}"
OGG="$SDK_AUDIO/libogg-1.3.6/libogg-1.3.6"
VORBIS="$SDK_AUDIO/libvorbis-1.3.7/libvorbis-1.3.7"
MINIAUDIO="$SDK_AUDIO/miniaudio-0.11.25/miniaudio-0.11.25"
MINIMP3="$SDK_AUDIO/minimp3-master/minimp3-master"

CC="${CC:-gcc}"
CFLAGS=(-O2 -std=gnu11 -Wall -Wno-unused-function)
INC=(-Isrc -Igen "-I$OGG/include" "-I$VORBIS/include" "-I$VORBIS/lib" "-I$MINIAUDIO" "-I$MINIMP3")

SRCS=(
  src/main.c src/fsb5.c src/fsb4.c src/fsb_vorbis.c src/decode.c src/wav.c src/player.c src/ma_impl.c
  src/adpcm.c src/mp3.c src/mp3_impl.c src/fsbcrypt.c
  gen/fsb_vorbis_setup_table.c
  "$OGG/src/framing.c" "$OGG/src/bitwise.c"
  "$VORBIS/lib/mdct.c" "$VORBIS/lib/smallft.c" "$VORBIS/lib/block.c" "$VORBIS/lib/window.c"
  "$VORBIS/lib/lsp.c" "$VORBIS/lib/lpc.c" "$VORBIS/lib/synthesis.c" "$VORBIS/lib/info.c"
  "$VORBIS/lib/floor1.c" "$VORBIS/lib/floor0.c" "$VORBIS/lib/res0.c" "$VORBIS/lib/mapping0.c"
  "$VORBIS/lib/registry.c" "$VORBIS/lib/codebook.c" "$VORBIS/lib/sharedbook.c"
  "$VORBIS/lib/lookup.c" "$VORBIS/lib/bitrate.c"
  "$VORBIS/lib/analysis.c" "$VORBIS/lib/envelope.c" "$VORBIS/lib/psy.c"
)

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OUT="fsbtool.exe"; LIBS=(-lole32 -lwinmm) ;;
  Darwin)               OUT="fsbtool";     LIBS=(-lm -lpthread) ;;
  *)                    OUT="fsbtool";     LIBS=(-lm -lpthread -ldl) ;;
esac

mkdir -p build/obj
OBJS=()
i=0
for s in "${SRCS[@]}"; do
  o="build/obj/$(printf '%03d' $i)_$(basename "$s" .c).o"
  echo "  CC $(basename "$s")"
  "$CC" "${CFLAGS[@]}" "${INC[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
  i=$((i+1))
done

echo "Linking -> $OUT"
"$CC" "${OBJS[@]}" -o "$OUT" "${LIBS[@]}"
echo "OK: $OUT"
