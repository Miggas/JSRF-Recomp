/* jsrf_modtool_cli: the texture work without the window (tests, scripts).
 *   list FILE...            the textures in each file
 *   export FILE DIR         every texture to DIR/<file>_<n>.png
 *   import FILE N PNG OUT   texture N replaced by PNG (the same size), saved as OUT
 *   stage DIR StgXX [OUT]   m26: a stage read whole (counts); with OUT, written there as .obj
 *   rebuild DIR StgXX [moved] [OUT]  m27: a stage's files rebuilt from their parts: same or not;
 *                           with OUT, written there */
#include "jtex.h"
#include "jstage.h"
#include "jsbuild.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG   /* m21: JPG, BMP, GIF (its first frame), PSD (the merged image) */
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_PSD
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char *base(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static int rb_out(const char *file, const uint8_t *d, size_t n, int same, void *u)
{
    const char *dir = (const char *)u;
    printf("  %-16s %9zu bytes  %s\n", file, n, same ? "identical" : "different");
    if (dir) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, file);
        if (!write_file(p, d, n)) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    JFile f;
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: %s list FILE... | export FILE DIR | import FILE N PNG OUT\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "stage") && argc >= 4) {   /* m26 */
        JStage st;
        char err[512];
        int k, sc[JST_NSURF] = { 0 };
        if (jst_load(argv[2], argv[3], &st, err, sizeof err)) { printf("%s\n", err); return 1; }
        for (k = 0; k < st.nct; k++) sc[jst_surf_index(st.csurf[k])]++;
        printf("%s: %d .dat files, %d textures; %d stage models (%d see-through), %d props: %d vertices, %d triangles\n",
               st.name, st.ndat, st.ntex, st.nmodels, st.nsee, st.nprops, st.nv, st.nt);
        printf("collision: %d models, %d triangles;", st.ncmodels, st.nct);
        for (k = 0; k < JST_NSURF; k++) if (sc[k]) printf(" %s %d", jst_surf[k].name, sc[k]);
        printf("\nrails: %d, %d points; box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", st.nrail, st.nrp,
               st.lo[0], st.lo[1], st.lo[2], st.hi[0], st.hi[1], st.hi[2]);
        { int nn = 0; for (k = 0; k < st.nt; k++) nn += st.ttex[k] < 0; printf("triangles without a known texture: %d\n", nn); }
        { int cl = 0, pw = 0; for (k = 0; k < st.ncmodel; k++) cl += st.cpiece[k] >= 0; for (k = 0; k < st.npiece; k++) pw += st.pieces[k].ncm > 0;
          printf("pieces: %d (%d with collision); collision models with a piece: %d of %d\n", st.npiece, pw, cl, st.ncmodel); }
        if (argc >= 5 && jst_write_obj(&st, argv[4], err, sizeof err)) { printf("%s\n", err); return 1; }
        jst_free(&st);
        return 0;
    }
    if (!strcmp(argv[1], "rebuild") && argc >= 4) {   /* m27 */
        char err[512];
        int moved = argc >= 5 && !strcmp(argv[4], "moved");
        const char *outdir = argc >= 5 + moved ? argv[4 + moved] : NULL;
        printf("%s%s:\n", argv[3], moved ? " (moved)" : "");
        if (jsb_rebuild(argv[2], argv[3], moved, rb_out, (void *)outdir, err, sizeof err)) { printf("%s\n", err); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "list")) {
        for (i = 2; i < argc; i++) {
            int k;
            if (jf_load(&f, argv[i])) { printf("%s: can't read\n", argv[i]); continue; }
            printf("%s: %s, %d texture(s)\n", base(argv[i]), f.kind, f.ntex);
            for (k = 0; k < f.ntex; k++)
                printf("  %3d  %4dx%-4d %-4s levels %2d%s%s  id %08X\n", k, f.tex[k].w, f.tex[k].h,
                       jt_fmt_name(f.tex[k].fmt), f.tex[k].levels, f.tex[k].alpha ? " alpha" : "",
                       f.tex[k].swizzled ? " swizzled" : "", f.tex[k].id);
            jf_free(&f);
        }
        return 0;
    }
    if (!strcmp(argv[1], "export") && argc == 4) {
        if (jf_load(&f, argv[2])) return 1;
        for (i = 0; i < f.ntex; i++) {
            char out[2048], nm[512], *dot;
            uint8_t *px = (uint8_t *)malloc((size_t)f.tex[i].w * f.tex[i].h * 4);
            snprintf(nm, sizeof nm, "%s", base(argv[2]));
            if ((dot = strrchr(nm, '.'))) *dot = '_';
            snprintf(out, sizeof out, "%s/%s_%d.png", argv[3], nm, i);
            if (!jt_decode(&f, &f.tex[i], px) && stbi_write_png(out, f.tex[i].w, f.tex[i].h, 4, px, f.tex[i].w * 4))
                printf("%s\n", out);
            else printf("texture %d: not exported\n", i);
            free(px);
        }
        jf_free(&f);
        return 0;
    }
    if (!strcmp(argv[1], "import") && argc == 6) {
        int n = atoi(argv[3]), w, h, c;
        uint8_t *px;
        if (jf_load(&f, argv[2]) || n < 0 || n >= f.ntex) { fprintf(stderr, "no texture %d\n", n); return 1; }
        px = stbi_load(argv[4], &w, &h, &c, 4);
        if (!px) { fprintf(stderr, "can't read %s\n", argv[4]); return 1; }
        if (w != f.tex[n].w || h != f.tex[n].h) {
            fprintf(stderr, "%s is %dx%d, the texture %dx%d\n", argv[4], w, h, f.tex[n].w, f.tex[n].h);
            return 1;
        }
        if (jt_encode(&f, &f.tex[n], px) || jf_save(&f, argv[5])) { fprintf(stderr, "failed\n"); return 1; }
        printf("%s: texture %d replaced\n", argv[5], n);
        stbi_image_free(px);
        jf_free(&f);
        return 0;
    }
    if (!strcmp(argv[1], "grow") && argc == 7) {      /* grow FILE N SIZE PNG OUT */
        int n = atoi(argv[3]), sz = atoi(argv[4]), w, h, c;
        uint8_t *px;
        int raw, th;
        if (jf_load(&f, argv[2]) || n < 0 || n >= f.ntex) { fprintf(stderr, "can't grow\n"); return 1; }
        raw = !f.tex[n].box && f.tex[n].hdr == (size_t)-1;      /* m16: a graffiti: SIZE its new width */
        th = raw ? f.tex[n].h * (sz / f.tex[n].w) : sz;
        if (raw ? (sz % f.tex[n].w || jt_grow_raw(&f, n, sz / f.tex[n].w)) : jt_grow(&f, n, sz)) {
            int mw, mh;
            if (raw && !jt_raw_max(&f, n, &mw, &mh))
                fprintf(stderr, "can't grow: a graffiti can be up to %dx%d (4 times its original), 2 or 4 times its size\n", mw, mh);
            else
                fprintf(stderr, "can't grow\n");
            return 1;
        }
        px = stbi_load(argv[5], &w, &h, &c, 4);
        if (!px || w != sz || h != th) { fprintf(stderr, "need a %dx%d image\n", sz, th); return 1; }
        if (jt_encode(&f, &f.tex[n], px) || jf_save(&f, argv[6])) { fprintf(stderr, "failed\n"); return 1; }
        printf("%s: texture %d now %dx%d\n", argv[6], n, sz, th);
        return 0;
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
