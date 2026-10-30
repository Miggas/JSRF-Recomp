@echo off
rem Builds jsrf_launcher.exe.
rem Needs only Windows 10 or 11 (curl and tar come with it): Zig (the
rem compiler) and SDL2 are downloaded into deps\ the first time.
setlocal
cd /d "%~dp0"

set "ZIG_URL=https://files.pythonhosted.org/packages/e0/0c/66f4710aedf5544e15779ff9c6e770322986e15daf9299b1a4df0be19e09/ziglang-0.13.0-py3-none-win_amd64.whl"
set "SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz"
set "ZIG=deps\zig\ziglang\zig.exe"
set "SDL=deps\SDL2-2.30.9\x86_64-w64-mingw32"

if not exist deps mkdir deps
if exist "%ZIG%" goto have_zig
echo Downloading Zig (about 80 MB)...
curl -fL -o deps\zig.zip "%ZIG_URL%" || goto fail
if exist deps\zig rmdir /s /q deps\zig
mkdir deps\zig
tar -xf deps\zig.zip -C deps\zig || goto fail
del deps\zig.zip
:have_zig

if exist "%SDL%\lib\libSDL2.a" goto have_sdl
echo Downloading SDL2...
curl -fL -o deps\sdl2.tar.gz "%SDL_URL%" || goto fail
tar -xzf deps\sdl2.tar.gz -C deps || goto fail
del deps\sdl2.tar.gz
:have_sdl

echo Building...
set "ZIG_GLOBAL_CACHE_DIR=%~dp0deps\zig-cache"
"%ZIG%" cc -target x86_64-windows-gnu -O2 -std=gnu11 ^
  -Ithird_party\nuklear -I"%SDL%\include\SDL2" ^
  src\main.c src\util.c src\ini.c src\jobs.c "%SDL%\lib\libSDL2.a" ^
  -lsetupapi -lwinmm -limm32 -lversion -lole32 -loleaut32 -lgdi32 -luser32 -lshell32 -ladvapi32 ^
  -Wl,--subsystem,windows -o jsrf_launcher.exe
if errorlevel 1 goto fail
echo Built: jsrf_launcher.exe
if "%CI%"=="" pause
exit /b 0

:fail
echo.
echo BUILD FAILED - the error is above.
if "%CI%"=="" pause
exit /b 1
