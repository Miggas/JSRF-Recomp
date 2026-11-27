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

void jp_free(JPaths *ps)
{
    int i;
    for (i = 0; i < ps->npath; i++) { free(ps->path[i].p); free(ps->path[i].key); }
    free(ps->path);
    ps->path = NULL;
    ps->npath = 0;
}

/* m29s: a mission, by its header's table of blocks: see jpaths.h */
static int blk(const uint8_t *b, size_t n, int k, uint32_t size, uint32_t *off, uint32_t *cnt)
{
    *off = u32(b + 52 + 8 * k); *cnt = u32(b + 56 + 8 * k);
    if (!*off || !*cnt || *cnt > 100000 || (size_t)*off + (size_t)size * *cnt > n) { *off = *cnt = 0; return 0; }
    return 1;
}
static void xyz(float *o, const uint8_t *q) { o[0] = f32(q); o[1] = f32(q + 4); o[2] = f32(q + 8); }

int jp_mission_read(const uint8_t *b, size_t n, JMission *m)
{
    uint32_t o, c, i, j;
    memset(m, 0, sizeof *m);
    if (n < 544) return -1;
    m->minor = (int)(b[0] | b[1] << 8);
    m->major = (int)(b[2] | b[3] << 8);
    if (m->major > 9 || m->minor > 9) return -1;
    if (blk(b, n, 1, 44, &o, &c) && (m->sp = (JSpawn *)calloc(c, sizeof(JSpawn))) != NULL)
        for (i = 0; i < c; i++) {
            const uint8_t *q = b + o + 44 * i;
            JSpawn *e = &m->sp[m->nsp++];
            e->type = (int)u32(q);
            xyz(e->p, q + 8); xyz(e->p2, q + 32);
            e->facing = (int)(u32(q + 24) & 0xFFFF);
        }
    if (blk(b, n, 6, 48, &o, &c) && (m->exitpl = (JPlane *)calloc(c, sizeof(JPlane))) != NULL)
        for (i = 0; i < c; i++, m->nexit++)
            for (j = 0; j < 4; j++) xyz(m->exitpl[i].c[j], b + o + 48 * i + 12 * j);
    if (blk(b, n, 8, 96, &o, &c) && (m->tp = (JTele *)calloc(c, sizeof(JTele))) != NULL)
        for (i = 0; i < c; i++, m->ntp++) {
            for (j = 0; j < 6; j++) xyz(m->tp[i].c[j], b + o + 96 * i + 12 * j);
            xyz(m->tp[i].cam, b + o + 96 * i + 80);
        }
    if (blk(b, n, 10, 8, &o, &c))                 /* pickup lists: (offset, count) of 16-byte pickups */
        for (i = 0; i < c; i++) {
            uint32_t lo = u32(b + o + 8 * i), lc = u32(b + o + 8 * i + 4);
            JPick *np;
            if (!lc || lc > 10000 || (size_t)lo + 16u * lc > n) continue;
            np = (JPick *)realloc(m->pk, sizeof(JPick) * (size_t)(m->npk + lc));
            if (!np) break;
            m->pk = np;
            for (j = 0; j < lc; j++) { JPick *e = &m->pk[m->npk++]; xyz(e->p, b + lo + 16 * j); e->type = (int)u32(b + lo + 16 * j + 12); }
        }
    if (blk(b, n, 12, 8, &o, &c))                 /* parked-car lists: 48-byte cars */
        for (i = 0; i < c; i++) {
            uint32_t lo = u32(b + o + 8 * i), lc = u32(b + o + 8 * i + 4);
            JCar *np;
            if (!lc || lc > 10000 || (size_t)lo + 48u * lc > n) continue;
            np = (JCar *)realloc(m->car, sizeof(JCar) * (size_t)(m->ncar + lc));
            if (!np) break;
            m->car = np;
            for (j = 0; j < lc; j++) {
                JCar *e = &m->car[m->ncar++];
                const uint8_t *q = b + lo + 48 * j;
                xyz(e->p, q); xyz(e->rot, q + 12); xyz(e->scale, q + 24); e->type = (int)u32(q + 36);
            }
        }
    if (blk(b, n, 13, 8, &o, &c))                 /* pedestrian lists: 20-byte walkers, each a run of points */
        for (i = 0; i < c; i++) {
            uint32_t lo = u32(b + o + 8 * i), lc = u32(b + o + 8 * i + 4);
            if (!lc || lc > 100000 || (size_t)lo + 20u * lc > n) continue;
            for (j = 0; j < lc; j++) {
                const uint8_t *q = b + lo + 20 * j;
                uint32_t len = q[3], at = u32(q + 4), t;
                JPath *np, *p;
                if (!len || (size_t)at + 12u * len > n) continue;
                np = (JPath *)realloc(m->ped.path, sizeof(JPath) * (size_t)(m->ped.npath + 1));
                if (!np) break;
                m->ped.path = np;
                p = &np[m->ped.npath];
                memset(p, 0, sizeof *p);
                p->p = (float *)malloc(sizeof(float) * 3 * len);
                if (!p->p) break;
                p->n = (int)len; p->idx = (int)j; p->file = (int)q[0];   /* file: its model, maybe */
                for (t = 0; t < len; t++) xyz(p->p + 3 * t, b + at + 12 * t);
                m->ped.npath++;
            }
        }
    if (blk(b, n, 15, 48, &o, &c) && (m->soul = (JSoul *)calloc(c, sizeof(JSoul))) != NULL)
        for (i = 0; i < c; i++, m->nsoul++) { xyz(m->soul[i].p, b + o + 48 * i); m->soul[i].id = (int)u32(b + o + 48 * i + 12); }
    if (blk(b, n, 24, 48, &o, &c) && (m->dw = (JWarp *)calloc(c, sizeof(JWarp))) != NULL)
        for (i = 0; i < c; i++) {
            const uint8_t *q = b + o + 48 * i;
            JWarp *e = &m->dw[m->ndw++];
            uint32_t po = u32(q + 4), pc = u32(q + 8);
            e->type = (int)u32(q);
            xyz(e->to, q + 16);
            if (pc && pc < 1000 && (size_t)po + 48u * pc <= n && (e->pl = (JPlane *)calloc(pc, sizeof(JPlane))) != NULL)
                for (j = 0; j < pc; j++, e->npl++) {
                    int v;
                    for (v = 0; v < 4; v++) xyz(e->pl[j].c[v], b + po + 48 * j + 12 * v);
                }
        }
    return 0;
}

void jp_mission_free(JMission *m)
{
    int i;
    free(m->sp); free(m->exitpl); free(m->tp); free(m->pk); free(m->car); free(m->soul);
    for (i = 0; i < m->ndw; i++) free(m->dw[i].pl);
    free(m->dw);
    jp_free(&m->ped);
    memset(m, 0, sizeof *m);
}

static const struct { int no; const char *name; } s_mnames[] = {
    { 90, "Title" },
    { 99, "Credits" },
    { 100, "Chapter 1 Garage" },
    { 101, "Opening tutorial" },
    { 110, "Chapter 1 Dogenzaka Hill" },
    { 111, "Dogenzaka Hill Beat \"running away\" scenes" },
    { 112, "Dogenzaka Hill Beat race" },
    { 113, "Dogenzaka Hill cop fight" },
    { 120, "Chapter 1 Shibuya Terminal" },
    { 121, "Shibuya Terminal Poison Jam/Combo skating scenes" },
    { 122, "Shibuya Terminal cop fight" },
    { 123, "Shibuya Terminal Combo street challenge" },
    { 194, "Chapter 1 post-cutscene character select (unused?)" },
    { 195, "Chapter 1 Roboy character select" },
    { 196, "Chapter 1 intro" },
    { 197, "Chapter 1 post-load character select" },
    { 200, "Chapter 2 Garage" },
    { 210, "Chapter 2 Shibuya Terminal" },
    { 220, "Chapter 2 Dogenzaka Hill" },
    { 230, "Chapter 2 Chuo Street" },
    { 231, "Chuo Street tank fight" },
    { 232, "Chuo Street Poison Jam race" },
    { 240, "Chapter 2 Rokkaku Dai Heights" },
    { 241, "Rokkaku Dai fights?" },
    { 242, "Rokkaku Dai Poison Jam \"running away\" scenes?" },
    { 243, "Chapter 2 Rokkaku Dai Rhyth \"running away\" scenes" },
    { 250, "Chapter 2 99th Street" },
    { 251, "99th Street spotlight fight" },
    { 252, "99th Street Rapid 99 flag battle" },
    { 294, "Chapter 2 post-cutscene character select" },
    { 295, "Chapter 2 Roboy character select" },
    { 296, "Chapter 2 intro" },
    { 297, "Chapter 2 post-load character select" },
    { 300, "Chapter 3 Garage" },
    { 310, "Chapter 3 Shibuya Terminal" },
    { 320, "Chapter 3 Dogenzaka Hill" },
    { 330, "Chapter 3 Chuo Street" },
    { 340, "Chapter 3 Rokkaku Dai Heights" },
    { 341, "Chapter 3 Rokkaku Dai Rhyth \"running away\" scenes" },
    { 350, "Chapter 3 99th Street" },
    { 360, "Chapter 3 Tokyo Underground Sewage Facility" },
    { 361, "Garam Halfpipe Tutorial" },
    { 370, "Chapter 3 Bottom Point of Sewage Facility" },
    { 371, "Poison Jam Tagger's Tag" },
    { 394, "Chapter 3 post-cutscene character select" },
    { 395, "Chapter 3 Roboy character select" },
    { 396, "Chapter 3 intro" },
    { 397, "Chapter 3 post-load character select" },
    { 400, "Chapter 4 Garage" },
    { 410, "Chapter 4 Shibuya Terminal" },
    { 415, "Chapter 4 Dogenzaka Hill" },
    { 420, "Chapter 4 Chuo Street" },
    { 425, "Chapter 4 Rokkaku Dai Heights" },
    { 426, "Chapter 4 Rokkaku Dai Rhyth \"running away\" scenes" },
    { 430, "Chapter 4 Tokyo Underground Sewage Facility" },
    { 435, "Chapter 4 Bottom Point of Sewage Facility" },
    { 440, "Chapter 4 99th Street" },
    { 450, "Chapter 4 Hikage Street" },
    { 451, "Hikage Street terror drone fight" },
    { 452, "Hikage Street cop fights" },
    { 460, "Chapter 4 Kibogaoka Hill" },
    { 461, "Kibogaoka Hill chopper fight" },
    { 462, "Chapter 4 Kibogaoka Hill Boogie encounter" },
    { 463, "Chapter 4 Kibogaoka Hill Boogie leaving" },
    { 470, "Chapter 4 Skyscraper District and Pharaoh Park" },
    { 471, "Skyscraper District and Pharaoh Park cop fights" },
    { 472, "Skyscraper District and Pharaoh Park tagger's tag" },
    { 480, "Chapter 4 Highway Zero (immediately triggers cutscene)" },
    { 481, "Highway Zero tagger's tag" },
    { 494, "Chapter 4 post-cutscene character select" },
    { 495, "Chapter 4 Roboy character select" },
    { 496, "Chapter 4 intro" },
    { 497, "Chapter 4 post-load character select" },
    { 510, "Future Site with Jazz and Noise Tank" },
    { 511, "Doom Riders death ball" },
    { 512, "Immortals death ball" },
    { 513, "Love Shockers death ball" },
    { 514, "Future Site terror drone fight" },
    { 515, "Death ball practice" },
    { 594, "Chapter 5 post-cutscene character select" },
    { 595, "Chapter 5 Roboy character select (unused?)" },
    { 596, "Chapter 5 intro" },
    { 597, "Chapter 5 post-load character select" },
    { 600, "Chapter 6 Garage" },
    { 610, "Chapter 6 Shibuya Terminal" },
    { 615, "Chapter 6 Chuo Street" },
    { 620, "Chapter 6 Dogenzaka Hill" },
    { 625, "Chapter 6 Hikage Street" },
    { 630, "Chapter 6 Rokkaku-Dai Heights" },
    { 631, "Chapter 6 Rokkaku Dai Rhyth \"running away\" scenes" },
    { 635, "Chapter 6 Tokyo Underground Sewage Facility" },
    { 640, "Chapter 6 Kibogaoka Hill" },
    { 641, "Chapter 6 Kibogaoka Hill Boogie encounter" },
    { 642, "Chapter 6 Kibogaoka Hill Boogie leaving" },
    { 645, "Chapter 6 Bottom Point of Sewage Facility" },
    { 650, "Chapter 6 99th Street" },
    { 660, "Chapter 6 Sky Dinosaurian Square" },
    { 661, "Noise Tanks tagger's tag" },
    { 665, "Chapter 6 Future Site of Rokkaku Expo Stadium" },
    { 666, "Chapter 6 Future Site Jazz race" },
    { 670, "Chapter 6 Highway Zero" },
    { 671, "Chapter 6 Highway Zero Soda race" },
    { 675, "Chapter 6 Skyscraper District and Pharaoh Park" },
    { 694, "Chapter 6 post-cutscene character select" },
    { 695, "Chapter 6 Roboy character select" },
    { 696, "Chapter 6 intro" },
    { 697, "Chapter 6 post-load character select" },
    { 700, "Chapter 7 Garage" },
    { 794, "Chapter 7 post-cutscene character select" },
    { 795, "Chapter 7 Roboy character select" },
    { 796, "Chapter 7 intro" },
    { 797, "Chapter 7 post-load character select" },
    { 800, "Chapter 8 Garage" },
    { 880, "Site of Tokyo Line train fight" },
    { 894, "Chapter 8 post-cutscene character select" },
    { 895, "Chapter 8 Roboy character select" },
    { 896, "Chapter 8 intro" },
    { 897, "Chapter 8 post-load character select" },
    { 900, "Chapter 9 Garage" },
    { 965, "Chapter 9 Future Site of Rokkaku Expo Stadium" },
    { 966, "Chapter 9 Jazz race" },
    { 994, "Chapter 9 post-cutscene character select (unused?)" },
    { 995, "Chapter 9 Roboy character select" },
    { 996, "Chapter 9 intro (ending stinger)" },
    { 997, "Chapter 9 post-load character select" },
    { 2000, "Vs. Mode: City Rush, Future Site Stadium, 2P" },
    { 2001, "Vs. Mode: City Rush, Future Site Stadium, 3P" },
    { 2002, "Vs. Mode: City Rush, Future Site Stadium, 4P" },
    { 2010, "Vs. Mode: City Rush, Secondary Site Stadium, 2P" },
    { 2011, "Vs. Mode: City Rush, Secondary Site Stadium, 3P" },
    { 2012, "Vs. Mode: City Rush, Secondary Site Stadium, 4P" },
    { 2020, "Vs. Mode: City Rush, Sewer Regulatory District, 2P" },
    { 2021, "Vs. Mode: City Rush, Sewer Regulatory District, 3P" },
    { 2022, "Vs. Mode: City Rush, Sewer Regulatory District, 4P" },
    { 2030, "Vs. Mode: City Rush, Chuo Street, 2P" },
    { 2040, "Vs. Mode: City Rush, Bottom Point of Sewers, 2P" },
    { 2050, "Vs. Mode: City Rush, Highway Zero, 2P" },
    { 2100, "Vs. Mode: City Rush, Future Site Stadium, 1P" },
    { 2110, "Vs. Mode: City Rush, Secondary Site Stadium, 1P" },
    { 2120, "Vs. Mode: City Rush, Sewer Regulatory District, 1P" },
    { 2130, "Vs. Mode: City Rush, Chuo Street, 1P" },
    { 2140, "Vs. Mode: City Rush, Bottom Point of Sewers, 1P" },
    { 2150, "Vs. Mode: City Rush, Highway Zero, 1P" },
    { 2200, "Vs. Mode: Ball Hog, Future Site Stadium, >1P" },
    { 2210, "Vs. Mode: Ball Hog, Secondary Site Stadium, >1P" },
    { 2220, "Vs. Mode: Ball Hog, Sewer Regulatory District, >1P" },
    { 2300, "Vs. Mode: Ball Hog, Future Site Stadium, 1P" },
    { 2301, "Vs. Mode: Ball Hog, Future Site Stadium, 2P vs. AI" },
    { 2310, "Vs. Mode: Ball Hog, Secondary Site Stadium, 1P" },
    { 2311, "Vs. Mode: Ball Hog, Secondary Site Stadium, 2P vs. AI" },
    { 2320, "Vs. Mode: Ball Hog, Sewer Regulatory District, 1P" },
    { 2321, "Vs. Mode: Ball Hog, Sewer Regulatory District, 2P vs. AI" },
    { 2400, "Vs. Mode: Flag Battle, Showa Park, >1P" },
    { 2410, "Vs. Mode: Flag Battle, Raden Park, >1P" },
    { 2420, "Vs. Mode: Flag Battle, Tsuzura Park, >1P" },
    { 2500, "Vs. Mode: Flag Battle, Showa Park, 1P" },
    { 2510, "Vs. Mode: Flag Battle, Raden Park, 1P" },
    { 2520, "Vs. Mode: Flag Battle, Tsuzura Park, 1P" },
    { 2600, "Vs. Mode: Graffiti Wars, Showa Park, >1P" },
    { 2610, "Vs. Mode: Graffiti Wars, Raden Park, >1P" },
    { 2620, "Vs. Mode: Graffiti Wars, Tsuzura Park, >1P" },
    { 2700, "Vs. Mode: Graffiti Wars, Showa Park, 1P" },
    { 2710, "Vs. Mode: Graffiti Wars, Raden Park, 1P" },
    { 2720, "Vs. Mode: Graffiti Wars, Tsuzura Park, 1P" },
    { 2830, "Vs. Mode: Tagger's Tag, Showa Park, >1P" },
    { 2840, "Vs. Mode: Tagger's Tag, Raden Park, >1P" },
    { 2850, "Vs. Mode: Tagger's Tag, Tsuzura Park, >1P" },
    { 2930, "Vs. Mode: Tagger's Tag, Showa Park, 1P" },
    { 2940, "Vs. Mode: Tagger's Tag, Raden Park, 1P" },
    { 2950, "Vs. Mode: Tagger's Tag, Tsuzura Park, 1P" },
    { 3010, "Test Run: Shibuya Terminal Jet Graffiti" },
    { 3110, "Test Run: Shibuya Terminal Jet Tech" },
    { 3211, "Test Run: Chuo Street Jet Dash" },
    { 3222, "Test Run: Kibogoaka Hill Jet Dash" },
    { 3310, "Test Run: Shibuya Terminal Jet Flag" },
    { 3400, "Tutorial: Basic Controls" },
    { 3401, "Tutorial: Cop Battle 1" },
    { 3402, "Tutorial: Turning" },
    { 3403, "Tutorial: Wallride" },
    { 3404, "Tutorial: Phone Pole Grind 1" },
    { 3405, "Tutorial: Cop Battle 2" },
    { 3406, "Tutorial: Graffiti" },
    { 3407, "Tutorial: Grind Combos" },
    { 3408, "Tutorial: Mystery Tape" },
    { 3409, "Tutorial: Halfpipe" },
    { 3410, "Tutorial: Phone Pole Grind 2" },
    { 3411, "Tutorial: Lock Dash" },
    { 3412, "Tutorial: Combo Air" },
    { 3413, "Tutorial: Halfpipe (Demonstration)" },
};
const char *jp_mission_name(int number)
{
    size_t i;
    for (i = 0; i < sizeof s_mnames / sizeof s_mnames[0]; i++) if (s_mnames[i].no == number) return s_mnames[i].name;
    return "";
}
