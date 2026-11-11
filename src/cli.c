/* jsrf_modtool_cli: the texture work without the window (tests, scripts).
 *   list FILE...            the textures in each file
 *   export FILE DIR         every texture to DIR/<file>_<n>.png
 *   import FILE N PNG OUT   texture N replaced by PNG (the same size), saved as OUT */
#include "jtex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char *base(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

int main(int argc, char **argv)
{
    JFile f;
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: %s list FILE... | export FILE DIR | import FILE N PNG OUT\n", argv[0]);
        return 2;
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
    fprintf(stderr, "bad arguments\n");
    return 2;
}
