/* m26: a JSRF stage read whole, for the stage viewer (stage editor, level 1).
 *
 * Formats learned from the files themselves and, for the .bin, from
 * neodoso's JSRF ModTool (GPLv3: formats only, no code taken):
 *
 * StgXX_YY.dat ("indexed", see jtex.h): blocks of type 1 are models: +4 the
 *   model's id (counted across all the stage's .dat files), +8 its size, +12
 *   its kind, +16 a count of texture ids that follow; type 0 blocks and the
 *   rest are textures. Kind 0: an MDLB (a prop, placed by the .bin). Kinds 1
 *   and 2: a stage model, already where it stands (2: drawn see-through):
 *   from its data's start (D), +0x10 the mesh header's offset and +0x80 the
 *   group table's offset (offsets from D + 0x10) and group count; the mesh
 *   header: the vertices' offset, count, Direct3D vertex format (FVF), size,
 *   the 16-bit indices' offset, count, and 1 when a strip; a group (32
 *   bytes): its triangle count, first index, ?, the texture (in the block's
 *   id list), ?, ?, ?, its bounding sphere's offset.
 * StgXX_.bin: three blocks (offset, size), then the stage model count.
 *   0 collision: (offset, count) of 32-byte areas (a box, then the offset
 *     and count of their 112-byte models: a box, three rows and a position
 *     placing the model, its 16-byte vertices and 8-byte triangles: three
 *     10-bit indices, then the surface's flags in the low 16 bits).
 *   1 grind rails: an id, then 1024 (offset, count) slots of lists of rail
 *     offsets; a rail: its points' offset and count, flags, a box; a point:
 *     position and normal.
 *   2 placements: a count, then (offset, count) of 12 lists of 80-byte
 *     placements: three rows and a position, the model's id and flags. */
#ifndef JSTAGE_H
#define JSTAGE_H
#include <stddef.h>
#include <stdint.h>

/* the collision surfaces (flags), with JSRF ModTool's names and colours */
#define JST_NSURF 12
typedef struct { uint16_t flag; const char *name; uint8_t r, g, b; } JSSurf;
extern const JSSurf jst_surf[JST_NSURF];   /* the last: anything else */
int jst_surf_index(uint16_t flag);

enum { JST_T_STAGE = 0, JST_T_SEE = 1, JST_T_PROP = 2 };

typedef struct { uint32_t id; int w, h; uint8_t *rgba; } JSTex;

typedef struct {
    char name[16];                      /* "Stg30" */
    /* what is drawn: stage models and props, in place */
    int nv, nt;
    float *p, *uv;                      /* 3, 2 a vertex */
    uint32_t *col;                      /* a vertex's colour (RGBA bytes) */
    uint32_t *ix;                       /* 3 a triangle */
    int *ttex;                          /* a triangle's texture (-1: none known) */
    uint8_t *tkind;                     /* JS_T_* */
    int ntex;
    JSTex *tex;
    int nmodels, nprops, nsee;
    /* collision, in place */
    int ncv, nct;
    float *cp;
    uint32_t *cix;
    uint16_t *csurf;
    int ncmodels;
    /* grind rails: rail k's points are rp[rs[k] .. rs[k] + rn[k]) (3 floats each) */
    int nrail, nrp;
    int *rs, *rn;
    float *rp, *rnrm;
    float lo[3], hi[3];                 /* the stage's box */
    int ndat;                           /* .dat files read */
} JStage;

/* stage `name` ("Stg30") from the folder dir (Media/Stage): 0 done; else err */
int jst_load(const char *dir, const char *name, JStage *s, char *err, size_t errn);
void jst_free(JStage *s);
/* the stage's name from one of its files' names ("stg30_02.dat" -> "Stg30"); 0 not a stage file */
int jst_name_of(const char *file, char *name, size_t n);

/* m26: as Wavefront .obj files in dir: <name>_visual.obj (+ .mtl, a PNG per
 * texture in <name>_tex/), <name>_collision.obj (a material per surface),
 * <name>_rails.obj (lines). 0 done */
int jst_write_obj(const JStage *s, const char *dir, char *err, size_t errn);

#endif
