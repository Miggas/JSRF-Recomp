/* Draws a small scene through the pushbuffer executor and dumps the colour
 * surface. Run with RECOMP_RENDERER=sw and without, then compare. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>

/* ---- stubs for the runtime the executor links against ---- */
static uint8_t *g_mem;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)g_mem; }
uint32_t g_xbox_image_lo = 0x10000, g_xbox_image_hi = 0x20000;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0x01000000; }
void xbox_FramebufferWindowFrameStats(uint32_t d) { (void)d; }
void xbox_FramebufferWindowPresent(uint32_t a, uint32_t b) { (void)a; (void)b; }
void xbox_FramebufferWindowSet(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a;(void)b;(void)c;(void)d; }
void xbox_FramebufferWindowStart(void) {}
void xbox_Nv2aFrameCounterFlip(void) {}
typedef union { struct { uint32_t lo; int32_t hi; } u; long long QuadPart; } LI;
int QueryPerformanceCounter(LI *l) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); l->QuadPart = t.tv_sec * 1000000000ll + t.tv_nsec; return 1; }
int QueryPerformanceFrequency(LI *l) { l->QuadPart = 1000000000ll; return 1; }
unsigned long GetCurrentThreadId(void) { return 1; }

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

static void M(uint32_t m, uint32_t p) { nv2a_pb_exec_method(0, m, p); }
static void MF(uint32_t m, float f) { uint32_t p; memcpy(&p, &f, 4); M(m, p); }

#define SURF_OFF 0x00100000u      /* -> VA 0x80100000 */
#define TEX_OFF  0x00200000u
#define TEX2_OFF 0x00210000u
#define VB_OFF   0x00300000u
#define VA(o) (0x80000000u + (o))

typedef struct { float x, y, z; uint32_t c; float u, v; } Vtx;

static void put_vtx(int i, float x, float y, float z, uint32_t c, float u, float v)
{
    Vtx *vb = (Vtx *)(g_mem + VA(VB_OFF));
    vb[i].x = x; vb[i].y = y; vb[i].z = z; vb[i].c = c; vb[i].u = u; vb[i].v = v;
}

static void draw(int prim, int first, int count)
{
    M(0x17FC, (uint32_t)prim);
    M(0x1810, (uint32_t)first | ((uint32_t)(count - 1) << 24));
    M(0x17FC, 0);
}

int main(int argc, char **argv)
{
    int i;
    const char *out = argc > 1 ? argv[1] : "out.raw";
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap"); return 1; }

    /* Linear 64x64 A8R8G8B8 checker texture, texel coordinates. */
    {
        uint32_t *t = (uint32_t *)(g_mem + VA(TEX_OFF)), x, y;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                t[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFC000u : 0xFF2040FFu;
    }
    /* Swizzled 32x32 A8R8G8B8 gradient (format 0x06), normalised coords. */
    {
        uint32_t lin[32 * 32], x, y;
        extern void test_swizzle(void *dst, const void *src, uint32_t w, uint32_t h);
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++)
                lin[y * 32 + x] = 0xFF000000u | (x * 8) << 16 | (y * 8) << 8 | 0x40;
        test_swizzle(g_mem + VA(TEX2_OFF), lin, 32, 32);
    }

    /* Surface: 640x480 R5G6B5, pitch 1280, Z24S8. */
    M(0x0200, 0u | (640u << 16));
    M(0x0204, 0u | (480u << 16));
    M(0x0208, 0x03u | (0x2u << 4));
    M(0x020C, 1280u | (2560u << 16));
    M(0x0210, SURF_OFF);
    M(0x0214, 0x00500000u);           /* zeta offset */
    M(0x1D90, 0x000020E4u);           /* packed 565 clear */
    M(0x1D8C, 0xFFFFFF00u);           /* depth far, stencil 0 */
    M(0x1D94, 0xF3u);

    /* Fixed function, composite = identity, viewport offset 0. */
    M(0x1E94, 0);
    for (i = 0; i < 16; i++)
        MF(0x0680 + i * 4, (i % 5 == 0) ? 1.0f : 0.0f);
    for (i = 0; i < 4; i++) MF(0x0A20 + i * 4, 0.0f);
    /* Vertex arrays: pos float3, diffuse D3DCOLOR, uv float2; stride 24. */
    M(0x1720 + 0 * 4, VB_OFF);      M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
    M(0x1720 + 3 * 4, VB_OFF + 12); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
    M(0x1720 + 9 * 4, VB_OFF + 16); M(0x1760 + 9 * 4, 2 | (2 << 4) | (24 << 8));
    for (i = 1; i < 16; i++)
        if (i != 3 && i != 9) M(0x1760 + i * 4, 2 | (0 << 4));

    /* Combiners: r0 = t0 * v0, both portions; final combiner off. */
    M(0x0AC0, 0x08040000u); M(0x1E40, 0x000000C0u);
    M(0x0260, 0x18140000u); M(0x0AA0, 0x000000C0u);
    M(0x1E70, 0x00000001u);           /* stage 0: 2D */
    M(0x1E60, 0x00000001u);           /* one stage */

    /* Texture stage 0: linear A8R8G8B8 64x64. */
    M(0x1B00, TEX_OFF);
    M(0x1B04, (0x12u << 8) | (1u << 4));
    M(0x1B08, 0x0101u);               /* wrap u/v */
    M(0x1B10, 256u << 16);            /* pitch */
    M(0x1B1C, (64u << 16) | 64u);
    M(0x1B14, 0x01010000u);           /* nearest */

    M(0x030C, 1); M(0x0354, 0x203); M(0x035C, 1);   /* depth LEQUAL, write */

    /* 1: big textured triangle, depth middle. */
    put_vtx(0, 40, 40, 8000000, 0xFFFFFFFFu, 0, 0);
    put_vtx(1, 600, 60, 8000000, 0xFFFFFFFFu, 128, 0);
    put_vtx(2, 100, 440, 8000000, 0xFFFFFFFFu, 0, 128);
    draw(5, 0, 3);

    /* 2: half-transparent red strip in front, crossing it. */
    M(0x0304, 1); M(0x0344, 0x302); M(0x0348, 0x303); M(0x0350, 0x8006);
    put_vtx(3, 200, 100, 4000000, 0x80FF0000u, 0, 0);
    put_vtx(4, 500, 120, 4000000, 0x80FF0000u, 64, 0);
    put_vtx(5, 220, 300, 4000000, 0x80FF0000u, 0, 64);
    put_vtx(6, 520, 330, 4000000, 0x80FF0000u, 64, 64);
    draw(6, 3, 4);
    M(0x0304, 0);

    /* 3: behind triangle 1 (fails depth). */
    put_vtx(7, 60, 60, 12000000, 0xFF00FF00u, 0, 0);
    put_vtx(8, 300, 60, 12000000, 0xFF00FF00u, 0, 0);
    put_vtx(9, 60, 300, 12000000, 0xFF00FF00u, 0, 0);
    draw(5, 7, 3);

    /* 4: stencil. Write 1 in a quad with colour off, then draw green where
     * stencil == 1 over a larger quad. */
    M(0x0358, 0);                     /* colour mask off */
    M(0x032C, 1); M(0x0360, 0xFF); M(0x0364, 0x207); M(0x0368, 1); M(0x036C, 0xFF);
    M(0x0370, 0x1E00); M(0x0374, 0x1E00); M(0x0378, 0x1E01);
    M(0x030C, 0);
    put_vtx(10, 450, 350, 100, 0xFFFFFFFFu, 0, 0);
    put_vtx(11, 600, 350, 100, 0xFFFFFFFFu, 0, 0);
    put_vtx(12, 600, 460, 100, 0xFFFFFFFFu, 0, 0);
    put_vtx(13, 450, 460, 100, 0xFFFFFFFFu, 0, 0);
    draw(7, 10, 4);
    M(0x0358, 0x01010101u);
    M(0x0364, 0x202);                 /* EQUAL 1 */
    M(0x0378, 0x1E00);
    M(0x1B00, TEX2_OFF);              /* swizzled gradient, normalised */
    M(0x1B04, (0x06u << 8) | (1u << 4) | (5u << 20) | (5u << 24));
    M(0x1B14, 0x02020000u);           /* bilinear */
    put_vtx(14, 400, 300, 100, 0xFFFFFFFFu, 0, 0);
    put_vtx(15, 639, 300, 100, 0xFFFFFFFFu, 1, 0);
    put_vtx(16, 639, 479, 100, 0xFFFFFFFFu, 1, 1);
    put_vtx(17, 400, 479, 100, 0xFFFFFFFFu, 0, 1);
    draw(8, 14, 4);                   /* QUADS */
    M(0x032C, 0);

    /* 5: culling: two triangles, opposite windings, back faces culled
     * (front = CW). Only one may show. */
    M(0x0308, 1); M(0x039C, 0x405); M(0x03A0, 0x900);
    M(0x1B00, TEX_OFF); M(0x1B04, (0x12u << 8) | (1u << 4)); M(0x1B14, 0x01010000u);
    put_vtx(18, 20, 380, 100, 0xFF00FFFFu, 0, 0);   /* CW on screen */
    put_vtx(19, 120, 380, 100, 0xFF00FFFFu, 0, 0);
    put_vtx(20, 20, 470, 100, 0xFF00FFFFu, 0, 0);
    put_vtx(21, 140, 380, 100, 0xFFFF00FFu, 0, 0);  /* CCW on screen */
    put_vtx(22, 140, 470, 100, 0xFFFF00FFu, 0, 0);
    put_vtx(23, 240, 380, 100, 0xFFFF00FFu, 0, 0);
    draw(5, 18, 6);
    M(0x0308, 0);

    /* 6: render to a 128x128 A8R8G8B8 target, then sample it. */
    M(0x0200, 0u | (128u << 16)); M(0x0204, 0u | (128u << 16));
    M(0x0208, 0x08u | (0x2u << 4)); M(0x020C, 512u | (512u << 16));
    M(0x0210, 0x00400000u); M(0x0214, 0x00600000u);
    M(0x1D90, 0xFFFF0000u); M(0x1D94, 0xF3u);
    M(0x030C, 0);
    M(0x1E70, 0); /* no textures: r0 = t0*v0 with t0 = (0,0,0,1) -> use v0 only */
    M(0x0AC0, 0x04200000u); M(0x0260, 0x14200000u);  /* A = v0, B = 1 (invert of zero) */
    M(0x1E70, 0x00000000u);
    put_vtx(24, 10, 10, 100, 0xFF00FF00u, 0, 0);
    put_vtx(25, 120, 20, 100, 0xFF00FF00u, 0, 0);
    put_vtx(26, 30, 118, 100, 0xFF00FF00u, 0, 0);
    draw(5, 24, 3);
    M(0x0200, 0u | (640u << 16)); M(0x0204, 0u | (480u << 16));
    M(0x0208, 0x03u | (0x2u << 4)); M(0x020C, 1280u | (2560u << 16));
    M(0x0210, SURF_OFF); M(0x0214, 0x00500000u);
    M(0x0AC0, 0x08040000u); M(0x0260, 0x18140000u);
    M(0x1E70, 0x00000001u);
    M(0x1B00, 0x00400000u); M(0x1B04, (0x12u << 8) | (1u << 4)); M(0x1B10, 512u << 16);
    M(0x1B1C, (128u << 16) | 128u); M(0x1B14, 0x01010000u); M(0x1B08, 0x0303u);
    put_vtx(27, 250, 340, 100, 0xFFFFFFFFu, 0, 0);
    put_vtx(28, 378, 340, 100, 0xFFFFFFFFu, 128, 0);
    put_vtx(29, 378, 468, 100, 0xFFFFFFFFu, 128, 128);
    put_vtx(30, 250, 468, 100, 0xFFFFFFFFu, 0, 128);
    draw(8, 27, 4);

    /* 7: pre-transformed (XYZRHW) quad, blended, z in [0,1]. */
    {
        float *q = (float *)(g_mem + VA(0x00320000u));
        float vv[4][7] = {
            {300, 20, 0.1f, 1.0f, 0, 0, 0}, {420, 20, 0.1f, 1.0f, 64, 0, 0},
            {420, 90, 0.1f, 1.0f, 64, 64, 0}, {300, 90, 0.1f, 1.0f, 0, 64, 0}};
        uint32_t col = 0x80FFFFFFu;
        int k;
        for (k = 0; k < 4; k++) {
            memcpy(q + k * 7, vv[k], 4 * 4);
            memcpy(q + k * 7 + 4, &col, 4);
            memcpy(q + k * 7 + 5, &vv[k][4], 8);
        }
        M(0x1720 + 0 * 4, 0x00320000u); M(0x1760 + 0 * 4, 2 | (4 << 4) | (28 << 8));
        M(0x1720 + 3 * 4, 0x00320010u); M(0x1760 + 3 * 4, 0 | (4 << 4) | (28 << 8));
        M(0x1720 + 9 * 4, 0x00320014u); M(0x1760 + 9 * 4, 2 | (2 << 4) | (28 << 8));
        M(0x1B00, TEX_OFF); M(0x1B04, (0x12u << 8) | (1u << 4)); M(0x1B10, 256u << 16);
        M(0x1B1C, (64u << 16) | 64u); M(0x1B14, 0x01010000u); M(0x1B08, 0x0101u);
        M(0x030C, 1);
        M(0x0304, 1);
        draw(8, 0, 4);
        M(0x0304, 0);
    }


    /* 8: a big indexed grid (ARRAY_ELEMENT16, strip-like sharing), fixed
     * function, enough distinct vertices for the vertex threads. */
    {
        int gx, gy, G = 120, k = 0;
        Vtx *vb = (Vtx *)(g_mem + VA(0x00400000u + 0x100000u));
        for (gy = 0; gy <= G; gy++)
            for (gx = 0; gx <= G; gx++) {
                Vtx *v = &vb[gy * (G + 1) + gx];
                v->x = 440 + gx * 1.6f + (gy & 1) * 0.3f; v->y = 150 + gy * 1.4f;
                v->z = 2000000 + gx * 1000.0f;
                v->c = 0xFF000000u | ((gx * 2) << 16) | ((gy * 2) << 8) | 0x80;
                v->u = gx * 0.5f; v->v = gy * 0.5f;
            }
        M(0x1720 + 0 * 4, 0x00500000u); M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 3 * 4, 0x0050000Cu); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
        M(0x1720 + 9 * 4, 0x00500010u); M(0x1760 + 9 * 4, 2 | (2 << 4) | (24 << 8));
        M(0x030C, 1);
        M(0x17FC, 5);
        for (gy = 0; gy < G; gy++)
            for (gx = 0; gx < G; gx++) {
                uint32_t a = gy * (G + 1) + gx, b = a + 1, c = a + G + 1, d = c + 1;
                uint32_t t[6] = {a, b, c, b, d, c};
                int q;
                for (q = 0; q < 6; q += 2)
                    M(0x1800, t[q] | (t[q + 1] << 16));
                k += 6;
            }
        M(0x17FC, 0);
        (void)k;
    }
    /* Flip: the GPU path writes the surface back here. */
    M(0x0130, 0);

    {
        FILE *f = fopen(out, "wb");
        fwrite(g_mem + VA(SURF_OFF), 1, 1280 * 480, f);
        fclose(f);
    }
    return 0;
}

#include "d3d/d3d8_swizzle.h"
void test_swizzle(void *dst, const void *src, uint32_t w, uint32_t h)
{
    xbox_swizzle_rect(dst, src, w, h, 4);
}
