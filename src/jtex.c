/* JSRF game files' textures: see jtex.h */
#include "jtex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

const char *jt_fmt_name(int fmt)
{
    switch (fmt) {
    case JT_DXT1: return "DXT1";
    case JT_DXT3: return "DXT3";
    case JT_DXT5: return "DXT5";
    case JT_ARGB: return "ARGB";
    case JT_TGA:  return "TGA";
    }
    return "?";
}

size_t jt_level_size(int fmt, int w, int h)
{
    size_t bw = (size_t)(w + 3) / 4, bh = (size_t)(h + 3) / 4;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    switch (fmt) {
    case JT_DXT1: return bw * bh * 8;
    case JT_DXT3: case JT_DXT5: return bw * bh * 16;
    default: return (size_t)w * h * 4;
    }
}

static size_t levels_size(int fmt, int w, int h, int levels)
{
    size_t n = 0;
    int i;
    for (i = 0; i < levels; i++) {
        n += jt_level_size(fmt, w, h);
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
    }
    return n;
}

static int is_pow2_size(int v) { return v >= 4 && v <= 4096 && !(v & (v - 1)); }

/* an item at buf[s, e) that is a texture: added */
static void add_item(JFile *f, size_t s, size_t e, int *cap)
{
    const uint8_t *b = f->buf + s;
    int res, fmt, levels;
    size_t need;
    JTex t;
    if (e <= s || e - s < 40 || e > f->size) return;
    if (u32(b) == 0x46464952u || u32(b) == 0x424C444Du || u32(b + 24) == 0x424C444Du) return;   /* RIFF, MDLB */
    res = (int)u32(b + 20);
    if (!is_pow2_size(res)) return;
    switch (b[24]) {
    case 5: fmt = JT_DXT1; break;
    case 6: fmt = JT_DXT3; break;
    case 7: fmt = JT_DXT5; break;
    default: return;
    }
    levels = b[28] ? b[28] : 1;
    need = levels_size(fmt, res, res, levels);
    if (need > e - s - 32) {                    /* not all its levels: the first only? */
        if (jt_level_size(fmt, res, res) > e - s - 32) return;
        levels = 1;
    }
    memset(&t, 0, sizeof t);
    t.fmt = fmt;
    t.w = t.h = res;
    t.levels = levels;
    t.swizzled = b[26] == 1;
    t.alpha = b[25] == 1;
    t.id = u32(b);
    t.hdr = s;
    t.data = s + 32;
    t.len = e - s - 32;
    if (f->ntex == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        f->tex = (JTex *)realloc(f->tex, sizeof(JTex) * (size_t)*cap);
    }
    f->tex[f->ntex++] = t;
}

static void norm_items(JFile *f, size_t start, size_t end, int *cap)
{
    size_t off = start + 16;
    int guard = 0;
    while (off + 8 <= end && off + 8 <= f->size && guard++ < 100000) {
        size_t s = u32(f->buf + off), e = u32(f->buf + off + 4);
        if (!e || start + e <= off) break;
        if (s && s <= e) add_item(f, start + s, start + e, cap);
        off = start + e;
    }
}

static void indexed_items(JFile *f, int *cap)
{
    size_t off = 0;
    int guard = 0;
    while (off + 28 < f->size && guard++ < 100000) {
        uint32_t t = u32(f->buf + off);
        size_t end;
        if (t == 1) {
            size_t n = u32(f->buf + off + 16);
            end = off + u32(f->buf + off + 8) + 20 + n * 4;
        } else if (t == 0) {
            end = off + u32(f->buf + off + 4) + 8;
            add_item(f, off + 8, end, cap);
        } else {
            end = off + t + 4;
            add_item(f, off + 4, end, cap);
        }
        if (end <= off) break;
        off = end;
    }
}

static int ends_with(const char *s, const char *x)
{
    size_t a = strlen(s), b = strlen(x), i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) {
        char c = s[a - b + i], d = x[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != d) return 0;
    }
    return 1;
}

static void add_raw(JFile *f, int fmt, int w, int h, size_t data, int *cap)
{
    JTex t;
    memset(&t, 0, sizeof t);
    t.fmt = fmt;
    t.w = w;
    t.h = h;
    t.levels = 1;
    t.alpha = 1;
    t.hdr = (size_t)-1;
    t.data = data;
    t.len = f->size - data;
    *cap = 1;
    f->tex = (JTex *)malloc(sizeof(JTex));
    f->tex[0] = t;
    f->ntex = 1;
}

int jf_load(JFile *f, const char *path)
{
    FILE *fp;
    long n;
    uint8_t *buf;
    size_t got;
    fp = fopen(path, "rb");
    if (!fp) { memset(f, 0, sizeof *f); return -1; }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > 256L * 1024 * 1024) { fclose(fp); memset(f, 0, sizeof *f); return -1; }
    buf = (uint8_t *)malloc((size_t)n);
    got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    return jf_parse(f, path, buf, got);
}

int jf_parse(JFile *f, const char *path, uint8_t *buf, size_t size)
{
    int cap = 0;
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->buf = buf;
    f->size = size;
    if (f->size < 32) { f->kind = "too small"; return 0; }
    if (!memcmp(f->buf, "MULT", 4)) {
        size_t off = 16;
        int guard = 0;
        f->kind = "MULT";
        while (off + 8 <= f->size && guard++ < 100000) {
            size_t s = u32(f->buf + off), e = u32(f->buf + off + 4);
            if (e <= off || s >= f->size) break;
            if (s + 4 <= f->size && !memcmp(f->buf + s, "NORM", 4)) norm_items(f, s, e, &cap);
            off = e;
        }
    } else if (!memcmp(f->buf, "NORM", 4)) {
        f->kind = "NORM";
        norm_items(f, 0, f->size, &cap);
    } else if (ends_with(path, ".grf") && f->buf[2] == 2 && f->buf[16] == 32) {
        f->kind = "graffiti (TGA)";
        add_raw(f, JT_TGA, f->buf[12] | f->buf[13] << 8, f->buf[14] | f->buf[15] << 8, 18u + f->buf[0], &cap);
    } else if (ends_with(path, ".prs") && f->size == 16384) {
        f->kind = "graffiti (DXT3)";
        add_raw(f, JT_DXT3, 128, 128, 0, &cap);
    } else if (ends_with(path, ".jtx") && f->size == 1048576) {
        f->kind = "graffiti sheet (DXT3)";
        add_raw(f, JT_DXT3, 1024, 1024, 0, &cap);
    } else if (u32(f->buf) <= 1u) {
        f->kind = "indexed";
        indexed_items(f, &cap);
    } else {
        f->kind = "other";
    }
    return 0;
}

void jf_free(JFile *f)
{
    free(f->buf);
    free(f->tex);
    memset(f, 0, sizeof *f);
}

int jf_save(const JFile *f, const char *path)
{
    FILE *fp = fopen(path, "wb");
    int ok;
    if (!fp) return -1;
    ok = fwrite(f->buf, 1, f->size, fp) == f->size;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

/* ---- decoding ---- */

static void rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* a DXT colour block into out (4x4 rgba), alpha left alone unless 3-colour */
static void color_block(const uint8_t *b, uint8_t out[16][4], int dxt1)
{
    uint8_t c[4][4];
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint32_t idx = u32(b + 4);
    int i, k;
    rgb565(c0, c[0]); rgb565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = c[3][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (k = 0; k < 3; k++) {
            c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
            c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
        }
    } else {
        for (k = 0; k < 3; k++) {
            c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
            c[3][k] = 0;
        }
        c[3][3] = 0;
    }
    for (i = 0; i < 16; i++) {
        int s = (idx >> (2 * i)) & 3;
        out[i][0] = c[s][0]; out[i][1] = c[s][1]; out[i][2] = c[s][2];
        if (dxt1) out[i][3] = c[s][3];
    }
}

static void dxt5_alpha(const uint8_t *b, uint8_t out[16][4])
{
    uint8_t a[8];
    uint64_t bits = 0;
    int i;
    a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else {
        for (i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    for (i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (i = 0; i < 16; i++) out[i][3] = a[(bits >> (3 * i)) & 7];
}

/* x, y -> the Xbox's swizzled index (square, power of two) */
static size_t swz(unsigned x, unsigned y, unsigned w)
{
    size_t r = 0;
    unsigned bit = 0, m;
    for (m = 1; m < w; m <<= 1) {
        if (x & m) r |= (size_t)1 << bit;
        bit++;
        if (y & m) r |= (size_t)1 << bit;
        bit++;
    }
    return r;
}

int jt_decode(const JFile *f, const JTex *t, uint8_t *rgba)
{
    const uint8_t *d = f->buf + t->data;
    int x, y, i;
    if (t->len < jt_level_size(t->fmt == JT_TGA ? JT_ARGB : t->fmt, t->w, t->h)) return -1;
    if (t->fmt == JT_TGA || t->fmt == JT_ARGB) {
        const int top = t->fmt == JT_TGA ? (f->buf[17] & 0x20) != 0 : 1;
        for (y = 0; y < t->h; y++)
            for (x = 0; x < t->w; x++) {
                size_t si = t->swizzled ? swz((unsigned)x, (unsigned)y, (unsigned)t->w)
                                        : (size_t)(top ? y : t->h - 1 - y) * t->w + x;
                const uint8_t *p = d + si * 4;
                uint8_t *o = rgba + ((size_t)y * t->w + x) * 4;
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];   /* BGRA */
            }
        return 0;
    }
    for (y = 0; y < t->h; y += 4)
        for (x = 0; x < t->w; x += 4) {
            uint8_t px[16][4];
            const uint8_t *b = d + ((size_t)(y / 4) * ((t->w + 3) / 4) + x / 4) * (t->fmt == JT_DXT1 ? 8 : 16);
            if (t->fmt == JT_DXT1) color_block(b, px, 1);
            else {
                color_block(b + 8, px, 0);
                if (t->fmt == JT_DXT3)
                    for (i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                else dxt5_alpha(b, px);
            }
            for (i = 0; i < 16; i++) {
                int px_x = x + (i & 3), px_y = y + i / 4;
                if (px_x < t->w && px_y < t->h) memcpy(rgba + ((size_t)px_y * t->w + px_x) * 4, px[i], 4);
            }
        }
    return 0;
}

/* ---- encoding ---- */

static void dxt1_block(uint8_t *o, uint8_t px[16][4], int punch)
{
    int i, holes = 0;
    if (punch) for (i = 0; i < 16; i++) holes += px[i][3] < 128;
    if (!holes) {
        stb_compress_dxt_block(o, &px[0][0], 0, STB_DXT_HIGHQUAL);
        return;
    }
    {
        /* 3-colour mode: c0 <= c1, index 3 transparent */
        uint8_t tmp[16][4], blk[8], c[3][3];
        uint16_t a, b;
        uint32_t idx = 0;
        int k, first = -1;
        for (i = 0; i < 16; i++) if (px[i][3] >= 128) { first = i; break; }
        for (i = 0; i < 16; i++) memcpy(tmp[i], px[i][3] >= 128 || first < 0 ? px[i] : px[first], 4);
        stb_compress_dxt_block(blk, &tmp[0][0], 0, STB_DXT_HIGHQUAL);
        a = (uint16_t)(blk[0] | blk[1] << 8);
        b = (uint16_t)(blk[2] | blk[3] << 8);
        if (a > b) { uint16_t s = a; a = b; b = s; }
        rgb565(a, c[0]); rgb565(b, c[1]);
        for (k = 0; k < 3; k++) c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
        for (i = 0; i < 16; i++) {
            int s = 3;
            if (px[i][3] >= 128) {
                int best = 1 << 30, j;
                for (j = 0; j < 3; j++) {
                    int dr = px[i][0] - c[j][0], dg = px[i][1] - c[j][1], db = px[i][2] - c[j][2];
                    int e = dr * dr + dg * dg + db * db;
                    if (e < best) { best = e; s = j == 2 ? 2 : j; }
                }
            }
            idx |= (uint32_t)s << (2 * i);
        }
        o[0] = (uint8_t)a; o[1] = (uint8_t)(a >> 8); o[2] = (uint8_t)b; o[3] = (uint8_t)(b >> 8);
        o[4] = (uint8_t)idx; o[5] = (uint8_t)(idx >> 8); o[6] = (uint8_t)(idx >> 16); o[7] = (uint8_t)(idx >> 24);
    }
}

static void encode_level(uint8_t *o, const uint8_t *rgba, int w, int h, int fmt, int punch, int swizzled)
{
    int x, y, i;
    if (fmt == JT_ARGB || fmt == JT_TGA) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
                size_t di = swizzled ? swz((unsigned)x, (unsigned)y, (unsigned)w) : (size_t)y * w + x;
                uint8_t *q = o + di * 4;
                q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = p[3];
            }
        return;
    }
    for (y = 0; y < h; y += 4)
        for (x = 0; x < w; x += 4) {
            uint8_t px[16][4];
            uint8_t *b = o + ((size_t)(y / 4) * ((w + 3) / 4) + x / 4) * (fmt == JT_DXT1 ? 8 : 16);
            for (i = 0; i < 16; i++) {
                int sx = x + (i & 3), sy = y + i / 4;
                if (sx >= w) sx = w - 1;
                if (sy >= h) sy = h - 1;
                memcpy(px[i], rgba + ((size_t)sy * w + sx) * 4, 4);
            }
            if (fmt == JT_DXT1) dxt1_block(b, px, punch);
            else if (fmt == JT_DXT5) stb_compress_dxt_block(b, &px[0][0], 1, STB_DXT_HIGHQUAL);
            else {
                for (i = 0; i < 8; i++)
                    b[i] = (uint8_t)(((px[2 * i][3] + 8) / 17) | ((px[2 * i + 1][3] + 8) / 17) << 4);
                stb_compress_dxt_block(b + 8, &px[0][0], 0, STB_DXT_HIGHQUAL);
            }
        }
}

int jt_encode(JFile *f, const JTex *t, const uint8_t *rgba)
{
    uint8_t *cur, *o = f->buf + t->data;
    int w = t->w, h = t->h, i;
    if (levels_size(t->fmt == JT_TGA ? JT_ARGB : t->fmt, w, h, t->levels) > t->len) return -1;
    if (t->fmt == JT_TGA) {
        /* written the way the file has it: bottom-up unless its descriptor says not */
        const int top = (f->buf[17] & 0x20) != 0;
        int y;
        for (y = 0; y < h; y++)
            encode_level(o + (size_t)(top ? y : h - 1 - y) * w * 4, rgba + (size_t)y * w * 4, w, 1, JT_TGA, 0, 0);
        return 0;
    }
    cur = (uint8_t *)malloc((size_t)w * h * 4);
    memcpy(cur, rgba, (size_t)w * h * 4);
    for (i = 0; i < t->levels; i++) {
        encode_level(o, cur, w, h, t->fmt, t->alpha, t->swizzled);
        o += jt_level_size(t->fmt, w, h);
        if (i + 1 < t->levels) {                /* the next level: 2x2 averages */
            int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y, k;
            uint8_t *nx = (uint8_t *)malloc((size_t)nw * nh * 4);
            for (y = 0; y < nh; y++)
                for (x = 0; x < nw; x++)
                    for (k = 0; k < 4; k++) {
                        int x0 = x * 2, y0 = y * 2, x1 = x0 + (w > 1), y1 = y0 + (h > 1);
                        nx[((size_t)y * nw + x) * 4 + k] = (uint8_t)((cur[((size_t)y0 * w + x0) * 4 + k] + cur[((size_t)y0 * w + x1) * 4 + k]
                            + cur[((size_t)y1 * w + x0) * 4 + k] + cur[((size_t)y1 * w + x1) * 4 + k] + 2) / 4);
                    }
            free(cur);
            cur = nx;
            w = nw;
            h = nh;
        }
    }
    free(cur);
    return 0;
}
