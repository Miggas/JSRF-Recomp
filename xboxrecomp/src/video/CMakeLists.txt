# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC xbox_d3d8 mfplat mfreadwrite mfuuid mf ole32)
else()
    # v244: the player is Windows-only; the presenter is an SDL window
    find_package(SDL2 REQUIRED)
    target_include_directories(xbox_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../d3d)
    target_link_libraries(xbox_video PUBLIC platform SDL2::SDL2)
endif()
