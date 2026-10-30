# JSRF launcher: set up, build, update, configure and play the game.
# Holds no game code, so the built launcher can be shared as it is.
#
#   cmake -S launcher -B build-launcher && cmake --build build-launcher --config Release
cmake_minimum_required(VERSION 3.20)
project(jsrf_launcher LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SDL2 as the game gets it: built statically on Windows, the system's on Linux
include(${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp/cmake/sdl2.cmake)

add_executable(jsrf_launcher WIN32
    src/main.c
    src/util.c
    src/ini.c
    src/jobs.c
)
target_include_directories(jsrf_launcher PRIVATE third_party/nuklear)

if(WIN32)
    target_link_libraries(jsrf_launcher PRIVATE SDL2::SDL2-static shell32)
    if(MSVC)
        target_compile_definitions(jsrf_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        # a window program, no console (MSVC gets that from WIN32 above)
        target_link_options(jsrf_launcher PRIVATE -Wl,--subsystem,windows)
    endif()
else()
    target_link_libraries(jsrf_launcher PRIVATE SDL2::SDL2 m)
endif()
if(NOT MSVC)
    target_compile_options(jsrf_launcher PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
