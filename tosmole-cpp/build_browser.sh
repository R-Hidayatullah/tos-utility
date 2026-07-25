#!/usr/bin/env bash
# Build tosbrowser.exe (Win32 + D3D11 IPF/XAC/XSM viewer + FSB audio).
set -e
cd "$(dirname "$0")"
MINGW="/c/Users/Ridwan Hidayatullah/Documents/codeblocks-25.03mingw-nosetup/MinGW/bin"
GXX="$MINGW/g++.exe"
GCC="$MINGW/gcc.exe"
ZINC="/c/Users/Ridwan Hidayatullah/Documents/sdk audio/zlib132/zlib-1.3.2"
STBINC="/c/Users/Ridwan Hidayatullah/Documents/sdk audio"
TINYXML2="/c/Users/Ridwan Hidayatullah/Documents/sdk audio/tinyxml2-11.0.0/tinyxml2-11.0.0"

# FSB sound-bank decoder (RE'd fsbtool) + its audio deps. Compiled as C with gcc
# (same MinGW toolchain as g++) into build/fsbobj, then linked into the exe.
SDK_AUDIO="/c/Users/Ridwan Hidayatullah/Documents/sdk audio"
FSB="/c/Users/Ridwan Hidayatullah/Documents/ToS SDK/fsbtool"
OGG="$SDK_AUDIO/libogg-1.3.6/libogg-1.3.6"
VORBIS="$SDK_AUDIO/libvorbis-1.3.7/libvorbis-1.3.7"
MINIAUDIO="$SDK_AUDIO/miniaudio-0.11.25/miniaudio-0.11.25"
MINIMP3="$SDK_AUDIO/minimp3-master/minimp3-master"

FSB_INC=(-I"$FSB/src" -I"$FSB/gen" -I"$OGG/include" -I"$VORBIS/include" -I"$VORBIS/lib" -I"$MINIAUDIO" -I"$MINIMP3")
FSB_SRCS=(
  "$FSB/src/fsb5.c" "$FSB/src/fsb4.c" "$FSB/src/fsb_vorbis.c" "$FSB/src/decode.c"
  "$FSB/src/adpcm.c" "$FSB/src/mp3.c" "$FSB/src/mp3_impl.c" "$FSB/src/fsbcrypt.c"
  "$FSB/src/ma_impl.c"
  "$FSB/gen/fsb_vorbis_setup_table.c"
  browser/fsb_player.c
  "$OGG/src/framing.c" "$OGG/src/bitwise.c"
  "$VORBIS/lib/mdct.c" "$VORBIS/lib/smallft.c" "$VORBIS/lib/block.c" "$VORBIS/lib/window.c"
  "$VORBIS/lib/lsp.c" "$VORBIS/lib/lpc.c" "$VORBIS/lib/synthesis.c" "$VORBIS/lib/info.c"
  "$VORBIS/lib/floor1.c" "$VORBIS/lib/floor0.c" "$VORBIS/lib/res0.c" "$VORBIS/lib/mapping0.c"
  "$VORBIS/lib/registry.c" "$VORBIS/lib/codebook.c" "$VORBIS/lib/sharedbook.c"
  "$VORBIS/lib/lookup.c" "$VORBIS/lib/bitrate.c"
  "$VORBIS/lib/analysis.c" "$VORBIS/lib/envelope.c" "$VORBIS/lib/psy.c"
)
mkdir -p build/fsbobj
FSB_OBJS=()
i=0
for s in "${FSB_SRCS[@]}"; do
  o="build/fsbobj/$(printf '%03d' $i)_$(basename "$s" .c).o"
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then
    echo "  CC $(basename "$s")"
    "$GCC" -O2 -std=gnu11 -Wno-unused-function "${FSB_INC[@]}" -c "$s" -o "$o"
  fi
  FSB_OBJS+=("$o")
  i=$((i+1))
done

"$GXX" -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ \
  -Iinclude -I"$ZINC" -I"$STBINC" -I"$TINYXML2" -Ibrowser \
  -I"$FSB/src" -I"$FSB/gen" \
  browser/browser_main.cpp browser/d3d_renderer.cpp browser/dds.cpp browser/stb_impl.cpp \
  browser/model_render.cpp browser/xac_geometry.cpp browser/game_data.cpp \
  browser/psb.cpp \
  "$TINYXML2/tinyxml2.cpp" \
  src/ipf/ipf_archive.cpp src/ipf/ipf_fs.cpp src/ies/ies.cpp src/tsv/tsv.cpp \
  src/emfx/xac.cpp src/emfx/xsm.cpp src/emfx/xsmtime.cpp \
  build/zobj/adler32.o build/zobj/crc32.o build/zobj/inffast.o build/zobj/inflate.o build/zobj/inftrees.o build/zobj/zutil.o \
  "${FSB_OBJS[@]}" \
  -o tosbrowser.exe \
  -ld3d11 -ld3dcompiler -ldxgi -lcomctl32 -lgdi32 -luser32 -lole32 -lshell32 -lwinmm
echo "built tosbrowser.exe"
