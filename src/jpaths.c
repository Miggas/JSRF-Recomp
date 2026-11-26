/* m29o: car routes and mission chains: see jpaths.h */
#include "jpaths.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static float f32(const uint8_t *b) { uint32_t v = u32(b); float f; memcpy(&f, &v, 4); return f; }

static JPath *jp_add(JPaths *ps, int n)
{
    JPath *np = (JPath *)realloc(ps->path, sizeof(JPath) * (size_t)(ps->npath + 1)), *p;
    if (!np) return NULL;
    ps->path = np;
    p = &np[ps->npath];
    memset(p, 0, sizeof *p);
    p->p = (float *)malloc(sizeof(float) * 3 * (size_t)n);
    if (!p->p) return NULL;
    p->n = n;
    ps->npath++;
    return p;
}

int jp_cars(const uint8_t *b, size_t n, int file, JPaths *out)
{
    size_t norm, nend, off;
    int got = 0, k = 0, guard = 0;
    if (n < 48 || memcmp(b, "MULT", 4)) return -1;
    norm = u32(b + 16);
    nend = u32(b + 20);
    if (norm + 16 > n || nend > n || nend <= norm || memcmp(b + norm, "NORM", 4)) return -1;
    off = norm + 16;
    while (off + 16 <= nend && guard++ < 10000) {     /* the NORM's items: (start, end, size) from the NORM */
        size_t st = u32(b + off), e = u32(b + off + 4), sz = u32(b + off + 8), a = norm + st;
        if (!e || norm + e <= off) break;
        if (sz >= 8 && a + sz <= n) {
            int np = (int)u32(b + a), loop = (int)u32(b + a + 4), i;
            if (np >= 2 && np < 100000 && (size_t)8 + (size_t)np * 16 == sz) {
                JPath *p = jp_add(out, np);
                if (!p) return got;
                p->loop = loop; p->file = file; p->idx = k;
                p->key = (int *)malloc(sizeof(int) * (size_t)np);
                for (i = 0; i < np; i++) {
                    const uint8_t *q = b + a + 8 + (size_t)i * 16;
                    if (p->key) p->key[i] = (int)u32(q);
                    p->p[i * 3] = f32(q + 4); p->p[i * 3 + 1] = f32(q + 8); p->p[i * 3 + 2] = f32(q + 12);
                }
                got++;
            }
        }
        k++;
        off = norm + e;
    }
    return got;
}

/* a plausible world point */
static int okpt(const uint8_t *q)
{
    float x = f32(q), y = f32(q + 4), z = f32(q + 8);
    return isfinite(x) && isfinite(y) && isfinite(z) && fabsf(x) < 20000 && fabsf(z) < 20000 && fabsf(y) < 5000
           && (x != 0 || z != 0);
}

/* an entry: count in the top byte, its points' place, 0, a turn, 0 */
static int entry(const uint8_t *b, size_t n, size_t o)
{
    uint32_t c = u32(b + o) >> 24, at = u32(b + o + 4), i;
    if (u32(b + o + 8) || u32(b + o + 16) || c < 1 || c > 200 || (at & 3) || at < 16 || (size_t)at + 12u * c > n) return 0;
    for (i = 0; i < c; i++) if (!okpt(b + at + 12 * i)) return 0;
    return 1;
}

int jp_mission(const uint8_t *b, size_t n, int file, JPaths *out)
{
    size_t o = 0;
    int got = 0;
    uint32_t *seen = NULL;
    size_t nseen = 0;
    while (o + 20 <= n) {
        size_t r = o;
        int run = 0;
        while (r + 20 <= n && entry(b, n, r)) { run++; r += 20; }
        if (run >= 8) {                        /* a table: each entry a chain (once, though tables share points) */
            size_t e;
            for (e = o; e < r; e += 20) {
                uint32_t at = u32(b + e + 4), c = u32(b + e) >> 24, i;
                size_t s;
                int dup = 0;
                for (s = 0; s < nseen; s++) if (seen[s] == at) { dup = 1; break; }
                if (dup) continue;
                {
                    uint32_t *ns = (uint32_t *)realloc(seen, sizeof(uint32_t) * (nseen + 1));
                    if (!ns) break;
                    seen = ns;
                    seen[nseen++] = at;
                }
                {
                    JPath *p = jp_add(out, (int)c);
                    if (!p) { free(seen); return got; }
                    p->file = file; p->idx = (int)((e - o) / 20);
                    for (i = 0; i < c; i++) {
                        p->p[i * 3] = f32(b + at + 12 * i);
                        p->p[i * 3 + 1] = f32(b + at + 12 * i + 4);
                        p->p[i * 3 + 2] = f32(b + at + 12 * i + 8);
                    }
                    got++;
                }
            }
            o = r;
        } else o += 4;
    }
    free(seen);
    return got;
}

void jp_free(JPaths *ps)
{
    int i;
    for (i = 0; i < ps->npath; i++) { free(ps->path[i].p); free(ps->path[i].key); }
    free(ps->path);
    ps->path = NULL;
    ps->npath = 0;
}
