# SDL2 for the whole toolkit, set up once (included by the top CMakeLists).
#
#   Windows: SDL2's source is downloaded and built statically
#            (SDL2::SDL2-static).
#   Linux:   the system's SDL2 development package (libsdl2-dev) when it is
#            there. Without it (Steam Deck, the launcher's own compiler) SDL2's
#            headers are taken from its source release and the game links the
#            system's libSDL2-2.0.so.0, which SteamOS and desktop Linux ship.
#            -DXBOXRECOMP_SDL2_RUNTIME=ON forces that.
set(XBOXRECOMP_SDL2_URL
    https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)

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
        FetchContent_Declare(SDL2 URL ${XBOXRECOMP_SDL2_URL}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(SDL2)
    endif()
else()
    option(XBOXRECOMP_SDL2_RUNTIME
        "Use SDL2's released headers and the system's libSDL2-2.0.so.0" OFF)
    if(NOT XBOXRECOMP_SDL2_RUNTIME AND NOT TARGET SDL2::SDL2)
        find_package(SDL2 QUIET)
    endif()
    if(NOT TARGET SDL2::SDL2)
        find_library(XBOXRECOMP_SDL2_SO
            NAMES libSDL2-2.0.so.0 SDL2-2.0 SDL2
            PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu
            NO_CMAKE_FIND_ROOT_PATH)
        if(NOT XBOXRECOMP_SDL2_SO)
            message(FATAL_ERROR "SDL2 not found: install SDL2 (libsdl2-2.0-0) "
                "or its development package (libsdl2-dev).")
        endif()
        include(FetchContent)
        # headers only: SOURCE_SUBDIR points at a folder without a
        # CMakeLists, so SDL2 itself is not built
        FetchContent_Declare(SDL2_headers URL ${XBOXRECOMP_SDL2_URL}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-build)
        FetchContent_MakeAvailable(SDL2_headers)
        add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
        set_target_properties(SDL2::SDL2 PROPERTIES
            IMPORTED_LOCATION "${XBOXRECOMP_SDL2_SO}"
            IMPORTED_NO_SONAME TRUE
            INTERFACE_INCLUDE_DIRECTORIES
                "${CMAKE_CURRENT_LIST_DIR}/../src/third_party/sdl2_linux;${sdl2_headers_SOURCE_DIR}/include")
        message(STATUS "SDL2: system runtime ${XBOXRECOMP_SDL2_SO}, released headers")
    endif()
endif()
