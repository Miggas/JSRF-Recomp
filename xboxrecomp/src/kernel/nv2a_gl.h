/**
 * NV2A rasterisation on the host GPU through OpenGL 3.3 core.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) still does everything up to the
 * triangle: it decodes methods, runs vertex programs and the fixed-function
 * transform on the CPU, and hands this backend screen-space vertices plus the
 * pipeline state. This file turns those into GL draws -- depth, stencil,
 * blending and culling as GL state, the register combiners as one fragment
 * shader that interprets the combiner words the title programmed.
 *
 * Surfaces stay authoritative in guest memory. Each colour surface has a GL
 * framebuffer that is filled from guest memory when its contents changed
 * behind the GPU's back (a CPU write, the software path) and written back to
 * guest memory when the executor moves to another surface or presents. The
 * window, the presentation path and any texture that samples a render target
 * therefore see exactly what they saw with the software rasteriser.
 *
 * RECOMP_RENDERER=sw keeps the software rasteriser; the default is this
 * backend whenever a GL 3.3 context can be created, falling back to software
 * when it cannot.
 */
#ifndef XBOXRECOMP_NV2A_GL_H
#define XBOXRECOMP_NV2A_GL_H

#include <stdint.h>
#include <stddef.h>
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"

typedef struct {
    int      valid;
    uint32_t offset;          /* guest VA of texel (0,0) (already resolved)  */
    uint32_t width, height, pitch, color, palette, levels, filter;
    uint32_t addr_u, addr_v;
    int      cube, normalised;/* normalised: coords in [0,1], else texels    */
    uint32_t bytes;           /* guest bytes the image occupies (all faces)  */
    uint32_t face_stride;
} NvGlTexture;

typedef struct {
    /* Colour surface: guest VA, its host pointer, pitch, bytes a pixel (2/4),
     * and the size of the GL framebuffer to keep for it. host may be NULL
     * when the address must not be written (the executor refused it). */
    uint32_t surf_va;
    uint8_t *surf_host;
    uint32_t pitch, bpp, width, height;
    int      surf_has_alpha;   /* A8R8G8B8 target: destination alpha stored */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t zeta_key;         /* zeta offset, or 0 */
    float    zmax;             /* depth units of the zeta format */

    uint32_t depth_test, depth_func, depth_mask;
    uint32_t stencil_test, stencil_wmask, stencil_func, stencil_ref,
             stencil_mask, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t blend_enable, blend_sfactor, blend_dfactor, blend_equation,
             blend_color;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t poly_offset;            /* depth bias on, factor, units */
    float    poly_factor, poly_units;
    uint32_t zdirect_off;            /* a vertex behind the eye: GL's depth */
    uint32_t screen_space;           /* v97: D3D's pre-transformed pass-through
                                      * program: positions are final screen
                                      * coordinates, drawn flat, never clipped by w */
    uint32_t alpha_test, alpha_func, alpha_ref;
    float    ws_k;                   /* v187: widescreen x squeeze, 1 (or 0) = none */
    float    ws_c;                   /* v187l: ...about this x (NDC): the drawing area's centre */
    uint32_t ws_2d;                  /* v187: a 2D batch: vertices at the screen's
                                      * left/right edge stay there (fades) */
    uint32_t ws_edge;                /* v191: a full-screen layer: corners at the
                                      * surface edge pushed one pixel past it */

    int          rc_seen;
    Nv2aCombiner rc;
    uint32_t     clip_plane_mode;
    float        fog_color[4];   /* r g b a */
    NvGlTexture  tex[4];
    uint32_t     bg_far;         /* v200d: the backdrop, drawn at the far plane */
    uint32_t     ws_stretch;     /* v209: a menu part drawn across the whole width */
    float        ws_cut;         /* v217c: on a row-filled menu, k: kept off the columns the
                                    sides are filled from (< 0: not drawn at all) */
} NvGlState;

/* One vertex as the GL backend's shader reads it. */
typedef struct {
    float pos[4], d0[4], d1[4], t[4][4], fog, pad[3];
} NvGlVertex;

static inline void nv2a_gl_vertex_from(NvGlVertex *g, const Nv2aVshOutput *o, float fog)
{
    int i;
    for (i = 0; i < 4; i++) {
        g->pos[i] = o->pos[i]; g->d0[i] = o->d0[i]; g->d1[i] = o->d1[i];
        g->t[0][i] = o->tex[0][i]; g->t[1][i] = o->tex[1][i];
        g->t[2][i] = o->tex[2][i]; g->t[3][i] = o->tex[3][i];
    }
    g->fog = fog;
}

/* 1 once a context exists and the shaders built; 0 means use software. */
int  nv2a_gl_active(void);

/* Triangles: n_idx indices into v (three per triangle, already wound so
 * that culling sees the title's facing), fogf[i] the fog factor of v[i]. */
void nv2a_gl_draw(const NvGlState *st, const Nv2aVshOutput *v,
                  const float *fogf, const uint32_t *idx, uint32_t n_idx);

/* Indexed drawing straight from the vertex buffer. map_vertices hands out
 * room for n vertices (write-only; any thread may fill it), draw_mapped draws
 * n_idx indices into them (0..n-1) and releases the room; unmap_vertices
 * releases it without drawing. Exactly one draw_mapped or unmap per map. */
NvGlVertex *nv2a_gl_map_vertices(uint32_t n);
void nv2a_gl_draw_mapped(const NvGlState *st, const uint32_t *idx, uint32_t n_idx);
void nv2a_gl_unmap_vertices(void);

/* v77: vertex processing on the GPU. The executor uploads each distinct
 * vertex's input attributes as they are (k float4s a vertex, in the order of
 * `slots`) and the GL vertex shader runs the fixed-function pipeline or the
 * title's vertex program on them -- the work transform_vertex/ff_transform
 * did on the CPU. */
#define NV_GL_VS_FF      1
#define NV_GL_VS_PROGRAM 2
typedef struct {
    int      mode;                   /* NV_GL_VS_FF or NV_GL_VS_PROGRAM */
    uint32_t present;                /* bit a: the batch supplies v_a */
    /* program (mode 2) */
    const uint32_t (*prog)[4];
    int      nins;
    uint32_t prog_gen;
    /* both: the constants (FF reads c[59], the viewport offset) */
    const float *consts;
    uint32_t const_gen;
    /* fixed function (mode 1) -- row-major, as the executor keeps them */
    struct NvGlFF {
        float    composite[16], mv[4][16], inv_mv0[16];
        float    texmat[4][16], texplane[4][4][4];
        int32_t  skin_mode, lighting, texmat_on[4], texgen[4][4];
        float    scene_ambient[3], emission[3], mat_alpha;
        uint32_t color_material, light_enable, specular_on;
        float    l_amb[8][3], l_dif[8][3], l_spc[8][3], l_half[8][3];
        float    l_dir[8][3], l_pos[8][3], l_att[8][3], l_range[8];
    } ff;
    int32_t  fog_enable;
    uint32_t fog_mode;
    float    fog_param[2];
} NvGlVsState;

/* 1 when the GPU vertex path can run (shader built, limits enough). */
int    nv2a_gl_gpu_vs_ok(void);
/* v79: cumulative shader compiles, texture uploads and GPU waits, for the
 * worst-frame breakdown in the [PERF] report. */
void   nv2a_gl_frame_stats(uint64_t *compiles, uint64_t *compile_ticks, uint64_t *texups,
                           uint64_t *texup_ticks, uint64_t *wait_ticks, uint64_t *freq);
/* v78: build (once) the shader for this packed vertex program; 0 = it
 * cannot run on the GPU, keep it on the CPU. */
int    nv2a_gl_prepare_vsh(const uint32_t (*prog)[4], int n);
/* Room for n vertices of k float4 inputs each; NULL when it cannot. */
float *nv2a_gl_map_raw(uint32_t n, uint32_t k);
/* Draw n_idx indices over the mapped vertices, whose k inputs are
 * attributes slots[0..k-1]. Exactly one draw_raw or unmap_raw per map. */
void   nv2a_gl_draw_raw(const NvGlState *st, const NvGlVsState *vs,
                        const uint8_t *slots, uint32_t k,
                        const uint32_t *idx, uint32_t n_idx);
void   nv2a_gl_unmap_raw(void);
/* v147: the batch's vertices as the title stored them, for the GPU vertex
 * path: each of k inputs (attribute loc, NV097 type 0 D3DCOLOR / 2 float /
 * 4 unsigned byte / 5 short, size components) at byte rel within stream
 * `stream`; ns streams of len[] bytes from src[], stride[] apart. Copied
 * into the vertex ring as they are, GL converting them; vertex 0 of the
 * streams is index 0 of the draw. Then exactly one draw_raw or unmap_raw.
 * 0 when it cannot (no persistent ring, too big). */
/* v149: type NV_GL_NAT_CONST is an input with one value for the whole
 * batch, cval[i], instead of an array */
#define NV_GL_NAT_CONST 0xFF
typedef struct { uint8_t loc, type, size, stream; uint32_t rel; } NvGlNatAttr;
int    nv2a_gl_map_native(uint32_t k, const NvGlNatAttr *a, const float (*cval)[4],
                          uint32_t ns, const uint8_t *const *src, const uint32_t *len,
                          const uint32_t *stride);

/* CLEAR_SURFACE param bits (Z 0x01, stencil 0x02, colour 0xF0). colour is
 * the clear value already widened to A8R8G8B8; zclear in depth units. */
void nv2a_gl_clear(const NvGlState *st, uint32_t param, uint32_t argb,
                   float zclear, uint32_t sclear);
/* The same, limited to the clear rectangle x0..x1, y0..y1 (inclusive,
 * surface pixels, y down) as SET_CLEAR_RECT_HORIZONTAL/VERTICAL give it. */
void nv2a_gl_clear_rect(const NvGlState *st, uint32_t param, uint32_t argb,
                        float zclear, uint32_t sclear,
                        int x0, int x1, int y0, int y1);

/* Write a surface the GPU has drawn back to guest memory. surf_va 0: every
 * surface with pending GPU output. */
void nv2a_gl_sync_to_guest(uint32_t surf_va);

/* A flip of surf_va: starts its read-back without waiting and returns the
 * guest address of the frame that is now in guest memory to show (the one
 * the previous flip started), surf_va itself when it was already current, or
 * 0 when there is nothing new to show yet. */
uint32_t nv2a_gl_present(uint32_t surf_va);

/* Frame boundary: per-frame texture validation restarts. */
void nv2a_gl_end_frame(void);

/* Statistics for the [PERF] report. */
void nv2a_gl_stats(uint64_t *draws, uint64_t *tris, uint64_t *uploads,
                   uint64_t *readbacks, uint64_t *tex_uploads);
/* Render-target textures copied on the GPU, and frames read for the window. */
/* ...and surfaces whose guest copy the CPU wrote while the GPU held them. */
void nv2a_gl_stats2(uint64_t *gpu_copies, uint64_t *present_reads, uint64_t *cpu_taken);
void nv2a_gl_stats3(uint64_t *tex_compressed);
void nv2a_gl_probe_top(uint32_t out[5]);   /* v92: frame-trace colour probe */
void nv2a_gl_probe_at(const int *x, const int *y, int n, uint32_t *out); /* v98 */
float nv2a_gl_probe_z(int i);   /* v99: depth at probe point i after probe_at */
int nv2a_gl_submit_stats(char *buf, size_t n);
int nv2a_gl_call_stats(char *buf, size_t n);   /* v169 */
void nv2a_gl_vis_enable(int on);     /* v123: visibility tests on the GPU */
void nv2a_gl_vis_clear(void);
uint32_t nv2a_gl_vis_count(void);
void nv2a_gl_trace_frame(int on);     /* v121: per-draw sample counts on traced frames */
void nv2a_gl_trace_draw(int draw_no);
void nv2a_gl_trace_report(void);
int nv2a_gl_gpu_frame_stats(char *buf, size_t n);   /* v101: GPU-side frame spacing */
/* v89: frames drawn into the window on the GPU since the last call, how
 * many frames the GPU was behind at each present, and their spacing. */
void nv2a_gl_window_stats(uint64_t *frames, double *behind_avg, uint64_t *behind_max,
                          uint64_t *behind2, double *iv_max_ms, uint64_t *iv20,
                          uint64_t *iv34);

/* Accumulated time: [0] draw calls (state, upload, draw), [1] readbacks,
 * [2] texture change checks, [3] texture uploads, in *freq ticks a second. */
void nv2a_gl_times(uint64_t t[4], uint64_t *freq);

/* Provided by nv2a_pb_exec.c: decode texture stage `stage` face `face` to
 * width*height A8R8G8B8 texels, row 0 first. Returns 0 on failure. */
int nv2a_exec_decode_texture(int stage, uint32_t face, uint32_t *out);

#endif
