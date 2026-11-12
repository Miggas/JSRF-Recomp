/* JSRF sounds: see jsnd.h */
#include "jsnd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }

const char *js_fmt_name(int fmt)
{
    return fmt == JS_PCM ? "PCM" : fmt == JS_XADPCM ? "Xbox ADPCM" : "other";
}

/* ---- finding them ---- */

/* the RIFF at b (len bytes): its format and samples; 0 when it's one */
static int riff(const uint8_t *b, size_t len, JSnd *s)
{
    size_t off = 12, fmt = 0, flen = 0;
    if (len < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return -1;
    s->data = 0;
    while (off + 8 <= len) {
        size_t cl = u32(b + off + 4);
        if (!memcmp(b + off, "fmt ", 4)) { fmt = off + 8; flen = cl; }
        else if (!memcmp(b + off, "data", 4)) {
            s->data = off + 8;
            s->dlen = cl > len - (off + 8) ? len - (off + 8) : cl;
            break;
        }
        off += 8 + cl + (cl & 1);
    }
    if (!fmt || flen < 16 || !s->data) return -1;
    s->fmt = u16(b + fmt);
    s->ch = u16(b + fmt + 2);
    s->rate = (int)u32(b + fmt + 4);
    s->bits = u16(b + fmt + 14);
    if (s->ch < 1 || s->ch > 8 || s->rate < 1000 || s->rate > 192000) return -1;
    if (s->fmt == JS_XADPCM) s->frames = (int)(s->dlen / (36u * (size_t)s->ch) * 65u);
    else if (s->fmt == JS_PCM && s->bits == 16) s->frames = (int)(s->dlen / (2u * (size_t)s->ch));
    else if (s->fmt == JS_PCM && s->bits == 8) s->frames = (int)(s->dlen / (size_t)s->ch);
    else s->frames = 0;
    return 0;
}

static void add(JSounds *o, int *cap, JSnd *s)
{
    if (o->n == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        o->s = (JSnd *)realloc(o->s, sizeof(JSnd) * (size_t)*cap);
    }
    o->s[o->n++] = *s;
}

static void norm_scan(const uint8_t *b, size_t size, size_t norm, size_t end, JSounds *o, int *cap)
{
    size_t off = norm + 16;
    int guard = 0;
    if (end > size) end = size;
    while (off + 16 <= end && guard++ < 100000) {
        size_t s = u32(b + off), e = u32(b + off + 4);
        if (!e || norm + e <= off) break;
        if (s && s < e && norm + e <= end && e - s >= 44 && !memcmp(b + norm + s, "RIFF", 4)) {
            JSnd sn;
            memset(&sn, 0, sizeof sn);
            if (!riff(b + norm + s, e - s, &sn)) {
                sn.item = norm + s;
                sn.len = e - s;
                sn.data += sn.item;
                sn.ent = off;
                sn.norm = norm;
                sn.norm_end = end;
                add(o, cap, &sn);
            }
        }
        off = norm + e;
    }
}

int js_scan(const uint8_t *b, size_t size, JSounds *o)
{
    int cap = 0;
    memset(o, 0, sizeof *o);
    if (size < 32) return -1;
    if (!memcmp(b, "MULT", 4)) {
        size_t off = 16;
        int guard = 0;
        while (off + 16 <= size && guard++ < 100000) {
            size_t s = u32(b + off), e = u32(b + off + 4);
            if (e <= off || s >= size) break;
            if (s + 16 <= size && !memcmp(b + s, "NORM", 4)) norm_scan(b, size, s, e, o, &cap);
            off = e;
        }
    } else if (!memcmp(b, "NORM", 4)) norm_scan(b, size, 0, size, o, &cap);
    else return -1;
    return 0;
}

void js_free(JSounds *ss)
{
    free(ss->s);
    ss->s = NULL;
    ss->n = 0;
}

/* ---- Xbox ADPCM (IMA steps) ---- */

static const int STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static const int IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int step_dec(int code, int *pred, int *idx)
{
    int st = STEP[*idx], d = st >> 3;
    if (code & 4) d += st;
    if (code & 2) d += st >> 1;
    if (code & 1) d += st >> 2;
    *pred = clampi(code & 8 ? *pred - d : *pred + d, -32768, 32767);
    *idx = clampi(*idx + IDX[code & 7], 0, 88);
    return *pred;
}

static int step_enc(int s, int *pred, int *idx)
{
    int st = STEP[*idx], diff = s - *pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= st) { code |= 4; diff -= st; }
    st >>= 1;
    if (diff >= st) { code |= 2; diff -= st; }
    st >>= 1;
    if (diff >= st) code |= 1;
    step_dec(code, pred, idx);              /* the decoder's view, so both stay together */
    return code;
}

int js_decode(const uint8_t *buf, const JSnd *s, int16_t **pcm, int *frames)
{
    const uint8_t *d = buf + s->data;
    int ch = s->ch, f, c;
    int16_t *o;
    *pcm = NULL;
    *frames = s->frames;
    if (s->frames <= 0) return -1;
    o = (int16_t *)malloc(sizeof(int16_t) * (size_t)s->frames * (size_t)ch);
    if (!o) return -1;
    if (s->fmt == JS_PCM && s->bits == 16) {
        for (f = 0; f < s->frames * ch; f++) o[f] = (int16_t)u16(d + (size_t)f * 2);
    } else if (s->fmt == JS_PCM && s->bits == 8) {
        for (f = 0; f < s->frames * ch; f++) o[f] = (int16_t)((d[f] - 128) << 8);
    } else {
        int blocks = s->frames / 65, k;
        for (k = 0; k < blocks; k++) {
            const uint8_t *bk = d + (size_t)k * 36 * (size_t)ch;
            int16_t *ob = o + (size_t)k * 65 * (size_t)ch;
            for (c = 0; c < ch; c++) {
                int pred = (int16_t)u16(bk + c * 4), idx = clampi(bk[c * 4 + 2], 0, 88), g, j;
                ob[c] = (int16_t)pred;
                for (g = 0; g < 8; g++) {
                    const uint8_t *p = bk + 4 * ch + (g * ch + c) * 4;
                    for (j = 0; j < 8; j++) {
                        int code = (p[j >> 1] >> ((j & 1) * 4)) & 15;
                        ob[(1 + g * 8 + j) * ch + c] = (int16_t)step_dec(code, &pred, &idx);
                    }
                }
            }
        }
    }
    *pcm = o;
    return 0;
}

/* ch channels of frames samples, as Xbox ADPCM: 65 a block (the last
 * block filled with its last sample); out: blocks * 36 * ch bytes */
static uint8_t *adpcm_encode(const int16_t *in, int frames, int ch, size_t *outlen)
{
    int blocks = (frames + 64) / 65, k, c, g, j, pred[8], idx[8];
    uint8_t *o;
    *outlen = (size_t)blocks * 36 * (size_t)ch;
    o = (uint8_t *)calloc(1, *outlen ? *outlen : 1);
    if (!o) return NULL;
    for (c = 0; c < ch; c++) { pred[c] = 0; idx[c] = 0; }
    for (k = 0; k < blocks; k++) {
        uint8_t *bk = o + (size_t)k * 36 * (size_t)ch;
        int base = k * 65;
        for (c = 0; c < ch; c++) {
            int first = in[(size_t)(base < frames ? base : frames - 1) * ch + c];
            pred[c] = first;
            put16(bk + c * 4, (uint16_t)(int16_t)first);
            bk[c * 4 + 2] = (uint8_t)idx[c];
            bk[c * 4 + 3] = 0;
            for (g = 0; g < 8; g++) {
                uint8_t *p = bk + 4 * ch + (g * ch + c) * 4;
                for (j = 0; j < 8; j++) {
                    int f = base + 1 + g * 8 + j;
                    int smp = in[(size_t)(f < frames ? f : frames - 1) * ch + c];
                    int code = step_enc(smp, &pred[c], &idx[c]);
                    p[j >> 1] |= (uint8_t)(code << ((j & 1) * 4));
                }
            }
        }
    }
    return o;
}

/* ---- WAV files ---- */

int js_read_wav(const char *path, int16_t **pcm, int *frames, int *ch, int *rate, char *err, size_t errn)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *b;
    long n;
    size_t off = 12, fmt = 0, data = 0, dlen = 0;
    int tag, bits, bps, f, c, total;
    int16_t *o;
    *pcm = NULL;
    if (!fp) { snprintf(err, errn, "Can't open %s", path); return -1; }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 44 || n > 512L * 1024 * 1024) { fclose(fp); snprintf(err, errn, "Not a WAV file: %s", path); return -1; }
    b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(b); snprintf(err, errn, "Can't read %s", path); return -1; }
    fclose(fp);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); snprintf(err, errn, "Not a WAV file: %s", path); return -1; }
    while (off + 8 <= (size_t)n) {
        size_t cl = u32(b + off + 4);
        if (!memcmp(b + off, "fmt ", 4)) fmt = off + 8;
        else if (!memcmp(b + off, "data", 4)) { data = off + 8; dlen = cl > (size_t)n - data ? (size_t)n - data : cl; break; }
        off += 8 + cl + (cl & 1);
    }
    if (!fmt || !data) { free(b); snprintf(err, errn, "A WAV file without its format or samples: %s", path); return -1; }
    tag = u16(b + fmt);
    *ch = u16(b + fmt + 2);
    *rate = (int)u32(b + fmt + 4);
    bits = u16(b + fmt + 14);
    if (tag == 0xFFFE && u32(b + fmt - 4) >= 40) tag = u16(b + fmt + 24);   /* WAVE_FORMAT_EXTENSIBLE: its sub-format */
    if (!((tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32))
        || *ch < 1 || *ch > 8 || *rate < 1000 || *rate > 192000) {
        free(b);
        snprintf(err, errn, "Only plain WAV files (PCM 8/16/24/32-bit or 32-bit float) can be imported");
        return -1;
    }
    bps = bits / 8;
    *frames = (int)(dlen / ((size_t)bps * (size_t)*ch));
    total = *frames * *ch;
    o = (int16_t *)malloc(sizeof(int16_t) * (size_t)(total ? total : 1));
    if (!o) { free(b); snprintf(err, errn, "Out of memory"); return -1; }
    for (f = 0; f < *frames; f++)
        for (c = 0; c < *ch; c++) {
            const uint8_t *p = b + data + ((size_t)f * *ch + c) * bps;
            int v;
            if (tag == 3) {
                float x;
                memcpy(&x, p, 4);
                v = (int)(x * 32767.0f);
            } else if (bits == 8) v = (p[0] - 128) << 8;
            else if (bits == 16) v = (int16_t)u16(p);
            else if (bits == 24) v = (int)((int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) >> 16);
            else v = (int)((int32_t)u32(p) >> 16);
            o[(size_t)f * *ch + c] = (int16_t)clampi(v, -32768, 32767);
        }
    free(b);
    *pcm = o;
    return 0;
}

int js_write_wav(const char *path, const int16_t *pcm, int frames, int ch, int rate)
{
    FILE *fp = fopen(path, "wb");
    uint8_t h[44];
    size_t dl = (size_t)frames * (size_t)ch * 2, i;
    if (!fp) return -1;
    memcpy(h, "RIFF", 4); put32(h + 4, (uint32_t)(36 + dl)); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, (uint16_t)ch); put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)rate * (uint32_t)ch * 2); put16(h + 32, (uint16_t)(ch * 2)); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, (uint32_t)dl);
    fwrite(h, 1, 44, fp);
    for (i = 0; i < (size_t)frames * (size_t)ch; i++) {
        uint8_t s[2];
        put16(s, (uint16_t)pcm[i]);
        fwrite(s, 1, 2, fp);
    }
    return fclose(fp) ? -1 : 0;
}

/* ---- replacing one ---- */

/* in (ich channels at irate) to och channels at orate, linear in between */
static int16_t *convert(const int16_t *in, int frames, int ich, int irate, int och, int orate, int *oframes)
{
    long long of = (long long)frames * orate / irate;
    int16_t *o;
    long long f;
    int c;
    if (of < 1) of = 1;
    if (of > 64LL * 1024 * 1024) return NULL;
    o = (int16_t *)malloc(sizeof(int16_t) * (size_t)of * (size_t)och);
    if (!o) return NULL;
    for (f = 0; f < of; f++) {
        double pos = (double)f * irate / orate;
        int i0 = (int)pos, i1 = i0 + 1;
        double t = pos - i0;
        if (i0 >= frames) i0 = frames - 1;
        if (i1 >= frames) i1 = frames - 1;
        for (c = 0; c < och; c++) {
            double a = 0, b = 0;
            if (och == 1 && ich > 1) {          /* mixed down */
                int k;
                for (k = 0; k < ich; k++) { a += in[(size_t)i0 * ich + k]; b += in[(size_t)i1 * ich + k]; }
                a /= ich; b /= ich;
            } else {
                int k = c < ich ? c : ich - 1;  /* mono to both sides; more than two: the first two */
                a = in[(size_t)i0 * ich + k];
                b = in[(size_t)i1 * ich + k];
            }
            o[(size_t)f * och + c] = (int16_t)clampi((int)(a + (b - a) * t + (a + (b - a) * t >= 0 ? 0.5 : -0.5)), -32768, 32767);
        }
    }
    *oframes = (int)of;
    return o;
}

int js_replace(const uint8_t *buf, size_t size, const JSnd *s, const int16_t *pcm, int frames, int ch, int rate,
               uint8_t **nb, size_t *nsize)
{
    int16_t *cv;
    int cf;
    uint8_t *samples, *item, *o;
    size_t slen, ilen, head = s->data - s->item, tail_at, tail, newlen, padded;
    long long d;
    *nb = NULL;
    if (s->fmt != JS_PCM && s->fmt != JS_XADPCM) return -1;
    if (s->fmt == JS_PCM && s->bits != 16) return -1;
    cv = convert(pcm, frames, ch, rate, s->ch, s->rate, &cf);   /* the game's rate and channels */
    if (!cv) return -1;
    if (s->fmt == JS_XADPCM) samples = adpcm_encode(cv, cf, s->ch, &slen);
    else {
        int i;
        slen = (size_t)cf * (size_t)s->ch * 2;
        samples = (uint8_t *)malloc(slen);
        if (samples) for (i = 0; i < cf * s->ch; i++) put16(samples + (size_t)i * 2, (uint16_t)cv[i]);
    }
    free(cv);
    if (!samples) return -1;
    /* the new RIFF: its own bytes up to the samples (the format kept), the
     * samples, and whatever chunks followed them */
    tail_at = s->data + s->dlen + (s->dlen & 1);
    tail = tail_at < s->item + s->len ? s->item + s->len - tail_at : 0;
    ilen = head + slen + (slen & 1) + tail;
    item = (uint8_t *)calloc(1, ilen);
    if (!item) { free(samples); return -1; }
    memcpy(item, buf + s->item, head);
    memcpy(item + head, samples, slen);
    if (tail) memcpy(item + head + slen + (slen & 1), buf + tail_at, tail);
    free(samples);
    put32(item + 4, (uint32_t)(ilen - 8));
    put32(item + head - 4, (uint32_t)slen);
    /* the file: the item swapped, what follows moved by d (kept on the
     * original's 16-byte rhythm), the NORM's entries and the MULT's fixed */
    newlen = ilen;
    if (newlen > s->len) padded = s->len + ((newlen - s->len + 15) & ~(size_t)15);
    else padded = s->len - ((s->len - newlen) & ~(size_t)15);
    d = (long long)padded - (long long)s->len;
    *nsize = (size_t)((long long)size + d);
    o = (uint8_t *)calloc(1, *nsize);
    if (!o) { free(item); return -1; }
    memcpy(o, buf, s->item);
    memcpy(o + s->item, item, ilen);
    memcpy(o + s->item + padded, buf + s->item + s->len, size - (s->item + s->len));
    free(item);
#define NEWPOS(p) ((p) < s->item + s->len ? (p) : (size_t)((long long)(p) + d))
    {
        size_t off = s->norm + 16;
        int guard = 0;
        while (off + 16 <= s->norm_end && guard++ < 100000) {
            size_t st = u32(buf + off), e = u32(buf + off + 4);
            uint8_t *n = o + NEWPOS(off);
            if (!e || s->norm + e <= off) break;
            if (s->norm + st == s->item) {
                put32(n + 4, (uint32_t)((long long)e + d));
                put32(n + 8, (uint32_t)((long long)u32(buf + off + 8) + d));
            } else if (s->norm + st > s->item) {
                put32(n, (uint32_t)((long long)st + d));
                put32(n + 4, (uint32_t)((long long)e + d));
            }
            off = s->norm + e;
        }
    }
    if (!memcmp(buf, "MULT", 4)) {
        size_t moff = 16;
        int guard = 0;
        while (moff + 16 <= size && guard++ < 100000) {
            size_t st = u32(buf + moff), e = u32(buf + moff + 4);
            uint8_t *n = o + NEWPOS(moff);
            if (e <= moff) break;
            if (st == s->norm) {
                put32(n + 4, (uint32_t)((long long)e + d));
                put32(n + 8, (uint32_t)((long long)u32(buf + moff + 8) + d));
            } else if (st > s->norm) {
                put32(n, (uint32_t)((long long)st + d));
                put32(n + 4, (uint32_t)((long long)e + d));
            }
            moff = e;
        }
    }
#undef NEWPOS
    *nb = o;
    return 0;
}
