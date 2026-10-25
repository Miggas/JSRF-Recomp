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
extern void xbox_MenuScreen(void);   /* v207b */
extern void xbox_WidescreenHold43(void);

/* guest pixels added at each side by the shape shown now */
static int ws_split(void);

static float ws_half_extra(void)
{
    double k = xbox_WidescreenK();
    if (!(k > 0.2 && k < 0.9999))
        return 0.0f;
    /* v187s: in split screen each view is squeezed about its own middle,
     * which already moves its HUD outwards by half of this; the other half
     * puts each piece as far from the screen's edge (or the middle line) as
     * the 4:3 game had it (runs 187r, and the 4:3 reference) */
    return (float)((320.0 / k - 320.0) * (ws_split() ? 0.5 : 1.0));
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
    { 0x000F7720u,  9 },    /* v191b: letterbox bars (cutscenes, dialogue) */
    { 0x000F7990u,  9 },    /* v191b: the bars opening and closing */
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

/* v187r: split screen. Each player's view is a viewport of half the
 * screen (320x480, or 320x240 with three or four players), squeezed with
 * the rest of the frame, so its HUD already sits where the 4:3 game put it
 * inside that view; moving it to the screen edges put the left player's
 * health bar and spray can against the middle line (run 187q). While the
 * game sets such viewports, the HUD moves by half as much (v187s; not at
 * all in v187r, which left each view's HUD centred in it, run 187r). */
static volatile DWORD s_split_ms;
static volatile int s_split_seen;

extern void D3D8_D3DDevice_SetViewport_gen(void);

void D3D8_D3DDevice_SetViewport(void)        /* (const D3DVIEWPORT8 *) stdcall */
{
    uint32_t vp = MEM32(g_esp + 4);
    if (vp) {
        uint32_t w = MEM32(vp + 8), h = MEM32(vp + 12);
        if (w == 320 && (h == 480 || h == 240)) {
            s_split_ms = GetTickCount();
            if (!s_split_seen) {
                s_split_seen = 1;
                fprintf(stderr, "[WS] split screen: the HUD moves half as far\n");
            }
        }
    }
    D3D8_D3DDevice_SetViewport_gen();
}

static int ws_split(void)
{
    return s_split_seen && GetTickCount() - s_split_ms < 200;
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

/* v191b, after Piaka's widescreen notes: the letterbox bars. The Xbox adds
 * 0.53125 to every position (D3D's pixel-centre bias), so bars the game
 * draws from x = 0 and y = 0 start half a pixel in; the console still fills
 * column 0 and row 0, an upscaled picture does not, and the scene showed in
 * a line above the top bar and left of both (in 4:3 too). Their corners on
 * the screen's edges go one pixel past them (anchor 9, sub_0015A880).
 * Independent of widescreen; RECOMP_GL_EDGE=0 turns it off with v191's. */
static int ws_edge_on(void)
{
    static int on = -1;
    if (on < 0)
        on = ws_flag("RECOMP_GL_EDGE");
    return on;
}

#define WS_EDGE_SCOPE(name, addr)                   \
    extern void name##_gen(void);                   \
    void name(void)                                 \
    {                                               \
        if (!ws_edge_on()) { name##_gen(); return; } \
        ws_enter(addr);                             \
        name##_gen();                               \
        ws_leave();                                 \
    }

WS_EDGE_SCOPE(sub_000F7720, 0x000F7720u)
WS_EDGE_SCOPE(sub_000F7990, 0x000F7990u)

/* Sprite quad submission (the 0x1E144C quad-batch interface, +0x14): copies
 * one quad of 4 vertices, 32 bytes each, x first, to the batch's write
 * pointer at this+0x1C and advances it by 0x80. stdcall, `this` first. */
extern void sub_0015A880_gen(void);

static void ws_sprite_queue(void);

/* v208: RECOMP_MENU_LOG=1 (or RECOMP_SPRITE_LOG=1) -- F9 logs the next
 * RECOMP_SPRITE_LOG_N sprite quads queued (default 1500), one [SPRITE] line
 * each: the queue, the arguments, the quad's box in 640x480 (after any HUD
 * move), its four corners, the first vertex's other words (colour, texture
 * coordinates) and the code addresses found on the stack, nearest first --
 * the game functions that drew it. With RECOMP_DRAW_WHO's and
 * RECOMP_GL_DRAWLOG's snapshots (RECOMP_MENU_LOG turns all three on) a menu
 * can be taken apart piece by piece: which function draws which part, and
 * where, so each part can be moved to the edge it belongs to in widescreen
 * (as the HUD is) instead of guessing. Idea by Miggas. */
static void sprite_log(uint32_t obj, uint32_t a2, uint32_t a3, uint32_t before,
                       const uint32_t *ret, int nret)
{
    static int on = -1, was;
    static unsigned left, nmax, snap, idx;
    int k, v;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPRITE_LOG"), *m = getenv("RECOMP_MENU_LOG"),
                   *n = getenv("RECOMP_SPRITE_LOG_N");
        on = (e && e[0] == '1') || (m && m[0] == '1');
        nmax = n && atoi(n) > 0 ? (unsigned)atoi(n) : 1500u;
        if (on)
            fprintf(stderr, "[SPRITE] on: F9 logs the next %u sprite quads\n", nmax);
    }
    if (!on)
        return;
    {
        int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !was && !left) {
            left = nmax;
            idx = 0;
            fprintf(stderr, "[SPRITE] ---- snapshot %u (screen held 4:3: %s) ----\n", ++snap,
                    xbox_WidescreenK() > 0.9999 ? "yes or 4:3 display" : "no, wide");
        }
        was = f9;
    }
    if (!left)
        return;
    left--;
    if (g_eax != 0 || MEM32(obj + 0x1C) - before != 0x80) {
        fprintf(stderr, "[SPRITE] %4u queue %08X (%08X %08X) not queued (eax %08X)\n",
                idx++, obj, a2, a3, g_eax);
        return;
    }
    for (v = 0; v < 4; v++) {
        float x = MEMF(before + v * 32u), y = MEMF(before + v * 32u + 4);
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    fprintf(stderr, "[SPRITE] %4u queue %08X (%08X %08X) box %.1f..%.1f x %.1f..%.1f"
            " | v0 %08X %08X %08X %08X %08X %08X | calls:", idx++, obj, a2, a3, x0, x1, y0, y1,
            MEM32(before + 8), MEM32(before + 12), MEM32(before + 16), MEM32(before + 20),
            MEM32(before + 24), MEM32(before + 28));
    for (k = 0; k < nret; k++)
        fprintf(stderr, " %08X", ret[k]);
    fputc('\n', stderr);
}

void sub_0015A880(void)
{
    uint32_t obj = MEM32(g_esp + 4), a2 = MEM32(g_esp + 8), a3 = MEM32(g_esp + 12);
    uint32_t before = MEM32(obj + 0x1C), ret[10];
    int nret = 0, k;
    for (k = 0; k < 128 && nret < 10; k++) {   /* code addresses on the stack */
        uint32_t w = MEM32(g_esp + 4u * (uint32_t)k);
        if (w >= 0x00011000u && w < 0x001D0000u)
            ret[nret++] = w;
    }
    ws_sprite_queue();
    sprite_log(obj, a2, a3, before, ret, nret);
}

static void ws_sprite_queue(void)
{
    uint32_t obj, before, v;
    int anchor;
    float extra;
    if (ws_depth <= 0 || ws_depth > 32) {
        sub_0015A880_gen();
        return;
    }
    anchor = ws_stack[ws_depth - 1].anchor;
    if (anchor != 9 && !ws_hud_on()) {
        sub_0015A880_gen();
        return;
    }
    obj = MEM32(g_esp + 4);
    before = MEM32(obj + 0x1C);
    sub_0015A880_gen();
    if (anchor == 9) {                  /* v191b: letterbox bars */
        /* v196f: and widened by 1/k about the middle while widescreen is
         * on, so they reach the edges even when the renderer squeezes them
         * with the picture (Rokkaku Dai Heights: the bars came out 4:3 --
         * there they are not drawn as a plain full-width layer, which the
         * renderer leaves unsqueezed). Unsqueezed they now overshoot the
         * screen, which is clipped. After Piaka's patch. */
        double k = xbox_WidescreenK();      /* 1 in 4:3 and on 4:3-held screens */
        if (!(k > 0.2 && k < 0.9999)) k = 1.0;
        if (g_eax != 0 || MEM32(obj + 0x1C) - before != 0x80)
            return;
        for (v = 0; v < 4; ++v) {
            uint32_t p = before + v * 32u;
            float x0 = MEMF(p), y = MEMF(p + 4);
            float x = (float)(320.0 + (x0 - 320.0) / k);
            if (x0 <= 1.0f) x -= 1.0f; else if (x0 >= 639.0f) x += 1.0f;
            if (y <= 1.0f) y -= 1.0f; else if (y >= 479.0f) y += 1.0f;
            MEMF(p) = x;
            MEMF(p + 4) = y;
        }
        return;
    }
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
/* v209: a 4:3 screen that has been made wide (the map screen) is not held */
static int wsm_map_active(void);

#define WS_HOLD_43(name)                            \
    extern void name##_gen(void);                   \
    void name(void)                                 \
    {                                               \
        if (ws_hold_on() && !wsm_map_active())      \
            xbox_WidescreenHold43();                \
        xbox_MenuScreen();      /* v207b: depth */  \
        name##_gen();                               \
    }

WS_HOLD_43(sub_00077730)
WS_HOLD_43(sub_0011AC50)

/* ── v209: menus in widescreen, one at a time: the map screen ────────────
 *
 * From Miggas's RECOMP_MENU_LOG run (v208) of the pause menu's map: nearly
 * every part of it is drawn by sub_001129E0, each from a call site of its
 * own -- through the game's object draw (sub_00048570, or sub_000488B0 for
 * the plain quads), so the return address says which part it is -- and
 * every string through sub_0003D5D0 (x, y, depth, colour, scale x, scale y,
 * text), drawn later in one pass. The header ("Rokkaku-dai Heights" and
 * the line above it) comes from sub_000FD190, the garage menus' header,
 * from the menu frame start sub_00077730. The 3D map itself (sub_00114A80
 * and the functions under it) stays in the middle, squeezed like any 3D.
 *
 * While the map screen is shown and widescreen is on, the screen is no
 * longer held at 4:3, and each part is placed as in Miggas's mockup:
 *   left  (-1): moved left by half the added width, as the HUD's left items
 *   right (+1): moved right likewise
 *   wide  ( 2): drawn unsqueezed, stretched across the whole width (the
 *               background grid and the bars): marked for the renderer with
 *               a z-bias no title uses (4000000; nv2a_pb_exec.c), as the
 *               backdrop is (1000000)
 * Models are moved through the device's screen-space offset (as the HUD's
 * 3D icons), text by its x when it is added. RECOMP_WS_MENU_MAP=0 keeps
 * the map screen at 4:3 as before. */
static const struct { uint32_t ret; int where; } wsm_map_parts[] = {
    { 0x00112AFEu,  2 },    /* the background grid (4086 indices) */
    { 0x00112B21u,  2 },    /* top bar */
    { 0x00112B31u,  1 },    /* RETURN TO GAME bar */
    { 0x00112B45u,  2 },    /* bottom bar and its marks */
    { 0x00112B7Cu,  1 },    /* the four buttons on the right */
    { 0x00112C18u,  1 },
    { 0x00112C7Du,  1 },
    { 0x00112CE1u,  1 },
    { 0x00112D21u,  1 },    /* TOTAL panel and its icon */
    { 0x00112D7Du, -1 },    /* the counter box */
    { 0x00112E65u, -1 },    /* ROTATE MAP's stick */
    { 0x00112EECu, -1 },    /* CHANGE MAP's L and R */
    { 0x00112F2Du, -1 },
};
static const struct { uint32_t ret; int where; } wsm_map_text[] = {
    { 0x00112D5Cu,  1 },    /* the graffiti soul count, x 540 */
    { 0x00112DB8u, -1 },    /* the counter's two lines, x 116 */
    { 0x00112DFAu, -1 },
    { 0x00112E9Fu, -1 },    /* ROTATE MAP, x 48 */
    { 0x00112F67u, -1 },    /* CHANGE MAP, x 48 */
    { 0x00113003u,  1 },    /* RETURN TO GAME, y 410 */
};

static volatile DWORD s_wsm_map_ms;
static RECOMP_TLS int wsm_in_map, wsm_in_header;

static int wsm_on(void)
{
    static int on = -1;
    if (on < 0) {
        double a = xbox_PresentAspect();
        on = a > 4.0 / 3.0 + 1e-6 && ws_flag("RECOMP_WS_MENU_MAP");
        if (on)
            fprintf(stderr, "[WS] the map screen in widescreen (RECOMP_WS_MENU_MAP=0: 4:3)\n");
    }
    return on;
}

static int wsm_map_active(void)
{
    return wsm_on() && s_wsm_map_ms && GetTickCount() - s_wsm_map_ms < 200;
}

extern void D3D8_D3DDevice_SetRenderState_ZBias(void);

/* the game's own SetRenderState_ZBias, registers kept (it runs between two
 * guest instructions); the value in effect is kept at 0x19E2E8 */
static void wsm_zbias(uint32_t v)
{
    uint32_t r[10] = { g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp,
                       (uint32_t)g_df };
    double fp[8];
    int top = g_fp_top, cmp = g_fp_cmp;
    uint16_t cc = g_fp_cc, cw = g_fp_control_word;
    memcpy(fp, g_fp_stack, sizeof fp);
    PUSH32(g_esp, v);
    PUSH32(g_esp, 0);                       /* return address slot */
    D3D8_D3DDevice_SetRenderState_ZBias();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_esp = r[3]; g_ebx = r[4];
    g_esi = r[5]; g_edi = r[6]; g_ebp = r[7]; g_seh_ebp = r[8]; g_df = (int)r[9];
    memcpy(g_fp_stack, fp, sizeof fp);
    g_fp_top = top; g_fp_cmp = cmp; g_fp_cc = cc; g_fp_control_word = cw;
}

/* where the part drawn from this return address goes: 0 = as it is */
static int wsm_part(uint32_t ret)
{
    unsigned k;
    if (wsm_in_header)
        return -1;
    if (!wsm_in_map)
        return 0;
    for (k = 0; k < sizeof wsm_map_parts / sizeof wsm_map_parts[0]; k++)
        if (wsm_map_parts[k].ret == ret)
            return wsm_map_parts[k].where;
    return 0;
}

/* draw one part placed as `where` says */
static void wsm_draw(void (*gen)(void), int where)
{
    float extra = ws_half_extra();
    if (!where || extra == 0.0f) {
        gen();
        return;
    }
    if (where == 2) {
        uint32_t zb = MEM32(0x19E2E8u);
        wsm_zbias(4000000u);
        gen();
        wsm_zbias(zb);
        return;
    }
    {
        uint32_t device = MEM32(0x19DCE0u);
        float x, y;
        if (!device) {
            gen();
            return;
        }
        x = MEMF(device + 0x9E8u) - MEMF(0x19A040u);
        y = MEMF(device + 0x9ECu) - MEMF(0x19A040u);
        ws_set_screen_offset(x + (float)where * extra, y);
        gen();
        ws_set_screen_offset(x, y);
    }
}

extern void sub_001129E0_gen(void);
extern void sub_000FD190_gen(void);
extern void sub_00048570_gen(void);
extern void sub_000488B0_gen(void);
extern void sub_0003D5D0_gen(void);

void sub_001129E0(void)             /* the map screen's parts */
{
    if (!wsm_on()) {
        sub_001129E0_gen();
        return;
    }
    s_wsm_map_ms = GetTickCount();
    wsm_in_map++;
    sub_001129E0_gen();
    wsm_in_map--;
}

void sub_000FD190(void)             /* the garage menus' header */
{
    int on = wsm_map_active();
    if (on) wsm_in_header++;
    sub_000FD190_gen();
    if (on) wsm_in_header--;
}

void sub_00048570(void)             /* the game's object draw */
{
    int where = (wsm_in_map || wsm_in_header) ? wsm_part(MEM32(g_esp)) : 0;
    wsm_draw(sub_00048570_gen, where);
}

void sub_000488B0(void)             /* ...and its plain quads */
{
    int where = (wsm_in_map || wsm_in_header) ? wsm_part(MEM32(g_esp)) : 0;
    wsm_draw(sub_000488B0_gen, where);
}

void sub_0003D5D0(void)             /* add a string: x at esp+4 */
{
    int where = 0;
    unsigned k;
    if (wsm_in_header)
        where = -1;
    else if (wsm_in_map) {
        uint32_t ret = MEM32(g_esp);
        for (k = 0; k < sizeof wsm_map_text / sizeof wsm_map_text[0]; k++)
            if (wsm_map_text[k].ret == ret)
                where = wsm_map_text[k].where;
    }
    if (where) {
        float extra = ws_half_extra();
        MEMF(g_esp + 4) += (float)where * extra;
    }
    sub_0003D5D0_gen();
}
