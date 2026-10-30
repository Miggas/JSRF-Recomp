#!/usr/bin/env bash
# Native Linux build (Steam Deck, desktop Linux). Builds the game and puts
# jsrf_recomp into the game folder, next to default.xbe, with jsrf.ini for
# its settings -- the same layout build_portable.bat makes on Windows.
#
#   ./build_linux.sh              game folder = game/ (here)
#   ./build_linux.sh /path/JSRF   a game folder somewhere else
#
# Needs: a C compiler, cmake, python3, and the SDL2 and OpenGL development
# files. On Ubuntu / Debian (WSL, or a distrobox on the Steam Deck):
#   sudo apt install build-essential cmake ninja-build python3 libsdl2-dev
# On Arch: sudo pacman -S base-devel cmake ninja python sdl2
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"
DEST="${1:-$HERE/game}"

if [ ! -f "$DEST/default.xbe" ]; then
    echo "default.xbe not found in \"$DEST\"."
    echo "Put the game files there first, or pass the game folder:"
    echo "  ./build_linux.sh /path/to/game"
    exit 1
fi

missing=""
for t in cmake python3; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || missing="$missing gcc"
if command -v pkg-config >/dev/null 2>&1 && ! pkg-config --exists sdl2; then
    missing="$missing libsdl2-dev"
fi
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    echo "Ubuntu / Debian: sudo apt install build-essential cmake ninja-build python3 libsdl2-dev"
    echo "Arch:            sudo pacman -S base-devel cmake ninja python sdl2"
    exit 1
fi

# The recompiled code is made from your own default.xbe, not shipped. Code
# generated on Windows (src/recomp/gen copied over) works here as it is.
if [ ! -f src/recomp/gen/recomp_dispatch.c ]; then
    echo "No recompiled code yet: generating it from $DEST/default.xbe (a few minutes)..."
    python3 tools/generate_code.py "$DEST/default.xbe"
fi
python3 tools/wrap_dsound.py "$HERE/src/recomp/gen"

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

# Under WSL the Windows drives (/mnt/c...) refuse some of what CMake does
# to its build folder ("Operation not permitted"), and are slow: the build
# folder then goes in your Linux home instead. The finished program still
# goes to the game folder.
BUILD="build-linux"
case "$HERE" in
    /mnt/*) BUILD="$HOME/jsrf-build-linux" ;;
esac
# a setup that failed halfway leaves a cache without the build files:
# start that folder over
if [ ! -f "$BUILD/build.ninja" ] && [ ! -f "$BUILD/Makefile" ]; then
    rm -rf "$BUILD"
    echo "Configuring..."
    cmake -S . -B "$BUILD" $GEN -DCMAKE_BUILD_TYPE=Release
fi
echo "Building (Release, the first time takes a while)..."
cmake --build "$BUILD" --parallel "$(nproc 2>/dev/null || echo 2)"

cp -f "$BUILD/jsrf_recomp" "$DEST/jsrf_recomp"
# (a Windows drive under WSL keeps no permissions: mark it executable on
# the Deck instead, see the README)
chmod +x "$DEST/jsrf_recomp" 2>/dev/null || true

# Your jsrf.ini is never overwritten.
if [ ! -f "$DEST/jsrf.ini" ]; then
    cp portable/jsrf.ini "$DEST/jsrf.ini"
    echo "Created \"$DEST/jsrf.ini\"."
else
    echo "Kept your \"$DEST/jsrf.ini\"."
fi

# The new build may make its shaders differently: the compiled ones go,
# recipes.bin stays (the shaders are compiled again in the menus).
if [ -d "$DEST/shader_cache" ]; then
    find "$DEST/shader_cache" -maxdepth 1 -name '*.bin' ! -name 'recipes.bin' -delete
    echo "Cleared the shader cache in \"$DEST/shader_cache\" (kept recipes.bin)."
fi

echo
echo "Build ready: \"$DEST/jsrf_recomp\""
echo "Run it from there, or add it to Steam as a non-Steam game (no Proton)."
