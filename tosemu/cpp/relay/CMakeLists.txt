cmake_minimum_required(VERSION 3.16)
project(tos_relay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Winsock and the standard library, nothing else -- the relay has to be
# copyable to a machine that only has the game on it.
add_executable(tos_relay
  main.cpp
  common.cpp
  log.cpp
  proto.cpp
  dump.cpp
  guard.cpp
  httpd.cpp
  proxy.cpp
  stats.cpp
)

if(WIN32)
  target_link_libraries(tos_relay PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(tos_relay PRIVATE /W4)
else()
  target_compile_options(tos_relay PRIVATE -Wall -Wextra)
  # Static runtime: the relay is run from the game directory as often as from
  # the build tree, and MinGW's DLLs are on neither PATH.
  target_link_options(tos_relay PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
