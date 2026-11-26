/* m27: the stage editor, level 2: a stage's files read into their parts and
 * written back from them (the unchanged rebuild). Every offset in the files
 * is worked out again on writing, so later levels can change sizes.
 *
 * StgNN_.bin: a 0xA0-byte header ((offset, size) of three blocks, the
 * stage model count, the rest kept as it is), each block padded to the next
 * multiple of 16 after it. Offsets inside a block count from its start.
 *  - block 0, collision: a header (areas' offset and count, then kept
 *    words), 32-byte areas (a box, then the (offset, count) of their run of
 *    models), 112-byte models (box, rows, position; +88 their vertices'
 *    and triangles' (offset, count); 8 bytes kept), 16-byte vertices
 *    (shared by models), 8-byte triangles.
 *  - block 1, grind rails: an id, 1024 slots of (offset, count) of lists
 *    of rail offsets (lists shared by slots; an empty slot points at the
 *    first list; m29f: never two slots on one list: the game makes a
 *    list's offsets into addresses in place when it loads), 36-byte rails (their points' (offset, count), the rest
 *    kept), 24-byte points (position, normal).
 *  - block 2, areas: a word, the areas' (offset, count), then an area's
 *    (offset, count) of 80-byte placements; 340-byte areas (+144 their
 *    144-byte portals, +152 their visible list of area numbers, +240..+264
 *    four lists always empty, pointing at the end).
 * StgNN_NN.dat: self-sized blocks one after another (jtex.h), then 4
 * bytes. */
#ifndef JSBUILD_H
#define JSBUILD_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *p; size_t n; } JBytes;

typedef struct { uint8_t box[24]; int first, count; } JCArea;
typedef struct { uint8_t head[88]; int vset, tset; uint8_t tail[8]; } JCModel;
typedef struct { uint8_t rest[28]; JBytes pts; } JRail;
typedef struct { uint8_t rec[340]; JBytes port, vis; JBytes ex[4]; int exn[4]; } JVArea;   /* m29n: ex, the lists at +240..264 (Stg11: 128-byte records at +264) */

typedef struct {
    uint8_t head[0xA0];
    uint8_t pad[3][32];
    int padn[3];
    /* block 0 */
    JBytes chead;                       /* its header, up to the areas */
    JCArea *ca; int nca;
    JCModel *cm; int ncm;
    JBytes *vs; int nvs;
    JBytes *ts; int nts;
    /* block 1 */
    uint32_t rid;
    int slot[1024];                     /* the list a slot uses, -1 none */
    int **list; int *nlist; int nlists; /* rail numbers */
    JRail *rail; int nrail;
    /* block 2 */
    uint32_t dw0;
    JBytes *pl;                         /* an area's placements */
    JVArea *va; int nva;
} JSBin;

/* 0 read; else err says what isn't as expected */
int  jsb_parse(const uint8_t *b, size_t n, JSBin *s, char *err, size_t errn);
/* the file again; moved: every part in another place (the order of the
 * shared vertices, triangles, rail points, portals and visible lists
 * reversed, the blocks 16 bytes further apart), to test that the game goes
 * by the offsets written. malloc'd, *n its size */
uint8_t *jsb_write(const JSBin *s, int moved, size_t *n);
void jsb_free(JSBin *s);

typedef struct { JBytes *blk; int nblk; JBytes tail; } JSDat;
int  jsd_parse(const uint8_t *b, size_t n, JSDat *d, char *err, size_t errn);
uint8_t *jsd_write(const JSDat *d, size_t *n);
void jsd_free(JSDat *d);

/* a stage's files (StgNN_.bin and StgNN_NN.dat in dir) rebuilt. Each file
 * is handed to out(name, data, size, same, u) (same: identical to the
 * file read). 0 done; else err */
int  jsb_rebuild(const char *dir, const char *name, int moved,
                 int (*out)(const char *file, const uint8_t *data, size_t n, int same, void *u), void *u,
                 char *err, size_t errn);

#endif
