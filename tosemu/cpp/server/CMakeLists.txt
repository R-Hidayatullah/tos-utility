cmake_minimum_required(VERSION 3.16)
project(tosemu_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# No third-party dependencies: winsock and the standard library only. The
# inflate/deflate the IPF reader and the compressed packets need is in
# src/deflate.cpp rather than zlib, so a game install is the only external
# thing the server needs.
add_executable(tosemu_server
  src/main.cpp
  src/core.cpp
  src/deflate.cpp
  src/blowfish.cpp
  src/packet.cpp
  src/schema.cpp
  src/pktlog.cpp
  src/ipf.cpp
  src/ies.cpp
  src/gamedata.cpp
  src/properties.cpp
  src/actor.cpp
  src/world.cpp
  src/zone_send.cpp
  src/barrack.cpp
  src/zone.cpp
)

target_include_directories(tosemu_server PRIVATE src)

if(WIN32)
  target_link_libraries(tosemu_server PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(tosemu_server PRIVATE /W4)
else()
  target_compile_options(tosemu_server PRIVATE -Wall -Wextra)
  # Static runtime so the binary can be run from anywhere without MinGW's
  # libstdc++/libgcc/winpthread DLLs on PATH.
  target_link_options(tosemu_server PRIVATE
    -static-libgcc -static-libstdc++ -static)
endif()
