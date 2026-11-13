# Vulkan renderer (vulkan-test branch): a second renderer beside OpenGL,
# chosen at run time with RECOMP_RENDERER=vulkan. OpenGL stays the default.
#
# Downloaded at build time, like SDL2 (cmake/sdl2.cmake):
#   Vulkan-Headers  the API's headers (nothing to link)
#   volk            loads vulkan-1.dll / libvulkan.so.1 at run time, so the
#                   game still starts on a computer without Vulkan
#   glslang         turns the renderer's GLSL into SPIR-V while the game runs
#                   (C++, built with the launcher's zig c++; no optimizer)
#
# A local copy instead of a download (a computer without the internet, or
# testing): -DFETCHCONTENT_SOURCE_DIR_XRVK_HEADERS=<dir> (and _XRVK_VOLK,
# _XRVK_GLSLANG). -DXBOXRECOMP_VULKAN=OFF builds without any of it.
option(XBOXRECOMP_VULKAN "Vulkan renderer beside OpenGL (RECOMP_RENDERER=vulkan)" ON)

if(XBOXRECOMP_VULKAN AND NOT TARGET xrvk_deps)
    include(FetchContent)
    set(XBOXRECOMP_VK_HEADERS_URL
        https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.tar.gz)
    set(XBOXRECOMP_VK_VOLK_URL
        https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.290.0.tar.gz)
    set(XBOXRECOMP_VK_GLSLANG_URL
        https://github.com/KhronosGroup/glslang/archive/refs/tags/15.1.0.tar.gz)

    # headers and volk: sources only (SOURCE_SUBDIR without a CMakeLists)
    FetchContent_Declare(xrvk_headers URL ${XBOXRECOMP_VK_HEADERS_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-build)
    FetchContent_Declare(xrvk_volk URL ${XBOXRECOMP_VK_VOLK_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-build)
    FetchContent_MakeAvailable(xrvk_headers xrvk_volk)

    # glslang: the library only (no tools, no HLSL, no optimizer, no tests)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(xrvk_glslang URL ${XBOXRECOMP_VK_GLSLANG_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(xrvk_glslang)

    # one target for the renderer to link: volk compiled here, the headers,
    # glslang and its default resource limits
    add_library(xrvk_deps STATIC ${xrvk_volk_SOURCE_DIR}/volk.c)
    target_include_directories(xrvk_deps PUBLIC
        ${xrvk_headers_SOURCE_DIR}/include ${xrvk_volk_SOURCE_DIR}
        ${xrvk_glslang_SOURCE_DIR})
    target_compile_definitions(xrvk_deps PUBLIC VK_NO_PROTOTYPES)
    if(WIN32)
        target_compile_definitions(xrvk_deps PUBLIC VK_USE_PLATFORM_WIN32_KHR)
    endif()
    target_link_libraries(xrvk_deps PUBLIC glslang glslang-default-resource-limits)
    if(UNIX)
        target_link_libraries(xrvk_deps PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()
