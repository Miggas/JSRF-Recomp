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
#include <stddef.h>

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
/* step 2 (vk10): the same, built in the background. A handle at once (0 if
 * this backend can't: build it with rb_program_build then); `like` is a
 * program drawn in its place meanwhile, whose pipelines are made for it
 * too (0: none). rb_program_ready: 1 when it can be drawn with (*id is then
 * the program), 0 not yet, -1 it failed. rb_program_delete takes either. */
uint32_t rb_program_build_async(const char *vsrc, const char *fsrc, uint32_t like);
int  rb_program_ready(uint32_t *id);
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

/* ── bindings ──────────────────────────────────────────────────────────── */
/* Targets are framebuffer handles (0: the window). The backend caches what
 * is bound; rb_bindings_lost / rb_vbuf_lost when something else changed it. */
void     rb_bind_draw(uint32_t target);
uint32_t rb_draw_bound(void);                 /* (uint32_t)-1: not known */
void     rb_draw_bound_set(uint32_t target);
void     rb_viewport(int w, int h);
void     rb_scissor(int x, int y, int w, int h);
void     rb_scissor_get(int sc[4]);
void     rb_scissor_once(int x, int y, int w, int h);
void     rb_bindings_lost(void);
void     rb_active_unit(int u);
void     rb_unit_bind(int u, uint32_t target_kind, uint32_t tex);   /* GL_TEXTURE_2D / _CUBE_MAP */
void     rb_bind_vbuf(uint32_t b);
void     rb_vbuf_lost(void);
void     rb_vbuf_bound_set(uint32_t b);
/* a whole image from one target to another, scaled, no scissor or masks */
void     rb_copy_target(uint32_t src, int sw, int sh, uint32_t dst, int dw, int dh, uint32_t filter);

/* ── anti-aliasing and MSAA (v238) ─────────────────────────────────────── */
/* the finished frame (texture src, w x h; src_fbo: a single-sampled target
 * holding it -- the resolved copy with MSAA) through FXAA / SMAA
 * (RECOMP_GL_AA): the target holding the result, or 0 when AA is off */
uint32_t rb_present_aa(uint32_t src, uint32_t src_fbo, int w, int h);
/* a single-sampled image into a multisampled target, scaled to w x h */
void     rb_ms_upscale(uint32_t src_tex, uint32_t dst_target, int w, int h);

/* ── the window ────────────────────────────────────────────────────────── */
/* a finished frame for the window: its target and texture (and the target
 * the multisampled frame was resolved into, else 0), at its rendered size,
 * with the side fill of wide menus (v217b; 0 = none) */
typedef struct {
    uint32_t fbo, tex, resolve;
    int w, h;
    double rowk;
} RbFrame;
/* shown in the game window, fitted (4:3 or widescreen); 0: the window path
 * can't (yet), the caller shows it another way */
int  rb_window_present(const RbFrame *f);
/* drawn over each shown frame: our menus (flip: rows top-down, DXGI) */
void rb_set_overlay(void (*fn)(int cw, int ch, int flip));
/* Linux: the SDL window the GL context was made with */
/* v101: the GPU-side spacing of shown frames, for the log; 0 = nothing yet */
int  rb_gpu_frame_stats(char *buf, size_t n);
/* v89: the window's pacing since the last call (nv2a_gl_window_stats) */
void rb_window_stats(uint64_t *frames, double *behind_avg, uint64_t *behind_max,
                     uint64_t *behind2, double *iv_max_ms, uint64_t *iv20, uint64_t *iv34);

/* ── render targets ────────────────────────────────────────────────────── */
void     rb_target_shape(uint32_t *fbo, uint32_t *tex, uint32_t *ms_tex, int w, int h, int alpha, int samples);
void     rb_depth_make(uint32_t *buf, int *is_tex, int w, int h, int samples, int depth32f, double clear_depth);
uint32_t rb_target_depth(uint32_t fbo, uint32_t buf, int is_tex);
void     rb_surface_upload(uint32_t fbo, uint32_t tex, int gw, int gh, int sw, int sh,
                           const uint32_t *px, int samples);
void     rb_read_pixels(uint32_t fbo, int w, int h, uint32_t *px);
uint32_t rb_resolve(uint32_t fbo, uint32_t tex, int w, int h);
uint32_t rb_target_scaled(uint32_t fbo, int sw, int sh, int gw, int gh);

/* ── textures (on the active unit's binding; OpenGL's numbers) ─────────── */
void rb_tex_new(uint32_t *id);
void rb_tex_delete(uint32_t *id);
void rb_tex_image(uint32_t target, int level, int ifmt, int w, int h, int border,
                  uint32_t format, uint32_t type, const void *data);
void rb_tex_image_dxt(uint32_t target, int level, uint32_t cfmt, int w, int h, int border,
                      int size, const void *data);
void rb_tex_param(uint32_t target, uint32_t pname, int v);
void rb_tex_mipmaps(uint32_t target);
void rb_errors_clear(void);
int  rb_no_error(void);
int  rb_max_aniso(void);
void rb_tex_from_target(uint32_t id, int remake, int one_alpha, int w, int h, uint32_t src_fbo, int sh);

/* ── vertex data, fences, draws ────────────────────────────────────────── */
void rb_buf_bind(uint32_t target, uint32_t b);
void rb_buf_data(uint32_t target, intptr_t size, const void *data, uint32_t usage);
void rb_buf_sub(uint32_t target, intptr_t off, intptr_t size, const void *data);
void rb_buf_storage(uint32_t target, intptr_t size, const void *data, uint32_t flags);
void *rb_buf_map(uint32_t target, intptr_t off, intptr_t len, uint32_t access);
int rb_buf_unmap(uint32_t target);
void rb_buf_new(int n, uint32_t *ids);
void rb_buf_delete(int n, const uint32_t *ids);
void *rb_fence(uint32_t cond, uint32_t flags);
uint32_t rb_fence_wait(void *f, uint32_t flags, uint64_t timeout_ns);
void rb_fence_delete(void *f);
void rb_finish(void);
void rb_va_new(int n, uint32_t *ids);
void rb_va_bind(uint32_t va);
void rb_attr_on(uint32_t loc);
void rb_attr_off(uint32_t loc);
void rb_attr_pointer(uint32_t loc, int size, uint32_t type, int norm, int stride, const void *ptr);
void rb_attr_format(uint32_t loc, int size, uint32_t type, int norm, uint32_t rel);
void rb_attr_binding(uint32_t loc, uint32_t stream);
void rb_vbufs(uint32_t first, int n, const uint32_t *bufs, const intptr_t *offs, const int *strides);
void rb_attr_const4fv(uint32_t loc, const float *v);
void rb_attr_const4f(uint32_t loc, float x, float y, float z, float w);
void rb_draw_elements(uint32_t mode, int count, uint32_t type, const void *off, int base);
void rb_draw_arrays(uint32_t mode, int first, int count);

/* ── clearing ── */
void rb_clear(int color, const float rgba[4], int alpha, int depth, double zval, int stencil, int sval);

/* ── occlusion queries ── */
void rb_query_new(int n, uint32_t *ids);
void rb_query_begin(uint32_t id);
void rb_query_end(void);
void rb_query_result(uint32_t id, uint32_t *v);

/* ── reading pixels ── */
void rb_read_pixels_buf(uint32_t fbo, int w, int h, uint32_t buf);
void rb_read_pixel(uint32_t fbo, int x, int y, uint32_t *c);
int  rb_read_depth(uint32_t fbo, int x, int y, float *z);


/* ── start-up ─────────────────────────────────────────────────────────── */
typedef struct {
    const char *version, *renderer, *vendor;   /* for the log */
    int max_target;            /* largest render target side */
    int max_samples;           /* MSAA */
    int vs_uniform_comps;      /* vertex-stage uniform components */
    int persistent_buffers;    /* mapped rings possible (GL: ARB_buffer_storage) */
    int dxt;                   /* S3TC textures */
    int aniso;                 /* anisotropic filtering */
    int attrib_binding;        /* vertex formats apart from buffers (GL: VAB + multi_bind) */
} RbCaps;
/* The device (GL: an SDL window and a 3.3 core context, its functions
 * loaded). 0 when there is none; the log says why. */
int  rb_init(void);
const RbCaps *rb_caps(void);
/* the state every draw starts from: depth clamp, scissor on, `clip_planes`
 * clip distances (the depth-range cull), tight pixel rows */
void rb_init_state(int clip_planes);
/* from here on the backend's calls run on its thread (GL: when it is on) */
void rb_thread_start(void);
/* the window's events, once a frame */
void rb_pump(void);
/* a texture onto the active unit, past the binding caches (start-up only) */
void rb_tex_bind_raw(uint32_t target, uint32_t id);
/* errors: all dropped; the next one (0 for none) */
void rb_errors_drain(void);
uint32_t rb_error(void);


/* ── our menus over the frame (the quit window, the list menu, notes) ── */
typedef struct { float x, y, u, v, r, g, b, a; } RbOvVert;   /* window pixels, row 0 at the top; u < 0: no texture */
/* the shader and vertex layout, and the font atlas (one channel, may be
 * NULL) as a texture; what was bound is put back. 0 when it can't be. */
int  rb_overlay_init(const unsigned char *atlas, int w, int h);
/* triangles over the frame of cw x ch pixels in the framebuffer bound for
 * drawing, alpha-blended; flip: that framebuffer's row 0 is the top (DXGI);
 * font: the atlas is there. What was bound is put back. */
void rb_overlay_draw(const RbOvVert *v, int n, int cw, int ch, int flip, int font);


/* ── the backend's thread and counters ────────────────────────────────── */
int  rb_threaded(void);            /* calls run on a thread of their own */
void rb_frame_end(void);           /* a frame queued (at most RECOMP_GL_THREAD_LAG) */
void rb_gpu_frame_mark(void);      /* GPU timing: a frame's work starts here */
/* the backend calls made since the last report, per draw, the most made,
 * and the thread's waits: one line for [PERF]; 0 when not counted */
int  rb_call_stats(char *buf, size_t n, uint64_t draws, uint64_t layouts_new);
/* the on-disk shader cache: on, and the file for a pair of sources */
int  rb_shader_cache_on(void);
void rb_shader_cache_path(char *b, size_t n, const char *vsrc, const char *fsrc);

#endif
