/* nv2a_rb.h: the renderer's backend interface (renderer backends plan, step 1).
 *
 * The renderer's own logic -- surfaces kept in step with guest memory, the
 * texture cache, stand-ins, widescreen, present fitting, the GLSL it
 * generates from combiner and vertex-program setups -- is written once and
 * talks to the GPU only through these calls. Two backends: OpenGL
 * (nv2a_rb_gl.c, the code that was in nv2a_gl.c) and native Vulkan (to come).
 *
 * Values that are NV2A / D3D enums (compare functions, blend factors and
 * equations, stencil ops, cull faces, front faces) are passed as the NV2A
 * gives them -- the same numbers as OpenGL's enums -- and each backend maps
 * them to its own. */
#ifndef NV2A_RB_H
#define NV2A_RB_H

#include <stdint.h>

/* ── fixed-function state (everything a draw needs that isn't a shader) ── */
typedef struct {
    uint8_t  depth_test, depth_mask;
    uint8_t  stencil_test;
    uint8_t  blend;
    uint8_t  cull;
    uint8_t  poly_offset;            /* the hardware depth bias (not the shader's) */
    uint8_t  cmask;                  /* colour writes: 1 R, 2 G, 4 B, 8 A */
    uint8_t  pad_;
    uint32_t depth_func;
    uint32_t stencil_func, stencil_ref, stencil_mask, stencil_wmask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t blend_src, blend_dst, blend_eq;
    float    blend_color[4];
    float    poly_factor, poly_units;
    uint32_t cull_face, front_face;
} RbFixed;

/* set for the draws that follow; the backend sends only what changed */
void rb_fixed(const RbFixed *f);
/* something outside rb_fixed changed that state: send it all next time */
void rb_fixed_lost(void);

/* ── programs ──────────────────────────────────────────────────────────── */
/* The renderer's say over the shader text and the program cache: lines to
 * put after each #version (NULL for none), and strings a cached program
 * depends on besides its sources. Asked for at each compile. */
void rb_shader_options(const char *(*defs)(void), int (*salt)(const char **out, int max));
/* a vertex and a fragment shader (GLSL 3.30) linked: a program handle, 0 if
 * it failed (the log says why). The OpenGL backend keeps linked programs
 * on disk (shader_cache). */
uint32_t rb_program_build(const char *vsrc, const char *fsrc);
/* one shader on its own, for programs put together by the caller (AA) */
uint32_t rb_shader_compile(int vertex, const char *src);
/* programs loaded from the cache, built, and the time building took (TM ticks) */
void rb_program_stats(unsigned *hits, unsigned *misses, uint64_t *ticks);

/* the program draws use from now on; a program no longer needed */
void rb_use_program(uint32_t p);
void rb_program_delete(uint32_t p);

/* ── uniforms ──────────────────────────────────────────────────────────── */
/* a uniform's location in a program (-1: not there); an array's elements
 * follow its first: name[i] is loc + i */
int  rb_uniform_loc(uint32_t p, const char *name);
/* a vec4 array's live length (0: not there, -1: its locations don't run on) */
int  rb_uniform_vec4_len(uint32_t p, const char *name, int loc0);
/* set on the program in use; each program keeps its own values */
void rb_u1i(int l, int a);
void rb_u1ui(int l, uint32_t a);
void rb_u1f(int l, float a);
void rb_u2f(int l, float a, float b);
void rb_u3f(int l, float a, float b, float c);
void rb_u4f(int l, float a, float b, float c, float d);
void rb_u1iv(int l, int n, const int32_t *v);
void rb_u1uiv(int l, int n, const uint32_t *v);
void rb_u1fv(int l, int n, const float *v);
void rb_u2fv(int l, int n, const float *v);
void rb_u3fv(int l, int n, const float *v);
void rb_u4fv(int l, int n, const float *v);
void rb_umat4(int l, int n, int transpose, const float *v);   /* n 4x4 matrices */

#endif
