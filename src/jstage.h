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

/* m26d: JST_T_SHADOW: see-through models with no texture coordinates (Direct3D
 * format 0x42: position and colour, white, texture id 0): not drawn as they
 * are in the game; the buildings' shadow volumes, stretched along the light: the game darkens what is inside them, the shapes themselves aren't seen (Miggas) */
enum { JST_T_STAGE = 0, JST_T_SEE = 1, JST_T_PROP = 2, JST_T_SHADOW = 3, JST_T_GONE = 4 /* m28: a deleted prop's */ };

/* m28: a prop as placed (stage editor, level 3): its placement in the .bin's
 * block 2 (area's list, entry; entry -1: a copy made in the editor), the
 * entry's 80 bytes, its rows and position (r: three rows, the position), and
 * where its vertices and triangles are in the stage, with its own
 * (unplaced) vertex positions to place it again */
typedef struct {
    int area, entry, model_id;
    uint8_t bytes[80];
    float r[12];
    int v0, nv, t0, nt;
    float *local;
    int removed;
} JSProp;

typedef struct { uint32_t id; int w, h; uint8_t *rgba; } JSTex;

/* m29: a stage model (kinds 1, 2): where it is in its .dat file (the data D
 * at doff), its vertices and triangles in the stage */
typedef struct { int id, kind, dat; size_t doff, boff; int v0, nv, t0, nt; } JSModel;   /* m29g: boff its block */

/* m29: a piece of the map: one connected part of a stage model (a building,
 * a pillar, a sign...): its vertices and triangles, its box and middle as
 * read; how it's been moved (turned yaw degrees about its middle, then
 * moved by t) or removed; the collision models moving with it */
typedef struct {
    int model;
    int *v, nv, *t, nt;
    float c[3], lo[3], hi[3];
    float pv[3];                        /* the point it turns about (its middle, or a group's) */
    float yaw, t3[3];
    int removed, touched;
    int src;                            /* m29g: a copy's source piece (-1: as read) */
    float yaw_base;                     /* m29g: a copy's turn from the file's at its start */
    int *cm, ncm;
    int *rail, nrail;                   /* m29: the grind rails moving with it */
} JSPiece;

/* m29: a collision model as read: its vertices and triangles in the stage */
typedef struct { int area, v0, nv, t0, nt, src, part; } JSCModel;   /* m29g: src a copy's source (-1); m29i: part 1 once split */

typedef struct {
    char name[16];                      /* "Stg30" */
    /* what is drawn: stage models and props, in place */
    int nv, nt;
    float *p, *uv;                      /* 3, 2 a vertex */
    uint32_t *col;                      /* a vertex's colour (RGBA bytes) */
    uint32_t *ix;                       /* 3 a triangle */
    int *ttex;                          /* a triangle's texture (-1: none known) */
    uint8_t *tkind;                     /* JS_T_* */
    int *tgroup;                        /* m29g: a triangle's group in its stage model (-1 a prop's) */
    int ntex;
    JSTex *tex;
    int nmodels, nprops, nsee, nshadow;
    /* collision, in place */
    int ncv, nct;
    float *cp;
    uint32_t *cix;
    uint16_t *csurf;
    int *ctsi;                          /* m29i: a collision triangle's place in its model's triangle set as read */
    int ncmodels;
    /* grind rails: rail k's points are rp[rs[k] .. rs[k] + rn[k]) (3 floats each) */
    int nrail, nrp;
    int *rs, *rn;
    float *rp, *rnrm;
    float lo[3], hi[3];                 /* the stage's box */
    int ndat;                           /* .dat files read */
    int has_bin;                        /* m26: its StgNN_.bin was found (else no collision, rails, props) */
    char binfile[260];                  /* m28: its name as on disk */
    char datfile[64][64];               /* m29: the .dat files' names, by number */
    JSModel *models; int nmodel;        /* m29 */
    JSPiece *pieces; int npiece;
    int *tpiece;                        /* a triangle's piece (-1: a prop's) */
    int *vpiece;                        /* a vertex's piece (-1) */
    float *p0, *cp0;                    /* positions as read (pieces, collision) */
    JSCModel *cmodels; int ncmodel;
    int *cpiece;                        /* a collision model's piece (-1: none) */
    int *ctpiece;                       /* m29i: a collision triangle's piece: the smallest round it (-1) */
    float *rp0, *rnrm0;                 /* rail points and normals as read */
    int *rpiece;                        /* a rail's piece (-1) */
    int *rsrc;                          /* m29g: a copied rail's source (-1) */
    uint32_t *rflag;                    /* m29m: a rail's type, as read (0: 10 points, 1: 500 points) */
    JSProp *props; int nprop;           /* m28 */
} JStage;

/* stage `name` ("Stg30") from the folder dir (Media/Stage): 0 done; else err */
int jst_load(const char *dir, const char *name, JStage *s, char *err, size_t errn);
void jst_free(JStage *s);
/* m28: prop k placed again from its r (or hidden when removed) */
void jst_prop_update(JStage *s, int k);
/* m28: a copy of prop k (in the same area, a new entry); its index, -1 out of memory */
int  jst_prop_dup(JStage *s, int k);
/* m29: piece k placed again from its yaw and t3 (or hidden when removed), its collision with it */
void jst_piece_update(JStage *s, int k);
/* m29g: copies of the n pieces in set (each in place over its source, as
 * the source is now), with their collision and rails; the first copy's
 * index, -1 out of memory */
int  jst_piece_copy(JStage *s, const int *set, int n);
int  jst_cmodel_split(JStage *s, int ci, const uint8_t *take, int to);   /* m29i */
int  jst_rail_add(JStage *s, const float *pts, const float *nrm, int n);   /* m29j */
/* m28: the prop a triangle belongs to, -1 none */
int  jst_prop_of_tri(const JStage *s, int tri);
/* the stage's name from one of its files' names ("stg30_02.dat" -> "Stg30"); 0 not a stage file */
int jst_name_of(const char *file, char *name, size_t n);

/* m26: as Wavefront .obj files in dir: <name>_visual.obj (+ .mtl, a PNG per
 * texture in <name>_tex/), <name>_collision.obj (a material per surface),
 * <name>_rails.obj (lines). 0 done */
int jst_write_obj(const JStage *s, const char *dir, char *err, size_t errn);

#endif
