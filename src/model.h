/* JSRF models (MDLB), read for the 3D view (m5).
 *
 * An MDLB (formats learned from JSRF ModTool's MDLB.cs, no code taken):
 * "MDLB", the part count, the material count; a 128-byte header per part
 * (+0 its vertex block's offset - 16, +12 its type: 0 the visible mesh, 1
 * shadow, 2 bone); the vertex block: +0 the vertices' offset - 16, +4 their
 * count, +12 a vertex's size, +16 the indices' offset - 16, +20 their count
 * (16-bit), +24 1 when a strip. A vertex: x, y, z, then by its size the
 * bone weights, a normal and the texture coordinates (u, v; v down).
 *
 * Which texture a model wears: in a character's .dat a NORM of models
 * follows a NORM of the same count of material lists (a count, then
 * texture ids); in objects a NORM of textures of the same count. */
#ifndef MODEL_H
#define MODEL_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int nv, nt;             /* vertices, triangles */
    float *p, *n, *t;       /* 3, 3 (when has_n), 2 a vertex */
    int has_n;
    uint32_t *ix;           /* 3 a triangle */
    uint32_t texid;         /* the texture it wears, 0 not known */
    float c[3], r;          /* its centre and radius */
    int index;              /* its number among the file's models */
    /* m9: for replacing it */
    int *tmat;              /* each triangle's material (its triangle group's) */
    size_t item, len;       /* the MDLB in the file */
    size_t norm, norm_end;  /* the NORM holding it */
    int part;               /* the one visible part, when its data ends the MDLB; else -1 */
    int nparts;
} JMesh;

typedef struct {
    JMesh *m;
    int n;
} JModels;

int jm_load(const uint8_t *buf, size_t size, JModels *out);

/* m8: m as a Wavefront .obj at obj_path (as the 3D view shows it, texture
 * coordinates up the image as Blender has them), its material in
 * mtl_path naming tex_png (a file name beside it; NULL: no texture);
 * name: the object's name. 0 done */
/* m9: the visible part of m replaced by the .obj at path (as jm_write_obj
 * writes them: mat<N> materials keep the game's material N), in the file
 * buf (size); the new file in nb, nsize (malloc'd). Each new vertex takes
 * its bone weights from the nearest old one. 0 done; else err says why */
int jm_import_obj(const uint8_t *buf, size_t size, const JMesh *m, const char *path,
                  uint8_t **nb, size_t *nsize, char *err, size_t errn);

int jm_write_obj(const JMesh *m, const char *obj_path, const char *mtl_path, const char *mtl_file,
                 const char *tex_png, const char *name);
void jm_free(JModels *ms);

#endif
