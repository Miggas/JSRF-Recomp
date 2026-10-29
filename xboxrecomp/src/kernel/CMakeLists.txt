cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_vsh_interp.c
    nv2a_combiner.c
)

# GPU rasterisation through OpenGL 3.3 (nv2a_gl.c). SDL2 provides the
# context: found on the system on Linux, downloaded and built statically on
# Windows. -DXBOXRECOMP_GL=OFF builds without it (software rasteriser only).
option(XBOXRECOMP_GL "Rasterise NV2A batches on the host GPU via OpenGL" ON)
if(XBOXRECOMP_GL)
    list(APPEND KERNEL_SOURCES nv2a_gl.c ../third_party/glad/src/gl.c)
else()
    list(APPEND KERNEL_SOURCES nv2a_gl_stub.c)
endif()

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

if(XBOXRECOMP_GL)
    target_include_directories(xbox_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/glad/include)
    if(WIN32)
        if(NOT TARGET SDL2::SDL2-static)
            include(FetchContent)
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST OFF CACHE BOOL "" FORCE)
            set(SDL_TESTS OFF CACHE BOOL "" FORCE)
            set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
            set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
            set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(SDL2
                URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            FetchContent_MakeAvailable(SDL2)
        endif()
        target_link_libraries(xbox_kernel PUBLIC SDL2::SDL2-static opengl32)
    else()
        find_package(SDL2 REQUIRED)
        find_package(OpenGL REQUIRED)
        target_link_libraries(xbox_kernel PUBLIC SDL2::SDL2 OpenGL::GL)
    endif()
endif()

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

# v244: on Linux the presenter is fb_present.c's SDL window
if(NOT WIN32)
    target_compile_definitions(xbox_kernel PRIVATE XBOXRECOMP_PRESENTER=1)
endif()
