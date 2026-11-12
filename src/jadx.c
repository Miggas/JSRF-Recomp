/* JSRF music: see jadx.h */
#include "jadx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static int be16(const uint8_t *b) { return b[0] << 8 | b[1]; }
static void put_be32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static void put_be16(uint8_t *b, int v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

int adx_parse(const uint8_t *b, size_t size, JAdx *a)
{
    size_t co;
    memset(a, 0, sizeof *a);
    if (size < 0x30 || b[0] != 0x80 || b[1] != 0x00) return -1;
    co = (size_t)be16(b + 2);
    if (co + 4 > size || co < 0x12 || memcmp(b + co - 2, "(c)CRI", 6)) return -1;
    if (b[4] != 3 || b[5] != 18 || b[6] != 4) return -1;      /* standard ADX only */
    a->ch = b[7];
    a->rate = (int)be32(b + 8);
    a->frames = (int)be32(b + 12);
    a->cutoff = be16(b + 16);
    a->version = b[18];
    a->flags = b[19];
    a->data = co + 4;
    if (a->ch < 1 || a->ch > 2 || a->rate < 1000 || a->rate > 96000 || a->flags & 0x08) return -1;   /* encrypted: not read */
    if (a->version == 3 && co >= 0x2C - 2) a->loop_at = 0x18;
    else if (a->version == 4 && co >= 0x38 - 2) a->loop_at = 0x24;
    if (a->loop_at) {
        a->loop = be32(b + a->loop_at) != 0;
        a->loop_start = (int)be32(b + a->loop_at + 4);
        a->loop_end = (int)be32(b + a->loop_at + 12);
    }
    {   /* no more samples than the file holds */
        long long blocks = (long long)(size - a->data) / (18LL * a->ch);
        if ((long long)a->frames > blocks * 32) a->frames = (int)(blocks * 32);
    }
    return a->frames > 0 ? 0 : -1;
}

int adx_head_ok(const uint8_t *b, size_t n)
{
    size_t co;
    if (n < 0x30 || b[0] != 0x80 || b[1] != 0x00) return 0;
    co = (size_t)be16(b + 2);
    return co + 4 <= n && co >= 0x12 && !memcmp(b + co - 2, "(c)CRI", 6) && b[4] == 3 && b[5] == 18 && b[6] == 4
        && b[7] >= 1 && b[7] <= 2 && !(b[19] & 0x08);
}

/* the predictor's two coefficients (x4096), from the cutoff and the rate */
static void coefs(const JAdx *a, int *c1, int *c2)
{
    double x = sqrt(2.0) - cos(2.0 * M_PI * (double)a->cutoff / (double)a->rate), y = sqrt(2.0) - 1.0;
    double z = (x - sqrt((x + y) * (x - y))) / y;
    *c1 = (int)floor(z * 8192.0);
    *c2 = (int)floor(-(z * z) * 4096.0);
}

int adx_decode(const uint8_t *b, size_t size, const JAdx *a, int16_t **pcm)
{
    int c1, c2, c, f;
    int h1[2] = { 0, 0 }, h2[2] = { 0, 0 };
    int16_t *o;
    (void)size;
    coefs(a, &c1, &c2);
    o = (int16_t *)malloc(sizeof(int16_t) * (size_t)a->frames * (size_t)a->ch);
    if (!o) return -1;
    for (f = 0; f < a->frames; f += 32) {
        size_t blk = a->data + (size_t)(f / 32) * 18 * (size_t)a->ch;
        for (c = 0; c < a->ch; c++) {
            const uint8_t *p = b + blk + (size_t)c * 18;
            int scale = be16(p), j;
            for (j = 0; j < 32 && f + j < a->frames; j++) {
                int nib = (p[2 + j / 2] >> (j & 1 ? 0 : 4)) & 15, s;
                if (nib & 8) nib -= 16;
                s = clamp16(nib * scale + ((c1 * h1[c] + c2 * h2[c]) >> 12));
                h2[c] = h1[c];
                h1[c] = s;
                o[(size_t)(f + j) * a->ch + c] = (int16_t)s;
            }
        }
    }
    *pcm = o;
    return 0;
}

/* one block (32 samples, stride st) for history h1, h2 with this scale:
 * its error, the block written to out */
static double enc_block(const int16_t *in, int st, int n, int c1, int c2, int scale, int *h1, int *h2, uint8_t *out)
{
    int j, a = *h1, b = *h2;
    double err = 0;
    memset(out, 0, 18);
    put_be16(out, scale);
    for (j = 0; j < 32; j++) {
        int s = j < n ? in[j * st] : in[(n - 1) * st], pred = (c1 * a + c2 * b) >> 12, nib, d;
        nib = (int)lrint((double)(s - pred) / scale);
        if (nib < -8) nib = -8;
        if (nib > 7) nib = 7;
        d = clamp16(nib * scale + pred);
        err += (double)(d - s) * (d - s);
        out[2 + j / 2] |= (uint8_t)((nib & 15) << (j & 1 ? 0 : 4));
        b = a;
        a = d;
    }
    *h1 = a;
    *h2 = b;
    return err;
}

/* in (ich at irate) to och at orate, linear in between */
static int16_t *convert(const int16_t *in, int frames, int ich, int irate, int och, int orate, int *oframes)
{
    long long of = (long long)frames * orate / irate, f;
    int16_t *o;
    int c;
    if (of < 1) of = 1;
    if (of > 256LL * 1024 * 1024) return NULL;
    o = (int16_t *)malloc(sizeof(int16_t) * (size_t)of * (size_t)och);
    if (!o) return NULL;
    for (f = 0; f < of; f++) {
        double pos = (double)f * irate / orate, t;
        int i0 = (int)pos, i1 = i0 + 1;
        t = pos - i0;
        if (i0 >= frames) i0 = frames - 1;
        if (i1 >= frames) i1 = frames - 1;
        for (c = 0; c < och; c++) {
            double x, y;
            if (och == 1 && ich > 1) {
                int k;
                x = y = 0;
                for (k = 0; k < ich; k++) { x += in[(size_t)i0 * ich + k]; y += in[(size_t)i1 * ich + k]; }
                x /= ich; y /= ich;
            } else {
                int k = c < ich ? c : ich - 1;
                x = in[(size_t)i0 * ich + k];
                y = in[(size_t)i1 * ich + k];
            }
            o[(size_t)f * och + c] = (int16_t)clamp16((int)lrint(x + (y - x) * t));
        }
    }
    *oframes = (int)of;
    return o;
}

int adx_build(const uint8_t *buf, size_t size, const JAdx *a, const int16_t *pcm, int frames, int ch, int rate,
              uint8_t **nb, size_t *nsize)
{
    int16_t *cv;
    int cf, c1, c2, f, c, h1[2] = { 0, 0 }, h2[2] = { 0, 0 };
    size_t blocks, dlen, blk = 18 * (size_t)a->ch, tail, ls = 0;
    uint8_t *o;
    *nb = NULL;
    cv = convert(pcm, frames, ch, rate, a->ch, a->rate, &cf);   /* the game's rate and channels */
    if (!cv) return -1;
    coefs(a, &c1, &c2);
    blocks = ((size_t)cf + 31) / 32;
    dlen = blocks * blk;
    /* the end: a block whose scale is 0x8001, then how many bytes follow it;
     * the looped ones (effect_005) padded so the file is whole 2048-byte
     * sectors, the others (title, djse001) just the 18-byte block */
    tail = 18;
    if (!(size % 2048)) tail += (2048 - (a->data + dlen + 18) % 2048) % 2048;
    *nsize = a->data + dlen + tail;
    if (a->loop_at && a->loop) {    /* the loop starts on a sector, as the game's own do (0x2E000) */
        size_t k;
        for (k = 0; k < 4096 && k < blocks; k++)
            if (!((a->data + k * blk) % 2048)) { ls = k; break; }
    }
    o = (uint8_t *)calloc(1, *nsize);
    if (!o) { free(cv); return -1; }
    memcpy(o, buf, a->data);
    for (f = 0; f < cf; f += 32) {
        int n = cf - f < 32 ? cf - f : 32;
        for (c = 0; c < a->ch; c++) {
            const int16_t *in = cv + (size_t)f * a->ch + c;
            uint8_t *out = o + a->data + (size_t)(f / 32) * 18 * (size_t)a->ch + (size_t)c * 18, best[18], tryb[18];
            int maxr = 0, j, scale, k, bh1 = 0, bh2 = 0;
            double be = -1;
            {   /* the scale: the largest step from a prediction off the input itself */
                int a1 = h1[c], a2 = h2[c];
                for (j = 0; j < n; j++) {
                    int s = in[j * a->ch], r = s - ((c1 * a1 + c2 * a2) >> 12);
                    if (abs(r) > maxr) maxr = abs(r);
                    a2 = a1;
                    a1 = s;
                }
            }
            scale = (maxr + 6) / 7;
            if (scale < 1) scale = 1;
            for (k = 0; k < 12; k++) {          /* scales around it, 0.75x to 2x: the least error */
                int s = scale * (6 + k) / 8 + (k > 2);
                int t1 = h1[c], t2 = h2[c];
                double e;
                if (s < 1) s = 1;
                if (s > 0x7FFF) s = 0x7FFF;
                e = enc_block(in, a->ch, n, c1, c2, s, &t1, &t2, tryb);
                if (be < 0 || e < be) { be = e; memcpy(best, tryb, 18); bh1 = t1; bh2 = t2; }
            }
            memcpy(out, best, 18);
            h1[c] = bh1;
            h2[c] = bh2;
        }
    }
    free(cv);
    put_be16(o + a->data + dlen, 0x8001);
    put_be16(o + a->data + dlen + 2, (int)(tail - 4));
    put_be32(o + 12, (uint32_t)cf);
    if (a->loop_at && a->loop) {                /* looped: the whole new piece */
        if (a->version == 3) put_be16(o + 0x14, 0);    /* no alignment samples before the loop */
        put_be32(o + a->loop_at + 4, (uint32_t)(ls * 32));
        put_be32(o + a->loop_at + 8, (uint32_t)(a->data + ls * blk));
        put_be32(o + a->loop_at + 12, (uint32_t)cf);
        put_be32(o + a->loop_at + 16, (uint32_t)(a->data + dlen));
    }
    *nb = o;
    return 0;
}
