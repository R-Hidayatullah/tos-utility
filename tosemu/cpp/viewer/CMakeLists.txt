cmake_minimum_required(VERSION 3.16)
project(tos_view CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

enable_language(RC)

# Win32 only -- no toolkit. The manifest is what turns on Common Controls v6
# and per-monitor DPI, so it is not optional.
add_executable(tos_view WIN32
  main.cpp
  capture.cpp
  filter.cpp
  hexview.cpp
  ptemplate.cpp
  theme.cpp
  app.rc
)

target_link_libraries(tos_view PRIVATE comctl32 uxtheme gdi32 comdlg32 shell32)

# Everything here calls the W entry points and passes wide resource ids like
# IDC_ARROW, which resolve to the ANSI form without this.
target_compile_definitions(tos_view PRIVATE UNICODE _UNICODE)

if(MSVC)
  target_compile_options(tos_view PRIVATE /W4)
else()
  target_compile_options(tos_view PRIVATE -Wall -Wextra -municode)
  target_link_options(tos_view PRIVATE
    -municode -mwindows -static-libgcc -static-libstdc++ -static)
endif()
