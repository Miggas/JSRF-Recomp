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
