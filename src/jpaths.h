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
void jp_free(JPaths *ps);

/* m29s: a mission file (Mission/mssnNNNN.bin), read by its header's table of blocks (offset,
 * count pairs from byte 52), as the GG Notebook (github: GG-Notebook, WTFPL) lays it out:
 * stage number at 0 (minor, major), then blocks: 1 positions, 6 plane regions (exit planes),
 * 8 teleporters, 10 pickup lists, 12 parked-car lists, 13 pedestrian lists (the crowd's
 * paths), 15 graffiti souls, 24 death warps. */
typedef struct { int type; float p[3]; int facing; float p2[3]; } JSpawn;   /* 0 a place and facing (respawns), 1 where you're sent back to (locked areas), 2 a spawn and where you skate to on entering */
typedef struct { float c[4][3]; } JPlane;
typedef struct { int type; JPlane *pl; int npl; float to[3]; } JWarp;      /* type 0 water, 1-2 pits; cross a plane: warped to `to` */
typedef struct { float c[6][3]; float cam[3]; } JTele;
typedef struct { float p[3]; int type; } JPick;                              /* 0 yellow can, 1 blue, 2-3 red, 4 green, 5 Vs item */
typedef struct { float p[3], rot[3], scale[3]; int type; } JCar;
typedef struct { float p[3]; int id; } JSoul;
typedef struct {
    int major, minor;
    JSpawn *sp; int nsp;
    JPlane *exitpl; int nexit;
    JWarp *dw; int ndw;
    JTele *tp; int ntp;
    JPick *pk; int npk;
    JCar *car; int ncar;
    JSoul *soul; int nsoul;
    JPaths ped;
} JMission;
int  jp_mission_read(const uint8_t *b, size_t n, JMission *m);   /* 0 read, -1 not one */
void jp_mission_free(JMission *m);
const char *jp_mission_name(int number);                         /* from the GG Notebook's list, or "" */
