/*
 * jsrf_boost.c -- v312c (diagnostic): where the boost effect is drawn
 * (Idea by Miggas: a new boost effect, a radial blur with speed lines, in
 * place of the game's trails).
 *
 * The finding is ReJet's (github.com/Rossybergg/ReJet; no code taken): while
 * boosting the game draws the scene into a 512x512 and a 128x128 render
 * target on alternate frames, clears the screen, lays the 512x512 over it at
 * about 55% and a blur from the 128x128 over that. Our own analysis of the US
 * code: the engine's SetRenderTarget is sub_0014D340 (renderer slot 42, vtable
 * 0x1E0F00 +0xA8; PAL 0x14DDF0), and the camera code (0x50180, 0x50690,
 * 0xBE930, ...) hands it the texture its camera names (+0xA4, through the
 * texture manager 0x252790).
 *
 * RECOMP_BOOST_LOG=1 logs, for the frames where a render target other than
 * the screen is set and the B button changes (boost is B; Miggas), every
 * render-target change with the game code that made it, every clear, and the
 * DrawVerticesUP draws (the full-screen pastes) with their textures and first
 * vertices. Lines start with [BOOST]. Nothing changes in the game.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#else
#include "win32_compat.h"
#endif
#include "gen/recomp_types.h"

extern size_t xbox_GetMappedSize(void);
extern void D3D8_D3DDevice_SetRenderTarget_gen(void);
extern void D3D8_D3DDevice_Clear_gen(void);
extern void D3D8_D3DDevice_Swap_gen(void);
extern void D3D8_D3DDevice_DrawVerticesUP_gen(void);
extern void sub_000A2960_gen(void);
void jsrf_ZBiasMark(uint32_t v);            /* jsrf_widescreen.c */
void jsrf_photo_place(uint32_t cam);        /* v313: jsrf_photo.c */
int  jsrf_photo_holding(void);

static int bl_on = -1;
static int boost_log_on(void)
{
    if (bl_on < 0) {
        const char *e = getenv("RECOMP_BOOST_LOG");
        bl_on = e && e[0] && e[0] != '0';
        if (bl_on)
            fprintf(stderr, "[BOOST] logging the boost's render targets (RECOMP_BOOST_LOG=1): boost with B\n");
    }
    return bl_on;
}

static int ok(uint32_t va, uint32_t n)
{
    uint64_t end = (uint64_t)va + n;
    if (va >= 0x1000u && end <= xbox_GetMappedSize()) return 1;
    return va >= 0x80000000u && end <= 0x84000000ull;
}

/* the frame's lines, kept until Swap decides whether the frame is worth it */
static char fb[16384];
static size_t fbn;
static int fb_offscreen;           /* a target other than the screen was set */
static unsigned frame, logged, draws;
static int b_now, b_was;           /* B held (XInputGetState) */
static uint32_t tex0;              /* texture on stage 0 (SetTexture) */

static void put(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (fbn >= sizeof fb - 256) return;
    va_start(ap, fmt);
    n = vsnprintf(fb + fbn, sizeof fb - fbn, fmt, ap);
    va_end(ap);
    if (n > 0) fbn += (size_t)n < sizeof fb - fbn ? (size_t)n : sizeof fb - fbn - 1;
}

/* the game's calls on the stack: return addresses in the game's code or the
 * engine's (below D3D) right after a call */
static void chain(void)
{
    uint32_t k, shown = 0;
    put(" from");
    for (k = 0; k < 160 && shown < 10; k++) {
        uint32_t v = MEM32(g_esp + 4 * k);
        if (v < 69640u || v >= 1625920u) continue;   /* 0x11008..0x18CB40 (decimal: not a game address to move) */
        if (MEM8(v - 5) == 0xE8                                       /* call rel32 */
            || (MEM8(v - 6) == 0xFF && (MEM8(v - 5) & 0x38) == 0x10)    /* call [reg+d32] */
            || (MEM8(v - 3) == 0xFF && (MEM8(v - 2) & 0x38) == 0x10)    /* call [reg+d8] */
            || (MEM8(v - 2) == 0xFF && (MEM8(v - 1) & 0x38) == 0x10)) { /* call reg / [reg] */
            put(" %06X", v);
            shown++;
        }
    }
}

/* a D3D resource's size: Size for linear ones, else log2 sizes in Format */
static void res_size(uint32_t r, unsigned *w, unsigned *h)
{
    uint32_t fmt = MEM32(r + 0xC), size = MEM32(r + 0x10);
    if (size) {
        *w = (size & 0xFFF) + 1;
        *h = ((size >> 12) & 0xFFF) + 1;
    } else {
        *w = 1u << ((fmt >> 20) & 15);
        *h = 1u << ((fmt >> 24) & 15);
    }
}

static void res_put(const char *what, uint32_t r)
{
    unsigned w = 0, h = 0;
    if (!r) { put(" %s none", what); return; }
    if (!ok(r, 0x14)) { put(" %s %08X?", what, r); return; }
    res_size(r, &w, &h);
    put(" %s %08X data %08X fmt %08X %ux%u", what, r, MEM32(r + 4), MEM32(r + 0xC), w, h);
}

/* boost_note_*: from recomp_manual.c's wrappers */
void boost_note_buttons(uint32_t ps)
{
    if (ps) b_now = MEM8(ps + 7) > 30;          /* bAnalogButtons[1]: B */
}

void boost_note_texture(uint32_t stage, uint32_t tex)
{
    if (stage == 0) tex0 = tex;
}

void D3D8_D3DDevice_SetRenderTarget(void)
{
    uint32_t rt = MEM32(g_esp + 4), z = MEM32(g_esp + 8);
    if (boost_log_on()) {
        unsigned w = 0, h = 0;
        if (rt && ok(rt, 0x14)) {
            res_size(rt, &w, &h);
            if (w != 640 || h != 480) fb_offscreen = 1;
        }
        put("[BOOST]  target:");
        res_put("rt", rt);
        put(" z %08X", z);
        chain();
        put("\n");
    }
    D3D8_D3DDevice_SetRenderTarget_gen();
}

void D3D8_D3DDevice_Clear(void)
{
    if (boost_log_on()) {
        put("[BOOST]  clear: rects %u flags %X colour %08X", MEM32(g_esp + 4),
            MEM32(g_esp + 12), MEM32(g_esp + 16));
        chain();
        put("\n");
    }
    D3D8_D3DDevice_Clear_gen();
}

void D3D8_D3DDevice_DrawVerticesUP(void)
{
    if (boost_log_on() && draws++ < 24) {
        uint32_t prim = MEM32(g_esp + 4), n = MEM32(g_esp + 8);
        uint32_t p = MEM32(g_esp + 12), stride = MEM32(g_esp + 16), i;
        put("[BOOST]  drawUP: prim %u count %u stride %u", prim, n, stride);
        res_put("tex0", tex0);
        if (p && stride && stride <= 64 && ok(p, stride * (n < 4 ? n : 4))) {
            uint32_t v, nv = n < 4 ? n : 4;
            for (v = 0; v < nv; v++) {
                put(" |");
                for (i = 0; i < stride / 4 && i < 8; i++) {
                    uint32_t d = MEM32(p + v * stride + i * 4);
                    if (i < 4) {                       /* x, y, z, rhw */
                        float f;
                        memcpy(&f, &d, 4);
                        put(" %.4g", f);
                    } else
                        put(" %08X", d);
                }
            }
        }
        chain();
        put("\n");
    }
    D3D8_D3DDevice_DrawVerticesUP_gen();
}

void D3D8_D3DDevice_Swap(void)
{
    if (boost_log_on()) {
        /* frames around a boost: B pressed or let go, or a target other than
         * the screen while B is held or for 3 s after; and the first 5
         * other frames with one, to see what the game draws off screen
         * without boosting */
        static unsigned after_b, plain;
        int want;
        if (b_now) after_b = 180; else if (after_b) after_b--;
        want = b_now != b_was || (fb_offscreen && (b_now || after_b))
            || (fb_offscreen && plain < 5 && ++plain);
        if (want && logged < 900) {
            logged++;
            fprintf(stderr, "[BOOST] frame %u: B %s%s\n%.*s", frame, b_now ? "held" : "up",
                    b_now != b_was ? (b_now ? " (pressed)" : " (let go)") : "", (int)fbn, fb);
            if (logged == 900)
                fprintf(stderr, "[BOOST] 900 frames logged: no more\n");
            fflush(stderr);
        }
        b_was = b_now;
        fbn = 0;
        fb_offscreen = 0;
        draws = 0;
        frame++;
    }
    D3D8_D3DDevice_Swap_gen();
}

/* ── v312e: the new boost effect (Idea by Miggas) ─────────────────────────
 *
 * v312c's log (Miggas) shows where the game draws its boost: sub_000A2960,
 * a draw-tree object's draw (thiscall, the pass in its argument). Its owner
 * ([this+0x24], the player) counts the boost down at +0xE60. While that is
 * above 0, in pass 0x13 it points the following views at its 512x512 and
 * 128x128 textures in turn ([this+0x170] / [this+0x16C], the count at
 * [this+0x1AC]), and in pass 0x14 it lays them over the screen: the trails,
 * a picture that moves at 30 fps (ReJet's finding, Rossybergg/ReJet).
 *
 * RECOMP_BOOST_EFFECT:
 *   new       (the default) the game draws as when not boosting (its count
 *             reads 0 for these two passes only), and where its pass 0x14
 *             drew the trails the renderer puts our effect, a radial blur
 *             clear around the player with speed lines, fading in and out
 *             over a few frames (a z-bias mark: nv2a_pb_exec.c, ws_fill_mark;
 *             nv2a_render.c, nv2a_gl_boost_fx)
 *   smooth    the same without any effect: the plain picture at 60 fps
 *   original  the game's own trails
 * RECOMP_BOOST_STRENGTH (0.1..1, default 0.55) scales the effect. */
static int same_word(const char *a, const char *b)
{
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}

static int boost_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_BOOST_EFFECT");
        m = 2;
        if (e && (same_word(e, "original") || !strcmp(e, "0"))) m = 0;
        else if (e && same_word(e, "smooth")) m = 1;
        fprintf(stderr, "[BOOST] boost effect: %s (RECOMP_BOOST_EFFECT)\n",
                m == 2 ? "new" : m == 1 ? "smooth" : "original");
    }
    return m;
}

void sub_000A2960(void)
{
    uint32_t self = g_ecx, pass = MEM32(g_esp + 4), owner, saved;
    int mode;
    jsrf_photo_place(self);                 /* v313: photo mode's eye, while it holds the world */
    if (pass != 0x13 && pass != 0x14) { sub_000A2960_gen(); return; }
    mode = boost_mode();
    owner = ok(self + 0x24, 4) ? MEM32(self + 0x24) : 0;
    if (!mode || !owner || !ok(owner + 0xE60, 4)) { sub_000A2960_gen(); return; }
    saved = MEM32(owner + 0xE60);
    MEM32(owner + 0xE60) = 0;               /* not boosting, for these two passes */
    sub_000A2960_gen();
    MEM32(owner + 0xE60) = saved;
    if (mode == 2 && pass == 0x14) {
        static float level, scale = -1.0f;
        int on = (int32_t)saved > 0;
        if (scale < 0.0f) {
            const char *e = getenv("RECOMP_BOOST_STRENGTH");
            scale = e && *e ? (float)atof(e) : 0.55f;   /* v312g: Miggas's choice */
            if (scale < 0.1f) scale = 0.1f;
            if (scale > 1.0f) scale = 1.0f;
        }
        level += on ? 1.0f / 8.0f : -1.0f / 14.0f;   /* in over 8 frames, out over 14 */
        if (jsrf_photo_holding()) level = 0.0f;       /* v313: not in a photo */
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
        if (level > 0.0f) {
            float st = level * level * (3.0f - 2.0f * level) * scale;
            jsrf_ZBiasMark(1300000000u + (uint32_t)(st * 500000000.0f));
        }
    }
}
