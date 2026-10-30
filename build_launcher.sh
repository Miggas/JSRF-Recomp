#!/usr/bin/env bash
# Builds jsrf_launcher for Linux (Steam Deck included).
# Needs only curl and tar: Zig (the compiler) and SDL2's headers are
# downloaded into deps/ the first time. The launcher uses the system's SDL2,
# which SteamOS and desktop Linux have.
set -e
cd "$(dirname "$0")"

ZIG_URL="https://files.pythonhosted.org/packages/0c/b7/ecfdce6368cc3664d301f7f52db4fe1004aa7da7a12c4a9bf1de534ff6ab/ziglang-0.13.0-py3-none-manylinux_2_12_x86_64.manylinux2010_x86_64.musllinux_1_1_x86_64.whl"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz"
ZIG=deps/zig/ziglang/zig
SDL_INC=deps/SDL2-2.30.9/include

mkdir -p deps
if [ ! -x "$ZIG" ]; then
    echo "Downloading Zig (about 80 MB)..."
    curl -fL -o deps/zig.zip "$ZIG_URL"
    rm -rf deps/zig && mkdir -p deps/zig
    # a zip: whichever unpacker is there
    if command -v unzip >/dev/null 2>&1; then unzip -q deps/zig.zip -d deps/zig
    elif command -v bsdtar >/dev/null 2>&1; then bsdtar -xf deps/zig.zip -C deps/zig
    else python3 -m zipfile -e deps/zig.zip deps/zig
    fi
    chmod +x "$ZIG"
    rm -f deps/zig.zip
fi
if [ ! -f "$SDL_INC/SDL.h" ]; then
    echo "Downloading SDL2's headers..."
    curl -fL -o deps/sdl2.tar.gz "$SDL_URL"
    tar -xzf deps/sdl2.tar.gz -C deps
    rm -f deps/sdl2.tar.gz
fi

SDL_SO=""
for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu; do
    if [ -e "$d/libSDL2-2.0.so.0" ]; then SDL_SO="$d/libSDL2-2.0.so.0"; break; fi
done
if [ -z "$SDL_SO" ]; then
    echo "SDL2 not found: install it (libsdl2-2.0-0 on Ubuntu / Debian, sdl2 on Arch)."
    exit 1
fi

echo "Building..."
# glibc 2.35: built on any distro, it also runs on SteamOS
export ZIG_GLOBAL_CACHE_DIR="$PWD/deps/zig-cache"
"$ZIG" cc -target x86_64-linux-gnu.2.35 -O2 -std=gnu11 \
    -Ithird_party/nuklear -Ithird_party/sdl2_linux -I"$SDL_INC" \
    src/main.c src/util.c src/ini.c src/jobs.c "$SDL_SO" -lm \
    -o jsrf_launcher
echo "Built: jsrf_launcher"
