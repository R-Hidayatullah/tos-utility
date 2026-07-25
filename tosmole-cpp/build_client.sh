#!/usr/bin/env bash
# Build tosclient.exe — Tree of Savior offline game shell (Win32 + D3D11).
# Reuses the browser's IPF/asset modules; adds the client scene shell + a 2D
# renderer + TTF text + MP3 (minimp3) title BGM played through miniaudio.
set -e
cd "$(dirname "$0")"
MINGW="/c/Users/Ridwan Hidayatullah/Documents/codeblocks-25.03mingw-nosetup/MinGW/bin"
GXX="$MINGW/g++.exe"
GCC="$MINGW/gcc.exe"

ZINC="/c/Users/Ridwan Hidayatullah/Documents/sdk audio/zlib132/zlib-1.3.2"
STBINC="/c/Users/Ridwan Hidayatullah/Documents/sdk audio"
TINYXML2="/c/Users/Ridwan Hidayatullah/Documents/sdk audio/tinyxml2-11.0.0/tinyxml2-11.0.0"
SDK_AUDIO="/c/Users/Ridwan Hidayatullah/Documents/sdk audio"
FSB="/c/Users/Ridwan Hidayatullah/Documents/ToS SDK/fsbtool"
MINIAUDIO="$SDK_AUDIO/miniaudio-0.11.25/miniaudio-0.11.25"
MINIMP3="$SDK_AUDIO/minimp3-master/minimp3-master"

# Audio backend: fsb_player (non-blocking miniaudio device) + miniaudio impl.
# BGM decoding is done in bgm.cpp via minimp3 (header-only, no extra objects).
mkdir -p build/clientobj
for pair in "browser/fsb_player.c:fsb_player" "$FSB/src/ma_impl.c:ma_impl"; do
  src="${pair%%:*}"; name="${pair##*:}"
  o="build/clientobj/$name.o"
  if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
    echo "  CC $name"
    "$GCC" -O2 -std=gnu11 -Wno-unused-function -I"$MINIAUDIO" -Ibrowser -c "$src" -o "$o"
  fi
done

echo "  CXX tosclient.exe"
"$GXX" -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ \
  -Iinclude -Ibrowser -Iclient -I"$ZINC" -I"$STBINC" -I"$TINYXML2" \
  -I"$MINIAUDIO" -I"$MINIMP3" -I"$FSB/src" \
  client/tosclient_main.cpp client/client_gfx.cpp client/app.cpp client/ui.cpp \
  client/scene_login.cpp client/scene_charselect.cpp client/bgm.cpp \
  client/scene_loading.cpp client/scene_ingame.cpp \
  client/model3d.cpp client/char_model.cpp client/map_model.cpp client/tex_decode.cpp \
  browser/stb_impl.cpp browser/game_data.cpp browser/dds.cpp browser/xac_geometry.cpp \
  "$TINYXML2/tinyxml2.cpp" \
  src/ipf/ipf_archive.cpp src/ipf/ipf_fs.cpp src/ies/ies.cpp src/tsv/tsv.cpp \
  src/emfx/xac.cpp src/emfx/xsm.cpp src/emfx/xsmtime.cpp \
  build/zobj/adler32.o build/zobj/crc32.o build/zobj/inffast.o build/zobj/inflate.o build/zobj/inftrees.o build/zobj/zutil.o \
  build/clientobj/fsb_player.o build/clientobj/ma_impl.o \
  -o tosclient.exe \
  -ld3d11 -ld3dcompiler -ldxgi -lgdi32 -luser32 -lole32 -lwinmm
echo "built tosclient.exe"
