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

#endif
