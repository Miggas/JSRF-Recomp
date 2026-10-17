/**
 * v187n: widescreen, game side -- the HUD at the screen edges, and the
 * screens built for 4:3 only. Adapted from Piaka's widescreen patch (his
 * addresses and the sprite-queue idea; his renderer half is not used, the
 * toolkit's nv2a_pb_exec.c / nv2a_gl.c squeeze does that part here).
 * Nothing here does anything unless widescreen is on (RECOMP_WIDESCREEN /
 * RECOMP_ASPECT).
 *
 *   - The HUD: the renderer squeezes the whole 640x480 frame by k about its
 *     middle, so the HUD would sit inside the old 4:3 area. Each listed HUD
 *     function moves what it draws by half the width the wider shape adds
 *     (320/k - 320 guest pixels: 106.67 at 16:9), left or right, so it ends
 *     at the new edges with its original margins and size. Sprites are moved
 *     when they are queued (sub_0015A880): the game draws its sprite batches
 *     later, often after the function has returned, so moving the screen
 *     offset while it ran split groups apart. The spray can and boost icon
 *     are 3D models drawn directly; their functions move the device's
 *     screen-space offset instead (anchor +-3).
 *   - Screens built for 4:3 (3D scenes that end at the 4:3 edges, with their
 *     animations waiting just outside): while one is shown, the picture
 *     slides back to 4:3 (fb_present.c, xbox_WidescreenHold43).
 *
 * RECOMP_WS_HUD=0 leaves the HUD in the middle; RECOMP_WS_HOLD43=0 keeps
 * those screens wide.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#include "gen/recomp_types.h"

extern double xbox_PresentAspect(void);
extern double xbox_WidescreenK(void);
extern void xbox_WidescreenHold43(void);

/* guest pixels added at each side by the shape shown now */
static float ws_half_extra(void)
{
    double k = xbox_WidescreenK();
    if (!(k > 0.2 && k < 0.9999))
        return 0.0f;
    return (float)(320.0 / k - 320.0);
}

static int ws_flag(const char *name)
{
    const char *e = getenv(name);
    return !(e && e[0] == '0');
}

static int ws_hud_on(void)
{
    static int on = -1;
    if (on < 0) {
        double a = xbox_PresentAspect();
        on = a > 4.0 / 3.0 + 1e-6 && ws_flag("RECOMP_WS_HUD");
        if (on)
            fprintf(stderr, "[WS] HUD at the screen edges (RECOMP_WS_HUD=0: in the middle)\n");
    }
    return on;
}

static int ws_hold_on(void)
{
    static int on = -1;
    if (on < 0) {
        double a = xbox_PresentAspect();
        on = a > 4.0 / 3.0 + 1e-6 && ws_flag("RECOMP_WS_HOLD43");
    }
    return on;
}

/* ── HUD anchors ────────────────────────────────────────────────────────
 * -1 left, +1 right: sprite quads moved by the anchor when queued.
 * -3, +3: the function also draws directly; the device screen-space offset
 *         is moved while it runs (sprites it queues are not moved twice). */
static const struct { uint32_t addr; int anchor; } ws_hud[] = {
    { 0x001060B0u,  1 },    /* health bar and border */
    { 0x0010D480u,  1 },    /* lower-right gauge and numbers */
    { 0x0010DC60u,  3 },    /* spray can (3D), "x" and count */
    { 0x00109B70u,  3 },    /* boost-ready icon (3D) */
    { 0x000FFFD0u, -1 },    /* trick count and score rows */
    { 0x00101F20u, -1 },    /* trick name */
};

extern void D3D8_D3DDevice_SetScreenSpaceOffset(void);

typedef struct { int anchor, changed, prev_screen; float x, y; } WsScope;
static RECOMP_TLS WsScope ws_stack[32];
static RECOMP_TLS int ws_depth;
static RECOMP_TLS int ws_screen;    /* anchor the screen offset currently has */

/* D3DDevice_SetScreenSpaceOffset(x, y) through the generated XDK code, with
 * the caller's register state kept: it runs between two guest instructions. */
static void ws_set_screen_offset(float x, float y)
{
    uint32_t r[10] = { g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp,
                       (uint32_t)g_df };
    double fp[8];
    int top = g_fp_top, cmp = g_fp_cmp;
    uint16_t cc = g_fp_cc, cw = g_fp_control_word;
    RecompXmm xm[8] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 };
    uint32_t bx, by;
    memcpy(fp, g_fp_stack, sizeof fp);
    memcpy(&bx, &x, 4);
    memcpy(&by, &y, 4);
    PUSH32(g_esp, by);
    PUSH32(g_esp, bx);
    PUSH32(g_esp, 0);                       /* return address slot */
    D3D8_D3DDevice_SetScreenSpaceOffset();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_esp = r[3]; g_ebx = r[4];
    g_esi = r[5]; g_edi = r[6]; g_ebp = r[7]; g_seh_ebp = r[8]; g_df = (int)r[9];
    memcpy(g_fp_stack, fp, sizeof fp);
    g_fp_top = top; g_fp_cmp = cmp; g_fp_cc = cc; g_fp_control_word = cw;
    g_xmm0 = xm[0]; g_xmm1 = xm[1]; g_xmm2 = xm[2]; g_xmm3 = xm[3];
    g_xmm4 = xm[4]; g_xmm5 = xm[5]; g_xmm6 = xm[6]; g_xmm7 = xm[7];
}

static void ws_enter(uint32_t addr)
{
    WsScope *s;
    unsigned k;
    int anchor = 0;
    if (ws_depth >= 32) {
        ws_depth++;
        return;
    }
    s = &ws_stack[ws_depth++];
    for (k = 0; k < sizeof ws_hud / sizeof ws_hud[0]; ++k)
        if (ws_hud[k].addr == addr)
            anchor = ws_hud[k].anchor;
    s->anchor = anchor;
    s->changed = 0;
    if (anchor == 3 || anchor == -3) {
        /* D3D device at 0x19DCE0; its screen-space offset is stored at +0x9E8
         * and +0x9EC plus the constant at 0x19A040 (pixel-centre bias). */
        uint32_t device = MEM32(0x19DCE0u);
        int screen = anchor / 3;
        float extra = ws_half_extra();
        if (device && screen != ws_screen && extra != 0.0f) {
            s->x = MEMF(device + 0x9E8u) - MEMF(0x19A040u);
            s->y = MEMF(device + 0x9ECu) - MEMF(0x19A040u);
            ws_set_screen_offset(s->x + (float)(screen - ws_screen) * extra, s->y);
            s->prev_screen = ws_screen;
            s->changed = 1;
            ws_screen = screen;
        }
    }
}

static void ws_leave(void)
{
    WsScope *s;
    if (ws_depth <= 0)
        return;
    if (--ws_depth >= 32)
        return;
    s = &ws_stack[ws_depth];
    if (s->changed) {
        ws_set_screen_offset(s->x, s->y);
        ws_screen = s->prev_screen;
    }
}

#define WS_HUD_SCOPE(name, addr)                    \
    extern void name##_gen(void);                   \
    void name(void)                                 \
    {                                               \
        if (!ws_hud_on()) { name##_gen(); return; } \
        ws_enter(addr);                             \
        name##_gen();                               \
        ws_leave();                                 \
    }

WS_HUD_SCOPE(sub_001060B0, 0x001060B0u)
WS_HUD_SCOPE(sub_0010D480, 0x0010D480u)
WS_HUD_SCOPE(sub_0010DC60, 0x0010DC60u)
WS_HUD_SCOPE(sub_00109B70, 0x00109B70u)
WS_HUD_SCOPE(sub_000FFFD0, 0x000FFFD0u)
WS_HUD_SCOPE(sub_00101F20, 0x00101F20u)

/* Sprite quad submission (the 0x1E144C quad-batch interface, +0x14): copies
 * one quad of 4 vertices, 32 bytes each, x first, to the batch's write
 * pointer at this+0x1C and advances it by 0x80. stdcall, `this` first. */
extern void sub_0015A880_gen(void);

void sub_0015A880(void)
{
    uint32_t obj, before, v;
    int anchor;
    float extra;
    if (!ws_hud_on() || ws_depth <= 0 || ws_depth > 32) {
        sub_0015A880_gen();
        return;
    }
    anchor = ws_stack[ws_depth - 1].anchor;
    obj = MEM32(g_esp + 4);
    before = MEM32(obj + 0x1C);
    sub_0015A880_gen();
    if (anchor != 1 && anchor != -1)
        return;
    if (g_eax != 0 || MEM32(obj + 0x1C) - before != 0x80)
        return;
    extra = ws_half_extra();
    for (v = 0; v < 4; ++v)
        MEMF(before + v * 32u) += (float)anchor * extra;
}

/* ── screens kept at 4:3 ─────────────────────────────────────────────────
 * Found by Piaka by comparing which game functions run in each screen and
 * never in gameplay:
 *   sub_00077730: menu frame start (viewport 0,0,640,480 and a clear), every
 *                 frame of the Roboy/garage menus, the music menu and the map;
 *   sub_0011AC50: character select. */
#define WS_HOLD_43(name)                            \
    extern void name##_gen(void);                   \
    void name(void)                                 \
    {                                               \
        if (ws_hold_on())                           \
            xbox_WidescreenHold43();                \
        name##_gen();                               \
    }

WS_HOLD_43(sub_00077730)
WS_HOLD_43(sub_0011AC50)
