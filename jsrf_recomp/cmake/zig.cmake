# Zig as the C compiler: the launcher's portable toolchain, so neither
# Visual Studio nor gcc has to be installed. ZIG_BIN is a folder with the
# launcher's small wrappers (zig-cc, zig-c++, zig-ar, zig-ranlib; .exe on
# Windows) that run "zig cc", "zig ar"...
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zig.cmake
#         -DZIG_BIN=/path/to/wrappers [-DZIG_TARGET=x86_64-linux-gnu.2.35]
#
# ZIG_TARGET defaults to this computer's system: Windows, or Linux with
# glibc 2.35 (so a build made on any distro also runs on SteamOS).
if(NOT ZIG_BIN)
    message(FATAL_ERROR "ZIG_BIN not set (the folder with the zig-cc wrapper).")
endif()
if(CMAKE_HOST_WIN32)
    set(_zx ".exe")
    set(_zdefault x86_64-windows-gnu)
else()
    set(_zx "")
    set(_zdefault x86_64-linux-gnu.2.35)
endif()
if(NOT ZIG_TARGET)
    set(ZIG_TARGET ${_zdefault})
endif()
# Only a target for another system makes this a cross build.
if(ZIG_TARGET MATCHES "windows" AND NOT CMAKE_HOST_WIN32)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
elseif(ZIG_TARGET MATCHES "linux" AND NOT CMAKE_HOST_UNIX)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
set(CMAKE_C_COMPILER   "${ZIG_BIN}/zig-cc${_zx}")
set(CMAKE_CXX_COMPILER "${ZIG_BIN}/zig-c++${_zx}")
set(CMAKE_AR           "${ZIG_BIN}/zig-ar${_zx}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${ZIG_BIN}/zig-ranlib${_zx}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET   ${ZIG_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${ZIG_TARGET})
# kept for the try_compile projects CMake makes while checking the compiler
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ZIG_BIN ZIG_TARGET)
