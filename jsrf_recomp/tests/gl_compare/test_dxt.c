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
/* beam_tris.h holds vertices captured from a frame of the game, so it is not
 * in the repository; capture your own to run TEST_BEAM. */
#if defined(__has_include)
#if __has_include("beam_tris.h")
#include "beam_tris.h"
#define HAVE_BEAM_TRIS 1
#endif
#endif

/* ---- stubs for the runtime the executor links against ---- */
static uint8_t *g_mem;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)g_mem; }
uint32_t g_xbox_image_lo = 0x10000, g_xbox_image_hi = 0x20000;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0x01000000; }
void xbox_FramebufferWindowFrameStats(uint32_t d) { (void)d; }
void xbox_FramebufferWindowPresent(uint32_t a, uint32_t b) { (void)a; (void)b; }
void xbox_FramebufferWindowSet(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a;(void)b;(void)c;(void)d; }
void xbox_FramebufferWindowStart(void) {}
static uint32_t *g_cap; static int g_caps;
int xbox_FramebufferWindowAccepts(uint32_t w, uint32_t h) { return getenv("TEST_WIN") && w == 640 && h == 480; }
int xbox_FramebufferWindowPresentPixels(const uint32_t *px, uint32_t w, uint32_t h)
{ uint32_t y; if (!g_cap) g_cap = malloc(640*480*4); for (y = 0; y < h; y++) memcpy(g_cap + y*w, px + (h-1-y)*w, w*4); g_caps++; return 1; }
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


/* v77 test: a vertex program, run by the CPU interpreter or the GPU shader. */
typedef struct { int neg, swz, mux, reg; } Src;
static void enc(uint32_t w[4], int mac, int ilu, int cidx, int vidx, Src A, Src B, Src C,
                int macmask, int tdst, int ilumask, int omask, int out_o, int oaddr,
                int out_ilu, int rel, int final)
{
    w[0] = 0;
    w[1] = (uint32_t)ilu << 25 | (uint32_t)mac << 21 | (uint32_t)cidx << 13 | (uint32_t)vidx << 9
         | (uint32_t)A.neg << 8 | (uint32_t)A.swz;
    w[2] = (uint32_t)A.reg << 28 | (uint32_t)A.mux << 26 | (uint32_t)B.neg << 25
         | (uint32_t)B.swz << 17 | (uint32_t)B.reg << 13 | (uint32_t)B.mux << 11
         | (uint32_t)C.neg << 10 | (uint32_t)C.swz << 2 | ((uint32_t)C.reg >> 2);
    w[3] = ((uint32_t)C.reg & 3) << 30 | (uint32_t)C.mux << 28 | (uint32_t)macmask << 24
         | (uint32_t)tdst << 20 | (uint32_t)ilumask << 16 | (uint32_t)omask << 12
         | (uint32_t)out_o << 11 | (uint32_t)oaddr << 3 | (uint32_t)out_ilu << 2
         | (uint32_t)rel << 1 | (uint32_t)final;
}
#define XYZW 0x1B
#define XXXX 0x00
#define WWWW 0xFF
static void run_program_test(void)
{
    Src none = {0, XYZW, 3, 0};
    uint32_t P[16][4];
    int n = 0, i;
    Src v0 = {0, XYZW, 2, 0}, c = {0, XYZW, 3, 0}, R12 = {0, XYZW, 1, 12}, R12w = {0, WWWW, 1, 12};
    Src R1x = {0, XXXX, 1, 1}, cx = {0, XXXX, 3, 0}, R3 = {0, XYZW, 1, 3}, R4 = {0, XYZW, 1, 4};
    Src R5 = {0, XYZW, 1, 5}, R6 = {0, XYZW, 1, 6}, nc = {1, XYZW, 3, 0};
    for (i = 0; i < 4; i++)                        /* dp4 o[0].? = v0, c[96+i] */
        enc(P[n++], 7, 0, 96 + i, 0, v0, c, none, 0, 0, 0, 8 >> i, 1, 0, 0, 0, 0);
    /* mul o[0].xyz = R12, c58 ; rcc R1.x = R12.w */
    enc(P[n++], 2, 3, 58, 0, R12, c, R12w, 0, 7, 8, 14, 1, 0, 0, 0, 0);
    /* mad o[0].xyz = R12, R1.x, c59 */
    enc(P[n++], 4, 0, 59, 0, R12, R1x, c, 0, 0, 0, 14, 1, 0, 0, 0, 0);
    /* arl a0 = c101.x */
    enc(P[n++], 13, 0, 101, 0, cx, none, none, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    /* mul oD0 = v3, c[a0+102] */
    { Src v3 = {0, XYZW, 2, 3};
      enc(P[n++], 2, 0, 102, 3, v3, c, none, 0, 0, 0, 15, 1, 3, 0, 1, 0); }
    /* exp R3 = c104.x ; log R4 = c105.x ; lit R5 = c106 */
    enc(P[n++], 0, 5, 104, 0, none, none, cx, 0, 3, 15, 0, 0, 0, 0, 0, 0);
    enc(P[n++], 0, 6, 105, 0, none, none, cx, 0, 4, 15, 0, 0, 0, 0, 0, 0);
    enc(P[n++], 0, 7, 106, 0, none, none, c, 0, 5, 15, 0, 0, 0, 0, 0, 0);
    /* mad R6 = R3, c107, R4 ; max R6 = R6, -c107 (dst mask xy) ; rsq R6.z = c105.x */
    enc(P[n++], 4, 0, 107, 0, R3, c, R4, 15, 6, 0, 0, 0, 0, 0, 0, 0);
    enc(P[n++], 10, 0, 107, 0, R6, nc, none, 12, 6, 0, 0, 0, 0, 0, 0, 0);
    /* dp3 R6.w = R5, c107 */
    enc(P[n++], 5, 0, 107, 0, R5, c, none, 1, 6, 0, 0, 0, 0, 0, 0, 0);
    /* mul oT0 = v9, R6 * (scaled via mad: oT0 = v9 * c108 + R6) FINAL */
    { Src v9 = {0, XYZW, 2, 9};
      enc(P[n++], 4, 0, 108, 9, v9, c, R6, 0, 0, 0, 15, 1, 9, 0, 0, 1); }
    M(0x1E9C, 0);                                  /* PROGRAM_LOAD slot 0 */
    for (i = 0; i < n; i++) { int k; for (k = 0; k < 4; k++) M(0x0B00 + (k * 4), P[i][k]); }
    M(0x1EA0, 0);                                  /* PROGRAM_START */
    {
        static const float cs[][4] = {
            {1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1},           /* 96-99 */
            {0,0,0,0},{2.0f,0,0,0},{0.1f,0.2f,0.3f,1},{0.5f,1.0f,0.75f,1},  /* 100-103 */
            {1.3f,0,0,0},{8.0f,0,0,0},{0.7f,0.5f,0,2.0f},{0.25f,0.5f,1.0f,1.0f}, /* 104-107 */
            {1.0f,1.0f,1,1} };                                  /* 108 */
        float c58[4] = {1,1,1,1}, c59[4] = {0,0,0,0};
        int k;
        M(0x1EA4, 58); for (k = 0; k < 4; k++) MF(0x0B80 + 4*k, c58[k]);
        for (k = 0; k < 4; k++) MF(0x0B80 + 4*k, c59[k]);
        M(0x1EA4, 96);
        for (i = 0; i < 13; i++) for (k = 0; k < 4; k++) MF(0x0B80 + 4*((i*4+k)&31), cs[i][k]);
    }
    M(0x1E94, 2);                                  /* program mode */
}

int main(int argc, char **argv)
{
    int i, k;
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

    if (getenv("TEST_DXT")) {
    /* 9 (v80): DXT1 / DXT3 / DXT5 64x64 with a mip chain, uploaded as S3TC
     * by the GL backend and decoded per texel by the software one. Random
     * blocks, so every colour mode and the DXT1 punch-through are hit. */
    {
        uint32_t seed = 12345u, n, q;
        uint8_t *p = g_mem + VA(0x00700000u);
        static const uint32_t fmt[3] = {0x0C, 0x0E, 0x0F};
        for (n = 0; n < 0x30000u; n++) {
            seed = seed * 1103515245u + 12345u;
            p[n] = (uint8_t)(seed >> 16);
        }
        M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 0 * 4, VB_OFF);      M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 3 * 4, VB_OFF + 12); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
        M(0x1720 + 9 * 4, VB_OFF + 16); M(0x1760 + 9 * 4, 2 | (2 << 4) | (24 << 8));
        M(0x030C, 0);
        M(0x0304, 1); M(0x0344, 0x302); M(0x0348, 0x303); M(0x0350, 0x8006);
        M(0x1B08, 0x0101u);
        for (q = 0; q < 3; q++) {
            float x0 = 20 + q * 140.0f, y0 = 20;
            M(0x1B00, 0x00700000u + q * 0x10000u);
            M(0x1B04, (fmt[q] << 8) | (1u << 4) | (4u << 16) | (6u << 20) | (6u << 24));
            M(0x1B14, q == 1 ? 0x01030000u : 0x01010000u);   /* one with a mip filter */
            put_vtx(40 + q * 4, x0, y0, 100, 0xFFFFFFFFu, 0, 0);
            put_vtx(41 + q * 4, x0 + 128, y0, 100, 0xFFFFFFFFu, 1, 0);
            put_vtx(42 + q * 4, x0 + 128, y0 + 128, 100, 0xFFFFFFFFu, 1, 1);
            put_vtx(43 + q * 4, x0, y0 + 128, 100, 0xFFFFFFFFu, 0, 1);
            draw(8, 40 + q * 4, 4);
        }
        M(0x0304, 0);
    }
    goto flip;
    }
    if (getenv("TEST_PERSP")) {
        /* a checkered floor receding from the camera: composite with w = z,
         * so texturing is only right if it is perspective-correct; the near
         * row passes behind the eye (z < 0), so clipping is exercised too */
        int r, c;
        float Mx[16] = {0};
        Mx[0] = 400; Mx[5] = -400; Mx[10] = 1.0e6f; Mx[14] = 1.0f;
        Mx[2] = 320; Mx[6] = 240;
        for (i = 0; i < 16; i++) MF(0x0680 + i * 4, Mx[i]);
        M(0x030C, 0); M(0x0304, 0);
        M(0x1B00, TEX_OFF); M(0x1B04, (0x12u << 8) | (1u << 4)); M(0x1B10, 256u << 16);
        M(0x1B1C, (64u << 16) | 64u); M(0x1B14, 0x01010000u); M(0x1B08, 0x0101u);
        k = 0;
        for (r = 0; r < 4; r++) for (c = 0; c < 4; c++, k++) {
            float x0 = -2 + c, z0 = (r == 0 ? -0.5f : r * 2.0f) ;
            put_vtx(100 + k * 4, x0, -1, z0, 0xFFFFFFFFu, 0, 0);
            put_vtx(101 + k * 4, x0 + 1, -1, z0, 0xFFFFFFFFu, 64, 0);
            put_vtx(102 + k * 4, x0 + 1, -1, z0 + 2, 0xFFFFFFFFu, 64, 64);
            put_vtx(103 + k * 4, x0, -1, z0 + 2, 0xFFFFFFFFu, 0, 64);
            draw(8, 100 + k * 4, 4);
        }
        goto flip;
    }
#ifdef HAVE_BEAM_TRIS
    if (getenv("TEST_BEAM")) {
        /* v96: the police-spotlight beam after tagging, replayed from a frame
         * trace: D3D's pre-transformed pass-through program run on the
         * beam's object-space vertices (pos3+normal3 read as pos4+2 colours) */
        static const uint32_t prog[12][4] = {
            {0x00000000,0x0020001B,0x0836106C,0x2F100FF8},{0x00000000,0x0420061B,0x083613FC,0x5011F818},
            {0x00000000,0x002008FF,0x0836106C,0x2070F828},{0x00000000,0x0240081B,0x1436186C,0x2F20F824},
            {0x00000000,0x0060201B,0x2436106C,0x3070F800},{0x00000000,0x00200200,0x0836106C,0x2070F830},
            {0x00000000,0x00200E1B,0x0836106C,0x2070F838},{0x00000000,0x0020101B,0x0836106C,0x2070F840},
            {0x00000000,0x0020121B,0x0836106C,0x2070F848},{0x00000000,0x0020141B,0x0836106C,0x2070F850},
            {0x00000000,0x0020161B,0x0836106C,0x2070F858},{0x00000000,0x0020181B,0x0836106C,0x2070F861}};
        static const float c0[4] = {1, 1, 16777215, 1}, c1[4] = {0.53125f, 0.53125f, 0, 0};
        float *vb = (float *)(g_mem + VA(0x00340000u));
        int t, v, q;
        M(0x1E9C, 0);
        for (t = 0; t < 12; t++) for (q = 0; q < 4; q++) M(0x0B00 + q * 4, prog[t][q]);
        M(0x1EA0, 0);
        M(0x1EA4, 0);
        for (q = 0; q < 4; q++) MF(0x0B80 + 4 * q, c0[q]);
        for (q = 0; q < 4; q++) MF(0x0B80 + 4 * q, c1[q]);
        M(0x1E94, 2);
        for (t = 0; t < (int)(sizeof beam / sizeof beam[0]); t++)
            for (v = 0; v < 3; v++) {
                float *d = vb + (t * 3 + v) * 12;
                for (q = 0; q < 12; q++) d[q] = beam[t][v][q];
            }
        for (q = 1; q < 16; q++) M(0x1760 + q * 4, 2 | (0 << 4));
        M(0x1720 + 0 * 4, 0x00340000u); M(0x1760 + 0 * 4, 2 | (4 << 4) | (48 << 8));
        M(0x1720 + 3 * 4, 0x00340010u); M(0x1760 + 3 * 4, 2 | (4 << 4) | (48 << 8));
        M(0x1720 + 4 * 4, 0x00340020u); M(0x1760 + 4 * 4, 2 | (4 << 4) | (48 << 8));
        M(0x0AC0, 0x04200000u); M(0x0260, 0x14200000u); M(0x1E70, 0);   /* colour = v0 (diffuse) */
        M(0x030C, 1); M(0x0354, 0x203); M(0x035C, 1);
        M(0x0308, 0);
        M(0x0304, 1); M(0x0344, 0x302); M(0x0348, 0x1);
        if (getenv("TEST_BEAM_ONE")) {
            int one = atoi(getenv("TEST_BEAM_ONE"));
            draw(5, 3 * one, 3);
        } else
            draw(5, 0, 3 * (int)(sizeof beam / sizeof beam[0]));
        goto flip;
    }
#endif
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
    /* 9 (v67): a 640x480 R5G6B5 target sampled back as LIN_R5G6B5, the way
     * JSRF reuses its previous frame. */
    {
        Vtx *vb = (Vtx *)(g_mem + VA(VB_OFF));
        M(0x1720 + 0 * 4, VB_OFF);      M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 3 * 4, VB_OFF + 12); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
        M(0x1720 + 9 * 4, VB_OFF + 16); M(0x1760 + 9 * 4, 2 | (2 << 4) | (24 << 8));
        (void)vb;
        M(0x0210, 0x00700000u);
        M(0x1D90, 0x001Fu); M(0x1D94, 0xF3u);       /* blue */
        M(0x030C, 0); M(0x1E70, 0);
        M(0x0AC0, 0x04200000u); M(0x0260, 0x14200000u);
        put_vtx(40, 110, 60, 100, 0xFFFFFF00u, 0, 0);
        put_vtx(41, 220, 70, 100, 0xFFFF00FFu, 0, 0);
        put_vtx(42, 120, 170, 100, 0xFF00FFFFu, 0, 0);
        draw(5, 40, 3);
        M(0x0210, SURF_OFF);
        M(0x0AC0, 0x08040000u); M(0x0260, 0x18140000u);
        M(0x1E70, 0x00000001u);
        M(0x1B00, 0x00700000u); M(0x1B04, (0x11u << 8) | (1u << 4)); M(0x1B10, 1280u << 16);
        M(0x1B1C, (640u << 16) | 480u); M(0x1B14, 0x01010000u); M(0x1B08, 0x0303u);
        put_vtx(43, 10, 10, 100, 0xFFFFFFFFu, 100, 50);
        put_vtx(44, 138, 10, 100, 0xFFFFFFFFu, 228, 50);
        put_vtx(45, 138, 138, 100, 0xFFFFFFFFu, 228, 178);
        put_vtx(46, 10, 138, 100, 0xFFFFFFFFu, 100, 178);
        draw(8, 43, 4);
    }
    if (getenv("TEST_PROG")) {
        run_program_test();
        M(0x0AC0, 0x08040000u); M(0x0260, 0x18140000u);
        M(0x1E70, 0x00000001u);
        M(0x1720 + 0 * 4, VB_OFF);      M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 3 * 4, VB_OFF + 12); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
        M(0x1720 + 9 * 4, VB_OFF + 16); M(0x1760 + 9 * 4, 2 | (2 << 4) | (24 << 8));
        M(0x1B00, TEX_OFF); M(0x1B04, (0x12u << 8) | (1u << 4)); M(0x1B10, 256u << 16);
        M(0x1B1C, (64u << 16) | 64u); M(0x1B14, 0x01010000u); M(0x1B08, 0x0101u);
        M(0x030C, 0);
        put_vtx(50, 300, 200, 100, 0xFFFFFFFFu, 0, 0);
        put_vtx(51, 620, 220, 100, 0xFF80FF40u, 70, 0);
        put_vtx(52, 610, 470, 100, 0xFF4080FFu, 70, 90);
        put_vtx(53, 320, 450, 100, 0xFFFF8040u, 0, 90);
        draw(8, 50, 4);
        M(0x1E94, 0);
    }
    if (getenv("TEST_LIGHT")) {
        float *nb = (float *)(g_mem + VA(VB_OFF + 0x8000));
        float *wb = (float *)(g_mem + VA(VB_OFF + 0x9000));
        int k;
        static const float L[][3] = {{0.1f,0.1f,0.1f},{0.7f,0.6f,0.5f},{0.5f,0.5f,0.5f}};
        M(0x1E94, 0);
        M(0x0AC0, 0x04200000u); M(0x0260, 0x14200000u);   /* r0 = v0 (lit colour) */
        M(0x1E70, 0);
        M(0x1720 + 0 * 4, VB_OFF);      M(0x1760 + 0 * 4, 2 | (3 << 4) | (24 << 8));
        M(0x1720 + 3 * 4, VB_OFF + 12); M(0x1760 + 3 * 4, 0 | (4 << 4) | (24 << 8));
        M(0x1720 + 2 * 4, VB_OFF + 0x8000); M(0x1760 + 2 * 4, 2 | (3 << 4) | (12 << 8));
        M(0x1720 + 1 * 4, VB_OFF + 0x9000); M(0x1760 + 1 * 4, 2 | (1 << 4) | (4 << 8));
        for (k = 0; k < 16; k++) {
            MF(0x0480 + k * 4, (k % 5 == 0) ? 1.0f : 0.0f);           /* MV0 = I */
            MF(0x0580 + k * 4, (k % 5 == 0) ? 1.0f : 0.0f);           /* inv = I */
            MF(0x04C0 + k * 4, (k % 5 == 0) ? 1.0f : (k == 3 ? 40.0f : (k == 7 ? -30.0f : 0.0f)));
        }
        M(0x0328, getenv("TEST_SKIN") ? 1 : 0);
        M(0x0314, 1);                                  /* lighting */
        M(0x03BC, 0x9);                                /* light0 infinite, light1 local */
        M(0x0298, 1u << 4);                            /* diffuse from vertex colour */
        M(0x03B8, 1);
        MF(0x03B4, 0.8f);
        MF(0x0A10, 0.05f); MF(0x0A14, 0.05f); MF(0x0A18, 0.1f);
        for (k = 0; k < 3; k++) {
            MF(0x1000 + k * 4, L[0][k]); MF(0x100C + k * 4, L[1][k]); MF(0x1018 + k * 4, L[2][k]);
            MF(0x1080 + k * 4, 0.0f);    MF(0x108C + k * 4, k == 2 ? 0.9f : 0.3f);
            MF(0x1098 + k * 4, 0.6f);
        }
        MF(0x1028, 0.0f); MF(0x102C, 0.3f); MF(0x1030, 0.95f);   /* half */
        MF(0x1034, 0.0f); MF(0x1038, 0.6f); MF(0x103C, 0.8f);    /* dir */
        MF(0x10DC, 450.0f); MF(0x10E0, 250.0f); MF(0x10E4, -150.0f);  /* light1 pos */
        MF(0x10E8, 1.0f); MF(0x10EC, 0.002f); MF(0x10F0, 0.0f);       /* light1 atten */
        MF(0x10A4, 0.0f);                                              /* range off */
        {
            int gx, gy, idx = 0;
            for (gy = 0; gy < 6; gy++) for (gx = 0; gx < 6; gx++, idx++) {
                float nx = (gx - 2.5f) / 3.0f, ny = (gy - 2.5f) / 3.0f, nz = 1.0f;
                put_vtx(60 + idx, 300 + gx * 60, 180 + gy * 55, 100, 0xFFFFC080u, 0, 0);
                nb[(60 + idx) * 3] = nx; nb[(60 + idx) * 3 + 1] = ny; nb[(60 + idx) * 3 + 2] = nz;
                wb[60 + idx] = gx / 5.0f;
            }
            M(0x17FC, 5);
            for (gy = 0; gy < 5; gy++) for (gx = 0; gx < 5; gx++) {
                uint32_t a0 = 60 + gy * 6 + gx, b0 = a0 + 1, c0 = a0 + 6, d0 = c0 + 1;
                M(0x1800, a0 | (b0 << 16)); M(0x1800, c0 | (b0 << 16)); M(0x1800, d0 | (c0 << 16));
            }
            M(0x17FC, 0);
        }
        M(0x0314, 0); M(0x0328, 0);
        M(0x1760 + 2 * 4, 2 | (0 << 4)); M(0x1760 + 1 * 4, 2 | (0 << 4));
    }
    /* Flip: the GPU path writes the surface back here. */
flip:
    M(0x0130, 0);
    if (getenv("TEST_CPUWRITE")) memset(g_mem + VA(SURF_OFF), 0x55, 1280 * 10);
    M(0x0130, 0);

    {
        FILE *f = fopen(out, "wb");
        { extern void nv2a_gl_stats2(uint64_t*,uint64_t*,uint64_t*); uint64_t cwx; extern void nv2a_gl_stats(uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*); uint64_t c,p,d,t,u,r,x; nv2a_gl_stats2(&c,&p,&cwx); nv2a_gl_stats(&d,&t,&u,&r,&x); fprintf(stderr, "window captures %d gpu copies %llu present reads %llu readbacks %llu texups %llu\n", g_caps,(unsigned long long)c,(unsigned long long)p,(unsigned long long)r,(unsigned long long)x); }
        if (g_cap) { uint16_t *o = (uint16_t *)(g_mem + VA(SURF_OFF)); int i;
            for (i = 0; i < 640*480; i++) { uint32_t c = g_cap[i];
                o[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F)); } }
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
