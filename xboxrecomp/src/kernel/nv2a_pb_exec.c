/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_gl.h"
#include "nv2a_combiner.h"
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#include "recomp_fastclock.h"   /* v169: per-draw timing from rdtsc */
#else
#define fast_qpc_li(q) QueryPerformanceCounter(q)
#define NV_TLS __thread
#include <pthread.h>
#include <unistd.h>
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Set on a raster worker while it runs xf_rows (raster_flush). */
static NV_TLS int t_in_xf;
static int s_ff_active;   /* the batch being clipped is fixed-function */
static int s_pre_active;  /* ...and pre-transformed (XYZRHW): no offset either */
static int s_fog_direct;  /* fog[0] already is the fog factor */
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
/* Semaphores: how D3D learns how far the GPU has got.
 *
 * D3D_SetFence pushes NV097_BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) with
 * the fence time, and D3D_BlockOnTime spins until that value appears at
 * the semaphore address, which D3D reads through CDevice+0x34. The address
 * is the base of DMA object 0xC plus NV097_SET_SEMAPHORE_OFFSET (0x1D6C).
 * The DMA object lives in instance memory this runtime does not model, so
 * the title tells the executor the CPU-side VA it polls, through
 * xbox_Nv2aSetSemaphoreVA. Until it does, a release is only recorded.
 * Without this, every wait for GPU idle (KickOffAndWaitForIdle, Swap with
 * a fence, BlockUntilIdle) spins forever: Jet Set Radio Future hung in
 * BlockOnTime right after its first Swap. */
static volatile uint32_t s_sem_va;
static uint32_t s_sem_offset;
static volatile uint32_t s_sem_last;
static volatile int s_sem_pending;

static void sem_write(uint32_t value)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    *(volatile uint32_t *)(mem + s_sem_va + s_sem_offset) = value;
}

void xbox_Nv2aSetSemaphoreVA(uint32_t va)
{
    if (s_sem_va == va)
        return;
    s_sem_va = va;
    fprintf(stderr, "[GPU] semaphore at 0x%08X (title-supplied)\n", va);
    if (va && s_sem_pending)
        sem_write(s_sem_last);
}

static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_PALETTE         0x1B20   /* offset | size<<2 | dma */
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  4 x u8 */
#define NV097_SET_VERTEX_DATA2S           0x1900   /* + attr*4,  2 x s16 */
#define NV097_SET_VERTEX_DATA4S_M         0x1980   /* + attr*8,  4 x s16 */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
/* An immediate-mode vertex: all 16 attributes as float4. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
/* Indices one BEGIN/END may carry. This was 4096, and JSRF draws stage
 * meshes with more: the rest of such a batch was dropped, which is the
 * platform-with-stairs that showed only as its outline. */
#define NV_MAX_INDICES  131072
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
    uint32_t ctl0;                      /* v119: SET_TEXTURE_CONTROL0     */
    int      ctl0_seen;                 /* ...written at least once      */
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    int        imm_pos4;                /* the last position had a real 4th component */
    int        imm_batch;               /* the batch being drawn is immediate-mode */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    /* v186d: window clip 0 (SET_WINDOW_CLIP_HORIZONTAL/VERTICAL, inclusive
     * min and max) and its type (0 = draw inside). D3D's viewports restrict
     * drawing with it: JSRF's split screen draws each player's view across
     * the whole screen, shifted, and clips it to its half. */
    uint32_t wclip_x0, wclip_x1, wclip_y0, wclip_y1, wclip_type, wclip_set;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear;
    /* Stencil: test enable, write mask, func/ref/mask, ops (GL enums). */
    uint32_t stencil_test, stencil_wmask, stencil_func, stencil_ref,
             stencil_mask, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t batches_noop_colour;
    /* Fixed-function transform state (TRANSFORM_EXECUTION_MODE 0). The
     * composite matrix takes a position to screen space before the divide:
     * x/w, y/w in pixels less the viewport offset (c[59]), z/w in depth
     * units. With skinning off it is the whole model-to-screen transform;
     * with skinning on D3D folds only view-projection-viewport into it and
     * the blended model-view matrices take the vertex to eye space first. */
    /* Face culling: CULL_FACE_ENABLE, CULL_FACE (0x404 front, 0x405 back,
     * 0x408 both), FRONT_FACE (0x900 CW, 0x901 CCW). */
    uint32_t cull_enable, cull_face, front_face;
    uint32_t tris_culled;
    /* Depth bias (D3D's ZBIAS): POLY_OFFSET_FILL_ENABLE, factor, units. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
    float    ff_mv[4][16], ff_composite[16];
    /* Texture coordinate generation and texture matrices, per stage:
     * TEXGEN_S/T/R/Q modes, eye planes, TEXTURE_MATRIX_ENABLE, the
     * matrices, and the inverse model-view the eye normal needs. */
    uint32_t ff_texgen[4][4], ff_texmat_on[4];
    float    ff_texplane[4][4][4], ff_texmat[4][16], ff_inv_mv0[16];
    uint32_t ff_skin_mode, ff_lighting, ff_composite_set;
    uint32_t batches_ff, verts_ff;
} s_gpu;

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

/* v119: CONTROL0's ENABLE bit (30). D3D's SetTexture(stage, NULL) leaves
 * the stage's offset and format as they were and clears only this, so a
 * stage it switched off went on sampling its last texture: JSRF's smoke
 * puffs, drawn with the cel-shading combiners and no ramp at stage 1, took
 * their colour from a 4444 particle atlas left there (pink, teal, cream).
 * A disabled stage reads as the stand-in, white, like any unusable one.
 * RECOMP_TEX_ENABLE_BIT=0 ignores the bit (pre-v119). */
static int tex_enable_bit(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_TEX_ENABLE_BIT");
        v = !(e && e[0] == '0');
    }
    return v;
}

static void tex_update_valid(Texture *t)
{
    t->valid = t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch)
            && !(t->ctl0_seen && !(t->ctl0 & (1u << 30)) && tex_enable_bit());
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    tex_update_valid(&s_gpu.texs[0]);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

/* Slot+1 of each method in s_unhandled. Checked against the slot because the
 * report reorders the table; a stale entry falls back to the search once. */
static uint16_t s_unh_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    int i;
    uint16_t k = s_unh_slot[(method & 0x1FFC) >> 2];

    s_gpu.unhandled_total++;
    if (k && k <= s_unhandled_count && s_unhandled[k - 1].method == method) {
        s_unhandled[k - 1].count++;
        s_unhandled[k - 1].last_param = param;
        return;
    }
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            s_unh_slot[(method & 0x1FFC) >> 2] = (uint16_t)(i + 1);
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        s_unh_slot[(method & 0x1FFC) >> 2] = (uint16_t)s_unhandled_count;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
/* Attributes whose current (inline) value the title has set with
 * SET_VERTEX_DATA*. On the NV2A an attribute with no array enabled reads that
 * value for every vertex -- D3D sets a constant diffuse colour this way for a
 * vertex format without one, which is how menu panels and the pause map get
 * their flat colours. Reading the default instead drew them white (run 58).
 * RECOMP_NO_CONST_ATTRS=1 restores the old default. */
static uint16_t s_imm_ever;

/* v151: settings read once. An uncached getenv at the end of every draw
 * (RECOMP_PB_EXEC_VERBOSE) cost ~4 us a draw under Proton, whose environment
 * is long and which the CRT walks on every call: 2.1 s in every 10 in a busy
 * area on the Deck (run 150), more than all the GL calls together. */
static int pb_verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    return v;
}
static int passthrough_3d(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_GL_PASSTHROUGH_3D") != NULL;
    return v;
}

static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride) {
        static int off = -1;
        ptrdiff_t k = a - s_gpu.attr;
        if (off < 0)
            off = getenv("RECOMP_NO_CONST_ATTRS") != NULL;
        if (!off && k >= 0 && k < NV_VERTEX_ATTRS && ((s_imm_ever >> k) & 1)) {
            memcpy(out, s_gpu.imm_attr[k], 4 * sizeof(float));
            return 1;
        }
        return 0;
    }
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);

static void note_drawn(void)
{
    /* A surface no display mode can show is a render target, not a frame.
     * JSRF's graffiti picker composites its preview into a 1024x1024 target
     * each frame; being the largest surface drawn, it won the frame and was
     * presented instead of the 640x480 back buffer -- a black screen (run
     * 61). */
    if (s_gpu.clip_w > 1280 || s_gpu.clip_h > 720)
        return;
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (NV097_SET_SURFACE_FORMAT_COLOR).
     * Deriving it from pitch / clip width -- the only way before -- is right
     * only while the clip spans the whole surface: Burnout 3 narrows the clip
     * to a 250-pixel window to draw its option values, 2560 / 250 came out as
     * 10 bytes a pixel, and every such text quad was refused. */
    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(offset)
                           + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* On the NV2A the clear value is in the surface's own format, and
             * the XDK's D3D packs it before pushing SET_COLOR_CLEAR_VALUE: a
             * 16-bit target gets the 5:6:5 (or 1:5:5:5) word in the low half.
             * Jet Set Radio Future clears its 565 target to white with
             * 0x0000FFFF and to Dolby's 0x231F20 with 0x000020E4; reading
             * those as A8R8G8B8 painted the Smilebit logo's white background
             * cyan and the Dolby logo's black one blue. A value with anything
             * in the high half cannot be a packed 16-bit word, so it is still
             * reduced from 8:8:8:8 as before. */
            uint16_t v = (s_gpu.clear_color & 0xFFFF0000u)
                ? (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                           | ((s_gpu.clear_color >> 5) & 0x07E0)
                           | ((s_gpu.clear_color >> 3) & 0x001F))
                : (uint16_t)s_gpu.clear_color;
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1) {                       /* wrap */
        if (!(size & (size - 1)))
            return c & (size - 1);         /* power of two: two's complement wraps */
        return (uint32_t)(((sc % n) + n) % n);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);   /* clamp, and the rest */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static int texel_decode(const Texture *t, uint32_t fmt, const uint8_t *p,
                        uint32_t u, uint32_t *argb);

/* Sample texel (u, v) of stage texture `t`; `face` offsets a cube map. */
static int sample_tex(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset;

    if (!t->valid)
        return 0;
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        /* Decoded blocks, direct-mapped by address. Every texel of a DXT
         * texture decodes its whole 4x4 block, and bilinear reads four
         * neighbours that nearly always share one -- this was a tenth of
         * the executor's time. Keyed on the block's bytes as well as its
         * address, so a texture rewritten in place (a render target reused,
         * a streamed mip) never returns stale texels.
         * ponytail: single-threaded executor, so a plain static table. */
        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = d3d8_format_dxt_block_bytes(fmt);
        uint32_t bx = u >> 2, by = v >> 2, bw = (t->width + 3) >> 2;
        const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(mem + base, fmt, bx * 4 + (i & 3),
                                      by * 4 + (i >> 2), t->width,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + base;
        {
            /* The masks depend only on the size; build them once per size
             * per thread rather than for every texel fetched. */
            static NV_TLS uint32_t mw, mh, mx, my;
            if (mw != t->width || mh != t->height) {
                xbox_swizzle_masks(t->width, t->height, &mx, &my);
                mw = t->width; mh = t->height;
            }
            u = swizzle_deposit(u, mx) | swizzle_deposit(v, my);
        }
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }
    return texel_decode(t, fmt, p, u, argb);
}

/* Texel u of row (or swizzled image) p, in linear format fmt. */
static int texel_decode(const Texture *t, uint32_t fmt, const uint8_t *p,
                        uint32_t u, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static uint32_t pack_color(const float c[4]);

/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    /* Shared counters, so not from the raster workers: sixteen threads
     * incrementing one cache line was a stall per pixel. raster_flush adds
     * their pixels in afterwards. */
    if (!t_in_xf) {
        s_gpu.pixels++;
        if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
            s_gpu.pixel_max = argb;
    }
    row = s_surface + (size_t)y * s_gpu.pitch;

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[x];
        } else {
            uint32_t t = ((const uint16_t *)row)[x];
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Depth buffers, host-side, one per zeta surface the title uses. The real one
 * lives in guest memory in a tiled format nothing here decodes, and nothing
 * the title does reads it back, so a float buffer per zeta offset is enough. */
#define NV_ZBUF_W 1024
#define NV_ZBUF_H 1024
static struct { uint32_t offset; float *z; uint8_t *s; } s_zbufs[4];
static int s_ftrace;            /* frame trace: 0 idle, 2 tracing, 3 done */
static uint32_t s_ftrace_draw;  /* v99: draws traced so far this frame */
static int s_zbuf_next;

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

static float *zbuf_current(int create)
{
    int i;
    size_t k;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == s_gpu.zeta_offset)
            return s_zbufs[i].z;
    if (!create)
        return NULL;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].s)
        s_zbufs[i].s = (uint8_t *)malloc((size_t)NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].z || !s_zbufs[i].s)
        return NULL;
    s_zbufs[i].offset = s_gpu.zeta_offset;
    for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    memset(s_zbufs[i].s, 0, (size_t)NV_ZBUF_W * NV_ZBUF_H);
    return s_zbufs[i].z;
}

/* The stencil plane that goes with depth buffer z. */
static uint8_t *sbuf_for(const float *z)
{
    int i;
    for (i = 0; i < 4; i++)
        if (z && s_zbufs[i].z == z)
            return s_zbufs[i].s;
    return NULL;
}

static int stencil_pass(uint8_t stored)
{
    uint32_t m = s_gpu.stencil_mask & 0xFF;
    uint32_t r = s_gpu.stencil_ref & m, v = stored & m;
    switch (s_gpu.stencil_func) {                 /* reference OP stored */
    case 0x200: return 0;
    case 0x201: return r <  v;
    case 0x202: return r == v;
    case 0x203: return r <= v;
    case 0x204: return r >  v;
    case 0x205: return r != v;
    case 0x206: return r >= v;
    default:    return 1;
    }
}

static void stencil_apply(uint8_t *sp, uint32_t op)
{
    uint32_t old = *sp, v, w = s_gpu.stencil_wmask & 0xFF;
    switch (op) {
    case 0x1E00: return;                          /* KEEP     */
    case 0x0000: v = 0; break;                    /* ZERO     */
    case 0x1E01: v = s_gpu.stencil_ref & 0xFF; break; /* REPLACE */
    case 0x1E02: v = old < 255 ? old + 1 : 255; break; /* INCRSAT */
    case 0x1E03: v = old ? old - 1 : 0; break;    /* DECRSAT  */
    case 0x150A: v = ~old & 0xFF; break;          /* INVERT   */
    case 0x8507: v = (old + 1) & 0xFF; break;     /* INCR wrap */
    case 0x8508: v = (old - 1) & 0xFF; break;     /* DECR wrap */
    default:     return;
    }
    *sp = (uint8_t)((old & ~w) | (v & w));
}

static void zbuf_clear(uint32_t which)
{
    float *z = zbuf_current(1), v = zclear_value();
    uint8_t *sb = sbuf_for(z);
    size_t k;
    if (z && (which & 1))
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
    /* Z24S8 keeps stencil in the low byte of the clear value. */
    if (sb && (which & 2))
        memset(sb, (int)(s_gpu.zstencil_clear & 0xFF), (size_t)NV_ZBUF_W * NV_ZBUF_H);
}

static int depth_pass(float z, float stored)
{
    switch (s_gpu.depth_func) {                   /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_texel(const Texture *t, uint32_t face, float u, float v,
                     float out[4])
{
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
    }
    /* TENT (bilinear) when the title asks for it, magnifying or minifying:
     * a 64x32 sky gradient stretched over the screen is blocks with nearest
     * texels and a gradient with four. MAG 2 is tent; MIN 2 is tent at LOD 0
     * and 4/6 the tent mip modes.
     * ponytail: level 0 only, no mip selection -- minified textures shimmer.
     * Add LOD from the screen-space derivative when that shows. */
    if (mag == 2 || min == 2 || min == 4 || min == 6) {
        float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
        int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;
        wu = fu - (float)iu;
        wv = fv - (float)iv;
        for (k = 0; k < 4; k++) {
            if (sample_tex(t, face, (uint32_t)(iu + (k & 1)),
                           (uint32_t)(iv + (k >> 1)), &texel))
                nv2a_rc_unpack(texel, c[k]);
            else
                c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
        }
        for (j = 0; j < 4; j++)
            out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                   + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
        return;
    }
    if (sample_tex(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v), &texel))
        nv2a_rc_unpack(texel, out);
    else
        out[0] = out[1] = out[2] = out[3] = 1.0f;
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int rc_stage_fetch(int st, const float c[4], float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        rc_texel(t, 0, c[0] / q, c[1] / q, out);
        return 1;
    case 3: {                                     /* CUBEMAP */
        float x = c[0], y = c[1], z = c[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
        uint32_t face;
        if (!t->cube) {
            rc_texel(t, 0, c[0], c[1], out);
            return 1;
        }
        if (ax >= ay && ax >= az) {
            face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
        } else {
            face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
        }
        if (ma == 0.0f)
            ma = 1.0f;
        rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
                 (tc / ma + 1.0f) * 0.5f, out);
        return 1;
    }
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:
        /* ponytail: bump-env and dot-product modes read zero; they are water
         * and bump effects -- add them when a title's look depends on one. */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    switch (s_gpu.alpha_func) {
    case 0x200: return 0;                         /* NEVER    */
    case 0x201: return v <  r;                    /* LESS     */
    case 0x202: return v == r;                    /* EQUAL    */
    case 0x203: return v <= r;                    /* LEQUAL   */
    case 0x204: return v >  r;                    /* GREATER  */
    case 0x205: return v != r;                    /* NOTEQUAL */
    case 0x206: return v >= r;                    /* GEQUAL   */
    default:    return 1;                         /* ALWAYS   */
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float *zb;
    uint8_t *sb;                  /* stencil plane, when the test is on */
    int minx, maxx, miny, maxy, use_rc, textured;
    int colour_noop;              /* blend/mask leave colour unchanged */
} XfTri;

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k;

    for (y = y0; y < T->maxy; y += step) {
        /* Only the span the triangle covers on this row. Walking the whole
         * bounding box tested every pixel of it, and a long thin triangle
         * -- a shadow-volume side, a rail, a cable -- has a box hundreds of
         * times its area: most of the renderer's time went there. The
         * per-pixel edge test below still decides coverage, so the span is
         * padded by a pixel either side and only has to be conservative. */
        int xs = T->minx, xe = T->maxx;
        {
            const float *P[3] = {a, b, c};
            float cy = (float)y + 0.5f, lo = 3.4e38f, hi = -3.4e38f;
            int e;
            for (e = 0; e < 3; e++) {
                const float *p = P[e], *q = P[(e + 1) % 3];
                if ((p[1] - cy) * (q[1] - cy) > 0.0f)
                    continue;                 /* edge does not cross this row */
                if (p[1] == q[1]) {
                    if (p[0] < lo) lo = p[0];
                    if (p[0] > hi) hi = p[0];
                    if (q[0] < lo) lo = q[0];
                    if (q[0] > hi) hi = q[0];
                } else {
                    float xi = p[0] + (cy - p[1]) * (q[0] - p[0]) / (q[1] - p[1]);
                    if (xi < lo) lo = xi;
                    if (xi > hi) hi = xi;
                }
            }
            if (lo > hi)
                continue;
            if (lo - 1.0f > (float)xs) xs = (int)floorf(lo) - 1;
            if (hi + 2.0f < (float)xe) xe = (int)ceilf(hi) + 2;
            if (xs < T->minx) xs = T->minx;
            if (xe > T->maxx) xe = T->maxx;
        }
        for (x = xs; x < xe; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                uint8_t *sp = T->sb ? &T->sb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (!T->colour_noop) || s_gpu.alpha_test;
                /* Stencil and depth test, then shade, then alpha test, and
                 * only then the stencil/depth writes: an alpha-tested texel
                 * that is cut away must not leave its depth behind (foliage,
                 * fences). With alpha test on, a stencil op must wait for it
                 * too, so the tests are only decided after shading then. */
                int s_ok = sp ? stencil_pass(*sp) : 1;
                int z_ok = (zp && s_gpu.depth_test) ? depth_pass(z, *zp) : 1;
                int late = sp && s_gpu.alpha_test;
                if (!late) {
                    if (!s_ok) {
                        stencil_apply(sp, s_gpu.stencil_fail);
                        continue;
                    }
                    if (!z_ok) {
                        if (sp) stencil_apply(sp, s_gpu.stencil_zfail);
                        cnt->depth_fail++;
                        continue;
                    }
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            for (j = 0; j < 4; j++)
                                tc[j] = (l0 * T->stc[0][st][j] + l1 * T->stc[1][st][j]
                                       + l2 * T->stc[2][st][j]) / pw;
                            keep = rc_stage_fetch(st, tc, t[st]);
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                               (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (late) {
                    if (!s_ok) {
                        stencil_apply(sp, s_gpu.stencil_fail);
                        continue;
                    }
                    if (!z_ok) {
                        stencil_apply(sp, s_gpu.stencil_zfail);
                        cnt->depth_fail++;
                        continue;
                    }
                }
                if (sp)
                    stencil_apply(sp, s_gpu.stencil_zpass);
                if (zp && s_gpu.depth_mask)
                    *zp = z;
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (T->colour_noop)
                    continue;              /* colour unchanged: depth/stencil only */
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Worker threads for big triangles.
 *
 * The rasteriser is a software GPU, and a Burnout 3 frame is dominated by a
 * handful of full-screen passes -- composites, blurs, the menu backdrops --
 * each two triangles of 300,000 pixels through the register combiners. Those
 * split cleanly by row: every pixel reads only its own depth and colour, so
 * interleaved rows on N threads need no locking. Small triangles stay on the
 * executor thread, where waking workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host.
 * ponytail: s_gpu.pixels/pixel_max in put_pixel are unsynchronised stats and
 * may undercount; nothing depends on them. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192

/* Triangles are queued per batch and rasterised together.
 *
 * The first version split each big triangle's rows across threads and ran
 * small ones on the executor thread. JSRF draws ~270,000 triangles a second
 * averaging a few hundred pixels, so nearly all of them were small: 96% of
 * the wall clock went to rasterising, on one core, at ~2 FPS.
 *
 * Now a batch's triangles are queued (raster_xf_triangle copies the three
 * vertices in) and raster_flush hands the whole queue to the pool. Thread k
 * owns screen rows y % n == k and walks every queued triangle in order, so
 * each pixel is still written by one thread in submission order -- blending
 * and depth come out exactly as before -- with one wake-up per batch instead
 * of one per triangle. Pipeline state is constant inside a batch, and the
 * queue is flushed before the batch returns, so nothing the workers read
 * changes under them. */
static uint64_t s_perf_vertex, s_perf_pixel, s_perf_vhits, s_perf_vruns;
typedef struct { XfTri T; Nv2aVshOutput v[3]; } XfJob;
#define NV_XF_QUEUE 8192
static XfJob *s_xfq;
static int s_xfq_n;
static uint64_t s_xfq_area;                 /* sum of bounding boxes */

#if defined(_WIN32)
static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool;

static void xf_rows_owned(int k, int n, XfCount *cnt)
{
    int j;
    t_in_xf = 1;
    for (j = 0; j < s_xfq_n; j++) {
        const XfTri *T = &s_xfq[j].T;
        int y0 = T->miny + ((k - T->miny % n) % n + n) % n;
        if (y0 < T->maxy)
            xf_rows(T, y0, n, cnt);
    }
    t_in_xf = 0;
}

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        memset(&s_pool.cnt[k], 0, sizeof s_pool.cnt[k]);
        xf_rows_owned(k, s_pool.n, &s_pool.cnt[k]);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void raster_flush(void)
{
    XfCount total = {0, 0, 0};
    int n, k;

    LARGE_INTEGER q0, q1;

    if (!s_xfq_n)
        return;
    fast_qpc_li(&q0);
    n = raster_pool_size();
    if (n <= 1 || s_xfq_area < NV_RASTER_MT_MIN_PIXELS) {
        xf_rows_owned(0, 1, &total);
    } else {
        s_pool.pending = n - 1;
        for (k = 0; k < n - 1; k++)
            SetEvent(s_pool.start[k]);
        xf_rows_owned(n - 1, n, &total);
        WaitForSingleObject(s_pool.done, INFINITE);
        for (k = 0; k < n - 1; k++) {
            total.depth_fail += s_pool.cnt[k].depth_fail;
            total.pixels += s_pool.cnt[k].pixels;
            total.zpass += s_pool.cnt[k].zpass;
        }
    }
    s_gpu.xf_depth_fail += total.depth_fail;
    s_gpu.xf_pixels += total.pixels;
    s_gpu.zpass_count += total.zpass;
    s_gpu.pixels += total.pixels;
    s_xfq_n = 0;
    s_xfq_area = 0;
    fast_qpc_li(&q1);
    s_perf_pixel += (uint64_t)(q1.QuadPart - q0.QuadPart);
}
#else
static int raster_pool_size(void) { return 1; }
static void raster_flush(void)
{
    XfCount total = {0, 0, 0};
    int j;
    t_in_xf = 1;
    for (j = 0; j < s_xfq_n; j++)
        xf_rows(&s_xfq[j].T, s_xfq[j].T.miny, 1, &total);
    t_in_xf = 0;
    s_gpu.xf_depth_fail += total.depth_fail;
    s_gpu.xf_pixels += total.pixels;
    s_gpu.zpass_count += total.zpass;
    s_gpu.pixels += total.pixels;
    s_xfq_n = 0;
    s_xfq_area = 0;
}
#endif

/* Queue one set-up triangle; its vertices are copied, since the clipper's
 * live on its stack. */
static void raster_enqueue(const XfTri *T)
{
    XfJob *j;
    if (!s_xfq) {
        s_xfq = (XfJob *)malloc(sizeof(XfJob) * NV_XF_QUEUE);
        if (!s_xfq)
            return;
    }
    if (s_xfq_n == NV_XF_QUEUE)
        raster_flush();
    j = &s_xfq[s_xfq_n++];
    j->T = *T;
    j->v[0] = *T->va; j->v[1] = *T->vb; j->v[2] = *T->vc;
    j->T.va = &j->v[0]; j->T.vb = &j->v[1]; j->T.vc = &j->v[2];
    j->T.a = j->v[0].pos; j->T.b = j->v[1].pos; j->T.c = j->v[2].pos;
    s_xfq_area += (uint64_t)(T->maxx - T->minx) * (uint64_t)(T->maxy - T->miny);
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    XfTri T;
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid;
    T.use_rc = s_gpu.rc_seen && !no_rc;
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
    /* Face culling. Never done before, and JSRF depends on it: its cel
     * outlines are the character mesh pushed out along the normals, drawn
     * black with the front faces culled so only a rim shows. Drawn whole,
     * the outline hull covered every character -- the black silhouettes.
     *
     * Screen y runs down here, so a positive area is clockwise as seen.
     * D3D's CULLMODE values are the NV2A front-face codes (D3DCULL_CW is
     * 0x900) and D3D programs FRONT_FACE with the opposite of the winding it
     * culls, so FRONT_FACE CW means "clockwise on screen is the front".
     * RECOMP_CULL_FLIP=1 inverts that reading, RECOMP_NO_CULL=1 turns
     * culling off. */
    if (s_gpu.cull_enable) {
        static int mode = -1;
        if (mode < 0)
            mode = getenv("RECOMP_NO_CULL") ? 2
                 : (getenv("RECOMP_CULL_FLIP") ? 1 : 0);
        if (mode != 2) {
            int cw = area > 0.0f;
            int front = (s_gpu.front_face == 0x901) ? !cw : cw;
            int drop;
            if (mode == 1)
                front = !front;
            switch (s_gpu.cull_face) {
            case 0x404: drop = front;  break;   /* FRONT          */
            case 0x408: drop = 1;      break;   /* FRONT_AND_BACK */
            default:    drop = !front; break;   /* BACK           */
            }
            if (drop) {
                s_gpu.tris_culled++;
                return;
            }
        }
    }
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (s_gpu.depth_test || s_gpu.depth_mask || s_gpu.stencil_test)
        T.zb = zbuf_current(1);
    if (s_gpu.stencil_test)
        T.sb = sbuf_for(T.zb);
    /* Passes that cannot change a colour: blend ZERO/ONE with ADD, or the
     * colour mask off. JSRF's shadow volumes are two of these per frame,
     * about two thirds of every pixel the renderer touched -- shading them
     * was pure cost. Depth and stencil still update. */
    T.colour_noop = !(s_gpu.color_mask & 0x01010101u)
        || (s_gpu.blend_enable && s_gpu.blend_sfactor == 0
            && s_gpu.blend_dfactor == 1
            && (s_gpu.blend_equation == 0x8006 || s_gpu.blend_equation == 0));
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = s_fog_direct ? v[k]->fog[0] : fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;

    raster_enqueue(&T);
    s_gpu.tris_drawn++;
    note_drawn();
}


/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport (c[58] scale, c[59]
 * offset) while leaving w itself alone. So each vertex is taken back to clip
 * space first: clip = (screen - offset) / scale * w. Everything else the
 * vertex carries -- colours, fog, texture coordinates -- is linear in clip
 * space and is interpolated directly. The polygon is clipped against
 * w = NV_CLIP_W (Sutherland-Hodgman, one plane), re-projected, and fanned.
 * ponytail: the near plane only; x/y/far fall to the rasteriser's bounds
 * check, which is correct, just slower for huge off-screen triangles. */
#define NV_CLIP_W 1e-3f

static void xf_to_clip(const Nv2aVshOutput *v, const float k[3],
                       const float o[3], float c[3])
{
    int i;
    for (i = 0; i < 3; i++)
        c[i] = k[i] != 0.0f ? (v->pos[i] - o[i]) / k[i] * v->pos[3] : 0.0f;
}

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}



/* Guard band, in pixels either side of the surface. A vertex just in front
 * of the eye projects to x, y in the billions, and float edge functions at
 * that magnitude are quantised to tens of pixels: the triangle comes out as
 * a staircase of slabs (JSRF's light shafts and glare sprites, white and
 * cyan steps across the screen). Clipping to a band a few thousand pixels
 * wide keeps every coordinate the rasteriser sees small enough to be exact
 * to well under a pixel, and changes nothing that is visible. */
#define NV_GUARD 2048.0f

static float xf_plane_dist(int plane, const float c[4], const float k[3],
                           const float o[3])
{
    switch (plane) {
    case 0:  return c[3] - NV_CLIP_W;                               /* near  */
    case 1:  return  k[0] * c[0] + (o[0] + NV_GUARD) * c[3];        /* x >= -G */
    case 2:  return -k[0] * c[0] + (NV_ZBUF_W + NV_GUARD - o[0]) * c[3];
    case 3:  return  k[1] * c[1] + (o[1] + NV_GUARD) * c[3];        /* y >= -G */
    default: return -k[1] * c[1] + (NV_ZBUF_H + NV_GUARD - o[1]) * c[3];
    }
}

static int xf_in_guard(const Nv2aVshOutput *v)
{
    return v->pos[0] >= -NV_GUARD && v->pos[0] <= NV_ZBUF_W + NV_GUARD
        && v->pos[1] >= -NV_GUARD && v->pos[1] <= NV_ZBUF_H + NV_GUARD;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput bufA[12], bufB[12], *src = bufA, *dst = bufB, *tmp;
    float k[3], o[3], cc[3];
    int n = 3, m, i, j, plane, nplanes;

    if (a->pos[3] > NV_CLIP_W && b->pos[3] > NV_CLIP_W && c->pos[3] > NV_CLIP_W
            && xf_in_guard(a) && xf_in_guard(b) && xf_in_guard(c)) {
        raster_xf_triangle(a, b, c);
        return;
    }
    if (a->pos[3] <= NV_CLIP_W && b->pos[3] <= NV_CLIP_W
        && c->pos[3] <= NV_CLIP_W) {
        s_gpu.tris_behind++;                  /* wholly behind: nothing */
        return;
    }
    {
        const float *sc = nv2a_vsh_constant(58), *of = nv2a_vsh_constant(59);
        /* Fixed function: the composite matrix already scaled to pixels,
         * so the only viewport step after the divide is the offset. */
        for (i = 0; i < 3; i++) {
            k[i] = s_ff_active ? 1.0f : sc[i];
            o[i] = s_pre_active ? 0.0f : of[i];
        }
    }
    /* Without a viewport scale clip space cannot be recovered; near plane
     * only, as before. */
    nplanes = (k[0] != 0.0f && k[1] != 0.0f) ? 5 : 1;

    /* Everything into clip space: (screen - offset) / scale * w. */
    in[0] = a; in[1] = b; in[2] = c;
    for (i = 0; i < 3; i++) {
        src[i] = *in[i];
        xf_to_clip(in[i], k, o, cc);
        for (j = 0; j < 3; j++) src[i].pos[j] = cc[j];
    }

    /* Sutherland-Hodgman, one plane at a time. Near first, so w > 0 for the
     * guard planes, which are only linear in clip space when it is. */
    for (plane = 0; plane < nplanes && n >= 3; plane++) {
        m = 0;
        for (i = 0; i < n; i++) {
            const Nv2aVshOutput *p = &src[i], *q = &src[(i + 1) % n];
            float dp = xf_plane_dist(plane, p->pos, k, o);
            float dq = xf_plane_dist(plane, q->pos, k, o);
            if (dp >= 0.0f && m < 12)
                dst[m++] = *p;
            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 12)
                xf_lerp(p, q, dp / (dp - dq), &dst[m++]);
        }
        n = m;
        tmp = src; src = dst; dst = tmp;
    }
    if (n < 3)
        return;

    /* Back to screen space: divide by the (now positive) w, viewport. */
    for (i = 0; i < n; i++)
        for (j = 0; j < 3; j++)
            src[i].pos[j] = src[i].pos[j] / src[i].pos[3] * k[j] + o[j];
    for (i = 1; i + 1 < n; i++)
        raster_xf_triangle(&src[0], &src[i], &src[i + 1]);
}

static Nv2aVshOutput s_xf[NV_MAX_INDICES];

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        fetch_attr(&s_gpu.attr[a], index, in[a]);   /* absent: 0,0,0,1 */
    return nv2a_vsh_run((const float (*)[4])in, out);
}

/* Post-transform vertex cache. An indexed batch names most vertices
 * several times (a triangle list of a closed mesh, about six), and every
 * mention used to run the vertex program again. Inside one batch a vertex's
 * output depends only on its index, so keep the last result per index slot;
 * the generation number empties the cache at each batch. */
#define NV_VCACHE 1024
static uint32_t s_vc_idx[NV_VCACHE], s_vc_gen_of[NV_VCACHE], s_vc_gen;
static Nv2aVshOutput s_vc_out[NV_VCACHE];

/* Bisection switches for the stretched-mesh hunt (run 45). */
static int dbg_no_vcache(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_NO_VCACHE") != NULL;
    return v;
}

static int vcache_get(uint32_t index, Nv2aVshOutput *out)
{
    if (dbg_no_vcache())
        return 0;
    uint32_t slot = index & (NV_VCACHE - 1);
    if (s_vc_gen_of[slot] == s_vc_gen && s_vc_idx[slot] == index) {
        *out = s_vc_out[slot];
        s_perf_vhits++;
        return 1;
    }
    return 0;
}

static void vcache_put(uint32_t index, const Nv2aVshOutput *v)
{
    uint32_t slot = index & (NV_VCACHE - 1);
    s_vc_gen_of[slot] = s_vc_gen;
    s_vc_idx[slot] = index;
    s_vc_out[slot] = *v;
    s_perf_vruns++;
}

/* ── GPU backend glue (nv2a_gl.c) ─────────────────────────────────────── */

static uint32_t tex_bytes(const Texture *t)
{
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    if (t->cube)
        return tex_face_stride(t) * 6;
    if (block)
        return ((t->width + 3) / 4) * ((t->height + 3) / 4) * block;
    if (tex_size_from_format(t->color))
        return t->width * t->height * tex_texel_bytes(t->color);
    return t->pitch * t->height;
}

/* A whole texture for the GL backend. The same texels sample_tex returns,
 * without its per-texel work: DXT blocks are decoded once each, and swizzled
 * addresses come from per-row and per-column tables. Uploads were 2.7 ms
 * each in run 50, about 1.3 s of every 10 in play. */
int nv2a_exec_decode_texture(int stage, uint32_t face, uint32_t *out)
{
    const Texture *t = &s_gpu.texs[stage];
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t u, v, off = face ? face * tex_face_stride(t) : 0;
    uint32_t base = t->offset + off, W = t->width, H = t->height;
    if (!t->valid)
        return 0;
    if (d3d8_format_dxt_block_bytes(t->color)) {
        uint32_t bx, by, i;
        for (by = 0; by < (H + 3) / 4; by++)
            for (bx = 0; bx < (W + 3) / 4; bx++)
                for (i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2), px;
                    if (x >= W || y >= H)
                        continue;
                    d3d8_dxt_decode_texel(mem + base, t->color, x, y, W, &px);
                    out[(size_t)y * W + x] = px;
                }
        return 1;
    }
    if (d3d8_format_is_swizzled(t->color) && W <= 4096 && H <= 4096) {
        static uint32_t tu[4096], tv[4096];
        uint32_t fmt = linear_twin(t->color), mx, my, ok = 1;
        xbox_swizzle_masks(W, H, &mx, &my);
        for (u = 0; u < W; u++) tu[u] = swizzle_deposit(u, mx);
        for (v = 0; v < H; v++) tv[v] = swizzle_deposit(v, my);
        for (v = 0; v < H && ok; v++)
            for (u = 0; u < W; u++) {
                uint32_t argb;
                if (!texel_decode(t, fmt, mem + base, tu[u] | tv[v], &argb)) {
                    ok = 0;
                    break;
                }
                out[(size_t)v * W + u] = argb;
            }
        if (ok)
            return 1;
    } else if (!d3d8_format_is_swizzled(t->color)) {
        uint32_t ok = 1;
        for (v = 0; v < H && ok; v++) {
            const uint8_t *row = mem + base + (size_t)v * t->pitch;
            for (u = 0; u < W; u++) {
                uint32_t argb;
                if (!texel_decode(t, t->color, row, u, &argb)) {
                    ok = 0;
                    break;
                }
                out[(size_t)v * W + u] = argb;
            }
        }
        if (ok)
            return 1;
    }
    {
        /* Whatever reaches here has texels sample_tex cannot read, and they
         * come out white. Name each such format once. */
        static uint32_t said[32];
        static int nsaid;
        int k;
        for (k = 0; k < nsaid && said[k] != t->color; k++)
            ;
        if (k == nsaid && nsaid < 32) {
            said[nsaid++] = t->color;
            fprintf(stderr, "[GPU] texture format 0x%02X (%ux%u at 0x%08X, stage %d) is not"
                            " decoded: drawn white\n", t->color, W, H, t->offset, stage);
        }
    }
    for (v = 0; v < t->height; v++)
        for (u = 0; u < t->width; u++) {
            uint32_t argb;
            if (!sample_tex(t, off, u, v, &argb))
                argb = 0xFFFFFFFFu;
            out[(size_t)v * t->width + u] = argb;
        }
    return 1;
}

/* Everything nv2a_gl.c needs to draw what the executor would. Returns 0 when
 * there is no drawable surface. */
static int gl_state(NvGlState *st)
{
    uint32_t bpp = surface_bpp(), base, zf, i;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();

    memset(st, 0, sizeof *st);
    if (!s_gpu.color_offset || !s_gpu.pitch || !bpp || !s_gpu.clip_h)
        return 0;
    base = dma_resolve(s_gpu.color_offset);
    st->surf_va = base;
    st->bpp = bpp;
    st->pitch = s_gpu.pitch;
    st->width = s_gpu.pitch / bpp;
    st->height = s_gpu.clip_y + s_gpu.clip_h;
    /* A surface the size of the screen is the screen: keep one GL target
     * for it however the title narrows the clip. */
    if (st->width == 640 && st->height < 480)
        st->height = 480;
    if (surface_hits_image(base, st->height * st->pitch))
        return 0;
    st->surf_host = mem + base;
    st->surf_has_alpha = (s_gpu.format & 0xF) == 0x8;
    st->clip_x = s_gpu.clip_x; st->clip_y = s_gpu.clip_y;
    st->clip_w = s_gpu.clip_w; st->clip_h = s_gpu.clip_h;
    /* v186d: and narrowed to window clip 0 when it draws inside a smaller
     * rectangle (the split-screen halves). An exclusive clip, or one that
     * leaves nothing, is left alone. RECOMP_GL_NO_WINDOW_CLIP=1 ignores it. */
    {
        static int off = -1;
        if (off < 0)
            off = getenv("RECOMP_GL_NO_WINDOW_CLIP") != NULL;
        if (!off && s_gpu.wclip_set && s_gpu.wclip_type == 0) {
            uint32_t cx0 = s_gpu.clip_x, cy0 = s_gpu.clip_y;
            uint32_t cx1 = s_gpu.clip_w ? s_gpu.clip_x + s_gpu.clip_w - 1 : st->width - 1;
            uint32_t cy1 = s_gpu.clip_h ? s_gpu.clip_y + s_gpu.clip_h - 1 : st->height - 1;
            uint32_t x0 = s_gpu.wclip_x0 > cx0 ? s_gpu.wclip_x0 : cx0;
            uint32_t x1 = s_gpu.wclip_x1 < cx1 ? s_gpu.wclip_x1 : cx1;
            uint32_t y0 = s_gpu.wclip_y0 > cy0 ? s_gpu.wclip_y0 : cy0;
            uint32_t y1 = s_gpu.wclip_y1 < cy1 ? s_gpu.wclip_y1 : cy1;
            if (x1 >= x0 && y1 >= y0
                    && (x0 != cx0 || x1 != cx1 || y0 != cy0 || y1 != cy1)) {
                st->clip_x = x0; st->clip_w = x1 - x0 + 1;
                st->clip_y = y0; st->clip_h = y1 - y0 + 1;
            }
        }
    }
    st->zeta_key = s_gpu.zeta_offset;
    st->screen_space = (s_gpu.xf_mode & 3) == 2 && nv2a_vsh_is_passthrough()
                       && !passthrough_3d();
    zf = (s_gpu.format >> 4) & 0xF;
    st->zmax = zf == 1 ? 65535.0f : 16777215.0f;

    st->depth_test = s_gpu.depth_test; st->depth_func = s_gpu.depth_func;
    st->depth_mask = s_gpu.depth_mask;
    st->stencil_test = s_gpu.stencil_test; st->stencil_wmask = s_gpu.stencil_wmask;
    st->stencil_func = s_gpu.stencil_func; st->stencil_ref = s_gpu.stencil_ref;
    st->stencil_mask = s_gpu.stencil_mask; st->stencil_fail = s_gpu.stencil_fail;
    st->stencil_zfail = s_gpu.stencil_zfail; st->stencil_zpass = s_gpu.stencil_zpass;
    st->blend_enable = s_gpu.blend_enable && !(s_gpu.blend_sfactor == 1
                                               && s_gpu.blend_dfactor == 0);
    st->blend_sfactor = s_gpu.blend_sfactor; st->blend_dfactor = s_gpu.blend_dfactor;
    st->blend_equation = s_gpu.blend_equation; st->blend_color = s_gpu.blend_color;
    st->color_mask = s_gpu.color_mask;
    st->cull_enable = s_gpu.cull_enable; st->cull_face = s_gpu.cull_face;
    st->front_face = s_gpu.front_face;
    st->poly_offset = s_gpu.poly_offset_fill;
    st->poly_factor = s_gpu.poly_offset_factor;
    st->poly_units = s_gpu.poly_offset_units;
    {
        static int no_cull = -1;
        if (no_cull < 0)
            no_cull = getenv("RECOMP_NO_CULL") != NULL;
        if (no_cull)
            st->cull_enable = 0;
    }
    st->alpha_test = s_gpu.alpha_test; st->alpha_func = s_gpu.alpha_func;
    st->alpha_ref = s_gpu.alpha_ref;
    {
        static int no_rc = -1;
        if (no_rc < 0)
            no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
        st->rc_seen = s_gpu.rc_seen && !no_rc;
    }
    st->rc = s_gpu.rc;
    st->clip_plane_mode = s_gpu.clip_plane_mode;
    {
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        float f[4];
        nv2a_rc_unpack(s_gpu.fog_color, f);
        st->fog_color[0] = f[2]; st->fog_color[1] = f[1];
        st->fog_color[2] = f[0]; st->fog_color[3] = f[3];
    }
    for (i = 0; i < 4; i++) {
        const Texture *t = &s_gpu.texs[i];
        NvGlTexture *g = &st->tex[i];
        g->valid = t->valid;
        if (!t->valid)
            continue;
        g->offset = t->offset; g->width = t->width; g->height = t->height;
        g->pitch = t->pitch; g->color = t->color; g->palette = t->palette;
        g->levels = t->levels; g->filter = t->filter;
        g->addr_u = t->addr_u; g->addr_v = t->addr_v;
        g->cube = t->cube;
        g->normalised = tex_size_from_format(t->color);
        g->bytes = tex_bytes(t);
        g->face_stride = t->cube ? tex_face_stride(t) : 0;
    }
    return 1;
}

static uint32_t s_gl_idx[NV_MAX_INDICES * 3];
static float s_gl_fog[NV_MAX_INDICES];

enum { XK_PROGRAM, XK_FIXED, XK_PRE };
static float ws_k_for(int kind, const NvGlState *st, uint32_t *is_2d);   /* v187 */

/* Hand a transformed batch (s_xf[0..n)) to the GPU. Strip triangles are
 * re-wound so every one keeps the strip's facing, as the software path does. */
static void gl_submit(uint32_t n)
{
    NvGlState st;
    uint32_t i, m = 0;

    for (i = 0; i < n; i++)
        s_gl_fog[i] = s_fog_direct ? s_xf[i].fog[0] : fog_factor(s_xf[i].fog[0]);
#define TRI(a, b, c) do { s_gl_idx[m++] = (a); s_gl_idx[m++] = (b); \
                          s_gl_idx[m++] = (c); } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) TRI(i + 1, i, i + 2);
            else       TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            TRI(i, i + 1, i + 2);
            TRI(i, i + 2, i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            TRI(i, i + 1, i + 3);
            TRI(i, i + 3, i + 2);
        }
        break;
    default:
        break;
    }
#undef TRI
    {
        /* [STRETCH]: a triangle in front of the eye with an edge longer
         * than the screen is either a huge polygon close up or a vertex
         * that went wrong. Log a few of them with what they were made of,
         * and every batch bigger than the old 4,096-index limit. */
        static int said, big;
        uint32_t j, lo = 0xFFFFFFFFu, hi = 0;
        for (j = 0; j < n; j++) {
            if (s_gpu.idx[j] < lo) lo = s_gpu.idx[j];
            if (s_gpu.idx[j] > hi) hi = s_gpu.idx[j];
        }
        if (n > 4096 && big < 6) {
            big++;
            fprintf(stderr, "[STRETCH] big batch: %s prim %u n %u indices %u..%u"
                    " v0 at %08X stride %u\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim, n, lo, hi,
                    s_gpu.attr[0].offset, s_gpu.attr[0].stride);
        }
        for (j = 0; j + 2 < m && said < 12; j += 3) {
            const Nv2aVshOutput *a = &s_xf[s_gl_idx[j]], *b = &s_xf[s_gl_idx[j + 1]],
                                *c = &s_xf[s_gl_idx[j + 2]];
            float dx1, dy1, dx2, dy2;
            if (a->pos[3] <= 1.0f || b->pos[3] <= 1.0f || c->pos[3] <= 1.0f)
                continue;
            dx1 = fabsf(a->pos[0] - b->pos[0]); dy1 = fabsf(a->pos[1] - b->pos[1]);
            dx2 = fabsf(a->pos[0] - c->pos[0]); dy2 = fabsf(a->pos[1] - c->pos[1]);
            if (dx1 + dy1 < 1500.0f && dx2 + dy2 < 1500.0f)
                continue;
            said++;
            fprintf(stderr, "[STRETCH] %s prim %u n %u (indices %u..%u, v0 at %08X stride %u):"
                    " tri %u idx %u %u %u pos (%.0f,%.0f,w%.1f) (%.0f,%.0f,w%.1f) (%.0f,%.0f,w%.1f)\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim, n, lo, hi,
                    s_gpu.attr[0].offset, s_gpu.attr[0].stride, j / 3,
                    s_gpu.idx[s_gl_idx[j]], s_gpu.idx[s_gl_idx[j + 1]], s_gpu.idx[s_gl_idx[j + 2]],
                    a->pos[0], a->pos[1], a->pos[3], b->pos[0], b->pos[1], b->pos[3],
                    c->pos[0], c->pos[1], c->pos[3]);
            {
                uint32_t k, at;
                for (k = 0; k < 3; k++) {
                    float in[4];
                    uint32_t vi = s_gpu.idx[s_gl_idx[j + k]];
                    fprintf(stderr, "[STRETCH]   v%u:", vi);
                    for (at = 0; at < 4; at++) {
                        if (!s_gpu.attr[at].size) continue;
                        fetch_attr(&s_gpu.attr[at], vi, in);
                        fprintf(stderr, " a%u(t%u)=%g,%g,%g,%g", at, s_gpu.attr[at].type,
                                in[0], in[1], in[2], in[3]);
                    }
                    fputc(10, stderr);
                }
            }
        }
    }
    if (m < 3 || !gl_state(&st))
        return;
    st.ws_k = ws_k_for(s_fog_direct ? XK_PRE                      /* v187 */
                       : (s_gpu.xf_mode & 3) == 2 ? XK_PROGRAM : XK_FIXED,
                       &st, &st.ws_2d);
    nv2a_gl_draw(&st, s_xf, s_gl_fog, s_gl_idx, m);
    s_gpu.tris_drawn += m / 3;
    note_drawn();
}

/* ── GPU path: each distinct vertex once, on several threads ──────────────
 *
 * The software path transforms per index position (with a small cache) into
 * s_xf, and the GPU path used to do the same and then send every position as
 * its own vertex: 26 million vertices a run-second's worth of 10 seconds in
 * JSRF, each copied twice and uploaded at 128 bytes. Now a batch's indices
 * are reduced to the distinct vertices they name, those are transformed --
 * across a pool of threads when there are enough of them -- straight into the
 * GL vertex buffer, and the triangles are drawn indexed.
 *
 * A vertex's result depends only on its index and on state that is constant
 * for the batch, so the order they are computed in does not matter. The one
 * exception is a vertex program that writes constants (CXT_WRITE_EN), which
 * stays serial and in index order. */
/* XK_PROGRAM, XK_FIXED, XK_PRE: declared above gl_submit (v187) */
static void ff_transform(uint32_t index, Nv2aVshOutput *o);
static void pre_transform(uint32_t index, Nv2aVshOutput *o, float zmax);

static uint32_t s_uq_vert[NV_MAX_INDICES];   /* vertex index of each distinct slot */
static uint32_t s_uq_slot[NV_MAX_INDICES];   /* slot of each index position */
static uint32_t s_uq_n;
#define NV_DD_RANGE (1u << 20)
static uint32_t *s_dd_stamp, *s_dd_slot, s_dd_gen;

static struct {
    int kind;
    float zmax;
    NvGlVertex *dst;
    volatile long next;
    uint32_t total;
    uint32_t chunk;
    volatile int behind;              /* some vertex has w <= 0 */
    float *raw;                       /* v77: raw inputs, k float4s a vertex */
    uint32_t k;
    uint8_t slots[16];
    /* v144: per-slot fast fetch for the raw path, set up once a batch */
    const uint8_t *fbase[16];
    uint32_t fstride[16];
    uint8_t ftype[16], fsize[16], ffast[16];
} s_vx;
static int s_raw_active;              /* v77: this batch's vertices go to the GPU raw */

#define NV_VX_CHUNK 128
#if defined(_WIN32)
#define VX_FETCH_ADD(p, v) InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
#else
#define VX_FETCH_ADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#endif

static void vx_one(uint32_t j)
{
    Nv2aVshOutput o;
    float fog;
    uint32_t index = s_uq_vert[j];
    if (s_vx.raw) {
        /* v144: the common array formats read straight from precomputed
         * bases, each vertex built locally and stored once (the destination
         * may be write-combined GPU memory); anything else through
         * fetch_attr as before */
        float *d = s_vx.raw + (size_t)j * s_vx.k * 4;
        uint32_t q;
        for (q = 0; q < s_vx.k; q++) {
            float t[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (s_vx.ffast[q]) {
                const uint8_t *p = s_vx.fbase[q] + (size_t)index * s_vx.fstride[q];
                uint32_t c, n = s_vx.fsize[q];
                switch (s_vx.ftype[q]) {
                case 0:
                    t[0] = (float)p[2] / 255.0f; t[1] = (float)p[1] / 255.0f;
                    t[2] = (float)p[0] / 255.0f; t[3] = (float)p[3] / 255.0f;
                    break;
                case 2:
                    memcpy(t, p, n * 4);
                    break;
                default:                    /* 4: unsigned byte, normalised */
                    for (c = 0; c < n; c++) t[c] = (float)p[c] / 255.0f;
                    break;
                }
            } else {
                fetch_attr(&s_gpu.attr[s_vx.slots[q]], index, t);
            }
            memcpy(d + 4 * q, t, sizeof t);
        }
        return;
    }
    switch (s_vx.kind) {
    case XK_PROGRAM: if (!transform_vertex(index, &o)) memset(&o, 0, sizeof o); break;
    case XK_FIXED:   ff_transform(index, &o); break;
    default:         pre_transform(index, &o, s_vx.zmax); break;
    }
    fog = s_vx.kind == XK_PRE ? o.fog[0] : fog_factor(o.fog[0]);
    if (!(o.pos[3] > 0.0f))
        s_vx.behind = 1;              /* benign race: only ever set */
    nv2a_gl_vertex_from(&s_vx.dst[j], &o, fog);
}

static void vx_drain(void)
{
    for (;;) {
        uint32_t ch = s_vx.chunk ? s_vx.chunk : NV_VX_CHUNK;
        long c = VX_FETCH_ADD(&s_vx.next, (long)ch);
        uint32_t j, end;
        if ((uint32_t)c >= s_vx.total)
            return;
        end = (uint32_t)c + ch;
        if (end > s_vx.total)
            end = s_vx.total;
        for (j = (uint32_t)c; j < end; j++)
            vx_one(j);
    }
}

/* The helper threads. vx_run(k) wakes k of them, drains the queue with them
 * and returns when all are done. */
#define NV_VX_MAX_THREADS 16
static int s_vx_threads = -1;

static int vx_count(void)
{
    if (s_vx_threads < 0) {
        const char *e = getenv("RECOMP_VERTEX_THREADS");
        int n;
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors / 2;
#else
        n = (int)sysconf(_SC_NPROCESSORS_ONLN) / 2;
#endif
        if (n > 8) n = 8;
        if (e) n = atoi(e);
        if (n < 0) n = 0;
        if (n > NV_VX_MAX_THREADS) n = NV_VX_MAX_THREADS;
        s_vx_threads = n;
    }
    return s_vx_threads;
}

#if defined(_WIN32)
/* Helpers spin briefly between batches instead of sleeping on an event.
 * A frame in a crowded Shibuya street is ~650 batches, and waking eight
 * threads through events and waiting for the last of them cost tens of
 * microseconds a batch -- enough that the threads barely paid for themselves
 * (run 61: 4.9 s of every 10 transforming 35 M vertices). A helper that has
 * seen no work for a few hundred microseconds sleeps until woken. */
/* v147: the batch number and how many helpers it wants are one word (ctl =
 * gen << 5 | want), read together. With them apart, a helper could see a new
 * gen, then read the *next* batch's want, join that batch, and then count
 * itself out of it twice -- the executor then saw the batch done while
 * another helper was still writing it, and that helper wrote through the
 * cleared buffer pointer (crash in vx_one, run 147). */
static struct {
    int n;
    HANDLE wake[NV_VX_MAX_THREADS];
    volatile LONG ctl, pending;
    volatile LONG sleeping[NV_VX_MAX_THREADS];
} s_vxpool;

static DWORD WINAPI vx_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    LONG seen = 0;
    for (;;) {
        int spins = 0;
        for (;;) {
            LONG c = s_vxpool.ctl;
            if ((c >> 5) != (seen >> 5)) {
                seen = c;
                if (k < (c & 31))
                    break;
                continue;
            }
            if (++spins < 20000) {
                YieldProcessor();
                continue;
            }
            /* v150: asleep until woken. The xchg here and the executor's
             * xchg of ctl before it reads `sleeping` are full barriers, so
             * one of the two always sees the other and no wake is lost; the
             * timeout is only a safety net. With 2 ms, four idle helpers woke
             * 500 times a second each and spun ~0.4 ms every time -- CPU the
             * Deck's four cores did not have to spare, now that the vertex
             * path rarely needs them (v149). */
            InterlockedExchange(&s_vxpool.sleeping[k], 1);
            if ((s_vxpool.ctl >> 5) == (seen >> 5))
                WaitForSingleObject(s_vxpool.wake[k], 200);
            InterlockedExchange(&s_vxpool.sleeping[k], 0);
            spins = 0;
        }
        vx_drain();
        InterlockedDecrement(&s_vxpool.pending);
    }
    return 0;
}

static int vx_pool(void)
{
    static int init;
    if (!init) {
        int k, n = vx_count();
        init = 1;
        for (k = 0; k < n; k++) {
            s_vxpool.wake[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
            CloseHandle(CreateThread(NULL, 0, vx_worker, (LPVOID)(intptr_t)k, 0, NULL));
        }
        s_vxpool.n = n;
        fprintf(stderr, "[GPU] %d vertex threads besides the executor"
                " (RECOMP_VERTEX_THREADS)\n", n);
    }
    return s_vxpool.n;
}

static void vx_run(int use)
{
    int k;
    LONG c = s_vxpool.ctl;
    InterlockedExchange(&s_vxpool.pending, use);
    InterlockedExchange(&s_vxpool.ctl, (LONG)((((uint32_t)c >> 5) + 1u) << 5 | (uint32_t)use));
    for (k = 0; k < use; k++)
        if (s_vxpool.sleeping[k])
            SetEvent(s_vxpool.wake[k]);
    vx_drain();
    while (s_vxpool.pending > 0)
        YieldProcessor();
}
#else
static struct {
    int n, want;
    unsigned gen;
    int pending;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
} s_vxpool = { 0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
               PTHREAD_COND_INITIALIZER };

static void *vx_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&s_vxpool.mu);
        while (s_vxpool.gen == seen || k >= s_vxpool.want)
            pthread_cond_wait(&s_vxpool.go, &s_vxpool.mu);
        seen = s_vxpool.gen;
        pthread_mutex_unlock(&s_vxpool.mu);
        vx_drain();
        pthread_mutex_lock(&s_vxpool.mu);
        if (--s_vxpool.pending == 0)
            pthread_cond_signal(&s_vxpool.done);
        pthread_mutex_unlock(&s_vxpool.mu);
    }
    return NULL;
}

static int vx_pool(void)
{
    static int init;
    if (!init) {
        int k, n = vx_count();
        init = 1;
        for (k = 0; k < n; k++) {
            pthread_t t;
            if (pthread_create(&t, NULL, vx_worker, (void *)(intptr_t)k) != 0)
                break;
            pthread_detach(t);
        }
        s_vxpool.n = k;
        fprintf(stderr, "[GPU] %d vertex threads besides the executor"
                " (RECOMP_VERTEX_THREADS)\n", k);
    }
    return s_vxpool.n;
}

static void vx_run(int use)
{
    pthread_mutex_lock(&s_vxpool.mu);
    s_vxpool.want = use;
    s_vxpool.pending = use;
    s_vxpool.gen++;
    pthread_cond_broadcast(&s_vxpool.go);
    pthread_mutex_unlock(&s_vxpool.mu);
    vx_drain();
    pthread_mutex_lock(&s_vxpool.mu);
    while (s_vxpool.pending)
        pthread_cond_wait(&s_vxpool.done, &s_vxpool.mu);
    pthread_mutex_unlock(&s_vxpool.mu);
}
#endif

/* Distinct vertices of the batch into s_uq_*. */
static void gl_dedupe(void)
{
    uint32_t i, n = s_gpu.idx_count, lo = 0xFFFFFFFFu, hi = 0, nu = 0;

    for (i = 0; i < n; i++) {
        uint32_t x = s_gpu.idx[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    if (!s_dd_stamp) {
        s_dd_stamp = (uint32_t *)calloc(NV_DD_RANGE, 4);
        s_dd_slot = (uint32_t *)malloc((size_t)NV_DD_RANGE * 4);
        if (!s_dd_slot) { free(s_dd_stamp); s_dd_stamp = NULL; }
    }
    if (s_dd_stamp && n && hi - lo < NV_DD_RANGE && !dbg_no_vcache()) {
        if (++s_dd_gen == 0) {
            memset(s_dd_stamp, 0, (size_t)NV_DD_RANGE * 4);
            s_dd_gen = 1;
        }
        for (i = 0; i < n; i++) {
            uint32_t d = s_gpu.idx[i] - lo;
            if (s_dd_stamp[d] != s_dd_gen) {
                s_dd_stamp[d] = s_dd_gen;
                s_dd_slot[d] = nu;
                s_uq_vert[nu++] = s_gpu.idx[i];
            }
            s_uq_slot[i] = s_dd_slot[d];
        }
    } else {
        for (i = 0; i < n; i++) {
            s_uq_vert[i] = s_gpu.idx[i];
            s_uq_slot[i] = i;
        }
        nu = n;
    }
    s_uq_n = nu;
    s_perf_vhits += n - nu;
    s_perf_vruns += nu;
}

/* Transform the batch's distinct vertices into a mapped GL vertex buffer.
 * s_xf[0..2] also get the first three positions' results, for the traces.
 * Returns 0 when there is nothing to draw (no program, no buffer). */
static int ff_lighting_off(void);

/* v78: the current program, packed for the GPU (nv2a_vsh_gpu_program),
 * repacked only when it changes. Returns its instruction count. */
static uint32_t s_vsh_pack[NV2A_VSH_SLOTS][4], s_vsh_pack_gen;
static int s_vsh_pack_n;
static int vsh_packed(uint32_t *gen_out)
{
    uint32_t gen = 0;
    nv2a_vsh_gpu_program(NULL, &gen);
    if (gen != s_vsh_pack_gen || !s_vsh_pack_n) {
        s_vsh_pack_n = nv2a_vsh_gpu_program(s_vsh_pack, &s_vsh_pack_gen);
    }
    if (gen_out) *gen_out = s_vsh_pack_gen;
    return s_vsh_pack_n;
}

/* v77: the GPU vertex path. 1 when the GL backend can run vertex programs and
 * the fixed-function pipeline in its vertex shader (RECOMP_GL_CPU_VERTEX=1
 * keeps them here). */
static int gpu_vs(void)
{
    static int v = -1;
    if (v < 0)
        v = nv2a_gl_gpu_vs_ok();
    return v;
}

/* Which inputs the batch supplies, and which the shader will read. */
static uint32_t s_raw_present;

/* v103: which batches write the title's own depth from the fragment shader
 * (u_zdirect, run 63). Writing gl_FragDepth turns off the GPU's early depth
 * test, so every hidden pixel of every layer ran the whole combiner shader
 * -- at 3x, 9x the pixels, the GPU took 11-12 ms of a frame and finished
 * frames unevenly (run 102: "feels like 30"). Exactness matters for flush 2D
 * layers (menus, HUD), which are pre-transformed, D3D's pass-through
 * program, or a fixed-function composite with no perspective (w row 0 0 0 c);
 * 3D batches now take GL's depth, with early depth testing.
 * RECOMP_GL_FRAGDEPTH=1 writes it for every batch again (pre-v103), =0 for
 * none. */
/* v187: widescreen, done on the GPU (as v137 tried, before v138 withdrew it).
 * The title's matrices are left exactly as they are -- v131-v136 scaled its
 * look-at, which the game's own logic also reads, and the skater vanished
 * behind walls the game would have hidden. Here the batches drawn into a
 * display-sized surface have their x squeezed about the centre by
 * k = (4/3) / shown aspect (0.75 at 16:9), so the wider picture shows more
 * of the scene. 2D batches (pre-transformed, D3D's pass-through program, a
 * composite without perspective) are squeezed too, keeping the HUD's shape,
 * but the shader leaves their vertices at the screen's left and right edges
 * where they are, so fades and full-screen layers still cover it all.
 * Smaller render targets (reflections, the graffiti previews) are left
 * alone. The game still culls for its 4:3 view, so things can appear late
 * at the far left and right. k comes from the presenter (fb_present.c). */
#if defined(_WIN32)
extern double xbox_WidescreenK(void);
#else
static double xbox_WidescreenK(void)    /* no presenter here: the harness */
{
    const char *e = getenv("RECOMP_GL_WS_K");
    return e ? atof(e) : 1.0;
}
#endif
/* v187b/c: the x span (guest pixels) of a small untextured batch -- at most
 * 24 vertices, so a fade, a flash or the letterbox bars, not a model or a
 * string of text. 0 when it is not one. */
static int ws_fill_span(int kind, const NvGlState *st, float *lo, float *hi)
{
    uint32_t i, n = s_gpu.idx_count;
    float p[4], x;
    *lo = 1.0e9f; *hi = -1.0e9f;
    if (s_gpu.texs[0].valid || n == 0 || n > 24)
        return 0;
    for (i = 0; i < n; i++) {
        uint32_t idx = s_gpu.idx[i];
        if (kind == XK_FIXED && !st->screen_space) {
            Nv2aVshOutput o;
            ff_transform(idx, &o);
            x = o.pos[0];                      /* screen x, already over w */
        } else {
            if (!fetch_attr(&s_gpu.attr[0], idx, p))
                return 0;
            x = p[0];
        }
        if (x < *lo) *lo = x;
        if (x > *hi) *hi = x;
    }
    return 1;
}

static float ws_k_for(int kind, const NvGlState *st, uint32_t *is_2d)
{
    static float k = -1.0f;
    static int gpu3d = -1;
    float lo, hi, w;
    if (k < 0.0f) {
        k = (float)xbox_WidescreenK();
        if (!(k > 0.2f && k <= 2.0f)) k = 1.0f;
    }
    if (gpu3d < 0) {
        /* RECOMP_WS_MODE=gpu: v137's way, the 3D squeezed here as well. By
         * default the title draws its 3D wide itself (recomp_manual.c: the
         * perspective setup, sub_00153FE0, widened; every other projection
         * given to SetTransform fitted to the 4:3 middle), so only the
         * pre-transformed 2D is fitted here. */
        const char *m = getenv("RECOMP_WS_MODE");
        gpu3d = m && !strcmp(m, "gpu");
    }
    *is_2d = 0;
    if (k == 1.0f || st->width < 640 || st->height < 480)
        return 1.0f;
    w = (float)st->width;
    if (kind == XK_PRE || st->screen_space) {
        /* 2D squeezed as a whole: logos, menus, text and the HUD keep their
         * shape in the middle, and whatever the game places over the world
         * from its own 4:3 projection lands on the widened 3D. Only an
         * untextured quad across the whole width -- a fade, a flash, the
         * letterbox bars -- stays full width. */
        *is_2d = 1;
        if (ws_fill_span(kind, st, &lo, &hi) && lo <= 2.0f && hi >= w - 2.0f)
            return 1.0f;
        return k;
    }
    if (kind == XK_FIXED) {
        const float *m = s_gpu.ff_composite;
        int flat = m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] != 0.0f;
        if (gpu3d) {
            *is_2d = flat;
            return k;
        }
        /* drawn through a projection already fitted into the middle: a fill
         * across that middle is widened back out to the whole screen */
        if (ws_fill_span(kind, st, &lo, &hi)
                && lo <= (1.0f - k) * w * 0.5f + 2.0f && hi >= (1.0f + k) * w * 0.5f - 2.0f) {
            *is_2d = 1;
            return 1.0f / k;
        }
        return 1.0f;
    }
    return gpu3d ? k : 1.0f;
}

static int zdirect_wanted(int kind, const NvGlState *st)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_GL_FRAGDEPTH");
        mode = e ? atoi(e) : 2;
    }
    if (mode != 2)
        return mode != 0;
    if (kind == XK_PRE || st->screen_space)
        return 1;
    if (kind == XK_FIXED) {
        const float *m = s_gpu.ff_composite;
        return m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] != 0.0f;
    }
    return 0;
}

/* v147: a batch whose inputs are all arrays in formats GL reads as they are
 * (D3DCOLOR, float, unsigned byte, short) goes to the GPU as the title
 * stored it: the bytes from its lowest to its highest index, one copy per
 * stream, no per-vertex work and no sorting out of distinct vertices. The
 * float4 fill this replaces was ~2 s of every 10 in busy areas on the Deck
 * (run 146). Batches whose indices are spread far apart (more than twice
 * their count) keep the old path, as do other formats and inline arrays.
 * RECOMP_VX_NATIVE=0 turns it off. */
static uint64_t s_perf_vnat, s_perf_vnat_b;
/* why batches kept the old path: [0] an input with no array (a constant),
 * [1] inline array, [2] indices spread out, [3] the ring refused,
 * [8 + t] an input of NV097 type t */
static uint64_t s_nat_why[24];
static int s_nat_const_on = 1;   /* RECOMP_VX_NATIVE_CONST=0: constant inputs keep the old path */
static uint32_t nat_bytes(const VertexAttr *a)
{
    switch (a->type) {
    case 0:  return 4;
    case 2:  return 4 * a->size;
    case 4:  return a->size;
    default: return 2 * a->size;
    }
}

static int nat_try(uint32_t k)
{
    static int on = -1;
    uint32_t i, q, n = s_gpu.idx_count, lo = 0xFFFFFFFFu, hi = 0, span, ns = 0;
    uint32_t ord[16], boff[16], ext[16], len[16], str[16], nk = 0;
    const uint8_t *src[16];
    NvGlNatAttr at[16];
    float cval[16][4];
    const uint8_t *gm;

    if (on < 0) {
        const char *e = getenv("RECOMP_VX_NATIVE");
        on = !(e && e[0] == '0');
        e = getenv("RECOMP_VX_NATIVE_CONST");
        s_nat_const_on = !(e && e[0] == '0');
    }
    if (!on || !n || !k || k > 16 || s_ftrace == 2)
        return 0;
    if (s_gpu.inline_active) { s_nat_why[1]++; return 0; }
    for (q = 0; q < k; q++) {
        const VertexAttr *a = &s_gpu.attr[s_vx.slots[q]];
        if (!a->size || !a->stride) {
            /* v149: one value for the whole batch (run 148: 3 in 4 of the
             * batches left on the old path had such an input) */
            if (!s_nat_const_on || !fetch_attr(a, 0, cval[q])) { s_nat_why[0]++; return 0; }
            continue;
        }
        if (!a->offset) { s_nat_why[0]++; return 0; }
        if (a->size > 4 || (a->type != 0 && a->type != 2 && a->type != 4 && a->type != 5)) {
            s_nat_why[8 + (a->type & 15)]++;
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        uint32_t x = s_gpu.idx[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    span = hi - lo + 1;
    if (span > 2 * n + 64) { s_nat_why[2]++; return 0; }
    /* array inputs by address; one stream per run of inputs a stride apart */
    for (q = 0; q < k; q++) {
        const VertexAttr *a = &s_gpu.attr[s_vx.slots[q]];
        uint32_t j = nk;
        if (!a->size || !a->stride) {
            at[q].loc = s_vx.slots[q];
            at[q].type = NV_GL_NAT_CONST;
            at[q].size = 4;
            at[q].stream = 0;
            at[q].rel = 0;
            continue;
        }
        while (j > 0 && s_gpu.attr[s_vx.slots[ord[j - 1]]].offset
                        > s_gpu.attr[s_vx.slots[q]].offset) {
            ord[j] = ord[j - 1];
            j--;
        }
        ord[j] = q;
        nk++;
    }
    if (!nk) { s_nat_why[0]++; return 0; }
    for (i = 0; i < nk; i++) {
        uint32_t qq = ord[i];
        const VertexAttr *a = &s_gpu.attr[s_vx.slots[qq]];
        uint32_t b = nat_bytes(a);
        at[qq].loc = s_vx.slots[qq];
        at[qq].type = (uint8_t)a->type;
        at[qq].size = (uint8_t)a->size;
        if (ns && str[ns - 1] == a->stride && a->offset - boff[ns - 1] < a->stride) {
            at[qq].stream = (uint8_t)(ns - 1);
            at[qq].rel = a->offset - boff[ns - 1];
            if (at[qq].rel + b > ext[ns - 1])
                ext[ns - 1] = at[qq].rel + b;
        } else {
            at[qq].stream = (uint8_t)ns;
            at[qq].rel = 0;
            boff[ns] = a->offset;
            str[ns] = a->stride;
            ext[ns] = b;
            ns++;
        }
    }
    gm = (const uint8_t *)xbox_GetMemoryOffset();
    for (i = 0; i < ns; i++) {
        src[i] = gm + boff[i] + (size_t)lo * str[i];
        len[i] = (span - 1) * str[i] + ext[i];
    }
    if (!nv2a_gl_map_native(k, at, (const float (*)[4])cval, ns, src, len, str)) { s_nat_why[3]++; return 0; }
    for (i = 0; i < n; i++)
        s_uq_slot[i] = s_gpu.idx[i] - lo;
    s_uq_n = span;
    s_perf_vnat += span;
    if (!s_perf_vnat_b++)
        fprintf(stderr, "[GPU] vertex arrays go to the GPU as stored, GL converting"
                " them (RECOMP_VX_NATIVE=0 for the CPU fill)\n");
    return 1;
}

static int gl_transform(int kind, float zmax)
{
    uint32_t i, n = s_gpu.idx_count;
    LARGE_INTEGER q0, q1;
    int helpers;

    fast_qpc_li(&q0);
    s_raw_active = 0;
    s_vx.raw = NULL;
    if (kind != XK_PRE && n && gpu_vs()
            && !(kind == XK_PROGRAM && nv2a_vsh_writes_constants())) {
        uint32_t need = 0, present = 0, a, k = 0;
        float tmp[4];
        if (kind == XK_PROGRAM) {
            int nins = vsh_packed(NULL);
            if (!nins)
                return 0;                          /* no program loaded */
            if (!nv2a_gl_prepare_vsh((const uint32_t (*)[4])s_vsh_pack, nins))
                goto cpu_path;                     /* its shader did not build */
            need = nv2a_vsh_input_mask();
        } else {
            int c;
            need = (1u << 0) | (1u << 3) | (1u << 4) | (0xFu << 9);
            if (s_gpu.ff_skin_mode)
                need |= 1u << 1;
            if (s_gpu.ff_lighting && !ff_lighting_off())
                need |= 1u << 2;
            for (a = 0; a < 4; a++)
                for (c = 0; c < 4; c++)
                    if (s_gpu.ff_texgen[a][c] == 0x2402 || s_gpu.ff_texgen[a][c] == 0x8511
                            || s_gpu.ff_texgen[a][c] == 0x8512)
                        need |= 1u << 2;
        }
        for (a = 0; a < NV_VERTEX_ATTRS && a < 16; a++)
            if (fetch_attr(&s_gpu.attr[a], s_gpu.idx[0], tmp))
                present |= 1u << a;
        for (a = 0; a < 16; a++)
            if ((need & present) & (1u << a))
                s_vx.slots[k++] = (uint8_t)a;
        if (!k)
            s_vx.slots[k++] = 0;                   /* never an empty layout */
        s_vx.k = k;
        s_raw_present = present;
        if (nat_try(k)) {
            s_raw_active = 1;
            s_vx.kind = kind;
            fast_qpc_li(&q1);
            s_perf_vertex += (uint64_t)(q1.QuadPart - q0.QuadPart);
            return 1;
        }
        gl_dedupe();
        s_vx.raw = nv2a_gl_map_raw(s_uq_n, k);
        if (s_vx.raw) {
            {
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                static int fast = -1;
                uint32_t q;
                if (fast < 0) {
                    const char *e = getenv("RECOMP_VX_FAST");
                    fast = !(e && e[0] == '0');
                }
                for (q = 0; q < k; q++) {
                    const VertexAttr *a = &s_gpu.attr[s_vx.slots[q]];
                    s_vx.ffast[q] = fast && !s_gpu.inline_active && a->size && a->stride
                        && a->offset && a->size <= 4
                        && (a->type == 0 || a->type == 2 || a->type == 4);
                    s_vx.fbase[q] = gm + a->offset;
                    s_vx.fstride[q] = a->stride;
                    s_vx.ftype[q] = (uint8_t)a->type;
                    s_vx.fsize[q] = (uint8_t)a->size;
                }
            }
            s_raw_active = 1;
            s_vx.kind = kind;
            s_vx.total = s_uq_n;
            s_vx.next = 0;
            helpers = vx_pool();
            if (helpers > 0 && s_uq_n >= 256) {
                int use = (int)(s_uq_n / 128) - 1;
                if (use > helpers) use = helpers;
                if (use < 1) use = 1;
                s_vx.chunk = (s_uq_n + (uint32_t)use) / (uint32_t)(use + 1);
                if (s_vx.chunk < 64) s_vx.chunk = 64;
                vx_run(use);
            } else {
                s_vx.chunk = s_uq_n;
                vx_drain();
            }
            s_vx.raw = NULL;
            if (s_ftrace == 2) {
                /* v98: trace frames also run the first vertices here, so
                 * the trace can say where they land on the GPU path too */
                uint32_t nk = n < 4 ? n : 4;
                if (kind == XK_PROGRAM) {
                    nv2a_vsh_prepare();
                    for (i = 0; i < nk; i++) transform_vertex(s_gpu.idx[i], &s_xf[i]);
                } else {
                    for (i = 0; i < nk; i++) ff_transform(s_gpu.idx[i], &s_xf[i]);
                }
            }
            fast_qpc_li(&q1);
            s_perf_vertex += (uint64_t)(q1.QuadPart - q0.QuadPart);
            return 1;
        }
        /* No room: fall through to the CPU path. */
    }
cpu_path:
    if (kind == XK_PROGRAM) {
        nv2a_vsh_prepare();
        if (!transform_vertex(s_gpu.idx[0], &s_xf[0]))
            return 0;                              /* no program loaded */
    }
    for (i = 0; i < 3 && i < n; i++) {
        if (kind == XK_PROGRAM) { if (i) transform_vertex(s_gpu.idx[i], &s_xf[i]); }
        else if (kind == XK_FIXED) ff_transform(s_gpu.idx[i], &s_xf[i]);
        else pre_transform(s_gpu.idx[i], &s_xf[i], zmax);
    }
    gl_dedupe();
    s_vx.dst = nv2a_gl_map_vertices(s_uq_n);
    if (!s_vx.dst)
        return 0;
    s_vx.kind = kind;
    s_vx.zmax = zmax;
    s_vx.behind = 0;
    s_vx.total = s_uq_n;
    s_vx.next = 0;
    helpers = vx_pool();
    if (kind == XK_PROGRAM && nv2a_vsh_writes_constants())
        helpers = 0;
    /* With the helpers spinning, handing out work costs a microsecond or
     * two, so batches down to ~100 distinct vertices are worth splitting:
     * a busy street averages ~175 a batch (run 62), which the old threshold
     * of 256 left almost entirely on this thread. */
    if (helpers > 0 && s_uq_n >= 96) {
        int use = (int)(s_uq_n / 32) - 1;
        if (use > helpers) use = helpers;
        if (use < 1) use = 1;
        s_vx.chunk = (s_uq_n + (uint32_t)use) / (uint32_t)(use + 1);
        if (s_vx.chunk < 16) s_vx.chunk = 16;
        if (s_vx.chunk > NV_VX_CHUNK) s_vx.chunk = NV_VX_CHUNK;
        vx_run(use);
    } else {
        s_vx.chunk = NV_VX_CHUNK;
        vx_drain();
    }
    fast_qpc_li(&q1);
    s_perf_vertex += (uint64_t)(q1.QuadPart - q0.QuadPart);
    return 1;
}

static void build_vs_state(NvGlVsState *vs, int kind);

/* v117: gl_draw_batch's own time -- indices, the state snapshot -- apart
 * from the GL calls it makes (nv2a_gl_times' "GL draws"). */
static uint64_t s_perf_glprep;

/* Triangles over the distinct vertices gl_transform produced, and the draw. */
static void gl_draw_batch(void)
{
    NvGlState st;
    uint32_t i, m = 0, n = s_gpu.idx_count;
    const uint32_t *S = s_uq_slot;
    LARGE_INTEGER q0, q1;
    fast_qpc_li(&q0);

#define TRI(a, b, c) do { s_gl_idx[m++] = S[a]; s_gl_idx[m++] = S[b]; \
                          s_gl_idx[m++] = S[c]; } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) TRI(i + 1, i, i + 2);
            else       TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            TRI(i, i + 1, i + 2);
            TRI(i, i + 2, i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            TRI(i, i + 1, i + 3);
            TRI(i, i + 3, i + 2);
        }
        break;
    default:
        break;
    }
#undef TRI
    if (m < 3 || !gl_state(&st)) {
        if (s_raw_active) nv2a_gl_unmap_raw();
        else nv2a_gl_unmap_vertices();
        s_raw_active = 0;
        return;
    }
    if (s_raw_active) {
        NvGlVsState vs;
        build_vs_state(&vs, s_vx.kind);
        st.zdirect_off = 0;                    /* the shader decides per vertex */
        if (!zdirect_wanted(s_vx.kind, &st))
            st.zdirect_off = 1;
        st.ws_k = ws_k_for(s_vx.kind, &st, &st.ws_2d);   /* v187 */
        fast_qpc_li(&q1);
        s_perf_glprep += (uint64_t)(q1.QuadPart - q0.QuadPart);
        if (s_ftrace == 2)
            nv2a_gl_trace_draw((int)s_ftrace_draw + 1);
        nv2a_gl_draw_raw(&st, &vs, s_vx.slots, s_vx.k, s_gl_idx, m);
        s_raw_active = 0;
    } else {
    st.zdirect_off = s_vx.behind || !zdirect_wanted(s_vx.kind, &st);
    st.ws_k = ws_k_for(s_vx.kind, &st, &st.ws_2d);       /* v187 */
    fast_qpc_li(&q1);
    s_perf_glprep += (uint64_t)(q1.QuadPart - q0.QuadPart);
    if (s_ftrace == 2)
        nv2a_gl_trace_draw((int)s_ftrace_draw + 1);
    nv2a_gl_draw_mapped(&st, s_gl_idx, m);
    }
    s_gpu.tris_drawn += m / 3;
    note_drawn();
}

static int s_gl_legacy = -1;   /* RECOMP_GL_LEGACY_SUBMIT=1: the v49 path */
static int gl_fast(void)
{
    if (s_gl_legacy < 0)
        s_gl_legacy = getenv("RECOMP_GL_LEGACY_SUBMIT") != NULL;
    return nv2a_gl_active() && !s_gl_legacy;
}

static void raster_batch_program(void)
{
    uint32_t i, n = s_gpu.idx_count;
    LARGE_INTEGER q0, q1;

    if (gl_fast()) {
        if (!gl_transform(XK_PROGRAM, 0.0f))
            return;
    } else {
    fast_qpc_li(&q0);
    if (++s_vc_gen == 0)
        s_vc_gen = 1;
    for (i = 0; i < n; i++) {
        if (vcache_get(s_gpu.idx[i], &s_xf[i]))
            continue;
        if (!transform_vertex(s_gpu.idx[i], &s_xf[i]))
            return;                                    /* no program loaded */
        vcache_put(s_gpu.idx[i], &s_xf[i]);
    }
    fast_qpc_li(&q1);
    s_perf_vertex += (uint64_t)(q1.QuadPart - q0.QuadPart);
    }
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && s_gpu.batches_program % 3000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.idx[0], s_gpu.idx[1], s_gpu.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g d0 %.2f %.2f %.2f %.2f"
                        " t0 %g %g %g %g t1 %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3], s_xf[i].d0[0], s_xf[i].d0[1],
                        s_xf[i].d0[2], s_xf[i].d0[3], s_xf[i].tex[0][0],
                        s_xf[i].tex[0][1], s_xf[i].tex[0][2], s_xf[i].tex[0][3],
                        s_xf[i].tex[1][0], s_xf[i].tex[1][1]);
            /* What the pixels will be made of: every stage, usable or not,
             * and whether the combiners are in charge. */
            for (i = 0; i < 4; i++)
                fprintf(stderr, "[VTRACE]   stage%u valid %d fmt 0x%02X %ux%u at 0x%08X"
                        " cube %d pal 0x%08X\n", i, s_gpu.texs[i].valid,
                        s_gpu.texs[i].color, s_gpu.texs[i].width,
                        s_gpu.texs[i].height, s_gpu.texs[i].offset,
                        s_gpu.texs[i].cube, s_gpu.texs[i].palette);
            fprintf(stderr, "[VTRACE]   combiners %s, stage program 0x%08X,"
                    " control 0x%08X, blend %u (%u/%u)\n",
                    s_gpu.rc_seen ? "on" : "off", s_gpu.rc.stage_program,
                    s_gpu.rc.control, s_gpu.blend_enable,
                    s_gpu.blend_sfactor, s_gpu.blend_dfactor);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    if (gl_fast()) {
        gl_draw_batch();
        return;
    }
    if (nv2a_gl_active()) {
        gl_submit(n);
        return;
    }
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* Every other strip triangle is wound the other way; swap those so
         * all of them keep the strip's facing (culling depends on it). */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
    raster_flush();
}

/* Fixed-function transform: what the GPU does for a batch drawn with
 * TRANSFORM_EXECUTION_MODE "fixed" -- position through the composite matrix
 * (after model-view blending when skinning is on), divide by w, viewport
 * offset; colours and texture coordinates passed through.
 *
 * These batches used to be classified as screen space or skipped. Jet Set
 * Radio Future draws its stages this way: 48,000 batches a run were skipped
 * and the level never appeared, only the vertex-program characters and
 * effects on top of the backdrop.
 *
 * ponytail: no lighting (the diffuse attribute is used as is, white when
 * absent and lighting is on), no texgen or texture matrices, fog from
 * eye-space depth. Each is a straight addition here when a title shows
 * it is needed. */
/* Fixed-function lighting state (xemu nv2a_regs.h names). Xbox D3D folds
 * the material into the light colours before it sends them, so the per-light
 * ambient/diffuse/specular here are already light x material, and the scene
 * ambient is the global ambient x material ambient. */
static struct {
    float scene_ambient[3], emission[3], alpha;
    uint32_t color_material, enable_mask, specular_on, control;
    struct {
        float ambient[3], diffuse[3], specular[3], range, half[3], dir[3];
        float pos[3], atten[3];
    } l[8];
} s_lt = { {0, 0, 0}, {0, 0, 0}, 1.0f, 0, 0, 0, 0, {{{0}}} };

static int ff_light_method(uint32_t method, uint32_t param)
{
    float f;
    memcpy(&f, &param, 4);
    if (method >= 0x0A10 && method < 0x0A1C) { s_lt.scene_ambient[(method - 0x0A10) / 4] = f; return 1; }
    if (method >= 0x03A8 && method < 0x03B4) { s_lt.emission[(method - 0x03A8) / 4] = f; return 1; }
    if (method == 0x03B4) { s_lt.alpha = f; return 1; }               /* MATERIAL_ALPHA */
    if (method == 0x0298) { s_lt.color_material = param; return 1; }  /* COLOR_MATERIAL */
    if (method == 0x03BC) { s_lt.enable_mask = param; return 1; }     /* LIGHT_ENABLE_MASK */
    if (method == 0x03B8) { s_lt.specular_on = param; return 1; }     /* SPECULAR_ENABLE */
    if (method == 0x0294) { s_lt.control = param; return 1; }         /* LIGHT_CONTROL */
    if (method >= 0x1000 && method < 0x1400) {
        uint32_t i = (method - 0x1000) / 0x80, o = (method - 0x1000) % 0x80;
        if (o < 0x0C)       s_lt.l[i].ambient[o / 4] = f;
        else if (o < 0x18)  s_lt.l[i].diffuse[(o - 0x0C) / 4] = f;
        else if (o < 0x24)  s_lt.l[i].specular[(o - 0x18) / 4] = f;
        else if (o == 0x24) s_lt.l[i].range = f;
        else if (o < 0x34)  s_lt.l[i].half[(o - 0x28) / 4] = f;
        else if (o < 0x40)  s_lt.l[i].dir[(o - 0x34) / 4] = f;
        else if (o >= 0x5C && o < 0x68) s_lt.l[i].pos[(o - 0x5C) / 4] = f;
        else if (o >= 0x68 && o < 0x74) s_lt.l[i].atten[(o - 0x68) / 4] = f;
        return 1;                                   /* spot terms: not modelled */
    }
    return 0;
}

static void mat4_apply(const float m[16], const float v[4], float out[4])
{
    int r;
    for (r = 0; r < 4; r++)
        out[r] = m[4*r] * v[0] + m[4*r+1] * v[1] + m[4*r+2] * v[2] + m[4*r+3] * v[3];
}

/* RECOMP_NO_FF_LIGHTING=1: the old behaviour, vertex colours as they are. */
static int ff_lighting_off(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_NO_FF_LIGHTING") != NULL;
    return v;
}

/* Fixed-function lighting, after xemu's vsh-ff. Before this a lit batch kept
 * its vertex colour, or white when it had none -- and JSRF's menus (Roboy,
 * character select, the pause map) are lit meshes with normals and no
 * colours, so they were all white (run 60). Infinite and local lights, the
 * colour-material sources, scene ambient and emission; spot cones, back
 * lights and two-sided lighting are not modelled, and specular uses a fixed
 * exponent. */
static void ff_light_vertex(uint32_t index, const float eye[4], Nv2aVshOutput *o)
{
    float nrm[4], n[4], l, vd[4], vs[4], c[3], sp[3];
    uint32_t cm = s_lt.color_material, i, k;
    int em_src = cm & 3, am_src = (cm >> 2) & 3, df_src = (cm >> 4) & 3, sp_src = (cm >> 6) & 3;
    const float *src_for[3];

    memcpy(vd, o->d0, sizeof vd);            /* vertex diffuse (white if none) */
    memcpy(vs, o->d1, sizeof vs);            /* vertex specular */
    src_for[0] = NULL; src_for[1] = vd; src_for[2] = vs;

    if (!fetch_attr(&s_gpu.attr[2], index, nrm)) {
        nrm[0] = 0; nrm[1] = 0; nrm[2] = 1;
    }
    nrm[3] = 0.0f;
    mat4_apply(s_gpu.ff_inv_mv0, nrm, n);
    l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0.0f) { n[0] /= l; n[1] /= l; n[2] /= l; }

    for (k = 0; k < 3; k++) {
        float e = s_lt.emission[k], a = s_lt.scene_ambient[k];
        if (em_src && src_for[em_src]) e = src_for[em_src][k];
        if (am_src && src_for[am_src]) a *= src_for[am_src][k];
        c[k] = e + a;
        sp[k] = 0.0f;
    }
    for (i = 0; i < 8; i++) {
        uint32_t mode = (s_lt.enable_mask >> (2 * i)) & 3;
        float vp[3], att = 1.0f, ndl, ndh, hv[3];
        if (!mode)
            continue;
        if (mode == 1) {                             /* infinite */
            vp[0] = s_lt.l[i].dir[0]; vp[1] = s_lt.l[i].dir[1]; vp[2] = s_lt.l[i].dir[2];
            hv[0] = s_lt.l[i].half[0]; hv[1] = s_lt.l[i].half[1]; hv[2] = s_lt.l[i].half[2];
        } else {                                     /* local (and spot, as local) */
            float d;
            vp[0] = s_lt.l[i].pos[0] - eye[0];
            vp[1] = s_lt.l[i].pos[1] - eye[1];
            vp[2] = s_lt.l[i].pos[2] - eye[2];
            d = sqrtf(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]);
            if (d > 0.0f) { vp[0] /= d; vp[1] /= d; vp[2] /= d; }
            {
                float den = s_lt.l[i].atten[0] + s_lt.l[i].atten[1] * d
                          + s_lt.l[i].atten[2] * d * d;
                att = den > 0.0f ? 1.0f / den : 1.0f;
            }
            if (s_lt.l[i].range > 0.0f && d > s_lt.l[i].range)
                att = 0.0f;
            hv[0] = vp[0]; hv[1] = vp[1]; hv[2] = vp[2] + 1.0f;
            {
                float hl = sqrtf(hv[0] * hv[0] + hv[1] * hv[1] + hv[2] * hv[2]);
                if (hl > 0.0f) { hv[0] /= hl; hv[1] /= hl; hv[2] /= hl; }
            }
        }
        ndl = n[0] * vp[0] + n[1] * vp[1] + n[2] * vp[2];
        if (ndl < 0.0f) ndl = 0.0f;
        ndh = n[0] * hv[0] + n[1] * hv[1] + n[2] * hv[2];
        if (ndh < 0.0f) ndh = 0.0f;
        for (k = 0; k < 3; k++) {
            float am = s_lt.l[i].ambient[k], df = s_lt.l[i].diffuse[k];
            if (am_src && src_for[am_src]) am *= src_for[am_src][k];
            if (df_src && src_for[df_src]) df *= src_for[df_src][k];
            c[k] += att * (am + df * ndl);
            if (s_lt.specular_on && ndl > 0.0f) {
                float spc = s_lt.l[i].specular[k];
                if (sp_src && src_for[sp_src]) spc *= src_for[sp_src][k];
                sp[k] += att * spc * powf(ndh, 10.0f);
            }
        }
    }
    for (k = 0; k < 3; k++) {
        o->d0[k] = c[k] < 0.0f ? 0.0f : (c[k] > 1.0f ? 1.0f : c[k]);
        o->d1[k] = sp[k] < 0.0f ? 0.0f : (sp[k] > 1.0f ? 1.0f : sp[k]);
    }
    o->d0[3] = (df_src && src_for[df_src]) ? src_for[df_src][3] : s_lt.alpha;
}

static void ff_transform(uint32_t index, Nv2aVshOutput *o)
{
    float pos[4], w[4] = {1, 0, 0, 0}, eye[4] = {0, 0, 0, 0}, clip[4], t[4];
    const float *off = nv2a_vsh_constant(59);
    int nmat = 1, i, j;

    memset(o, 0, sizeof *o);
    fetch_attr(&s_gpu.attr[0], index, pos);

    switch (s_gpu.ff_skin_mode) {               /* SKIN_MODE: count, implicit */
    case 1: case 2: nmat = 2; break;
    case 3: case 4: nmat = 3; break;
    case 5: case 6: nmat = 4; break;
    default: nmat = 1; break;
    }
    if (nmat > 1) {
        float wt[4];
        fetch_attr(&s_gpu.attr[1], index, wt);
        float sum = 0.0f;
        for (i = 0; i < nmat - 1; i++) { w[i] = wt[i]; sum += wt[i]; }
        /* Odd modes give the last weight implicitly: 1 - the rest. */
        w[nmat - 1] = (s_gpu.ff_skin_mode & 1) ? 1.0f - sum : wt[nmat - 1];
        for (i = 0; i < nmat; i++) {
            mat4_apply(s_gpu.ff_mv[i], pos, t);
            for (j = 0; j < 4; j++) eye[j] += w[i] * t[j];
        }
        mat4_apply(s_gpu.ff_composite, eye, clip);
    } else {
        mat4_apply(s_gpu.ff_mv[0], pos, eye);
        mat4_apply(s_gpu.ff_composite, pos, clip);
    }

    o->pos[3] = clip[3];
    if (clip[3] != 0.0f)
        for (j = 0; j < 3; j++)
            o->pos[j] = clip[j] / clip[3] + off[j];

    if (!fetch_attr(&s_gpu.attr[3], index, o->d0))
        o->d0[0] = o->d0[1] = o->d0[2] = o->d0[3] = 1.0f;
    if (!fetch_attr(&s_gpu.attr[4], index, o->d1))
        memset(o->d1, 0, sizeof o->d1);
    if (s_gpu.ff_lighting && !ff_lighting_off())
        ff_light_vertex(index, eye, o);
    o->fog[0] = fabsf(eye[2]);
    {
        /* Texture coordinates: generated per component where TEXGEN asks
         * (xemu vsh-ff.c), then through the stage's texture matrix when it
         * is enabled. */
        float nrm[4], en[4] = {0, 0, 0, 0}, refl[3] = {0, 0, 0}, ev[3];
        int need_n = 0, c;
        for (i = 0; i < 4; i++)
            for (c = 0; c < 4; c++)
                if (s_gpu.ff_texgen[i][c] == 0x2402 || s_gpu.ff_texgen[i][c] == 0x8511
                        || s_gpu.ff_texgen[i][c] == 0x8512)
                    need_n = 1;
        if (need_n) {
            float l, d;
            fetch_attr(&s_gpu.attr[2], index, nrm);
            nrm[3] = 0.0f;
            mat4_apply(s_gpu.ff_inv_mv0, nrm, en);
            l = sqrtf(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
            if (l > 0.0f) { en[0] /= l; en[1] /= l; en[2] /= l; }
            l = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
            for (c = 0; c < 3; c++) ev[c] = l > 0.0f ? eye[c] / l : 0.0f;
            d = en[0] * ev[0] + en[1] * ev[1] + en[2] * ev[2];
            for (c = 0; c < 3; c++) refl[c] = ev[c] - 2.0f * d * en[c];
        }
        for (i = 0; i < 4; i++) {
            float *t = o->tex[i];
            fetch_attr(&s_gpu.attr[9 + i], index, t);  /* absent: 0,0,0,1 */
            for (c = 0; c < 4; c++) {
                const float *pl = s_gpu.ff_texplane[i][c];
                switch (s_gpu.ff_texgen[i][c]) {
                case 0x2400:                           /* EYE_LINEAR */
                    t[c] = pl[0] * eye[0] + pl[1] * eye[1] + pl[2] * eye[2] + pl[3] * eye[3];
                    break;
                case 0x2401:                           /* OBJECT_LINEAR */
                    t[c] = pl[0] * pos[0] + pl[1] * pos[1] + pl[2] * pos[2] + pl[3] * pos[3];
                    break;
                case 0x2402: {                         /* SPHERE_MAP */
                    float m = 2.0f * sqrtf(refl[0] * refl[0] + refl[1] * refl[1]
                                           + (refl[2] + 1.0f) * (refl[2] + 1.0f));
                    if (c < 2) t[c] = m > 0.0f ? refl[c] / m + 0.5f : 0.5f;
                    break;
                }
                case 0x8511:                           /* NORMAL_MAP */
                    if (c < 3) t[c] = en[c];
                    break;
                case 0x8512:                           /* REFLECTION_MAP */
                    if (c < 3) t[c] = refl[c];
                    break;
                default:
                    break;
                }
            }
            if (s_gpu.ff_texmat_on[i]) {
                float r[4];
                mat4_apply(s_gpu.ff_texmat[i], t, r);
                memcpy(t, r, sizeof r);
            }
        }
    }
}

/* v77: what the GPU vertex shader needs for this batch. */
static void build_vs_state(NvGlVsState *vs, int kind)
{
    int i, c;
    memset(vs, 0, sizeof *vs);
    vs->present = s_raw_present;
    vs->consts = nv2a_vsh_constants(&vs->const_gen);
    vs->fog_enable = (int32_t)s_gpu.fog_enable;
    vs->fog_mode = s_gpu.fog_mode;
    vs->fog_param[0] = s_gpu.fog_param[0];
    vs->fog_param[1] = s_gpu.fog_param[1];
    if (kind == XK_PROGRAM) {
        vs->mode = NV_GL_VS_PROGRAM;
        vs->nins = vsh_packed(&vs->prog_gen);
        vs->prog = (const uint32_t (*)[4])s_vsh_pack;
        return;
    }
    vs->mode = NV_GL_VS_FF;
    memcpy(vs->ff.composite, s_gpu.ff_composite, sizeof vs->ff.composite);
    memcpy(vs->ff.mv, s_gpu.ff_mv, sizeof vs->ff.mv);
    memcpy(vs->ff.inv_mv0, s_gpu.ff_inv_mv0, sizeof vs->ff.inv_mv0);
    memcpy(vs->ff.texmat, s_gpu.ff_texmat, sizeof vs->ff.texmat);
    memcpy(vs->ff.texplane, s_gpu.ff_texplane, sizeof vs->ff.texplane);
    vs->ff.skin_mode = (int32_t)s_gpu.ff_skin_mode;
    vs->ff.lighting = s_gpu.ff_lighting && !ff_lighting_off();
    for (i = 0; i < 4; i++) {
        vs->ff.texmat_on[i] = s_gpu.ff_texmat_on[i] != 0;
        for (c = 0; c < 4; c++)
            vs->ff.texgen[i][c] = (int32_t)s_gpu.ff_texgen[i][c];
    }
    if (!vs->ff.lighting)
        return;
    memcpy(vs->ff.scene_ambient, s_lt.scene_ambient, sizeof vs->ff.scene_ambient);
    memcpy(vs->ff.emission, s_lt.emission, sizeof vs->ff.emission);
    vs->ff.mat_alpha = s_lt.alpha;
    vs->ff.color_material = s_lt.color_material;
    vs->ff.light_enable = s_lt.enable_mask;
    vs->ff.specular_on = s_lt.specular_on;
    for (i = 0; i < 8; i++) {
        memcpy(vs->ff.l_amb[i], s_lt.l[i].ambient, 12);
        memcpy(vs->ff.l_dif[i], s_lt.l[i].diffuse, 12);
        memcpy(vs->ff.l_spc[i], s_lt.l[i].specular, 12);
        memcpy(vs->ff.l_half[i], s_lt.l[i].half, 12);
        memcpy(vs->ff.l_dir[i], s_lt.l[i].dir, 12);
        memcpy(vs->ff.l_pos[i], s_lt.l[i].pos, 12);
        memcpy(vs->ff.l_att[i], s_lt.l[i].atten, 12);
        vs->ff.l_range[i] = s_lt.l[i].range;
    }
}

/* Pre-transformed vertices (D3D's XYZRHW) in fixed-function mode: position
 * is already x, y in pixels, z, and 1/w. Recognised by a four-component
 * position, which no untransformed FVF has.
 *
 * Until v31 these went through the screen-space test and drew raw; the
 * fixed-function transform then sent them through the composite matrix,
 * which put JSRF's particles -- smoke, dust -- off the screen. z arrives in
 * [0,1] and is scaled to the depth buffer's units; a z already in depth
 * units is left alone. The fog factor is the specular alpha, as D3D
 * defines it for pre-transformed vertices. */
static uint32_t s_batches_pre;

static void pre_transform(uint32_t index, Nv2aVshOutput *o, float zmax)
{
    float p[4];
    int i;

    memset(o, 0, sizeof *o);
    fetch_attr(&s_gpu.attr[0], index, p);
    o->pos[0] = p[0];
    o->pos[1] = p[1];
    o->pos[2] = fabsf(p[2]) <= 2.0f ? p[2] * zmax : p[2];
    o->pos[3] = (p[3] > 0.0f && p[3] == p[3]) ? 1.0f / p[3] : 1.0f;
    if (!fetch_attr(&s_gpu.attr[3], index, o->d0))
        o->d0[0] = o->d0[1] = o->d0[2] = o->d0[3] = 1.0f;
    if (fetch_attr(&s_gpu.attr[4], index, o->d1)) {
        o->fog[0] = o->d1[3];
    } else {
        memset(o->d1, 0, sizeof o->d1);
        o->fog[0] = 1.0f;
    }
    for (i = 0; i < 4; i++)
        fetch_attr(&s_gpu.attr[9 + i], index, o->tex[i]);
}

static void raster_batch_pretransformed(void)
{
    uint32_t i, n = s_gpu.idx_count;
    float zmax = ((s_gpu.format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;

    if (gl_fast()) {
        if (!gl_transform(XK_PRE, zmax))
            return;
    } else {
        for (i = 0; i < n; i++)
            pre_transform(s_gpu.idx[i], &s_xf[i], zmax);
    }
    s_batches_pre++;
    if (s_gpu.texs[0].valid)
        note_texture_use();
    {
        static int said;
        if (said < 4 && n >= 3) {
            said++;
            fprintf(stderr, "[PRE] pre-transformed batch: prim %u n %u v0 %g %g %g w %g"
                    " d0 %.2f %.2f %.2f %.2f t0 %g %g\n", s_gpu.prim, n,
                    s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2], s_xf[0].pos[3],
                    s_xf[0].d0[0], s_xf[0].d0[1], s_xf[0].d0[2], s_xf[0].d0[3],
                    s_xf[0].tex[0][0], s_xf[0].tex[0][1]);
        }
    }
    if (gl_fast()) {
        gl_draw_batch();
        return;
    }
    s_fog_direct = 1;
    if (nv2a_gl_active()) {
        gl_submit(n);
        s_fog_direct = 0;
        return;
    }
    s_ff_active = 1;
    s_pre_active = 1;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
    raster_flush();
    s_ff_active = 0;
    s_pre_active = 0;
    s_fog_direct = 0;
}

static void raster_batch_fixed(void)
{
    uint32_t i, n = s_gpu.idx_count;

    if (gl_fast()) {
        if (!gl_transform(XK_FIXED, 0.0f))
            return;
    } else {
        LARGE_INTEGER q0, q1;
        fast_qpc_li(&q0);
        if (++s_vc_gen == 0)
            s_vc_gen = 1;
        for (i = 0; i < n; i++) {
            if (vcache_get(s_gpu.idx[i], &s_xf[i]))
                continue;
            ff_transform(s_gpu.idx[i], &s_xf[i]);
            vcache_put(s_gpu.idx[i], &s_xf[i]);
        }
        fast_qpc_li(&q1);
        s_perf_vertex += (uint64_t)(q1.QuadPart - q0.QuadPart);
    }
    {
        /* RECOMP_FF_TRACE=<n>: n fixed-function batches, spread out --
         * matrices, the first vertex in and the first three out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_FF_TRACE");
            left = t ? atoi(t) : 0;
        }
        if (left > 0 && n >= 3 && s_gpu.batches_ff % 5000 == 0) {
            float in[4];
            const float *c = s_gpu.ff_composite, *m = s_gpu.ff_mv[0];
            left--;
            fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], in);
            fprintf(stderr, "[FFTRACE] prim %u n %u skin %u light %u v0 type %u size %u"
                    " = %g %g %g %g\n", s_gpu.prim, n, s_gpu.ff_skin_mode,
                    s_gpu.ff_lighting, s_gpu.attr[0].type, s_gpu.attr[0].size,
                    in[0], in[1], in[2], in[3]);
            for (i = 0; i < 4; i++)
                fprintf(stderr, "[FFTRACE]   comp %g %g %g %g   mv0 %g %g %g %g\n",
                        c[4*i], c[4*i+1], c[4*i+2], c[4*i+3],
                        m[4*i], m[4*i+1], m[4*i+2], m[4*i+3]);
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[FFTRACE]   out%u pos %g %g %g %g d0 %.2f %.2f %.2f %.2f"
                        " t0 %g %g\n", i, s_xf[i].pos[0], s_xf[i].pos[1],
                        s_xf[i].pos[2], s_xf[i].pos[3], s_xf[i].d0[0], s_xf[i].d0[1],
                        s_xf[i].d0[2], s_xf[i].d0[3], s_xf[i].tex[0][0], s_xf[i].tex[0][1]);
        }
    }
    s_gpu.batches_ff++;
    s_gpu.verts_ff += n;
    if (s_gpu.texs[0].valid)
        note_texture_use();

    if (gl_fast()) {
        gl_draw_batch();
        return;
    }
    if (nv2a_gl_active()) {
        gl_submit(n);
        return;
    }
    s_ff_active = 1;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* Every other strip triangle is wound the other way; swap those so
         * all of them keep the strip's facing (culling depends on it). */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
    raster_flush();
    s_ff_active = 0;
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    static int no_vsh = -1;
    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    if ((s_gpu.xf_mode & 3) == 2 && !no_vsh) {
        raster_batch_program();
        return;
    }
    {
        /* Fixed function goes through the composite matrix whenever the
         * title has loaded one. The screen-space test is scale, and a stage
         * modelled in big units (JSRF's span thousands) passes it: drawn raw,
         * it came out as one texture stretched across the screen. D3D draws
         * pre-transformed (XYZRHW) vertices with a vertex program, so in
         * fixed-function mode nothing is legitimately screen space.
         * RECOMP_NO_FF=1: the old behaviour (screen-space test, else skip).
         * RECOMP_FF_SCREEN_FIRST=1: screen-space test first, then this. */
        static int no_ff = -1, screen_first;
        if (no_ff < 0) {
            no_ff = getenv("RECOMP_NO_FF") != NULL;
            screen_first = getenv("RECOMP_FF_SCREEN_FIRST") != NULL;
        }
        static int pre = -1;
        if (pre < 0)
            pre = getenv("RECOMP_NO_PRETRANSFORMED") == NULL;
        /* On by default: this is what brought JSRF's smoke particles back
         * (run 40). RECOMP_NO_PRETRANSFORMED=1 turns it off. */
        /* Immediate-mode batches carry every attribute as a float4, so
         * their format says nothing; what counts is whether the position
         * was sent with four components (SET_VERTEX4F) or three. JSRF's
         * braking smoke may be three-component immediate geometry in world
         * space, which this read as screen pixels. */
        if (pre && !no_ff && s_gpu.attr[0].size == 4 && s_gpu.attr[0].type == 2
                && (!s_gpu.imm_batch || s_gpu.imm_pos4)) {
            raster_batch_pretransformed();
            return;
        }
        if (s_gpu.ff_composite_set && !no_ff
                && (!screen_first || !batch_is_screen_space())) {
            raster_batch_fixed();
            return;
        }
    }
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }
    /* The software path draws into guest memory: make it current first. The
     * GPU copy is refreshed from it on its next use. */
    if (nv2a_gl_active())
        nv2a_gl_sync_to_guest(0);   /* the target, and any texture it samples */

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* RECOMP_FRAME_TRACE=<flag file>: once the file exists, log every batch of
 * the next whole frame (flip to flip) -- where it drew, with what texture,
 * blend and depth state, and how many pixels it actually wrote. "The frame
 * is black" has many causes and this is what tells them apart. */

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {
        /* Deleting the flag file re-arms the trace for the next time it
         * appears, so one run can trace several moments. */
        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        nv2a_gl_trace_report();
        nv2a_gl_trace_frame(0);
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        nv2a_vsh_trace_reset();
        s_ftrace_draw = 0;
        nv2a_gl_trace_frame(1);
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

/* v119: on a traced frame, each texture a draw has bound is also written out
 * once, decoded, as ftrace_tex_<address>_<w>x<h>_f<format>.bmp next to the
 * exe -- what the GPU is given to sample, to tell a wrong image from a wrong
 * use of a right one. Up to 96 textures a frame, 1024x1024 at most, no cube
 * maps. RECOMP_FTRACE_TEXDUMP=0 turns it off. */
static void ftrace_write_bmp(const char *name, const uint32_t *px, uint32_t w, uint32_t h)
{
    FILE *f = fopen(name, "wb");
    uint8_t hd[54];
    uint32_t y, sz = w * h * 4;
    if (!f)
        return;
    memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M';
    *(uint32_t *)(hd + 2) = 54 + sz;
    *(uint32_t *)(hd + 10) = 54;
    *(uint32_t *)(hd + 14) = 40;
    *(int32_t *)(hd + 18) = (int32_t)w;
    *(int32_t *)(hd + 22) = -(int32_t)h;          /* top-down */
    *(uint16_t *)(hd + 26) = 1;
    *(uint16_t *)(hd + 28) = 32;
    *(uint32_t *)(hd + 34) = sz;
    fwrite(hd, 1, sizeof hd, f);
    for (y = 0; y < h; y++)
        fwrite(px + (size_t)y * w, 4, w, f);   /* BGRA = ARGB little-endian */
    fclose(f);
}

static void ftrace_dump_textures(void)
{
    static int on = -1;
    static uint32_t frame = ~0u, done[96];
    static int ndone;
    int k, j;
    if (on < 0) {
        const char *e = getenv("RECOMP_FTRACE_TEXDUMP");
        on = !(e && e[0] == '0');
    }
    if (!on)
        return;
    if (frame != s_gpu.flips) {
        frame = s_gpu.flips;
        ndone = 0;
    }
    for (k = 0; k < 4; k++) {
        const Texture *t = &s_gpu.texs[k];
        uint32_t *px;
        char name[96];
        if (!t->valid || t->cube || !t->width || !t->height
                || t->width > 1024 || t->height > 1024 || ndone >= 96)
            continue;
        for (j = 0; j < ndone; j++)
            if (done[j] == t->offset)
                break;
        if (j < ndone)
            continue;
        done[ndone++] = t->offset;
        px = (uint32_t *)calloc((size_t)t->width * t->height, 4);
        if (!px)
            continue;
        if (nv2a_exec_decode_texture(k, 0, px)) {
            snprintf(name, sizeof name, "ftrace_tex_%08X_%ux%u_f%02X.bmp",
                     t->offset, t->width, t->height, t->color);
            ftrace_write_bmp(name, px, t->width, t->height);
            fprintf(stderr, "[FTRACE]     texture %08X written to %s\n", t->offset, name);
        }
        free(px);
    }
}

static uint64_t s_perf_raster;          /* QPC ticks inside raster_batch */
static DWORD s_perf_tid;
static unsigned s_perf_thread_changes;

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
static uint64_t s_dp_ticks, s_dp_raster, s_dp_n;   /* v150: rdtsc, RECOMP_PROFILE */
static void draw_primitive_body(void);
static int s_mt_on;
static void draw_primitive(void)
{
    uint64_t t0;
    if (s_mt_on <= 0) {
        draw_primitive_body();
        return;
    }
    t0 = __rdtsc();
    draw_primitive_body();
    s_dp_ticks += __rdtsc() - t0;
    s_dp_n++;
}

static void draw_primitive_body(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 50000) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
        uint32_t r_cull = s_gpu.tris_culled, r_behind = s_gpu.tris_behind,
                 r_deg = s_gpu.xf_degenerate, r_off = s_gpu.tris_skipped_offscreen;
        {
            /* Where a frame's time goes: the rasteriser, against the wall
             * clock between reports ([PERF] line). */
            LARGE_INTEGER q0, q1;
            uint64_t r0 = s_mt_on > 0 ? __rdtsc() : 0;
            fast_qpc_li(&q0);
            raster_batch();
            fast_qpc_li(&q1);
            if (s_mt_on > 0) s_dp_raster += __rdtsc() - r0;
            s_perf_raster += (uint64_t)(q1.QuadPart - q0.QuadPart);
            if (GetCurrentThreadId() != s_perf_tid) {
                s_perf_tid = GetCurrentThreadId();
                s_perf_thread_changes++;
            }
        }
        if (s_ftrace == 2) {
            s_ftrace_draw++;
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X | z test %u func %X mask %u | cull %u %X %X"
                    " | stencil %u func %X ref %X mask %X/%X ops %X %X %X | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.idx_count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.cull_enable, s_gpu.cull_face, s_gpu.front_face,
                    s_gpu.stencil_test, s_gpu.stencil_func, s_gpu.stencil_ref,
                    s_gpu.stencil_mask, s_gpu.stencil_wmask, s_gpu.stencil_fail,
                    s_gpu.stencil_zfail, s_gpu.stencil_zpass,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if (nv2a_gl_active()) {
                /* v92: what the top of the screen looks like after this
                 * draw, at x 40/160/320/480/600 (y 20/50/20/50/20) */
                uint32_t pc[5];
                nv2a_gl_probe_top(pc);
                fprintf(stderr, "[FTRACE]     top %06X %06X %06X %06X %06X\n",
                        pc[0], pc[1], pc[2], pc[3], pc[4]);
                {
                    /* v98: more probe points, RECOMP_FTRACE_PROBE="x,y x,y ..."
                     * (guest pixels, up to 8); by default across the bottom */
                    static int np = -1, ppx[8], ppy[8];
                    uint32_t pv[8];
                    int q;
                    if (np < 0) {
                        const char *e = getenv("RECOMP_FTRACE_PROBE");
                        static const int dx[6] = {150, 300, 300, 400, 500, 600},
                                         dy[6] = {450, 400, 450, 450, 460, 470};
                        np = 0;
                        if (e) {
                            int a, b, used;
                            while (np < 8 && sscanf(e, " %d,%d%n", &a, &b, &used) == 2) {
                                ppx[np] = a; ppy[np] = b; np++; e += used;
                            }
                        }
                        if (!np)
                            for (; np < 6; np++) { ppx[np] = dx[np]; ppy[np] = dy[np]; }
                    }
                    nv2a_gl_probe_at(ppx, ppy, np, pv);
                    fprintf(stderr, "[FTRACE]     probe");
                    for (q = 0; q < np; q++)
                        fprintf(stderr, " %d,%d=%06X/z%.6f", ppx[q], ppy[q], pv[q],
                                nv2a_gl_probe_z(q));
                    fputc(10, stderr);
                }
            }
            fprintf(stderr, "[FTRACE]     rejected: %u culled, %u behind the eye,"
                    " %u degenerate, %u off-surface\n",
                    s_gpu.tris_culled - r_cull, s_gpu.tris_behind - r_behind,
                    s_gpu.xf_degenerate - r_deg, s_gpu.tris_skipped_offscreen - r_off);
            {
                /* What the first vertex came out as, and what each attribute
                 * was read from: an array (type/size/stride) or, with no
                 * array, its inline value. White menus (run 59) are either
                 * white vertex colours or white texels, and this says which. */
                int k;
                fprintf(stderr, "[FTRACE]     out0 d0 %.2f %.2f %.2f %.2f d1 %.2f %.2f %.2f %.2f"
                        " t0 %g %g %g %g t1 %g %g | attrs",
                        s_xf[0].d0[0], s_xf[0].d0[1], s_xf[0].d0[2], s_xf[0].d0[3],
                        s_xf[0].d1[0], s_xf[0].d1[1], s_xf[0].d1[2], s_xf[0].d1[3],
                        s_xf[0].tex[0][0], s_xf[0].tex[0][1], s_xf[0].tex[0][2],
                        s_xf[0].tex[0][3], s_xf[0].tex[1][0], s_xf[0].tex[1][1]);
                for (k = 0; k < NV_VERTEX_ATTRS; k++) {
                    const VertexAttr *at = &s_gpu.attr[k];
                    if (at->size && at->stride)
                        fprintf(stderr, " v%d:%u/%u/%u", k, at->type, at->size, at->stride);
                    else if ((s_imm_ever >> k) & 1)
                        fprintf(stderr, " v%d=(%.2f %.2f %.2f %.2f)", k, s_gpu.imm_attr[k][0],
                                s_gpu.imm_attr[k][1], s_gpu.imm_attr[k][2], s_gpu.imm_attr[k][3]);
                }
                fprintf(stderr, " | alpha test %u func %X ref %02X | fog %u mode %X"
                        " | stages", s_gpu.alpha_test, s_gpu.alpha_func,
                        s_gpu.alpha_ref & 0xFF, s_gpu.fog_enable, s_gpu.fog_mode);
                for (k = 0; k < 4; k++)
                    fprintf(stderr, " %d:%s%08X %ux%u f%02X p%u c0 %08X", k,
                            s_gpu.texs[k].valid ? "" : "-", s_gpu.texs[k].offset,
                            s_gpu.texs[k].width, s_gpu.texs[k].height,
                            s_gpu.texs[k].color, s_gpu.texs[k].pitch, s_gpu.texs[k].ctl0);
                fputc(10, stderr);
                ftrace_dump_textures();
            }
            /* v91: where the first vertices land on screen, with their
             * texture coordinates and alpha (CPU vertex path only; the GPU
             * path never brings them back). A draw that covers the screen
             * shows here as coordinates far outside 0..640 x 0..480. */
            if (1) {
                uint32_t k, nk = s_gpu.idx_count < 4 ? s_gpu.idx_count : 4;
                /* v99: RECOMP_FTRACE_VERTS="2,3,65": every vertex of those
                 * draws (numbered from 1 in the frame, as the traces count) */
                {
                    static const char *ev = (const char *)-1;
                    char key[16];
                    size_t kl;
                    const char *hit;
                    if (ev == (const char *)-1) ev = getenv("RECOMP_FTRACE_VERTS");
                    kl = (size_t)snprintf(key, sizeof key, "%u", s_ftrace_draw);
                    for (hit = ev; hit && (hit = strstr(hit, key)) != NULL; hit += kl)
                        if ((hit == ev || hit[-1] == ',') && (hit[kl] == ',' || !hit[kl]))
                            break;
                    if (hit) {
                        uint32_t vi;
                        for (vi = 0; vi < s_gpu.idx_count; vi++) {
                            Nv2aVshOutput o;
                            if ((s_gpu.xf_mode & 3) == 2) { nv2a_vsh_prepare(); if (!transform_vertex(s_gpu.idx[vi], &o)) break; }
                            else ff_transform(s_gpu.idx[vi], &o);
                            fprintf(stderr, "[FTRACE]     vert %u idx %u | %.3f %.3f z %.9g w %.9g uv %.4f %.4f a %.3f\n",
                                    vi, s_gpu.idx[vi], o.pos[0], o.pos[1], o.pos[2], o.pos[3],
                                    o.tex[0][0], o.tex[0][1], o.d0[3]);
                        }
                    }
                }
                fprintf(stderr, "[FTRACE]     screen");
                for (k = 0; k < nk; k++)
                    fprintf(stderr, " | %.1f %.1f z %.4g w %.4g uv %.3f %.3f a %.2f",
                            s_xf[k].pos[0], s_xf[k].pos[1], s_xf[k].pos[2], s_xf[k].pos[3],
                            s_xf[k].tex[0][0], s_xf[k].tex[0][1], s_xf[k].d0[3]);
                fputc(10, stderr);
                /* v129: every stage's coordinates for those vertices (the
                 * river's bump map and water texture are on stages 0 and 1) */
                {
                    int st;
                    for (st = 1; st < 4; st++) {
                        if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F) && !s_gpu.texs[st].valid)
                            continue;
                        fprintf(stderr, "[FTRACE]     t%d", st);
                        for (k = 0; k < nk; k++)
                            fprintf(stderr, " | %.4g %.4g %.4g %.4g", s_xf[k].tex[st][0],
                                    s_xf[k].tex[st][1], s_xf[k].tex[st][2], s_xf[k].tex[st][3]);
                        fputc(10, stderr);
                    }
                    if ((s_gpu.xf_mode & 3) != 2)
                        for (st = 0; st < 4; st++)
                            if (s_gpu.ff_texmat_on[st] || s_gpu.ff_texgen[st][0] || s_gpu.ff_texgen[st][1]
                                    || s_gpu.ff_texgen[st][2] || s_gpu.ff_texgen[st][3])
                                fprintf(stderr, "[FTRACE]     ff stage %d texgen %X %X %X %X matrix %s"
                                        " %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n", st,
                                        s_gpu.ff_texgen[st][0], s_gpu.ff_texgen[st][1],
                                        s_gpu.ff_texgen[st][2], s_gpu.ff_texgen[st][3],
                                        s_gpu.ff_texmat_on[st] ? "on" : "off",
                                        s_gpu.ff_texmat[st][0], s_gpu.ff_texmat[st][1], s_gpu.ff_texmat[st][2], s_gpu.ff_texmat[st][3],
                                        s_gpu.ff_texmat[st][4], s_gpu.ff_texmat[st][5], s_gpu.ff_texmat[st][6], s_gpu.ff_texmat[st][7],
                                        s_gpu.ff_texmat[st][8], s_gpu.ff_texmat[st][9], s_gpu.ff_texmat[st][10], s_gpu.ff_texmat[st][11],
                                        s_gpu.ff_texmat[st][12], s_gpu.ff_texmat[st][13], s_gpu.ff_texmat[st][14], s_gpu.ff_texmat[st][15]);
                }
                /* v121: the depth buffer under those vertices after the draw
                 * (for a batch that does not write depth, what it was tested
                 * against), beside the batch's own depth there. */
                if (nv2a_gl_active() && nk) {
                    int px[4], py[4], q, nq = 0;
                    uint32_t pv[4];
                    float zm = ((s_gpu.format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
                    for (k = 0; k < nk; k++) {
                        if (s_xf[k].pos[0] < 0 || s_xf[k].pos[1] < 0
                                || s_xf[k].pos[0] >= 4096 || s_xf[k].pos[1] >= 4096)
                            continue;
                        px[nq] = (int)s_xf[k].pos[0]; py[nq] = (int)s_xf[k].pos[1]; nq++;
                    }
                    if (nq) {
                        nv2a_gl_probe_at(px, py, nq, pv);
                        fprintf(stderr, "[FTRACE]     depth under them");
                        for (q = 0; q < nq; q++)
                            fprintf(stderr, " | %d,%d buffer %.6f", px[q], py[q],
                                    nv2a_gl_probe_z(q));
                        fprintf(stderr, " | batch v0 %.6f\n", s_xf[0].pos[2] / zm);
                    }
                }
            }
            {
                /* v93: the first vertex's inputs as the title supplied them
                 * (every attribute with an array), and the vertex program's
                 * outputs beside them -- separates "the title's data says
                 * draw this" from "the program run here got it wrong". */
                int k;
                fprintf(stderr, "[FTRACE]     in0");
                for (k = 0; k < NV_VERTEX_ATTRS; k++) {
                    float pv[4] = {0, 0, 0, 0};
                    const VertexAttr *at = &s_gpu.attr[k];
                    if (at->size && at->stride && fetch_attr(at, s_gpu.idx[0], pv))
                        fprintf(stderr, " v%d=(%.3g %.3g %.3g %.3g)", k, pv[0], pv[1], pv[2], pv[3]);
                }
                if ((s_gpu.xf_mode & 3) == 2)
                    fprintf(stderr, " | out d0 %.2f %.2f %.2f %.2f fog %.3g",
                            s_xf[0].d0[0], s_xf[0].d0[1], s_xf[0].d0[2], s_xf[0].d0[3],
                            s_xf[0].fog[0]);
                fputc(10, stderr);
                if ((s_gpu.xf_mode & 3) == 2) {
                    /* v95: the next two vertices' inputs too, and the program
                     * with its constants, for an off-line replay */
                    uint32_t vi;
                    for (vi = 1; vi < 3 && vi < s_gpu.idx_count; vi++) {
                        fprintf(stderr, "[FTRACE]     in%u", vi);
                        for (k = 0; k < NV_VERTEX_ATTRS; k++) {
                            float pv[4] = {0, 0, 0, 0};
                            const VertexAttr *at = &s_gpu.attr[k];
                            if (at->size && at->stride && fetch_attr(at, s_gpu.idx[vi], pv))
                                fprintf(stderr, " v%d=(%.9g %.9g %.9g %.9g)", k, pv[0], pv[1], pv[2], pv[3]);
                        }
                        fputc(10, stderr);
                    }
                    nv2a_vsh_trace_dump();
                }
            }
            if ((s_gpu.xf_mode & 3) == 2)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
                for (st = 1; st < 4; st++) {
                    uint32_t md = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F;
                    if (md == 6 || md == 7)       /* v105: bump env mapping */
                        fprintf(stderr, "[FTRACE]       bump t%d mat %g %g %g %g scale %g offset %g"
                                " input %08X filter %08X%c", st,
                                s_gpu.rc.bump_mat[st][0], s_gpu.rc.bump_mat[st][1],
                                s_gpu.rc.bump_mat[st][2], s_gpu.rc.bump_mat[st][3],
                                s_gpu.rc.bump_ls[st][0], s_gpu.rc.bump_ls[st][1],
                                s_gpu.rc.other_input, s_gpu.texs[st - 1].filter, 10);
                }
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    if (pb_verbose()) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static uint32_t s_imm_batches, s_imm_batches4;

static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. Every attribute is a float4;
     * the ones the batch never set carry their standing values, as on the
     * GPU. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    s_gpu.imm_batch = 1;
    s_imm_batches++;
    if (s_gpu.imm_pos4)
        s_imm_batches4++;
    draw_primitive();
    s_gpu.imm_batch = 0;
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append all 16 attributes in the layout
 * draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
/* The immediate-mode attribute methods (xemu pgraph.c SET_VERTEX_DATA*).
 * Each sets an attribute's current value; completing attribute 0 -- the
 * position -- emits a vertex carrying every attribute as it stands. This used
 * to keep position, diffuse and texcoord 0 only, which dropped any quad with
 * a second texture coordinate: Burnout 3 composites its whole 3D scene into
 * the frame with exactly such a quad, and the frame never got the scene. */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1; s_imm_ever |= 1;
        s_gpu.imm_pos4 = 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1; s_imm_ever |= 1;
        s_gpu.imm_pos4 = 0;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        if (attr == 0)
            s_gpu.imm_pos4 = 0;
        s_gpu.imm_used |= (uint16_t)(1u << attr); s_imm_ever |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr); s_imm_ever |= (uint16_t)(1u << attr);
        if (attr == 0)
            s_gpu.imm_pos4 = 1;
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr); s_imm_ever |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        /* Bytes in register order, x from the low byte (xemu). */
        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr); s_imm_ever |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        s_gpu.imm_used |= (uint16_t)(1u << attr); s_imm_ever |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
/* The texture stage registers, all four stages. Stage i's registers are
 * 0x1B00 + 0x40*i onwards; only stage 0 used to be decoded, which was
 * enough for menus drawn one texture at a time and not for a 3D frame that
 * composites its scene through a second stage. */
static void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:
        /* A DMA-object offset like a surface's: physical, reached through
         * the contiguous window when it names contiguous memory. */
        t->offset = dma_resolve(param);
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->levels = (param >> 16) & 0xF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V. It has to: IMAGE_RECT describes a linear image, and
         * a title that only uses swizzled textures never sends one. */
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:
        /* The low six bits carry the DMA context and the entry count. */
        t->palette = dma_resolve(param & ~0x3Fu);
        break;
    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 clamp-to-edge; mirror and border
         * fall back to clamp, wrong at an edge rather than everywhere. */
        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;          /* linear formats only */
        break;
    case 0x1B0C:                                  /* SET_TEXTURE_CONTROL0 */
        t->ctl0 = param;
        t->ctl0_seen = 1;
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:                                  /* SET_TEXTURE_FILTER */
        t->filter = param;
        break;
    case 0x1B28: case 0x1B2C: case 0x1B30: case 0x1B34:  /* SET_BUMP_ENV_MAT */
        memcpy(&s_gpu.rc.bump_mat[stage][(reg - 0x1B28) / 4], &param, 4);
        break;
    case 0x1B38:                                  /* SET_BUMP_ENV_SCALE */
        memcpy(&s_gpu.rc.bump_ls[stage][0], &param, 4);
        break;
    case 0x1B3C:                                  /* SET_BUMP_ENV_OFFSET */
        memcpy(&s_gpu.rc.bump_ls[stage][1], &param, 4);
        break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

/* Frame cap. On the console a swap waits for the next vertical blank, so a
 * frame takes at least 1/60 s; here the flip completes at once, and a title
 * whose game logic runs once per frame -- JSRF's does -- runs fast as soon
 * as the renderer can exceed 60. RECOMP_FPS_CAP=<n> sets the cap (default
 * 60), 0 removes it. Sleeps for most of the wait and spins the last
 * millisecond, with the system timer at 1 ms so the sleep is accurate. */
volatile uint64_t g_pb_exec_ticks;      /* nv2a_pb_run, timed by the ack thread */
static uint64_t s_perf_present, s_perf_pace;

/* v68: how even the frames are. An average of 60 fps says nothing about a
 * stutter; the longest gap between flips and how many missed a 60 Hz slot
 * does. Each gap also records how much of it the executor was busy, which
 * says whether the GPU emulation or the title's own thread was late. */
static uint64_t s_fi_last, s_fi_ex_last;
volatile uint64_t g_last_flip_qpc;   /* for RECOMP_PROFILE (jsrf main.c) */
static double   s_fi_worst_ms, s_fi_worst_busy_ms;
/* v79: what the worst frame contained. */
static uint64_t s_fi_l[5];
static double   s_fi_w_compiles, s_fi_w_compile_ms, s_fi_w_texups, s_fi_w_texup_ms, s_fi_w_wait_ms;
static unsigned s_fi_over20, s_fi_over34, s_fi_over100;

static void flip_interval_note(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    if (s_fi_last) {
        double ms = (double)((uint64_t)now.QuadPart - s_fi_last) * 1000.0 / freq.QuadPart;
        double busy = (double)(g_pb_exec_ticks - s_fi_ex_last) * 1000.0 / freq.QuadPart;
        if (ms > 20.0) s_fi_over20++;
        if (ms > 34.0) s_fi_over34++;
        if (ms > 100.0) s_fi_over100++;
        if (ms > s_fi_worst_ms) {
            uint64_t c[5], f = 1;
            s_fi_worst_ms = ms; s_fi_worst_busy_ms = busy;
            if (nv2a_gl_active()) {
                nv2a_gl_frame_stats(&c[0], &c[1], &c[2], &c[3], &c[4], &f);
                s_fi_w_compiles = (double)(c[0] - s_fi_l[0]);
                s_fi_w_compile_ms = (double)(c[1] - s_fi_l[1]) * 1000.0 / (double)f;
                s_fi_w_texups = (double)(c[2] - s_fi_l[2]);
                s_fi_w_texup_ms = (double)(c[3] - s_fi_l[3]) * 1000.0 / (double)f;
                s_fi_w_wait_ms = (double)(c[4] - s_fi_l[4]) * 1000.0 / (double)f;
            }
        }
    }
    if (nv2a_gl_active()) {
        uint64_t f;
        nv2a_gl_frame_stats(&s_fi_l[0], &s_fi_l[1], &s_fi_l[2], &s_fi_l[3], &s_fi_l[4], &f);
    }
    s_fi_last = (uint64_t)now.QuadPart;
    s_fi_ex_last = g_pb_exec_ticks;
    g_last_flip_qpc = (uint64_t)now.QuadPart;
}

static void flip_pace_inner(void);
static void flip_pace(void)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    flip_pace_inner();
    QueryPerformanceCounter(&b);
    s_perf_pace += (uint64_t)(b.QuadPart - a.QuadPart);
}

static void flip_pace_inner(void)
{
    static int cap = -1;
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    long long period;

    if (cap < 0) {
        const char *e = getenv("RECOMP_FPS_CAP");
        cap = e ? atoi(e) : 60;
        QueryPerformanceFrequency(&freq);
#if defined(_WIN32)
        {
            typedef unsigned (__stdcall *tbp_t)(unsigned);
            HMODULE w = LoadLibraryA("winmm.dll");
            tbp_t tbp = w ? (tbp_t)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
            if (tbp)
                tbp(1);
        }
#endif
        if (cap > 0)
            fprintf(stderr, "[GPU] frame rate capped at %d (RECOMP_FPS_CAP)\n", cap);
    }
    if (cap <= 0 || !freq.QuadPart)
        return;
    period = freq.QuadPart / cap;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart > next.QuadPart + 2 * period) {
        /* First frame, or far behind (a load, a stall): start over rather
         * than racing to catch up. */
        next.QuadPart = now.QuadPart + period;
        return;
    }
    while (now.QuadPart < next.QuadPart) {
        long long left_ms = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
#if defined(_WIN32)
        if (left_ms >= 2)
            Sleep(1);
        else
            SwitchToThread();
#else
        if (left_ms >= 2) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
#endif
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += period;
}

/* RECOMP_MAX_INDICES=<n>: the per-batch index limit, for bisecting (the
 * limit before v43 was 4096). */
static uint32_t s_max_indices(void)
{
    static uint32_t v;
    if (!v) {
        const char *e = getenv("RECOMP_MAX_INDICES");
        v = e ? (uint32_t)atoi(e) : NV_MAX_INDICES;
        if (v < 3 || v > NV_MAX_INDICES) v = NV_MAX_INDICES;
    }
    return v;
}

/* x1 | x2 << 16 and y1 | y2 << 16, inclusive (xemu pgraph CLEAR_SURFACE). */
static uint32_t s_clear_rect[2];
static int s_clear_rect_set;

static void (*s_notify_hook)(uint32_t);
void xbox_Nv2aSetNotifyHook(void (*fn)(uint32_t)) { s_notify_hook = fn; }

/* v145: with RECOMP_PROFILE, executor time per method (rdtsc, cheap),
 * reported as the costliest methods in [PERF]; the draw itself shows under
 * SET_BEGIN_END (0x17FC). */
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
static uint64_t s_mt_ticks[2048], s_mt_count[2048];
static int s_mt_on = -1;
static void exec_method_body(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (s_mt_on < 0)
        s_mt_on = getenv("RECOMP_PROFILE") != NULL;
    if (s_mt_on) {
        uint64_t t0 = __rdtsc();
        uint32_t k = (method >> 2) & 2047u;
        exec_method_body(subch, method, param);
        s_mt_ticks[k] += __rdtsc() - t0;
        s_mt_count[k]++;
        return;
    }
    exec_method_body(subch, method, param);
}

/* the methods that took the most executor time since the last call */
static void method_time_report(double ms_per_tick)
{
    static uint64_t l_t[2048], l_c[2048];
    int top[12], nt = 0, i, j;
    double total = 0.0;
    for (i = 0; i < 2048; i++) {
        uint64_t d = s_mt_ticks[i] - l_t[i];
        total += (double)d;
        for (j = nt; j > 0 && (s_mt_ticks[top[j - 1]] - l_t[top[j - 1]]) < d; j--)
            if (j < 12) top[j] = top[j - 1];
        if (j < 12) { top[j] = i; if (nt < 12) nt++; }
    }
    fprintf(stderr, "[PERF]   methods %.0f ms in all; costliest:", total * ms_per_tick);
    for (i = 0; i < nt; i++) {
        int m = top[i];
        uint64_t d = s_mt_ticks[m] - l_t[m], c = s_mt_count[m] - l_c[m];
        if (!d) break;
        fprintf(stderr, " %04X %.0f ms/%llu", m << 2, (double)d * ms_per_tick, (unsigned long long)c);
    }
    fputc(10, stderr);
    memcpy(l_t, s_mt_ticks, sizeof l_t);
    memcpy(l_c, s_mt_count, sizeof l_c);
    {
        /* v150: of SET_BEGIN_END's time, the draws themselves and the part
         * the [PERF] "rasterising" figure covers */
        static uint64_t l_dp, l_dr, l_dn;
        fprintf(stderr, "[PERF]   draws: %.0f ms in %llu (%.2f us each), %.0f ms of it in raster_batch\n",
                (double)(s_dp_ticks - l_dp) * ms_per_tick, (unsigned long long)(s_dp_n - l_dn),
                s_dp_n > l_dn ? (double)(s_dp_ticks - l_dp) * ms_per_tick * 1000.0 / (double)(s_dp_n - l_dn) : 0.0,
                (double)(s_dp_raster - l_dr) * ms_per_tick);
        l_dp = s_dp_ticks; l_dr = s_dp_raster; l_dn = s_dp_n;
    }
}

/* v150: what the timing calls themselves cost here (RECOMP_PROFILE, once):
 * the executor reads the clock several times a draw, and under Wine the
 * clock can be a system call */
static void clock_costs(void)
{
    LARGE_INTEGER a, b, q;
    uint64_t t0, t1, t2, t3, t4;
    volatile uint32_t sink = 0;
    int i;
    const int N = 20000;
    QueryPerformanceCounter(&a);
    t0 = __rdtsc();
    for (i = 0; i < N; i++) { QueryPerformanceCounter(&q); sink += (uint32_t)q.QuadPart; }
    t1 = __rdtsc();
    for (i = 0; i < N; i++) sink += (uint32_t)GetCurrentThreadId();
    t2 = __rdtsc();
    for (i = 0; i < N; i++) sink += (uint32_t)__rdtsc();
    t3 = __rdtsc();
    {   /* ~20 ms more, to calibrate rdtsc against the timer */
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        do QueryPerformanceCounter(&c); while (c.QuadPart - a.QuadPart < f.QuadPart / 50);
    }
    t4 = __rdtsc();
    QueryPerformanceCounter(&b);
    {
        LARGE_INTEGER f;
        double ns, ms;
        QueryPerformanceFrequency(&f);
        ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        ns = ms * 1e6 / (double)(t4 - t0);       /* ns per rdtsc tick */
        fprintf(stderr, "[PERF] clock costs: QueryPerformanceCounter %.0f ns, GetCurrentThreadId %.0f ns,"
                " rdtsc %.0f ns (timer frequency %lld)\n",
                (double)(t1 - t0) * ns / N, (double)(t2 - t1) * ns / N, (double)(t3 - t2) * ns / N,
                (long long)f.QuadPart);
    }
    (void)sink;
}

static int s_exec_inited;

/* v146: a run of ARRAY_ELEMENT16 words (two 16-bit indices each) in one
 * call, from the pushbuffer walk: 39 million of these words in 10 s in a
 * busy area on the Deck, each one a full method dispatch before. Same
 * effect as the method, word by word. Returns 0 when the walk should take
 * the ordinary path (the executor not set up yet, a frame being traced). */
int nv2a_pb_exec_indices16(const uint32_t *p, uint32_t n)
{
    uint64_t t0;
    uint32_t i, room, c;
    if (!s_exec_inited || s_ftrace == 2)
        return 0;
    t0 = s_mt_on > 0 ? __rdtsc() : 0;
    if (s_gpu.prim) {
        room = s_max_indices();
        c = s_gpu.idx_count;
        for (i = 0; i < n; i++) {
            uint32_t w = p[i];
            if (c + 2 > NV_MAX_INDICES) {
                static int said;
                if (!said++)
                    fprintf(stderr, "[GPU] a batch has more than %d indices; the rest"
                            " are dropped\n", NV_MAX_INDICES);
            }
            if (c + 2 > room)
                break;
            s_gpu.idx[c++] = (uint16_t)(w & 0xFFFF);
            s_gpu.idx[c++] = (uint16_t)(w >> 16);
        }
        s_gpu.idx_count = c;
    }
    if (s_mt_on > 0) {
        s_mt_ticks[0x1800 >> 2] += __rdtsc() - t0;
        s_mt_count[0x1800 >> 2] += n;
    }
    return 1;
}

static void exec_method_body(uint32_t subch, uint32_t method, uint32_t param)
{
    if (!s_exec_inited) {
        s_exec_inited = 1;
        if (getenv("RECOMP_PROFILE"))
            clock_costs();
        s_gpu.color_mask = 0x01010101u;           /* all channels, as reset */
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Cached: this runs for every method, and an
     * uncached getenv here was a quarter of the executor's time. */
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {
        /* RECOMP_FRAME_TRACE_METHODS: every method of the traced frame,
         * less the bulk (vertex data, program and constant uploads). */
        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && !(method >= 0x1800 && method < 0x1A00)
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    if (subch == 0 && method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return;
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case 0x02B4:                           /* SET_WINDOW_CLIP_TYPE (v186d) */
        s_gpu.wclip_type = param;
        break;
    case 0x02C0:                           /* SET_WINDOW_CLIP_HORIZONTAL(0) */
        s_gpu.wclip_x0 = param & 0xFFF;
        s_gpu.wclip_x1 = (param >> 16) & 0xFFF;
        s_gpu.wclip_set = 1;
        break;
    case 0x02E0:                           /* SET_WINDOW_CLIP_VERTICAL(0) */
        s_gpu.wclip_y0 = param & 0xFFF;
        s_gpu.wclip_y1 = (param >> 16) & 0xFFF;
        s_gpu.wclip_set = 1;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:                                  /* SET_COLOR_MASK */
        s_gpu.color_mask = param;
        break;

    /* Register combiners. */
    case 0x0288: s_gpu.rc.final0 = param; break;  /* SPECULAR_FOG_CW0 */
    case 0x028C: s_gpu.rc.final1 = param; break;  /* SPECULAR_FOG_CW1 */
    case 0x1E20: s_gpu.rc.final_c0 = param; break;/* SPECULAR_FOG_FACTOR */
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:                                  /* COMBINER_CONTROL */
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x1E78: s_gpu.rc.other_input = param; break;   /* v104: OTHER_STAGE_INPUT */
    case 0x17F8: s_gpu.clip_plane_mode = param; break;

    /* Alpha test and fog. */
    case 0x0308: s_gpu.cull_enable = param; break;  /* CULL_FACE_ENABLE */
    case 0x0338: s_gpu.poly_offset_fill = param; break; /* POLY_OFFSET_FILL_ENABLE */
    case 0x0384: memcpy(&s_gpu.poly_offset_factor, &param, 4); break;
    case 0x0388: memcpy(&s_gpu.poly_offset_units, &param, 4); break;
    case 0x032C: s_gpu.stencil_test = param; break; /* STENCIL_TEST_ENABLE */
    case 0x0360: s_gpu.stencil_wmask = param; break;/* STENCIL_MASK */
    case 0x0364: s_gpu.stencil_func = param; break; /* STENCIL_FUNC */
    case 0x0368: s_gpu.stencil_ref = param; break;  /* STENCIL_FUNC_REF */
    case 0x036C: s_gpu.stencil_mask = param; break; /* STENCIL_FUNC_MASK */
    case 0x0370: s_gpu.stencil_fail = param; break; /* STENCIL_OP_FAIL */
    case 0x0374: s_gpu.stencil_zfail = param; break;/* STENCIL_OP_ZFAIL */
    case 0x0378: s_gpu.stencil_zpass = param; break;/* STENCIL_OP_ZPASS */
    case 0x039C: s_gpu.cull_face = param; break;    /* CULL_FACE */
    case 0x03A0: s_gpu.front_face = param; break;   /* FRONT_FACE */
    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;  /* R in bits 0-7 */
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x1D6C:                                  /* SET_SEMAPHORE_OFFSET */
        s_sem_offset = param;
        break;
    case 0x1D70:                                  /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        s_sem_last = param;
        s_sem_pending = 1;
        if (s_sem_va)
            sem_write(param);
        break;
    case 0x17C8:                                  /* CLEAR_REPORT_VALUE */
        s_gpu.zpass_count = 0;
        if (nv2a_gl_active())
            nv2a_gl_vis_clear();                  /* v123 */
        break;
    case 0x17CC:                                  /* SET_ZPASS_PIXEL_COUNT_ENABLE */
        s_gpu.zpass_enable = param;
        if (nv2a_gl_active())
            nv2a_gl_vis_enable(param != 0);       /* v123 */
        break;
    case 0x17D0: {                                /* GET_REPORT */
        /* The GPU's answer to a visibility test: 16 bytes at the report
         * DMA offset -- a timestamp, the pixel count, and 0 for "done".
         * Never written, a title polling it (D3D's GetVisibilityTestResult
         * starts the slot at 0xFFFFFFFF) sees every test incomplete. Burnout
         * 3 culls its world with these, so the race's main view drew only
         * the sky while the cube-map passes, which do not test, drew it all.
         * The report DMA object starts at physical 0 here, as D3D sets it. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t at = dma_resolve(param & 0x00FFFFFFu);
        uint64_t stamp = (uint64_t)s_gpu.reports++ * 1000u;
        uint32_t cnt = nv2a_gl_active() ? nv2a_gl_vis_count() : s_gpu.zpass_count;
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] visibility test report: %u pixels\n", cnt);
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &cnt, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case 0x1D98:                                  /* SET_CLEAR_RECT_HORIZONTAL */
        s_clear_rect[0] = param; s_clear_rect_set = 1;
        break;
    case 0x1D9C:                                  /* SET_CLEAR_RECT_VERTICAL */
        s_clear_rect[1] = param; s_clear_rect_set = 1;
        break;
    case NV097_CLEAR_SURFACE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, 10);
        if (nv2a_gl_active()) {
            NvGlState st;
            uint32_t argb = s_gpu.clear_color;
            if (surface_bpp() == 2 && !(argb & 0xFFFF0000u)) {
                /* A packed 5:6:5 word, as clear_surface explains. */
                uint32_t r = (argb >> 11) & 31, g = (argb >> 5) & 63, b = argb & 31;
                argb = 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                     | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
            if (gl_state(&st)) {
                if (s_clear_rect_set)
                    nv2a_gl_clear_rect(&st, param, argb, zclear_value(),
                                       s_gpu.zstencil_clear & 0xFF,
                                       (int)(s_clear_rect[0] & 0xFFFF), (int)(s_clear_rect[0] >> 16),
                                       (int)(s_clear_rect[1] & 0xFFFF), (int)(s_clear_rect[1] >> 16));
                else
                    nv2a_gl_clear(&st, param, argb, zclear_value(),
                                  s_gpu.zstencil_clear & 0xFF);
            }
            s_gpu.clears++;
            break;
        }
        clear_surface(param);
        if (param & 0x03)                         /* Z, stencil */
            zbuf_clear(param & 0x03);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < s_max_indices(); i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        break;
    }

    case 0x1808:                                  /* ARRAY_ELEMENT32 */
        /* One index per word. D3D packs indices two to a word with
         * ARRAY_ELEMENT16 and sends the odd one out with this, so without it
         * every batch with an odd index count lost its last index -- the last
         * triangle of a list, of a strip, of a fan: 83,000 of them in one
         * JSRF run, the missing triangles and the flicker. */
        {
            static int off = -1;
            if (off < 0) off = getenv("RECOMP_NO_ELEMENT32") != NULL;
            if (off) break;
        }
        if (s_gpu.prim && s_gpu.idx_count < s_max_indices())
            s_gpu.idx[s_gpu.idx_count++] = param & 0xFFFFu;   /* Xbox D3D indices are 16-bit; the high half is not an index */
        break;

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 > NV_MAX_INDICES) {
            static int said;
            if (!said++)
                fprintf(stderr, "[GPU] a batch has more than %d indices; the rest"
                        " are dropped\n", NV_MAX_INDICES);
        }
        if (s_gpu.prim && s_gpu.idx_count + 2 <= s_max_indices()) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return;

    case 0x0100:                                  /* NO_OPERATION */
        /* With a non-zero argument the NV2A raises a software-method
         * interrupt here, and D3D uses that as its "GPU reached time T"
         * notification: BlockOnTime patches NO_OPERATION(5) into the fence
         * it is waiting for and sleeps on an event its ISR sets. Nothing
         * raised it, so the first time the GPU fell far enough behind for
         * D3D to sleep rather than spin, the title slept forever (run 48,
         * in D3DVertexBuffer_Lock). The title registers what its ISR would
         * have done (xbox_Nv2aSetNotifyHook). */
        if (param && s_notify_hook) {
            static unsigned n;
            if (n++ < 4)
                fprintf(stderr, "[GPU] NO_OPERATION(%u): notifying the title\n", param);
            s_notify_hook(param);
        }
        return;

    case NV097_FLIP_STALL:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Hand the window a copy of the frame just finished.
         *
         * The buffer the title has finished is the one the last batch drew
         * into, which is what drawn_offset holds and why it exists: by the
         * flip, color_offset has already moved to the next buffer. Copying
         * here, rather than letting the window read guest memory on its own
         * clock, is what stops it showing a surface the rasteriser is still
         * writing. */
        LARGE_INTEGER pr0, pr1;
        /* v90: wait for the frame's 60 Hz slot BEFORE showing it, not after.
         * Showing it the moment it was finished and pacing afterwards meant
         * every frame reached the screen whenever its own work happened to
         * end -- 8 ms after the last one, then 25 -- and on a 240 Hz display
         * that unevenness is what you see (run 89: ~100 presents in 10 s
         * more than 20 ms apart while the GPU kept up). Now each frame is
         * held to its slot, so they arrive 16.7 ms apart; the work for the
         * next frame was already queued, so nothing is lost.
         * RECOMP_PACE_AFTER=1 restores the old order. */
        static int pace_after = -1;
        if (pace_after < 0)
            pace_after = getenv("RECOMP_PACE_AFTER") != NULL;
        if (!pace_after)
            flip_pace();
        QueryPerformanceCounter(&pr0);
        {
            extern void xbox_FramebufferWindowFrameStats(uint32_t);
            static uint32_t draws_at_flip;
            xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
            draws_at_flip = s_gpu.draws;
        }
        {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            static uint32_t last_pitch;
            uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                               : s_gpu.color_offset;
            uint32_t pitch = s_gpu.drawn_offset ? s_gpu.drawn_pitch
                                                : s_gpu.pitch;
            uint32_t show = done ? dma_resolve(done) : 0, show_pitch = pitch;
            if (nv2a_gl_active()) {
                /* Asynchronous: what is shown is the frame whose read-back
                 * the previous flip started (nv2a_gl_present). */
                uint32_t got = show ? nv2a_gl_present(show) : 0;
                if (got && got != show)
                    show_pitch = last_pitch ? last_pitch : pitch;
                show = got;
                nv2a_gl_end_frame();
            }
            if (s_gpu.pitch && show) {
                xbox_FramebufferWindowSet(show, show_pitch);
                xbox_FramebufferWindowPresent(show, show_pitch);
            }
            last_pitch = pitch;
        }
        {
            /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
             * report -- a consecutive sequence, which is what a headless
             * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
             * other value names a file, and frames are dumped while it
             * exists, so a capture can start mid-game without writing
             * gigabytes of menus first. */
            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
                if (on)
                    dump_surface_bmp();
            }
        }
        s_gpu.drawn_stale = 1;
        QueryPerformanceCounter(&pr1);
        s_perf_present += (uint64_t)(pr1.QuadPart - pr0.QuadPart);
        if (pace_after)
            flip_pace();
        flip_interval_note();

        if (pb_verbose()) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;


    /* Vertex programs and their constants: forwarded as they arrive. */
    case 0x1E94:                                  /* TRANSFORM_EXECUTION_MODE */
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:                                  /* _PROGRAM_CXT_WRITE_EN */
        nv2a_vsh_set_cxt_write(param);
        break;
    case 0x1E9C:                                  /* _PROGRAM_LOAD */
        nv2a_vsh_set_load_slot(param);
        break;
    case 0x1EA0:                                  /* _PROGRAM_START */
        nv2a_vsh_set_start_slot(param);
        break;
    case 0x1EA4:                                  /* _CONSTANT_LOAD */
        nv2a_vsh_set_constant_load(param);
        break;

    /* Depth. */
    case 0x030C: s_gpu.depth_test = param; break; /* DEPTH_TEST_ENABLE */
    case 0x0354: s_gpu.depth_func = param; break; /* DEPTH_FUNC */
    case 0x035C: s_gpu.depth_mask = param; break; /* DEPTH_MASK */
    case 0x0214: s_gpu.zeta_offset = param; break;/* SURFACE_ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; break; /* ZSTENCIL_CLEAR_VALUE */

    default:
        if (method >= 0x0260 && method < 0x0280) {         /* ALPHA_ICW(i) */
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {         /* COLOR_ICW(i) */
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {         /* COLOR_OCW(i) */
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {         /* ALPHA_OCW(i) */
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {         /* FACTOR0(i) */
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {         /* FACTOR1(i) */
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }
        /* SET_VIEWPORT_OFFSET / _SCALE. The GPU keeps these in the constant
         * file, at c[59] and c[58] -- exactly where the D3D epilogue reads
         * them -- so they are constants that happen to arrive by method.
         * Dropped, every vertex program put its whole batch at the origin:
         * no 3D anywhere, and a black car on the garage screen. */
        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }
        if (method >= 0x0480 && method < 0x0580) {    /* MODEL_VIEW_MATRIX0..3 */
            uint32_t k = (method - 0x0480) / 4;
            memcpy(&s_gpu.ff_mv[k >> 4][k & 15], &param, 4);
            break;
        }
        if (method >= 0x0580 && method < 0x05C0) {    /* INVERSE_MODEL_VIEW_MATRIX0 */
            memcpy(&s_gpu.ff_inv_mv0[(method - 0x0580) / 4], &param, 4);
            break;
        }
        if (method >= 0x06C0 && method < 0x07C0) {    /* TEXTURE_MATRIX0..3 */
            uint32_t k = (method - 0x06C0) / 4;
            memcpy(&s_gpu.ff_texmat[k >> 4][k & 15], &param, 4);
            break;
        }
        if (method >= 0x03C0 && method < 0x0400) {    /* TEXGEN_S/T/R/Q per stage */
            uint32_t k = (method - 0x03C0) / 4;
            s_gpu.ff_texgen[k >> 2][k & 3] = param;
            break;
        }
        if (method >= 0x0420 && method < 0x0430) {    /* TEXTURE_MATRIX_ENABLE */
            s_gpu.ff_texmat_on[(method - 0x0420) / 4] = param;
            break;
        }
        if (method >= 0x0840 && method < 0x0940) {    /* TEXGEN_SPLANE.. per stage */
            uint32_t k = (method - 0x0840) / 4;
            memcpy(&s_gpu.ff_texplane[k >> 4][(k >> 2) & 3][k & 3], &param, 4);
            break;
        }
        if (method >= 0x0680 && method < 0x06C0) {    /* COMPOSITE_MATRIX */
            memcpy(&s_gpu.ff_composite[(method - 0x0680) / 4], &param, 4);
            s_gpu.ff_composite_set = 1;
            break;
        }
        if (method == 0x0328) { s_gpu.ff_skin_mode = param; break; }  /* SKIN_MODE */
        if (method == 0x0314) { s_gpu.ff_lighting = param; break; }   /* LIGHTING_ENABLE */
        if (ff_light_method(method, param))
            break;
        if (method >= 0x0B00 && method < 0x0B80) {    /* TRANSFORM_PROGRAM(i) */
            nv2a_vsh_program_word(param);
            break;
        }
        if (method >= 0x0B80 && method < 0x0C00) {    /* TRANSFORM_CONSTANT(i) */
            nv2a_vsh_constant_word(param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {
        /* Frames per second of guest time, from flips between reports. */
        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        last_flips = s_gpu.flips;
        last_ms = now;
    }
    {
        /* RECOMP perf: share of wall time spent rasterising since the last
         * report, and on which thread. If this is near 100% the software
         * GPU is the frame rate; if it is low, the title's own code is. */
        static LARGE_INTEGER last, freq;
        static uint64_t last_raster, last_px, last_tris, last_v, last_p, last_h, last_r;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        if (last.QuadPart && now.QuadPart > last.QuadPart) {
            double wall = (double)(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            double ras = (double)(s_perf_raster - last_raster) * 1000.0 / freq.QuadPart;
            fprintf(stderr, "[PERF] %.0f ms wall: %.0f ms rasterising (%.0f%%) on thread %lu"
                    " (%u thread switches), %llu triangles, %llu pixels, %d raster threads\n",
                    wall, ras, wall > 0 ? ras * 100.0 / wall : 0.0,
                    (unsigned long)s_perf_tid, s_perf_thread_changes,
                    (unsigned long long)(s_gpu.tris_drawn - last_tris),
                    (unsigned long long)(s_gpu.xf_pixels - last_px),
                    nv2a_gl_active() ? 0 : raster_pool_size());
        }
        if (last.QuadPart && now.QuadPart > last.QuadPart) {
            double f = 1000.0 / (double)freq.QuadPart;
            uint64_t h = s_perf_vhits - last_h, r = s_perf_vruns - last_r;
            static uint64_t last_gp;
            static uint64_t last_nat, last_natb;
            fprintf(stderr, "[PERF]   of which: vertices %.0f ms (%llu run, %llu from cache,"
                    " %llu copied as stored in %llu batches),"
                    " batch setup (indices, state) %.0f ms,"
                    " pixels %.0f ms, the rest (incl. GL draws) %.0f ms\n",
                    (double)(s_perf_vertex - last_v) * f, (unsigned long long)r,
                    (unsigned long long)h, (unsigned long long)(s_perf_vnat - last_nat),
                    (unsigned long long)(s_perf_vnat_b - last_natb),
                    (double)(s_perf_glprep - last_gp) * f,
                    (double)(s_perf_pixel - last_p) * f,
                    (double)((s_perf_raster - last_raster) - (s_perf_vertex - last_v)
                             - (s_perf_pixel - last_p) - (s_perf_glprep - last_gp)) * f);
            last_gp = s_perf_glprep;
            last_nat = s_perf_vnat; last_natb = s_perf_vnat_b;
            {
                static uint64_t l_why[24];
                int w, any = 0;
                for (w = 0; w < 24; w++) any |= s_nat_why[w] != l_why[w];
                if (any) {
                    fprintf(stderr, "[PERF]   batches not copied as stored: %llu with a constant input,"
                            " %llu inline, %llu spread out, %llu too big;",
                            (unsigned long long)(s_nat_why[0] - l_why[0]),
                            (unsigned long long)(s_nat_why[1] - l_why[1]),
                            (unsigned long long)(s_nat_why[2] - l_why[2]),
                            (unsigned long long)(s_nat_why[3] - l_why[3]));
                    for (w = 8; w < 24; w++)
                        if (s_nat_why[w] != l_why[w])
                            fprintf(stderr, " %llu with a type-%d input",
                                    (unsigned long long)(s_nat_why[w] - l_why[w]), w - 8);
                    fputc(10, stderr);
                }
                memcpy(l_why, s_nat_why, sizeof l_why);
            }
        }
        if (last.QuadPart && now.QuadPart > last.QuadPart) {
            /* The executor thread as a whole. Busy near 100% (less the frame
             * cap's wait) means emulating the GPU is what limits the frame
             * rate; well below means the title's own code does. */
            static uint64_t l_ex, l_pr, l_pc, l_gl[4];
            double f = 1000.0 / (double)freq.QuadPart;
            double wall = (double)(now.QuadPart - last.QuadPart) * f;
            double ex = (double)(g_pb_exec_ticks - l_ex) * f;
            double pc = (double)(s_perf_pace - l_pc) * f;
            double pr = (double)(s_perf_present - l_pr) * f;
            uint64_t g[4], gf = 0;
            nv2a_gl_times(g, &gf);
            fprintf(stderr, "[PERF]   executor busy %.0f ms (%.0f%%) less %.0f ms frame-cap"
                    " wait; present %.0f ms", ex - pc, wall > 0 ? (ex - pc) * 100.0 / wall : 0.0,
                    pc, pr);
            if (gf)
                fprintf(stderr, "; GL draws %.0f ms, readbacks %.0f ms, texture checks"
                        " %.0f ms, texture uploads %.0f ms",
                        (double)(g[0] - l_gl[0]) * 1000.0 / gf,
                        (double)(g[1] - l_gl[1]) * 1000.0 / gf,
                        (double)(g[2] - l_gl[2]) * 1000.0 / gf,
                        (double)(g[3] - l_gl[3]) * 1000.0 / gf);
            fputc(10, stderr);
            fprintf(stderr, "[PERF]   frames: longest %.1f ms (executor busy %.1f ms of it;"
                    " %.0f shader compiles %.1f ms, %.0f texture uploads %.1f ms, GPU wait %.1f ms),"
                    " %u over 20 ms, %u over 34 ms, %u over 100 ms\n",
                    s_fi_worst_ms, s_fi_worst_busy_ms, s_fi_w_compiles, s_fi_w_compile_ms,
                    s_fi_w_texups, s_fi_w_texup_ms, s_fi_w_wait_ms,
                    s_fi_over20, s_fi_over34, s_fi_over100);
            s_fi_worst_ms = s_fi_worst_busy_ms = 0;
            s_fi_over20 = s_fi_over34 = s_fi_over100 = 0;
            if (s_mt_on > 0) {
                /* rdtsc against the performance counter over this interval */
                static uint64_t l_tsc;
                static LARGE_INTEGER l_qpc;
                LARGE_INTEGER q, fq;
                uint64_t tsc = __rdtsc();
                QueryPerformanceCounter(&q);
                QueryPerformanceFrequency(&fq);
                if (l_tsc && q.QuadPart > l_qpc.QuadPart && tsc > l_tsc) {
                    double ms = (double)(q.QuadPart - l_qpc.QuadPart) * 1000.0 / (double)fq.QuadPart;
                    method_time_report(ms / (double)(tsc - l_tsc));
                }
                l_tsc = tsc; l_qpc = q;
            }
            l_ex = g_pb_exec_ticks; l_pc = s_perf_pace; l_pr = s_perf_present;
            memcpy(l_gl, g, sizeof g);
        }
        if (nv2a_gl_active()) {
            static uint64_t l_d, l_t, l_u, l_r, l_x;
            uint64_t d, t, u, r, x;
            nv2a_gl_stats(&d, &t, &u, &r, &x);
            static uint64_t l_c, l_p, l_w, l_z;
            uint64_t c, pr, cw, z;
            nv2a_gl_stats2(&c, &pr, &cw);
            nv2a_gl_stats3(&z);
            fprintf(stderr, "[PERF]   GPU: %llu draws, %llu triangles, %llu surface uploads,"
                    " %llu readbacks, %llu texture uploads (%llu as DXT), %llu render-target copies on the"
                    " GPU, %llu frames read for the window, %llu CPU-written surfaces\n",
                    (unsigned long long)(d - l_d), (unsigned long long)(t - l_t),
                    (unsigned long long)(u - l_u), (unsigned long long)(r - l_r),
                    (unsigned long long)(x - l_x), (unsigned long long)(z - l_z),
                    (unsigned long long)(c - l_c),
                    (unsigned long long)(pr - l_p), (unsigned long long)(cw - l_w));
            l_d = d; l_t = t; l_u = u; l_r = r; l_x = x; l_c = c; l_p = pr; l_w = cw; l_z = z;
            {
                uint64_t wf, bm, b2, w20, w34;
                double ba, im;
                nv2a_gl_window_stats(&wf, &ba, &bm, &b2, &im, &w20, &w34);
                if (wf)
                    fprintf(stderr, "[PERF]   window: %llu frames shown; GPU behind by %.2f"
                            " frames on average (most %llu, %llu times 2+); presents up to"
                            " %.1f ms apart, %llu over 20 ms, %llu over 34 ms\n",
                            (unsigned long long)wf, ba, (unsigned long long)bm,
                            (unsigned long long)b2, im,
                            (unsigned long long)w20, (unsigned long long)w34);
                {
                    char gb[512];
                    if (nv2a_gl_gpu_frame_stats(gb, sizeof gb))
                        fprintf(stderr, "[PERF]   GPU side: %s\n", gb);
                    if (nv2a_gl_submit_stats(gb, sizeof gb))
                        fprintf(stderr, "[PERF]   submit: %s\n", gb);
                    if (nv2a_gl_call_stats(gb, sizeof gb))
                        fprintf(stderr, "[PERF]   GL calls: %s\n", gb);
                }
            }
        }
        last_v = s_perf_vertex; last_p = s_perf_pixel;
        last_h = s_perf_vhits; last_r = s_perf_vruns;
        last = now;
        last_raster = s_perf_raster;
        last_px = s_gpu.xf_pixels;
        last_tris = s_gpu.tris_drawn;
    }
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    {
        /* Once, a few reports in: every unhandled method, not just the top
         * of the table -- a method sent rarely can still be the one a
         * missing effect needs. */
        static int reports;
        if (++reports == 6) {
            int i;
            fprintf(stderr, "[GPU] all %d unhandled methods (method x count, last value):\n",
                    s_unhandled_count);
            for (i = 0; i < s_unhandled_count; i++)
                fprintf(stderr, "  [UNH] %04X x%u last %08X\n", s_unhandled[i].method,
                        s_unhandled[i].count, s_unhandled[i].last_param);
        }
    }
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    if (!s_flip_dumping)
        dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    fprintf(stderr, "[GPU] culling: %u triangles culled (enable %u, face 0x%X,"
                    " front 0x%X)\n", s_gpu.tris_culled, s_gpu.cull_enable,
            s_gpu.cull_face, s_gpu.front_face);
    fprintf(stderr, "[GPU] pre-transformed (XYZRHW): %u batches; immediate-mode batches %u"
                    " (%u with 4-component positions)\n", s_batches_pre, s_imm_batches,
            s_imm_batches4);
    fprintf(stderr, "[GPU] fixed function: %u batches, %u vertices; skin mode %u,"
                    " lighting %u\n", s_gpu.batches_ff, s_gpu.verts_ff,
            s_gpu.ff_skin_mode, s_gpu.ff_lighting);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}
