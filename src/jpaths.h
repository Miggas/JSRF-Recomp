/* m29o: paths things follow on a stage, read from the game's files.
 *  - Car routes: StgObj/CarObjNN.bin (a MULT; its first NORM's items): a point
 *    count n, a loop time in frames, then n points of 16 bytes (the frame the
 *    car is there, x, y, z). Closed loops; the routes of an area's stages
 *    together (CarObj01: Shibuya Terminal, Chuo Street and one more).
 *  - Mission chains: Mission/mssnNNNN.bin: tables of 20-byte entries (top byte
 *    of the first word the point count, then the first point's place in the
 *    file, 0, a turn, 0) into runs of 12-byte points (x, y, z). Short chains on
 *    the stage's floor: where the crowd walks, it seems (not yet sure). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int n, loop;            /* points; loop time in frames (0: not a loop) */
    float *p;               /* n * 3 */
    int *key;               /* n frames, or NULL */
    int file, idx;          /* where it came from (the caller's file number, the item or entry) */
} JPath;

typedef struct {
    JPath *path;
    int npath;
} JPaths;

/* car routes from one CarObj file's bytes (appended to out); how many, -1 not one */
int  jp_cars(const uint8_t *b, size_t n, int file, JPaths *out);
/* mission chains from one mission file's bytes (appended); how many */
int  jp_mission(const uint8_t *b, size_t n, int file, JPaths *out);
void jp_free(JPaths *ps);

/* m29p: a mission's marks (Mission/mssnNNNN.bin, by the header's section table at word 13: (offset, count)
 * pairs). Section 2: 44-byte entries (type, 0, x y z, 0, facing (0x10000 a turn), 0, x2 y2 z2): type 0 a
 * spawn (place and facing), 1 a second point only, 2 both. Section 7: four-corner planes (48 bytes: a
 * huge flat one far under the map, upright ones at its edges). Section 9: an upright frame and points after it. */
typedef struct { int type; float p[3]; int facing; float p2[3]; } JSpawn;
typedef struct { float c[4][3]; } JPlane;
typedef struct {
    JSpawn *sp; int nsp;
    JPlane *pl; int npl;
    float frame[4][3], fpt[3][3]; int has_frame;
} JMarks;
int  jp_marks(const uint8_t *b, size_t n, JMarks *m);   /* 0 read, -1 not one */
void jp_marks_free(JMarks *m);
