cmake_minimum_required(VERSION 3.15)
project(fsbtool C)

set(CMAKE_C_STANDARD 11)

# Location of the provided third-party audio libraries (override with -DSDK_AUDIO=...).
set(SDK_AUDIO "C:/Users/Ridwan Hidayatullah/Documents/sdk audio" CACHE PATH "sdk audio root")
set(OGG       "${SDK_AUDIO}/libogg-1.3.6/libogg-1.3.6")
set(VORBIS    "${SDK_AUDIO}/libvorbis-1.3.7/libvorbis-1.3.7")
set(MINIAUDIO "${SDK_AUDIO}/miniaudio-0.11.25/miniaudio-0.11.25")
set(MINIMP3   "${SDK_AUDIO}/minimp3-master/minimp3-master")

add_executable(fsbtool
    src/main.c src/fsb5.c src/fsb4.c src/fsb_vorbis.c src/decode.c src/wav.c src/player.c src/ma_impl.c
    src/adpcm.c src/mp3.c src/mp3_impl.c src/fsbcrypt.c
    gen/fsb_vorbis_setup_table.c
    ${OGG}/src/framing.c ${OGG}/src/bitwise.c
    ${VORBIS}/lib/mdct.c ${VORBIS}/lib/smallft.c ${VORBIS}/lib/block.c ${VORBIS}/lib/window.c
    ${VORBIS}/lib/lsp.c ${VORBIS}/lib/lpc.c ${VORBIS}/lib/synthesis.c ${VORBIS}/lib/info.c
    ${VORBIS}/lib/floor1.c ${VORBIS}/lib/floor0.c ${VORBIS}/lib/res0.c ${VORBIS}/lib/mapping0.c
    ${VORBIS}/lib/registry.c ${VORBIS}/lib/codebook.c ${VORBIS}/lib/sharedbook.c
    ${VORBIS}/lib/lookup.c ${VORBIS}/lib/bitrate.c
    ${VORBIS}/lib/analysis.c ${VORBIS}/lib/envelope.c ${VORBIS}/lib/psy.c)

target_include_directories(fsbtool PRIVATE
    src gen ${OGG}/include ${VORBIS}/include ${VORBIS}/lib ${MINIAUDIO} ${MINIMP3})

if(WIN32)
    target_link_libraries(fsbtool PRIVATE ole32 winmm)
elseif(APPLE)
    target_link_libraries(fsbtool PRIVATE m pthread)
    find_library(COREAUDIO CoreAudio)
    find_library(AUDIOTOOLBOX AudioToolbox)
    target_link_libraries(fsbtool PRIVATE ${COREAUDIO} ${AUDIOTOOLBOX})
else()
    target_link_libraries(fsbtool PRIVATE m pthread dl)
endif()
