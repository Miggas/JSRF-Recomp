# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
    # v189: the controllers XInput does not see (DualSense, DualShock 4,
    # Switch Pro) through the SDL2 the renderer already builds
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(xbox_input PUBLIC SDL2::SDL2-static)
        target_compile_definitions(xbox_input PRIVATE XBOX_INPUT_SDL=1)
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
