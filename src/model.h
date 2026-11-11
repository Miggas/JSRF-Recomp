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
} JMesh;

typedef struct {
    JMesh *m;
    int n;
} JModels;

int jm_load(const uint8_t *buf, size_t size, JModels *out);
void jm_free(JModels *ms);

#endif
