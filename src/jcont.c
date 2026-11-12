/* the game's containers: see jcont.h */
#include "jcont.h"
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

int jc_replace(const uint8_t *buf, size_t size, size_t item, size_t len, size_t norm, size_t norm_end,
               const uint8_t *nitem, size_t nlen, uint8_t **nb, size_t *nsize)
{
    size_t padded;
    long long d;
    uint8_t *o;
    *nb = NULL;
    if (item + len > size || norm_end > size) return -1;
    /* what follows moves by d, kept on the original's 16-byte rhythm */
    if (nlen > len) padded = len + ((nlen - len + 15) & ~(size_t)15);
    else padded = len - ((len - nlen) & ~(size_t)15);
    d = (long long)padded - (long long)len;
    *nsize = (size_t)((long long)size + d);
    o = (uint8_t *)calloc(1, *nsize);
    if (!o) return -1;
    memcpy(o, buf, item);
    memcpy(o + item, nitem, nlen);
    memcpy(o + item + padded, buf + item + len, size - (item + len));
#define NEWPOS(p) ((p) < item + len ? (p) : (size_t)((long long)(p) + d))
    {   /* the NORM's children: this one's end and size, the later ones moved */
        size_t off = norm + 16;
        int guard = 0;
        while (off + 16 <= norm_end && guard++ < 100000) {
            size_t st = u32(buf + off), e = u32(buf + off + 4);
            uint8_t *n = o + NEWPOS(off);
            if (!e || norm + e <= off) break;
            if (norm + st == item) {
                put32(n + 4, (uint32_t)((long long)e + d));
                put32(n + 8, (uint32_t)((long long)u32(buf + off + 8) + d));
            } else if (norm + st > item) {
                put32(n, (uint32_t)((long long)st + d));
                put32(n + 4, (uint32_t)((long long)e + d));
            }
            off = norm + e;
        }
    }
    if (!memcmp(buf, "MULT", 4)) {   /* the MULT's entries: the NORM's end and size, the later NORMs moved */
        size_t moff = 16;
        int guard = 0;
        while (moff + 16 <= size && guard++ < 100000) {
            size_t st = u32(buf + moff), e = u32(buf + moff + 4);
            uint8_t *n = o + NEWPOS(moff);
            if (e <= moff) break;
            if (st == norm) {
                put32(n + 4, (uint32_t)((long long)e + d));
                put32(n + 8, (uint32_t)((long long)u32(buf + moff + 8) + d));
            } else if (st > norm) {
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

/* ---- m13: listing the items ---- */

static const char *kind_of(const uint8_t *b, size_t len)
{
    uint32_t r;
    if (!len) return "empty";
    if (len >= 4 && !memcmp(b, "MDLB", 4)) return "model";
    if (len >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return "sound";
    if (len >= 40) {
        r = u32(b + 20);
        if ((b[24] == 5 || b[24] == 6 || b[24] == 7) && r >= 4 && r <= 4096 && !(r & (r - 1))) return "texture";
    }
    if (len >= 8 && u32(b) >= 1 && u32(b) <= 16 && 4 + 4 * (size_t)u32(b) <= len && len <= 4 + 4 * (size_t)u32(b) + 15)
        return "material list";
    return "data";
}

static void list_norm(const uint8_t *b, size_t size, size_t norm, size_t end, int norm_no, JItem **o, int *n, int *cap)
{
    size_t off = norm + 16;
    int guard = 0, no = 0;
    if (end > size) end = size;
    while (off + 16 <= end && guard++ < 100000) {
        size_t s = u32(b + off), e = u32(b + off + 4);
        JItem *it;
        if (!e || norm + e <= off) break;
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *o = (JItem *)realloc(*o, sizeof(JItem) * (size_t)*cap); }
        it = &(*o)[(*n)++];
        it->norm_no = norm_no;
        it->no = no++;
        it->norm = norm;
        it->norm_end = end;
        it->ent = off;
        it->item = s && s <= e && norm + e <= end ? norm + s : norm + e;
        it->len = s && s <= e && norm + e <= end ? e - s : 0;
        it->kind = kind_of(b + it->item, it->len);
        off = norm + e;
    }
}

int jc_list(const uint8_t *b, size_t size, JItem **items, int *n)
{
    int cap = 0;
    *items = NULL;
    *n = 0;
    if (size < 32) return -1;
    if (!memcmp(b, "MULT", 4)) {
        size_t off = 16;
        int guard = 0, k = 0;
        while (off + 16 <= size && guard++ < 100000) {
            size_t s = u32(b + off), e = u32(b + off + 4);
            if (e <= off || s >= size) break;
            if (s + 16 <= size && !memcmp(b + s, "NORM", 4)) list_norm(b, size, s, e, k, items, n, &cap);
            k++;
            off = e;
        }
    } else if (!memcmp(b, "NORM", 4)) list_norm(b, size, 0, size, 0, items, n, &cap);
    else return -1;
    return 0;
}
