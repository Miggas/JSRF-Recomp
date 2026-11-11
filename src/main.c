/* JSRF Mod Tool: the textures in Jet Set Radio Future's game files, shown,
 * exported to PNG and replaced from PNG, on Windows and Linux.
 *
 * An edited file is changed in place, in the game folder, after its
 * original is kept in mod_backups/ at the game's own path
 * (mod_backups/Media/Player/Beat.dat); Undo puts it back. (m1b: m1 saved
 * edited files to mods/ for the recomp to open instead: not seen by the
 * game, Miggas; in place needs nothing from the recomp.)
 *
 * The window is drawn with Nuklear (third_party/nuklear) on SDL2, as the
 * launcher's. File formats: see jtex.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "nuklear.h"
#include "nuklear_sdl_renderer.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "jtex.h"
#include "util.h"

#define MODTOOL_VERSION "m1b"

/* ---- the files ---- */

typedef struct {
    char rel[512];      /* from the game folder, '/' between: Media/Player/Beat.dat */
    int ntex;
} Entry;

static char s_game[LPATH];              /* the game folder (it has Media/) */
static Entry *s_files;
static int s_nfiles, s_capfiles;
static SDL_atomic_t s_scan_busy, s_scan_done, s_scan_seen;
static SDL_mutex *s_mx;

static int s_sel_file = -1, s_sel_tex = -1;
static JFile s_cur;                     /* the selected file, its mod if there is one */
static int s_cur_modded;
static SDL_Texture *s_preview;
static int s_pw, s_ph;
static char s_msg[LPATH + 600];
static char s_filter[64];

static int want_ext(const char *n)
{
    const char *d = strrchr(n, '.');
    if (!d) return 0;
    return !SDL_strcasecmp(d, ".dat") || !SDL_strcasecmp(d, ".grf") || !SDL_strcasecmp(d, ".prs")
        || !SDL_strcasecmp(d, ".jtx");
}

static void game_path(char *out, const char *rel)        /* the original */
{
    char r[LPATH], *p;
    snprintf(r, sizeof r, "%s", rel);
    for (p = r; *p; p++) if (*p == '/') *p = SEP[0];
    pjoin(out, s_game, r);
}

static void backup_path(char *out, const char *rel)      /* its original, kept */
{
    char r[LPATH], *p;
    snprintf(r, sizeof r, "mod_backups/%s", rel);
    for (p = r; *p; p++) if (*p == '/') *p = SEP[0];
    pjoin(out, s_game, r);
}

/* m1b: the game copies most of its files (characters, stages, objects,
 * HUD...) to its cache (Z:, the save folder's Cache: savegame/Cache in
 * the game folder) the first time and reads them from there after: an
 * edit goes to that copy too, when there is one */
static void cache_path(char *out, const char *rel)
{
    char r[LPATH], *p;
    snprintf(r, sizeof r, "savegame/Cache/%s", rel);
    for (p = r; *p; p++) if (*p == '/') *p = SEP[0];
    pjoin(out, s_game, r);
}

static void edit_path(char *out, const char *rel, int n)  /* where a texture is exported */
{
    char nm[600], *p;
    snprintf(nm, sizeof nm, "texture_edits" SEP "%s", rel + (strncmp(rel, "Media/", 6) ? 0 : 6));
    for (p = nm + 14; *p; p++) if (*p == '/' || *p == '.') *p = '_';
    snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "_%d.png", n);
    pjoin(out, s_game, nm);
}

/* ---- scanning, on a thread ---- */

typedef struct { char dir[LPATH]; char rel[512]; } Walk;

static void walk(const char *dir, const char *rel);

static void walk_cb(const char *name, bool isdir, void *u)
{
    const Walk *w = (const Walk *)u;
    char p[LPATH], r[512];
    if (name[0] == '.') return;
    pjoin(p, w->dir, name);
    snprintf(r, sizeof r, "%s/%s", w->rel, name);
    if (isdir) { walk(p, r); return; }
    if (!want_ext(name)) return;
    SDL_AtomicAdd(&s_scan_seen, 1);
    {
        size_t len = 0;
        char *b = read_file(p, &len);
        JFile f;
        if (!b) return;
        jf_parse(&f, p, (uint8_t *)b, len);
        if (f.ntex > 0) {
            SDL_LockMutex(s_mx);
            if (s_nfiles == s_capfiles) {
                s_capfiles = s_capfiles ? s_capfiles * 2 : 256;
                s_files = (Entry *)realloc(s_files, sizeof(Entry) * (size_t)s_capfiles);
            }
            snprintf(s_files[s_nfiles].rel, sizeof s_files[s_nfiles].rel, "%s", r);
            s_files[s_nfiles].ntex = f.ntex;
            s_nfiles++;
            SDL_UnlockMutex(s_mx);
        }
        jf_free(&f);
    }
}

static void walk(const char *dir, const char *rel)
{
    Walk w;
    snprintf(w.dir, sizeof w.dir, "%s", dir);
    snprintf(w.rel, sizeof w.rel, "%s", rel);
    list_dir(dir, walk_cb, &w);
}

static int scan_thread(void *u)
{
    char media[LPATH];
    (void)u;
    pjoin(media, s_game, "Media");
    walk(media, "Media");
    SDL_AtomicSet(&s_scan_done, 1);
    SDL_AtomicSet(&s_scan_busy, 0);
    return 0;
}

static void close_file(void)
{
    jf_free(&s_cur);
    if (s_preview) SDL_DestroyTexture(s_preview);
    s_preview = NULL;
    s_sel_tex = -1;
}

static void start_scan(void)
{
    char media[LPATH];
    if (SDL_AtomicGet(&s_scan_busy)) return;
    pjoin(media, s_game, "Media");
    if (!is_dir(media)) {
        snprintf(s_msg, sizeof s_msg, "No Media folder in %s: choose the game folder (the one with default.xbe and Media).", s_game);
        return;
    }
    close_file();
    s_sel_file = -1;
    SDL_LockMutex(s_mx);
    s_nfiles = 0;
    SDL_UnlockMutex(s_mx);
    SDL_AtomicSet(&s_scan_seen, 0);
    SDL_AtomicSet(&s_scan_done, 0);
    SDL_AtomicSet(&s_scan_busy, 1);
    s_msg[0] = 0;
    SDL_DetachThread(SDL_CreateThread(scan_thread, "scan", NULL));
}

static int cmp_entry(const void *a, const void *b)
{
    return SDL_strcasecmp(((const Entry *)a)->rel, ((const Entry *)b)->rel);
}

/* ---- settings: the game folder, remembered beside the program ---- */

static void settings_path(char *out)
{
    char d[LPATH];
    exe_dir(d);
    pjoin(out, d, "jsrf_modtool.ini");
}

static void load_settings(void)
{
    char p[LPATH];
    size_t n;
    char *b;
    settings_path(p);
    b = read_file(p, &n);
    if (!b) return;
    if (!strncmp(b, "game=", 5)) {
        char *e = strpbrk(b + 5, "\r\n");
        if (e) *e = 0;
        snprintf(s_game, sizeof s_game, "%s", b + 5);
    }
    free(b);
}

static void save_settings(void)
{
    char p[LPATH], t[LPATH + 16];
    settings_path(p);
    snprintf(t, sizeof t, "game=%s\n", s_game);
    write_file(p, t, strlen(t));
}

static void set_game(const char *d)
{
    char parent[LPATH], *s;
    snprintf(s_game, sizeof s_game, "%s", d);
    /* the Media folder itself chosen: its parent */
    s = strrchr(s_game, SEP[0]);
    if (s && !SDL_strcasecmp(s + 1, "Media")) {
        snprintf(parent, sizeof parent, "%s", s_game);
        parent[s - s_game] = 0;
        snprintf(s_game, sizeof s_game, "%s", parent);
    }
    save_settings();
    start_scan();
}

/* ---- the selected file and texture ---- */

static int open_file(int i)
{
    char p[LPATH];
    size_t n;
    char *b;
    close_file();
    s_sel_file = i;
    if (i < 0) return -1;
    backup_path(p, s_files[i].rel);
    s_cur_modded = is_file(p);
    game_path(p, s_files[i].rel);
    b = read_file(p, &n);
    if (!b) { snprintf(s_msg, sizeof s_msg, "Can't read %s", p); return -1; }
    jf_parse(&s_cur, p, (uint8_t *)b, n);
    return 0;
}

static void show_texture(SDL_Renderer *ren, int k)
{
    const JTex *t;
    uint8_t *px;
    int x, y;
    if (s_preview) SDL_DestroyTexture(s_preview);
    s_preview = NULL;
    s_sel_tex = k;
    if (k < 0 || k >= s_cur.ntex) return;
    t = &s_cur.tex[k];
    px = (uint8_t *)malloc((size_t)t->w * t->h * 4);
    if (jt_decode(&s_cur, t, px)) { free(px); return; }
    for (y = 0; y < t->h; y++)                 /* over a checkerboard, for the transparent parts */
        for (x = 0; x < t->w; x++) {
            uint8_t *p = px + ((size_t)y * t->w + x) * 4;
            int bg = ((x / 8) ^ (y / 8)) & 1 ? 150 : 100, a = p[3], c;
            for (c = 0; c < 3; c++) p[c] = (uint8_t)((p[c] * a + bg * (255 - a)) / 255);
            p[3] = 255;
        }
    s_preview = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t->w, t->h);
    if (s_preview) SDL_UpdateTexture(s_preview, NULL, px, t->w * 4);
    s_pw = t->w;
    s_ph = t->h;
    free(px);
}

static void export_texture(int k)
{
    char out[LPATH], dir[LPATH], *s;
    const JTex *t = &s_cur.tex[k];
    uint8_t *px = (uint8_t *)malloc((size_t)t->w * t->h * 4);
    edit_path(out, s_files[s_sel_file].rel, k);
    snprintf(dir, sizeof dir, "%s", out);
    if ((s = strrchr(dir, SEP[0]))) { *s = 0; make_dirs(dir); }
    {
        int n = 0;
        unsigned char *png = jt_decode(&s_cur, t, px) ? NULL : stbi_write_png_to_mem(px, t->w * 4, t->w, t->h, 4, &n);
        int ok = png && write_file(out, png, (size_t)n);   /* write_file: any path, on Windows too */
        free(png);
        if (!ok) {
            snprintf(s_msg, sizeof s_msg, "Couldn't export to %s", out);
            free(px);
            return;
        }
    }
    snprintf(s_msg, sizeof s_msg, "Exported: %s. Edit it, keep its name, then Import.", out);
    free(px);
}

/* rgba w x h to tw x th, bilinear */
static uint8_t *resize(const uint8_t *src, int w, int h, int tw, int th)
{
    uint8_t *d = (uint8_t *)malloc((size_t)tw * th * 4);
    int x, y, c;
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            float fx = ((float)x + 0.5f) * w / tw - 0.5f, fy = ((float)y + 0.5f) * h / th - 0.5f;
            int x0 = (int)fx, y0 = (int)fy, x1, y1;
            float ax, ay;
            if (fx < 0) fx = 0, x0 = 0;
            if (fy < 0) fy = 0, y0 = 0;
            x1 = x0 + 1 < w ? x0 + 1 : x0;
            y1 = y0 + 1 < h ? y0 + 1 : y0;
            ax = fx - x0;
            ay = fy - y0;
            for (c = 0; c < 4; c++) {
                float a = src[((size_t)y0 * w + x0) * 4 + c] * (1 - ax) + src[((size_t)y0 * w + x1) * 4 + c] * ax;
                float b = src[((size_t)y1 * w + x0) * 4 + c] * (1 - ax) + src[((size_t)y1 * w + x1) * 4 + c] * ax;
                d[((size_t)y * tw + x) * 4 + c] = (uint8_t)(a * (1 - ay) + b * ay + 0.5f);
            }
        }
    return d;
}

static void import_texture(SDL_Renderer *ren, int k, const char *png)
{
    char in[LPATH], out[LPATH], dir[LPATH], *s, note[96] = "";
    const JTex *t = &s_cur.tex[k];
    int w, h, c;
    uint8_t *px;
    size_t n;
    char *b;
    if (png) snprintf(in, sizeof in, "%s", png);
    else edit_path(in, s_files[s_sel_file].rel, k);
    b = read_file(in, &n);
    if (!b) { snprintf(s_msg, sizeof s_msg, "No %s: Export it first, or drop a PNG on the window.", in); return; }
    px = stbi_load_from_memory((const stbi_uc *)b, (int)n, &w, &h, &c, 4);
    free(b);
    if (!px) { snprintf(s_msg, sizeof s_msg, "%s isn't a PNG or TGA image.", in); return; }
    if (w != t->w || h != t->h) {
        uint8_t *r = resize(px, w, h, t->w, t->h);
        stbi_image_free(px);
        px = r;
        snprintf(note, sizeof note, " (resized from %dx%d)", w, h);
    }
    if (jt_encode(&s_cur, t, px)) {
        snprintf(s_msg, sizeof s_msg, "Couldn't encode the texture.");
    } else {
        char bak[LPATH];
        game_path(out, s_files[s_sel_file].rel);
        backup_path(bak, s_files[s_sel_file].rel);
        snprintf(dir, sizeof dir, "%s", bak);
        if ((s = strrchr(dir, SEP[0]))) { *s = 0; make_dirs(dir); }
        if (!is_file(bak) && !copy_file(out, bak))          /* the original kept first, once */
            snprintf(s_msg, sizeof s_msg, "Couldn't keep the original in %s: nothing changed", bak);
        else if (!write_file(out, s_cur.buf, s_cur.size))
            snprintf(s_msg, sizeof s_msg, "Couldn't write %s", out);
        else {
            char cp[LPATH];
            cache_path(cp, s_files[s_sel_file].rel);
            s_cur_modded = 1;
            if (is_file(cp) && !write_file(cp, s_cur.buf, s_cur.size))
                snprintf(s_msg, sizeof s_msg, "Texture %d replaced%s, but the game's cached copy %s couldn't be: "
                         "delete it by hand", k, note, cp);
            else
                snprintf(s_msg, sizeof s_msg, "Texture %d replaced%s in %s (the original is in mod_backups)", k, note, out);
        }
    }
    free(px);
    show_texture(ren, k);
}

static void revert_file(SDL_Renderer *ren)
{
    char bak[LPATH], p[LPATH];
    int k = s_sel_tex;
    backup_path(bak, s_files[s_sel_file].rel);
    game_path(p, s_files[s_sel_file].rel);
    if (is_file(bak) && copy_file(bak, p)) {
        char cp[LPATH];
        cache_path(cp, s_files[s_sel_file].rel);
        if (is_file(cp)) copy_file(bak, cp);            /* the game's cached copy too */
        remove_file(bak);
        snprintf(s_msg, sizeof s_msg, "Back to the original: %s", p);
    } else snprintf(s_msg, sizeof s_msg, "Couldn't put the original back from %s", bak);
    open_file(s_sel_file);
    show_texture(ren, k < s_cur.ntex ? k : 0);
}

/* ---- the window ---- */

static void load_font(struct nk_context *ctx, float size)
{
    static const char *const fonts[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", NULL };
    struct nk_font_atlas *atlas;
    struct nk_font *font = NULL;
    int i;
    nk_sdl_font_stash_begin(&atlas);
    for (i = 0; fonts[i] && !font; i++)
        if (is_file(fonts[i])) font = nk_font_atlas_add_from_file(atlas, fonts[i], size, NULL);
    if (!font) font = nk_font_atlas_add_default(atlas, size, NULL);
    nk_sdl_font_stash_end();
    nk_style_set_font(ctx, &font->handle);
}

static void style(struct nk_context *ctx)
{
    struct nk_color t[NK_COLOR_COUNT];
    t[NK_COLOR_TEXT] = nk_rgba(225, 225, 230, 255);
    t[NK_COLOR_WINDOW] = nk_rgba(28, 28, 34, 255);
    t[NK_COLOR_HEADER] = nk_rgba(40, 40, 48, 255);
    t[NK_COLOR_BORDER] = nk_rgba(60, 60, 70, 255);
    t[NK_COLOR_BUTTON] = nk_rgba(52, 52, 62, 255);
    t[NK_COLOR_BUTTON_HOVER] = nk_rgba(70, 70, 84, 255);
    t[NK_COLOR_BUTTON_ACTIVE] = nk_rgba(240, 175, 30, 255);
    t[NK_COLOR_TOGGLE] = nk_rgba(60, 60, 72, 255);
    t[NK_COLOR_TOGGLE_HOVER] = nk_rgba(80, 80, 94, 255);
    t[NK_COLOR_TOGGLE_CURSOR] = nk_rgba(240, 175, 30, 255);
    t[NK_COLOR_SELECT] = nk_rgba(44, 44, 54, 255);
    t[NK_COLOR_SELECT_ACTIVE] = nk_rgba(200, 140, 20, 255);
    t[NK_COLOR_SLIDER] = nk_rgba(44, 44, 54, 255);
    t[NK_COLOR_SLIDER_CURSOR] = nk_rgba(240, 175, 30, 255);
    t[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgba(250, 190, 50, 255);
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgba(255, 205, 60, 255);
    t[NK_COLOR_PROPERTY] = nk_rgba(44, 44, 54, 255);
    t[NK_COLOR_EDIT] = nk_rgba(20, 20, 26, 255);
    t[NK_COLOR_EDIT_CURSOR] = nk_rgba(230, 230, 230, 255);
    t[NK_COLOR_COMBO] = nk_rgba(44, 44, 54, 255);
    t[NK_COLOR_CHART] = nk_rgba(44, 44, 54, 255);
    t[NK_COLOR_CHART_COLOR] = nk_rgba(240, 175, 30, 255);
    t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 0, 0, 255);
    t[NK_COLOR_SCROLLBAR] = nk_rgba(36, 36, 44, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgba(80, 80, 94, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgba(100, 100, 116, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgba(120, 120, 136, 255);
    t[NK_COLOR_TAB_HEADER] = nk_rgba(40, 40, 48, 255);
    t[NK_COLOR_KNOB] = t[NK_COLOR_SLIDER];
    t[NK_COLOR_KNOB_CURSOR] = t[NK_COLOR_SLIDER_CURSOR];
    t[NK_COLOR_KNOB_CURSOR_HOVER] = t[NK_COLOR_SLIDER_CURSOR_HOVER];
    t[NK_COLOR_KNOB_CURSOR_ACTIVE] = t[NK_COLOR_SLIDER_CURSOR_ACTIVE];
    nk_style_from_table(ctx, t);
    ctx->style.window.padding = nk_vec2(12, 10);
    ctx->style.window.spacing = nk_vec2(8, 6);
    ctx->style.button.rounding = 6;
}

static char *s_dropped;                  /* a PNG dropped on the window */

static void draw(struct nk_context *ctx, SDL_Renderer *ren, int ww, int wh)
{
    const float row = 30, top = 92;
    int busy = SDL_AtomicGet(&s_scan_busy), i;
    if (SDL_AtomicGet(&s_scan_done)) {
        SDL_AtomicSet(&s_scan_done, 0);
        SDL_LockMutex(s_mx);
        qsort(s_files, (size_t)s_nfiles, sizeof(Entry), cmp_entry);
        SDL_UnlockMutex(s_mx);
        snprintf(s_msg, sizeof s_msg, "%d files with textures (of %d looked at).", s_nfiles, SDL_AtomicGet(&s_scan_seen));
    }
    if (s_dropped) {
        if (s_sel_file >= 0 && s_sel_tex >= 0) import_texture(ren, s_sel_tex, s_dropped);
        else snprintf(s_msg, sizeof s_msg, "Pick a texture first, then drop the image on it.");
        SDL_free(s_dropped);
        s_dropped = NULL;
    }

    if (nk_begin(ctx, "top", nk_rect(0, 0, (float)ww, top), NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_template_begin(ctx, row);
        nk_layout_row_template_push_static(ctx, 110);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 110);
        nk_layout_row_template_push_static(ctx, 90);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, "Game folder", NK_TEXT_LEFT);
        nk_label(ctx, s_game[0] ? s_game : "(none: choose the folder with default.xbe and Media)", NK_TEXT_LEFT);
        if (nk_button_label(ctx, "Choose...")) {
            char d[LPATH];
            if (pick_folder(s_game, d)) set_game(d);
        }
        if (nk_button_label(ctx, busy ? "..." : "Rescan")) start_scan();
        nk_layout_row_dynamic(ctx, row - 4, 1);
        if (busy) nk_labelf(ctx, NK_TEXT_LEFT, "Looking through the game files... %d", SDL_AtomicGet(&s_scan_seen));
        else nk_label_wrap(ctx, s_msg);
    }
    nk_end(ctx);

    /* the files */
    if (nk_begin(ctx, "Files", nk_rect(0, top, 330, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, row, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s_filter, sizeof s_filter, nk_filter_default);
        SDL_LockMutex(s_mx);
        nk_layout_row_dynamic(ctx, row - 6, 1);
        for (i = 0; i < s_nfiles; i++) {
            const char *shown = s_files[i].rel + (strncmp(s_files[i].rel, "Media/", 6) ? 0 : 6);
            int sel = i == s_sel_file;
            if (s_filter[0] && !SDL_strcasestr(shown, s_filter)) continue;
            char lb[600];
            snprintf(lb, sizeof lb, "%s  (%d)", shown, s_files[i].ntex);
            if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel && !busy) {
                open_file(i);
                if (s_cur.ntex) show_texture(ren, 0);
            }
        }
        SDL_UnlockMutex(s_mx);
    }
    nk_end(ctx);

    /* its textures */
    if (nk_begin(ctx, "Textures", nk_rect(330, top, 250, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, row - 6, 1);
        for (i = 0; i < s_cur.ntex; i++) {
            const JTex *t = &s_cur.tex[i];
            int sel = i == s_sel_tex;
            char lb[64];
            snprintf(lb, sizeof lb, "%2d   %dx%d  %s", i, t->w, t->h, jt_fmt_name(t->fmt));
            if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel)
                show_texture(ren, i);
        }
    }
    nk_end(ctx);

    /* the texture */
    if (nk_begin(ctx, "Texture", nk_rect(580, top, (float)ww - 580, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        if (s_sel_file >= 0 && s_sel_tex >= 0 && s_sel_tex < s_cur.ntex) {
            const JTex *t = &s_cur.tex[s_sel_tex];
            float avail = (float)ww - 580 - 30, side = (float)wh - top - 6 * row - 40;
            float sz = avail < side ? avail : side;
            nk_layout_row_dynamic(ctx, row - 6, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "%s, texture %d: %dx%d %s, %d level%s%s%s",
                      s_files[s_sel_file].rel, s_sel_tex, t->w, t->h, jt_fmt_name(t->fmt), t->levels,
                      t->levels == 1 ? "" : "s", t->alpha ? ", transparency" : "", t->swizzled ? ", swizzled" : "");
            nk_label_colored(ctx, s_cur_modded ? "Modded (the original is kept in mod_backups)" : "Original",
                             NK_TEXT_LEFT, s_cur_modded ? nk_rgb(240, 175, 30) : nk_rgb(150, 150, 160));
            nk_layout_row_template_begin(ctx, row);
            nk_layout_row_template_push_static(ctx, 130);
            nk_layout_row_template_push_static(ctx, 130);
            nk_layout_row_template_push_static(ctx, 170);
            nk_layout_row_template_push_static(ctx, 150);
            nk_layout_row_template_end(ctx);
            if (nk_button_label(ctx, "Export PNG")) export_texture(s_sel_tex);
            if (nk_button_label(ctx, "Import PNG")) import_texture(ren, s_sel_tex, NULL);
            if (nk_button_label(ctx, "Open edits folder")) {
                char d[LPATH];
                pjoin(d, s_game, "texture_edits");
                make_dirs(d);
                open_folder(d);
            }
            if (s_cur_modded) {
                if (nk_button_label(ctx, "Undo all (file)")) revert_file(ren);
            } else nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, row - 8, 1);
            nk_label_colored(ctx, "Import reads the exported PNG back, or drop any PNG on the window.", NK_TEXT_LEFT,
                             nk_rgb(150, 150, 160));
            if (s_preview && sz > 32) {
                struct nk_image im = nk_image_ptr(s_preview);
                if (sz > 2.0f * (float)s_pw) sz = 2.0f * (float)s_pw;
                nk_layout_row_static(ctx, sz, (int)sz, 1);
                nk_image(ctx, im);
            }
        } else {
            nk_layout_row_dynamic(ctx, row, 1);
            nk_label(ctx, s_nfiles ? "Pick a file, then a texture." : "Choose the game folder.", NK_TEXT_LEFT);
        }
    }
    nk_end(ctx);
}

int main(int argc, char **argv)
{
    SDL_Window *win;
    SDL_Renderer *ren;
    struct nk_context *ctx;
    int running = 1;
    const char *shot = NULL, *test_rel = NULL, *test_png = NULL;
    int i, test_n = 0;
    SDL_SetMainReady();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(s_game, sizeof s_game, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];   /* a screenshot, then quit (tests) */
        else if (!strcmp(argv[i], "--test") && i + 3 < argc) {   /* tests: import PNG into REL's texture N, or undo (N -1) */
            test_rel = argv[++i];
            test_n = atoi(argv[++i]);
            test_png = argv[++i];
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    s_mx = SDL_CreateMutex();
    if (!s_game[0]) load_settings();
    win = SDL_CreateWindow("JSRF Mod Tool " MODTOOL_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    ctx = nk_sdl_init(win, ren);
    load_font(ctx, 18);
    style(ctx);
    if (s_game[0]) start_scan();
    while (running) {
        SDL_Event e;
        int ww, wh;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_DROPFILE) {
                if (s_dropped) SDL_free(s_dropped);
                s_dropped = e.drop.file;
            }
            nk_sdl_handle_event(&e);
        }
        nk_input_end(ctx);
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        draw(ctx, ren, ww, wh);
        if (test_rel && !SDL_AtomicGet(&s_scan_busy) && !SDL_AtomicGet(&s_scan_done) && SDL_AtomicGet(&s_scan_seen)) {
            for (i = 0; i < s_nfiles && strcmp(s_files[i].rel, test_rel); i++) ;
            if (i < s_nfiles && !open_file(i)) {
                if (test_n >= 0) { show_texture(ren, test_n); import_texture(ren, test_n, test_png); }
                else revert_file(ren);
            } else snprintf(s_msg, sizeof s_msg, "no %s", test_rel);
            printf("%s\n", s_msg);
            break;
        }
        if (shot && !SDL_AtomicGet(&s_scan_busy) && s_nfiles && s_sel_file < 0) {
            open_file(0);
            if (s_cur.ntex) show_texture(ren, 0);
        }
        SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        if (shot && s_preview) {
            static int frames;
            if (++frames == 5) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32, SDL_PIXELFORMAT_ABGR8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ABGR8888, s->pixels, s->pitch);
                stbi_write_png(shot, ww, wh, 4, s->pixels, s->pitch);
                SDL_FreeSurface(s);
                running = 0;
            }
        }
        SDL_RenderPresent(ren);
        if (!running) break;
        SDL_Delay(SDL_AtomicGet(&s_scan_busy) ? 30 : 10);
    }
    close_file();
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
