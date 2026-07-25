cmake_minimum_required(VERSION 3.16)
project(tosmole_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(tos_emfx
    src/emfx/xac.cpp
)
target_include_directories(tos_emfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(emfx_dump src/tools/emfx_dump.cpp)
target_link_libraries(emfx_dump PRIVATE tos_emfx)

# IES (tabular data) + TSV (language tables) parsers — C++ ports of ies.rs/tsv.rs.
add_library(tos_data
    src/ies/ies.cpp
    src/tsv/tsv.cpp
)
target_include_directories(tos_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(tos_data PUBLIC Threads::Threads)
endif()

add_executable(ies_dump src/tools/ies_dump.cpp)
target_link_libraries(ies_dump PRIVATE tos_data)
