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
