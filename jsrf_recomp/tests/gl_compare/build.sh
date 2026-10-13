#!/bin/sh
# Draws one test scene with the software rasteriser and with the GL backend
# and compares the two (Linux, needs SDL2 and an OpenGL 3.3 driver; Mesa's
# llvmpipe works headless). Run from this directory.
T=../../../xboxrecomp/src; G=$T/third_party/glad
gcc -O1 -g -I$T -I$T/kernel -I$T/../include -I$G/include $(sdl2-config --cflags) test.c \
    $T/kernel/nv2a_pb_exec.c $T/kernel/nv2a_vsh_interp.c $T/kernel/nv2a_combiner.c \
    $T/kernel/nv2a_gl.c $G/src/gl.c $(sdl2-config --libs) -lm -lpthread -o t || exit 1
RECOMP_RENDERER=sw ./t sw.raw 2>sw.log
SDL_VIDEODRIVER=offscreen ./t gl.raw 2>gl.log
python3 cmp.py
