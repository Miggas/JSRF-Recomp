/*
 * jsrf_graffiti.c -- v310: bigger graffiti (Idea by Miggas).
 *
 * The game does not draw a graffiti from its own file: at a stage's start
 * the graffiti manager (sub_0002DBE0's states) packs them into 1024x1024
 * DXT3 sheets, four bands of 256 pixels a sheet, each band holding one
 * graffiti of each size: S (128x128, two of them), M (384x256), L
 * (512x256). The slot of each size is in a table at 0x1EE5A0 (16 bytes a
 * size: x, y, width, height in 128-pixel cells; the band adds 2 to y).
 * sub_0002DA70(size, band) copies the graffiti just read (the .prs, plain
 * DXT3 of the slot's size, at manager +0x1888, its file size at +0x17C)
 * into the sheet in memory ([0x230DBC], 1024 four-byte words a row of
 * blocks), or the player's own graffiti for a custom slot; sub_0002D8D0
 * writes the finished sheet to the cache drive (Z:\Media\Mark\DEFAULT\
 * JSRF_TEXS0/1.JTX), which a stage then loads into its sheet textures.
 * Everything there is a fixed size, so a bigger .prs came out scrambled.
 *
 * Now a .prs 2, 4 or 8 times as wide and tall as its slot (the mod tool
 * makes them, m16) goes into the game's sheet made small again, so the
 * game sees what it always did, and into a sheet of our own as big as the
 * biggest graffiti in it needs, the rest of it the game's sheet made
 * bigger. When the game writes its sheet, ours is handed to the renderer
 * as the stand-in for it (nv2a_gl_tex_standin, by the sheet's contents):
 * the sheet texture the stage loads goes to the GPU as ours. Texture
 * coordinates are 0..1, so walls, the HUD and menus show the detail with
 * nothing else changed. RECOMP_HIRES_GRAFFITI=0 turns it off.
 */

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


void nv2a_gl_tex_standin(const void *guest, size_t n, int w, int h, const void *data, size_t size);
void nv2a_gl_dxt3_shrink(const void *src, int w, int h, int k, void *dst);
void nv2a_gl_tex_standin_rgba(const void *small, int sw, int sh, const void *big, int w, int h);
void nv2a_gl_tex_standin_bgra(const void *guest, size_t n, int w, int h, const void *bgra);
void nv2a_gl_dxt3_decode(const void *src, int w, int h, void *bgra);
#if defined(_WIN32)
BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);
#else
BOOL xbox_translate_path(const char *xbox_path, char *host_path_buf, DWORD buf_size);
#endif

#define SLOTS       0x001EE5A0u       /* 16 bytes a graffiti size */
#define SHEET_PTR   0x00230DBCu       /* the sheet being packed */
#define GRF_NAMES   0x001EF988u       /* the graffiti select's .grf files, by number */
#define PRS_NAMES   0x001EE218u       /* the .prs files, by graffiti number */
#define CUSTOM_OBJ  0x001EFFB0u       /* sub_0003A870's this: is this slot the player's own */
#define RET_MARK    0x0007BDD0u       /* return address our own guest calls push */
#define SHEET_PX    1024
#define SHEET_BYTES (1024u * 1024u)   /* DXT3: a byte a pixel */

extern void sub_0002DA70_gen(void);
extern void sub_0002D8D0_gen(void);
extern void sub_0003A870(void);
extern void sub_0003A820(void);
extern void sub_00038DF0_gen(void);

static int on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_HIRES_GRAFFITI");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* the graffiti put in since the last sheet was written: big ones only */
#define MAXP 16
typedef struct {
    int x, y, w, h;     /* the slot, in pixels of the game's sheet */
    int k;              /* how much bigger */
    uint8_t *dxt;       /* (w * k) x (h * k), DXT3, or NULL */
    uint8_t *bgra;      /* v310i: or as well, from the .grf beside the .prs (v310j): B8G8R8A8 */
} Piece;
static Piece s_p[MAXP];
static int s_np;

static void pieces_clear(void)
{
    int i;
    for (i = 0; i < s_np; i++) { free(s_p[i].dxt); free(s_p[i].bgra); }
    s_np = 0;
}

/* DXT3 blocks: 16 bytes, 8 of alpha (4 bits a pixel, rows of 16 bits),
 * 4 of colours, 4 of indices (2 bits a pixel, a byte a row). One pixel
 * (px, py) of block `in` put at (qx, qy) of block `out`. */
static void px_copy(const uint8_t *in, int px, int py, uint8_t *out, int qx, int qy)
{
    int a = (in[px >> 1 | py << 1] >> ((px & 1) * 4)) & 15;
    int c = (in[12 + py] >> (px * 2)) & 3;
    uint8_t *ab = &out[qx >> 1 | qy << 1];
    *ab = (uint8_t)((*ab & ~(15 << ((qx & 1) * 4))) | a << ((qx & 1) * 4));
    out[12 + qy] = (uint8_t)((out[12 + qy] & ~(3 << (qx * 2))) | c << (qx * 2));
}

/* src (sw x sh pixels, DXT3, rows of blocks one after another) made k times
 * bigger into dst (rows dpitch bytes), each pixel a k x k square. Each new
 * block takes its colours from the block its pixels come from (k >= 2: a
 * new block's pixels all come from one old block), so nothing is lost. */
static void dxt_up(const uint8_t *src, int sw, int sh, int k, uint8_t *dst, size_t dpitch)
{
    int bx, by, nbw = sw * k / 4, nbh = sh * k / 4;
    for (by = 0; by < nbh; by++)
        for (bx = 0; bx < nbw; bx++) {
            /* the new block's pixel (0,0) is old pixel (bx*4/k, by*4/k) */
            int ox = bx * 4 / k, oy = by * 4 / k, x, y;
            const uint8_t *in = src + ((size_t)(oy / 4) * (size_t)(sw / 4) + (size_t)(ox / 4)) * 16;
            uint8_t *out = dst + (size_t)by * dpitch + (size_t)bx * 16;
            memcpy(out + 8, in + 8, 4);     /* its colours */
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    px_copy(in, (bx * 4 + x) / k - (ox & ~3), (by * 4 + y) / k - (oy & ~3), out, x, y);
        }
}

/* is the slot about to be filled the player's own graffiti (sub_0003A870,
 * thiscall on CUSTOM_OBJ, the manager's +0x1894, +0x1898, +0x1890, ret 12) */
static int slot_custom(uint32_t mgr)
{
    uint32_t sv_ecx = g_ecx, r;
    g_ecx = CUSTOM_OBJ;
    PUSH32(g_esp, MEM32(mgr + 0x1890u));
    PUSH32(g_esp, MEM32(mgr + 0x1898u));
    PUSH32(g_esp, MEM32(mgr + 0x1894u));
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(0x0003A870u, sub_0003A870);
    r = g_eax;
    g_ecx = sv_ecx;
    return r != 0;
}

/* the graffiti number of the slot about to be filled (sub_0003A820, the
 * same call as sub_0003A870's) */
static uint32_t slot_number(uint32_t mgr)
{
    uint32_t sv_ecx = g_ecx, r;
    g_ecx = CUSTOM_OBJ;
    PUSH32(g_esp, MEM32(mgr + 0x1890u));
    PUSH32(g_esp, MEM32(mgr + 0x1898u));
    PUSH32(g_esp, MEM32(mgr + 0x1894u));
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(0x0003A820u, sub_0003A820);
    r = g_eax;
    g_ecx = sv_ecx;
    return r;
}

/* v310j: the .grf beside .prs number num (the same graffiti, as the
 * graffiti select's picture: a TGA, 32 bits a pixel, nothing lost), if it
 * is 2, 4 or 8 times sw x sh. B8G8R8A8 rows top first, its times in *k.
 * (v310i read a .png the mod tool wrote there instead.) */
static uint8_t *prs_grf(uint32_t num, uint32_t sw, uint32_t sh, int *k)
{
    char xp[160];
    uint32_t name = num < 4096u ? MEM32(PRS_NAMES + num * 4u) : 0u;
    int i, n;
    char *dot;
    FILE *f = NULL;
    uint8_t hd[18], *o;
    uint32_t w, h, y;
    size_t row;
    if (!name)
        return NULL;
    for (i = 0; i < (int)sizeof xp - 5; i++) {
        xp[i] = (char)MEM8(name + (uint32_t)i);
        if (!xp[i]) break;
    }
    xp[i] = 0;
    dot = strrchr(xp, '.');
    if (!dot || (dot[1] | 32) != 'p' || (dot[2] | 32) != 'r' || (dot[3] | 32) != 's' || dot[4])
        return NULL;
    strcpy(dot, ".grf");
    /* v310k: the names say A:, which the game reads from the disc (D:) */
    if ((xp[0] | 32) == 'a' && xp[1] == ':')
        xp[0] = 'D';
    {
#if defined(_WIN32)
        WCHAR hp[1024];
        if (xbox_translate_path(xp, hp, 1024)) f = _wfopen(hp, L"rb");
#else
        char hp[1024];
        if (xbox_translate_path(xp, hp, (DWORD)sizeof hp)) f = fopen(hp, "rb");
#endif
    }
    if (!f) {
        fprintf(stderr, "[GRAFFITI] %s (graffiti %u): not opened\n", xp, num);
        return NULL;
    }
    if (fread(hd, 1, 18, f) != 18 || hd[2] != 2 || hd[16] != 32) {
        fprintf(stderr, "[GRAFFITI] %s: not a 32-bit TGA\n", xp);
        fclose(f);
        return NULL;
    }
    w = (uint32_t)(hd[12] | hd[13] << 8);
    h = (uint32_t)(hd[14] | hd[15] << 8);
    for (n = 2; n <= 8; n *= 2)
        if (w == sw * (uint32_t)n && h == sh * (uint32_t)n) break;
    if (n > 8) {                        /* its own size (or another): nothing to add */
        if (w != sw || h != sh)
            fprintf(stderr, "[GRAFFITI] %s is %ux%u, not 2, 4 or 8 times %ux%u: not used\n", xp, w, h, sw, sh);
        fclose(f);
        return NULL;
    }
    row = (size_t)w * 4u;
    o = (uint8_t *)malloc(row * h);
    if (!o || fseek(f, 18L + hd[0], SEEK_SET)) {
        free(o);
        fclose(f);
        return NULL;
    }
    for (y = 0; y < h; y++) {           /* TGA rows go bottom up unless bit 5 says not */
        uint32_t dy = (hd[17] & 0x20) ? y : h - 1u - y;
        if (fread(o + (size_t)dy * row, 1, row, f) != row) {
            free(o);
            fclose(f);
            return NULL;
        }
    }
    fclose(f);
    *k = n;
    return o;
}

/* sub_0002DA70(size, band): thiscall on the manager, ret 8 */
void sub_0002DA70(void)
{
    const uint32_t mgr = g_ecx;
    const uint32_t size = MEM32(g_esp + 4u), band = MEM32(g_esp + 8u);
    uint32_t buf, have, need, sx, sy, sw, sh;
    int k;
    if (!on() || size > 7u || !mgr) {
        sub_0002DA70_gen();
        return;
    }
    sx = MEM32(SLOTS + size * 16u) * 128u;
    sy = (MEM32(SLOTS + size * 16u + 4u) + band * 2u) * 128u;
    sw = MEM32(SLOTS + size * 16u + 8u) * 128u;
    sh = MEM32(SLOTS + size * 16u + 12u) * 128u;
    buf = MEM32(mgr + 0x1888u);
    have = MEM32(mgr + 0x17Cu);
    need = sw * sh;                             /* DXT3: a byte a pixel */
    {
        /* v310c: what each slot got, the first 64 */
        static int logged;
        if (logged < 64) {
            logged++;
            fprintf(stderr, "[GRAFFITI] slot size %u band %u: %ux%u at %u,%u, file %u bytes (the slot's: %u), buffer 0x%08X\n",
                    size, band, sw, sh, sx, sy, have, need, buf);
        }
    }
    if (!buf || !need || sx + sw > SHEET_PX || sy + sh > SHEET_PX || have < need || s_np >= MAXP
            || slot_custom(mgr)) {
        sub_0002DA70_gen();
        return;
    }
    for (k = 2; k <= 8; k *= 2)
        if (have == need * (uint32_t)(k * k)) break;
    {
        int pk = 0;
        uint8_t *pic = prs_grf(slot_number(mgr), sw, sh, &pk);
        uint8_t *big = NULL;
        if (k <= 8) {
            /* keep the big one; the game's copy made small, in place */
            uint8_t *small = (uint8_t *)malloc(need);
            big = (uint8_t *)malloc(have);
            if (big && small) {
                memcpy(big, (const uint8_t *)XBOX_PTR(buf), have);
                nv2a_gl_dxt3_shrink(big, (int)sw * k, (int)sh * k, k, small);
                memcpy((uint8_t *)XBOX_PTR(buf), small, need);
            } else {
                free(big);
                big = NULL;
            }
            free(small);
        }
        if (big || pic) {
            Piece *p = &s_p[s_np++];
            p->x = (int)sx; p->y = (int)sy; p->w = (int)sw; p->h = (int)sh;
            p->dxt = big && (!pic || pk == k) ? big : NULL;
            if (big && !p->dxt) free(big);
            p->bgra = pic;
            p->k = pic ? pk : k;
            fprintf(stderr, "[GRAFFITI] a %ux%u graffiti for the %ux%u slot at %u,%u (x%d%s)\n",
                    sw * (uint32_t)p->k, sh * (uint32_t)p->k, sw, sh, sx, sy, p->k,
                    pic ? ", lossless from its .grf" : "");
        }
    }
    sub_0002DA70_gen();
}

/* the big sheet, DXT3, from the game's (g) and the pieces with DXT3 */
static void sheet_dxt(const uint8_t *g, int k)
{
    size_t pitch = (size_t)SHEET_PX * (size_t)k / 4u * 16u;
    uint8_t *big = (uint8_t *)malloc(pitch * (size_t)(SHEET_PX * k / 4));
    int i, row;
    if (!big)
        return;
    dxt_up(g, SHEET_PX, SHEET_PX, k, big, pitch);
    for (i = 0; i < s_np; i++) {
        const Piece *p = &s_p[i];
        int f = k / p->k;
        uint8_t *at = big + (size_t)(p->y * k / 4) * pitch + (size_t)(p->x * k / 4) * 16u;
        if (!p->dxt)
            continue;
        if (f == 1) {
            size_t rb = (size_t)(p->w * p->k / 4) * 16u;
            for (row = 0; row < p->h * p->k / 4; row++)
                memcpy(at + (size_t)row * pitch, p->dxt + (size_t)row * rb, rb);
        } else {
            dxt_up(p->dxt, p->w * p->k, p->h * p->k, f, at, pitch);
        }
    }
    nv2a_gl_tex_standin(g, SHEET_BYTES, SHEET_PX * k, SHEET_PX * k, big, pitch * (size_t)(SHEET_PX * k / 4));
    free(big);
}

/* v310i: the big sheet uncompressed, when a piece has its .grf: the game's
 * sheet decoded and spread out, the pieces over it (their .grf, else their
 * DXT3 decoded) */
static void sheet_bgra(const uint8_t *g, int k)
{
    const size_t W = (size_t)SHEET_PX * (size_t)k;
    uint32_t *small = (uint32_t *)malloc((size_t)SHEET_PX * SHEET_PX * 4u);
    uint32_t *big = (uint32_t *)malloc(W * W * 4u);
    size_t x, y;
    int i;
    if (!small || !big) {
        free(small);
        free(big);
        sheet_dxt(g, k);
        return;
    }
    nv2a_gl_dxt3_decode(g, SHEET_PX, SHEET_PX, small);
    for (y = 0; y < W; y++) {
        const uint32_t *src = small + (y / (size_t)k) * SHEET_PX;
        uint32_t *dst = big + y * W;
        for (x = 0; x < W; x++) dst[x] = src[x / (size_t)k];
    }
    for (i = 0; i < s_np; i++) {
        const Piece *p = &s_p[i];
        const size_t pw = (size_t)(p->w * p->k), ph = (size_t)(p->h * p->k);
        const int f = k / p->k;
        uint32_t *pic = (uint32_t *)p->bgra, *dec = NULL;
        if (!pic && p->dxt) {
            dec = (uint32_t *)malloc(pw * ph * 4u);
            if (dec) nv2a_gl_dxt3_decode(p->dxt, (int)pw, (int)ph, dec);
            pic = dec;
        }
        if (!pic)
            continue;
        for (y = 0; y < ph * (size_t)f; y++) {
            uint32_t *dst = big + ((size_t)p->y * (size_t)k + y) * W + (size_t)p->x * (size_t)k;
            const uint32_t *src = pic + (y / (size_t)f) * pw;
            for (x = 0; x < pw * (size_t)f; x++) dst[x] = src[x / (size_t)f];
        }
        free(dec);
    }
    nv2a_gl_tex_standin_bgra(g, SHEET_BYTES, (int)W, (int)W, big);
    free(small);
    free(big);
}

/* sub_0002D8D0: the finished sheet written to the cache drive (thiscall,
 * no arguments). Our big one built from it and given to the renderer. */
void sub_0002D8D0(void)
{
    uint32_t sheet = MEM32(SHEET_PTR);
    if (on() && s_np && sheet) {
        const uint8_t *g = (const uint8_t *)XBOX_PTR(sheet);
        int k = 1, i, lossless = 0;
        for (i = 0; i < s_np; i++) {
            if (s_p[i].k > k) k = s_p[i].k;
            lossless |= s_p[i].bgra != NULL;
        }
        if (lossless && k <= 4)
            sheet_bgra(g, k);
        else
            sheet_dxt(g, k);
        fprintf(stderr, "[GRAFFITI] sheet %u: %d bigger graffiti, %dx%d%s (the game's at 0x%08X)\n",
                MEM32(g_ecx + 0x1890u), s_np, SHEET_PX * k, SHEET_PX * k,
                lossless && k <= 4 ? ", uncompressed" : "", sheet);
    }
    pieces_clear();
    sub_0002D8D0_gen();
}

/* v310d: the graffiti select's pictures. sub_00038DF0 (a method of the
 * menu's loader, thiscall, no arguments) reads .grf number +0x187C of the
 * names at GRF_NAMES (A:\Media\Mark\TEX\grf_<size>_<nn>.grf and the like)
 * into a buffer (+0x1874, file size +0x17C), state +0x1880 0 -> 1 -> 2
 * (all in one call when the read ends at once, as it does here).
 * The .grf is a TGA (32 bits a pixel, uncompressed) of its slot's size,
 * which the menu takes for granted: a bigger one came out scrambled, in the
 * menu and on its wall. Now, once read, a .grf 2, 4 or 8 times its slot is
 * made that size again (k x k boxes averaged), its header and size with it.
 * v310e: the big one goes to the renderer as the stand-in for the
 * texture the menu makes of it, matched by its texels. */
static int grf_size_class(uint32_t name_va)
{
    char nm[96];
    const char *b, *u;
    int i, c;
    for (i = 0; i < (int)sizeof nm - 1; i++) {
        nm[i] = (char)MEM8(name_va + (uint32_t)i);
        if (!nm[i]) break;
    }
    nm[i] = 0;
    b = strrchr(nm, '\\');
    b = b ? b + 1 : nm;
    u = strchr(b, '_');                         /* grf_0_01, rpolice_04_31 */
    if (!u || u[1] < '0' || u[1] > '9') return -1;
    c = atoi(u + 1);
    return c >= 0 && c <= 7 ? c : -1;
}

void sub_00038DF0(void)
{
    const uint32_t obj = g_ecx;
    const uint32_t before = MEM32(obj + 0x1880u);
    uint32_t num, buf, have, sw, sh, w, h, idl, px, tail, nsz, x, y;
    int c, k;
    uint8_t *p, *big;
    sub_00038DF0_gen();
    if (!on() || before == 2u || MEM32(obj + 0x1880u) != 2u)   /* v310f: 0 -> 2 in one call when the read is at once */
        return;
    num = MEM32(obj + 0x187Cu);
    buf = MEM32(obj + 0x1874u);
    have = MEM32(obj + 0x17Cu);
    if (num >= 175u || !buf || have < 18u)
        return;
    c = grf_size_class(MEM32(GRF_NAMES + num * 4u));
    if (c < 0)
        return;
    p = (uint8_t *)XBOX_PTR(buf);
    if (p[2] != 2 || p[16] != 32)               /* not a plain 32-bit TGA */
        return;
    idl = p[0];
    w = (uint32_t)(p[12] | p[13] << 8);
    h = (uint32_t)(p[14] | p[15] << 8);
    sw = MEM32(SLOTS + (uint32_t)c * 16u + 8u) * 128u;
    sh = MEM32(SLOTS + (uint32_t)c * 16u + 12u) * 128u;
    if (w != sw || h != sh)
        fprintf(stderr, "[GRAFFITI] menu picture %u: %ux%u, file %u bytes, its slot %ux%u\n", num, w, h, have, sw, sh);
    if (!sw || !sh || w <= sw)
        return;
    for (k = 2; k <= 8; k *= 2)
        if (w == sw * (uint32_t)k && h == sh * (uint32_t)k) break;
    if (k > 8 || 18u + idl + w * h * 4u > have)
        return;
    px = 18u + idl;
    tail = have - px - w * h * 4u;              /* a TGA footer, kept */
    big = (uint8_t *)malloc((size_t)w * h * 4u);
    if (big)
        memcpy(big, p + px, (size_t)w * h * 4u);
    for (y = 0; y < sh; y++)                    /* in place: row y only reads rows >= y*k */
        for (x = 0; x < sw; x++) {
            uint32_t sum[4] = {0, 0, 0, 0}, i, j, q;
            for (j = 0; j < (uint32_t)k; j++)
                for (i = 0; i < (uint32_t)k; i++) {
                    const uint8_t *s = p + px + ((size_t)(y * k + j) * w + (x * k + i)) * 4u;
                    for (q = 0; q < 4; q++) sum[q] += s[q];
                }
            for (q = 0; q < 4; q++)
                p[px + ((size_t)y * sw + x) * 4u + q] = (uint8_t)((sum[q] + (uint32_t)(k * k) / 2u) / (uint32_t)(k * k));
        }
    memmove(p + px + sw * sh * 4u, p + px + w * h * 4u, tail);
    nsz = px + sw * sh * 4u + tail;
    p[12] = (uint8_t)sw; p[13] = (uint8_t)(sw >> 8);
    p[14] = (uint8_t)sh; p[15] = (uint8_t)(sh >> 8);
    MEM32(obj + 0x17Cu) = nsz;
    if (big) {
        /* v310e: the big one for the renderer, as the texture's texels
         * would be if the menu keeps the file's rows as they are, and
         * turned over (TGA rows go bottom up) */
        uint8_t *sf = (uint8_t *)malloc((size_t)sw * sh * 4u), *bf = (uint8_t *)malloc((size_t)w * h * 4u);
        nv2a_gl_tex_standin_rgba(p + px, (int)sw, (int)sh, big, (int)w, (int)h);
        if (sf && bf) {
            for (y = 0; y < sh; y++)
                memcpy(sf + (size_t)y * sw * 4u, p + px + (size_t)(sh - 1u - y) * sw * 4u, (size_t)sw * 4u);
            for (y = 0; y < h; y++)
                memcpy(bf + (size_t)y * w * 4u, big + (size_t)(h - 1u - y) * w * 4u, (size_t)w * 4u);
            nv2a_gl_tex_standin_rgba(sf, (int)sw, (int)sh, bf, (int)w, (int)h);
        }
        free(sf);
        free(bf);
        free(big);
    }
    fprintf(stderr, "[GRAFFITI] menu picture %u: %ux%u made %ux%u (x%d)\n", num, w, h, sw, sh, k);
}
