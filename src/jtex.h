/* JSRF game files: finding, decoding and replacing their textures.
 *
 * Containers (formats learned from JSRF_ModTool by neodoso, GPLv3, and his
 * jsrf-inside blog; no code taken from it):
 *   MULT  "MULT", count, then entries (start, end, size, ?) of NORMs, each
 *         entry's end being the next entry's offset;
 *   NORM  "NORM", count, then children (start, end, ?, ?) relative to the
 *         NORM, each child's end the next child's offset;
 *   indexed (stage files): blocks one after another: type 1 a model
 *         (size at +8, a list of texture ids at +16), type 0 a block of the
 *         size at +4, else the first word is the block's size.
 * A texture item: a 32-byte header (+0 id, +20 the size, square, +24 the
 * format: 5 DXT1, 6 DXT3, 7 DXT5, +25 1 alpha / 5 none, +26 1 swizzled,
 * +28 the mip levels, 0 one), then its levels, largest first.
 * Files on their own: .grf (graffiti, a TGA), .prs (graffiti, DXT3 128x128),
 * .JTX (DXT3 1024x1024). */
#ifndef JTEX_H
#define JTEX_H
#include <stddef.h>
#include <stdint.h>

enum { JT_DXT1 = 1, JT_DXT3, JT_DXT5, JT_ARGB, JT_TGA };

typedef struct {
    int fmt;            /* JT_* */
    int w, h, levels;   /* levels: 1 or more */
    int swizzled, alpha;
    uint32_t id;
    size_t hdr;         /* the item's header in the file (no header: (size_t)-1) */
    size_t data, len;   /* its pixels, the room they have */
    /* m2: where it sits, for making it bigger: box 1 a NORM's child (the
     * NORM at norm..norm_end), 2 an indexed block (its size at blk) */
    int box;
    size_t norm, norm_end, blk;
} JTex;

typedef struct {
    char path[1024];
    const char *kind;   /* "MULT", "NORM", "indexed", "graffiti (TGA)", ... */
    uint8_t *buf;
    size_t size;
    JTex *tex;
    int ntex;
} JFile;

/* 0 when the file was read and looked at (ntex may be 0) */
int jf_load(JFile *f, const char *path);
/* the same from a file already read (buf: malloc'd, the JFile owns it after) */
int jf_parse(JFile *f, const char *path, uint8_t *buf, size_t size);
void jf_free(JFile *f);
int jf_save(const JFile *f, const char *path);

const char *jt_fmt_name(int fmt);
/* rgba: w * h * 4 bytes, the largest level */
int jt_decode(const JFile *f, const JTex *t, uint8_t *rgba);
/* the texture replaced by rgba (the same size), its smaller levels made
 * again from it; 0 done */
int jt_encode(JFile *f, const JTex *t, const uint8_t *rgba);
/* m2: a texture inside a .dat made size x size (bigger, square, a power of
 * two up to 4096), everything after it moved along and the container's
 * offsets and sizes with it, its levels as many as before plus the new
 * ones; the pixels left blank for jt_encode. The file is parsed again
 * (f->tex changes). 0 done */
int jt_grow(JFile *f, int k, int size);

/* bytes of one level of w x h in fmt */
size_t jt_level_size(int fmt, int w, int h);

#endif
