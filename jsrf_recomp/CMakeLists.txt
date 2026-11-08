cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Jet Set Radio Future - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(jsrf_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# v249: built with Zig only (cmake/zig.cmake; the launcher sets it up), on
# Windows and Linux
if(MSVC)
    message(FATAL_ERROR "jsrf_recomp is built with Zig: use the launcher, or "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/zig.cmake (see cmake/zig.cmake).")
endif()

# Debug info for the toolkit libraries too (Release otherwise has none), so a
# crash or hang sample inside the runtime is named rather than "no symbol".
# Windows: line tables, written to jsrf_recomp.pdb (v248b).
if(WIN32)
    string(APPEND CMAKE_C_FLAGS_RELEASE " -g1")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g1")
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
if(WIN32)
    set(GAME_MAIN src/main.c)
else()
    set(GAME_MAIN src/main_linux.c)   # v244: the Linux build's start-up
endif()
# Which release of the game the generated code is for (generate_code.py
# writes src/recomp/gen/release.txt; none: the US one). For another release
# our own code's US addresses are moved to it at build time
# (tools/translate_release.py, from analysis/<tag>/addr_map.txt), so these
# two files are compiled from copies in the build folder.
set(JSRF_RELEASE usa)
set(_rel_file ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/release.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_rel_file})
if(EXISTS ${_rel_file})
    file(STRINGS ${_rel_file} _rel LIMIT_COUNT 1)
    if(_rel)
        set(JSRF_RELEASE ${_rel})
    endif()
endif()
set(JSRF_OWN_SOURCES
    src/recomp_manual.c
    src/jsrf_widescreen.c   # v187n: HUD at the screen edges, 4:3 screens (widescreen only)
    src/jsrf_dsound_lift.c  # v275: the DSOUND lift (RECOMP_DSOUND_LIFT=1)
    src/jsrf_chapter_select.c  # v276: chapter select (RECOMP_CHAPTER_SELECT)
)
if(NOT JSRF_RELEASE STREQUAL "usa")
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "The ${JSRF_RELEASE} release needs Python at build time "
                            "(-DPython3_EXECUTABLE=...): update the launcher")
    endif()
    message(STATUS "Game release: ${JSRF_RELEASE}")
    set(_moved)
    foreach(_src ${JSRF_OWN_SOURCES})
        get_filename_component(_name ${_src} NAME)
        set(_out ${CMAKE_CURRENT_BINARY_DIR}/release/${_name})
        add_custom_command(OUTPUT ${_out}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/translate_release.py
                    ${JSRF_RELEASE} ${CMAKE_CURRENT_SOURCE_DIR}/${_src} ${_out}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_src}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/translate_release.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/analysis/${JSRF_RELEASE}/addr_map.txt
            COMMENT "Moving ${_name} to the ${JSRF_RELEASE} release's addresses"
            VERBATIM)
        list(APPEND _moved ${_out})
    endforeach()
    set(JSRF_OWN_SOURCES ${_moved})
endif()
set(GAME_SOURCES
    ${GAME_MAIN}
    ${JSRF_OWN_SOURCES}
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(WIN32)
add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
else()
add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(WIN32)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput1_4   # by the DLL's name (MinGW)
    winmm
    shell32     # v140: copying the old saves over (SHFileOperation)
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)
else()
# Linux (v244): the toolkit's POSIX layer, SDL2 and OpenGL come with xboxrecomp
target_link_libraries(${PROJECT_NAME} PRIVATE m pthread dl)
# xboxrecomp.h names the D3D8 layer's header; the layer itself is not built
target_include_directories(${PROJECT_NAME} PRIVATE ${XBOXRECOMP_DIR}/src/d3d)
# Big generated functions: keep the build reasonable, symbols for crash reports
target_compile_options(${PROJECT_NAME} PRIVATE -g1 -fno-strict-aliasing -fwrapv
    -Wno-unused -Wno-unused-label -Wno-unused-but-set-variable)
# v256: function names for the stall sampler's host call chain (dladdr)
target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
# Each guest call is a host call; the main thread's stack is set by the
# launcher (ulimit) and guest threads get their own (see win32_compat.c)
endif()

# Windows:
#  - a 256 MB stack reserve (only reserved) for the game thread and every
#    guest thread: each guest call is a host C call and lifted frames are
#    much bigger than the x86 ones, so the 1 MB default overflows on deep
#    but legitimate call chains, and that kills the process silently
#  - image base 0x140000000 (v247): the default 0x400000 puts the exe, and
#    what loads next to it, in the low 4 GB the Xbox memory is mapped into
#  - a window program (v248c; CMake does not pass WIN32 on to Zig): as a
#    console program every log line went to a console window (slow), and
#    RECOMP_LOG was ignored (stderr already looked redirected)
if(WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--stack,268435456
        -Wl,--image-base,0x140000000 -Wl,--subsystem,windows)
    # v248: fast memcpy / memmove / memset / memcmp. Without them Zig links
    # its own, which copy a byte at a time (slow loading screens).
    target_sources(${PROJECT_NAME} PRIVATE ${XBOXRECOMP_DIR}/src/platform/win_memfuncs.c)
    set_source_files_properties(${XBOXRECOMP_DIR}/src/platform/win_memfuncs.c
        PROPERTIES COMPILE_OPTIONS "-fno-builtin")
    target_compile_options(${PROJECT_NAME} PRIVATE -fno-strict-aliasing -fwrapv
        -Wno-unused -Wno-unused-label -Wno-unused-but-set-variable)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
