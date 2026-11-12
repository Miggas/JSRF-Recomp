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
#include "paint.h"
#include "view3d.h"
#include "jsnd.h"
#include "util.h"

#define MODTOOL_VERSION "m11"

/* ---- the files ---- */

typedef struct {
    char rel[512];      /* from the game folder, '/' between: Media/Player/Beat.dat */
    int ntex;
    int nsnd;           /* m7: its sounds */
    int w, h;           /* its first texture's size */
    /* m3b: where it's listed: its tab, its section (in the tab's order, a
     * name), the name it's shown by */
    int cat, sec;
    char secname[48], label[96];
} Entry;

/* m3b: tabs by what the files hold (Miggas), each in collapsible sections */
enum { CAT_ALL, CAT_CHAR, CAT_GRAF, CAT_STAGE, CAT_OBJ, CAT_EVENT, CAT_UI, CAT_MISSION, CAT_OTHER, CAT_SOUND, CAT_N };
static const char *const CAT_NAMES[CAT_N] = {
    "All", "Characters", "Graffiti", "Stages", "Objects", "Cutscenes", "Menus & HUD", "Missions", "Other", "Sounds" };

/* the characters by the names they have in the game (the files keep the
 * old Jet Set Radio ones, and TypeA / TypeB) */
static const char *char_name(const char *file)
{
    static const char *const m[][2] = {
        { "Beat", "Beat" }, { "Bis", "Rhyth" }, { "Clutch", "Clutch" }, { "Combo", "Combo" },
        { "Corn", "Corn" }, { "Cube", "Cube" }, { "djk", "Professor K" }, { "Galam", "Garam" },
        { "Gouji", "Gouji" }, { "Gum", "Gum" }, { "HellMets", "Golden Rhino" }, { "Immortal", "Immortal" },
        { "Jazz", "Jazz" }, { "LShocker", "Love Shocker" }, { "NTank", "Noise Tank" },
        { "PJam", "Poison Jam" }, { "Pots", "Pots" }, { "Rapid99", "Rapid 99" }, { "Soda", "Soda" },
        { "Sugar", "Boogie" }, { "TypeA", "Roboy" }, { "TypeB", "A. KU. MU" }, { "Yoyo", "Yoyo" },
        { "YoyoB", "NT 3000" }, { "ZeroBeat", "Zero Beat" } };
    char b[64], *d;
    size_t i;
    snprintf(b, sizeof b, "%s", file);
    if ((d = strrchr(b, '.'))) *d = 0;
    for (i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!SDL_strcasecmp(b, m[i][0])) return m[i][1];
    return NULL;
}

static void classify(Entry *e)
{
    static const struct { const char *dir; int cat, sec; const char *name; } m[] = {
        { "Player", CAT_CHAR, 0, "Characters" },
        { "Mark/PRESS", CAT_GRAF, 10, "Graffiti sheets" },
        { "Mark/DEFAULT", CAT_GRAF, 11, "Default graffiti" },
        { "Mark", CAT_GRAF, 12, "Graffiti editor" },
        { "Stage/Custom", CAT_STAGE, 1, "Custom" },
        { "Stage", CAT_STAGE, 0, "Areas" },
        { "StgObj", CAT_OBJ, 0, "Street objects" }, { "People", CAT_OBJ, 1, "Pedestrians" },
        { "Enemy", CAT_OBJ, 2, "Enemies" }, { "Effect", CAT_OBJ, 3, "Effects" }, { "Misc", CAT_OBJ, 4, "Misc" },
        { "Event/TalkEvent", CAT_EVENT, 1, "Talk scenes" }, { "Event/Event", CAT_EVENT, 0, "Cutscenes" },
        { "Event", CAT_EVENT, 2, "Events" },
        { "Disp/charicon", CAT_UI, 1, "Character icons" }, { "Disp/map", CAT_UI, 2, "Maps" },
        { "Disp", CAT_UI, 0, "HUD" }, { "Font", CAT_UI, 3, "Fonts" }, { "Title", CAT_UI, 4, "Title screen" },
        { "Garage", CAT_UI, 5, "Garage" }, { "Progress", CAT_UI, 6, "Progress" }, { "Ending", CAT_UI, 7, "Ending" },
        { "Mission", CAT_MISSION, 0, "Missions" }, { "Sounds/SE", CAT_SOUND, 0, "Sound effects" }, { "Sounds", CAT_SOUND, 1, "Sounds" } };
    static const char *const graf[5] = { "SS", "S", "M", "L", "XL" };
    const char *r = e->rel + (SDL_strncasecmp(e->rel, "Media/", 6) ? 0 : 6);
    const char *base = strrchr(r, '/') ? strrchr(r, '/') + 1 : r;
    size_t dl = (size_t)(base - r), i;
    e->cat = CAT_OTHER;
    e->sec = 99;
    snprintf(e->secname, sizeof e->secname, "%.*s", dl ? (int)dl - 1 : 0, r);
    snprintf(e->label, sizeof e->label, "%s", base);
    if (!e->secname[0]) snprintf(e->secname, sizeof e->secname, "Media");
    for (i = 0; i < sizeof m / sizeof m[0]; i++) {
        size_t n = strlen(m[i].dir);
        if (dl > n && !SDL_strncasecmp(r, m[i].dir, n) && (r[n] == '/')) {
            /* the deepest listed folder wins: Mark/PRESS before Mark (listed first) */
            e->cat = m[i].cat;
            e->sec = m[i].sec;
            snprintf(e->secname, sizeof e->secname, "%s", m[i].name);
            if (dl - 1 != n) {                  /* a folder below it: its own section */
                e->sec = m[i].sec * 10 + 5;
                snprintf(e->secname, sizeof e->secname, "%s: %.*s", m[i].name, (int)(dl - 2 - n), r + n + 1);
            }
            break;
        }
    }
    if (!e->ntex && e->nsnd) e->cat = CAT_SOUND;   /* m7: sounds only: the Sounds tab, by its folder */
    if (e->cat == CAT_CHAR) {
        const char *nm = char_name(base);
        if (nm) snprintf(e->label, sizeof e->label, "%s", nm);
    } else if (!SDL_strncasecmp(r, "Mark/TEX/", 9)) {
        /* graffiti by size: grf_0_ SS ... grf_4_ XL (Miggas), the rest after */
        e->cat = CAT_GRAF;
        if (!SDL_strncasecmp(base, "grf_", 4) && base[4] >= '0' && base[4] <= '4' && base[5] == '_') {
            e->sec = base[4] - '0';
            snprintf(e->secname, sizeof e->secname, "%s", graf[e->sec]);
        } else {
            e->sec = 9;
            snprintf(e->secname, sizeof e->secname, "Other graffiti");
        }
    }
}

/* m3c: the tabs shown (Miggas: no All, Missions or Other; their files
 * aren't listed) */
static const int TABS[] = { CAT_CHAR, CAT_GRAF, CAT_STAGE, CAT_OBJ, CAT_EVENT, CAT_UI, CAT_SOUND };
#define NTABS ((int)(sizeof TABS / sizeof TABS[0]))
static int s_tab = CAT_CHAR;

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
static uint8_t *s_tex_rgba;             /* m5: the picked texture's pixels, for the 3D view */
static unsigned s_tex_ver;
static char s_msg[LPATH + 600];
static int s_restore_ask;               /* m10: Restore all pressed once */
static int s_mat_open, s_mat_sel, s_mat_hl = 1, s_mat_dirty;   /* m10: the material editor */
static char s_restore_msg[200];         /* ... its result, shown when the rescan ends */
/* m7: the file's sounds, the picked one decoded, playing */
static JSounds s_snd;
static int s_sel_snd = -1;
static int16_t *s_snd_pcm;
static int s_snd_frames;
static SDL_AudioDeviceID s_audio;
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

/* m1c: the cached copies are padded to whole 512-byte sectors (Rapid99.bin
 * 358608 bytes in the game folder, 358912 in the cache), and the game
 * reads them by their padded size; written unpadded the read failed and the
 * character select waited forever (Miggas). The copy keeps its size, at
 * least the data's rounded up to 512, the rest zero. */
static int write_cache(const char *cp, const uint8_t *data, size_t n)   /* the copy: n rounded up to 512 */
{
    size_t sz = (n + 511) & ~(size_t)511;   /* m2: the data's own, so a texture made bigger fits */
    uint8_t *o;
    int ok;
    o = (uint8_t *)calloc(1, sz);
    if (!o) return 0;
    memcpy(o, data, n);
    ok = write_file(cp, o, sz);
    free(o);
    return ok;
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
        JSounds ss;
        int nsnd;
        if (!b) return;
        js_scan((const uint8_t *)b, len, &ss);     /* m7: its sounds */
        nsnd = ss.n;
        js_free(&ss);
        jf_parse(&f, p, (uint8_t *)b, len);
        if (f.ntex > 0 || nsnd > 0) {
            SDL_LockMutex(s_mx);
            if (s_nfiles == s_capfiles) {
                s_capfiles = s_capfiles ? s_capfiles * 2 : 256;
                s_files = (Entry *)realloc(s_files, sizeof(Entry) * (size_t)s_capfiles);
            }
            snprintf(s_files[s_nfiles].rel, sizeof s_files[s_nfiles].rel, "%s", r);
            s_files[s_nfiles].ntex = f.ntex;
            s_files[s_nfiles].nsnd = nsnd;
            s_files[s_nfiles].w = f.ntex ? f.tex[0].w : 0;
            s_files[s_nfiles].h = f.ntex ? f.tex[0].h : 0;
            classify(&s_files[s_nfiles]);
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

static void stop_sound(void)
{
    if (s_audio) SDL_CloseAudioDevice(s_audio);
    s_audio = 0;
}

static void close_file(void)
{
    stop_sound();
    js_free(&s_snd);
    free(s_snd_pcm);
    s_snd_pcm = NULL;
    s_sel_snd = -1;
    jf_free(&s_cur);
    s_mat_dirty = 0;                            /* m10: the edits went with it */
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

/* m11: placed next to the JSRF launcher (its launcher.ini beside us), the
 * launcher's game folder: GAME_DIR= as set in its Play tab, else the
 * jsrf/ (or older game/) folder it keeps the game in. 1 when found */
static int s_from_launcher;

static int launcher_game(char *out)
{
    char d[LPATH], li[LPATH], g[LPATH], m[LPATH];
    size_t n;
    char *b, *line;
    exe_dir(d);
    pjoin(li, d, "launcher.ini");
    if (!is_file(li)) return 0;
    g[0] = 0;
    if ((b = read_file(li, &n))) {
        for (line = strtok(b, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (!strncmp(line, "GAME_DIR=", 9) && line[9]) {
                size_t l;
                snprintf(g, sizeof g, "%s", line + 9);
                l = strlen(g);
                while (l && (g[l - 1] == ' ' || g[l - 1] == '\t')) g[--l] = 0;
            }
        }
        free(b);
    }
    if (g[0]) { pjoin(m, g, "Media"); if (is_dir(m)) { snprintf(out, LPATH, "%s", g); return 1; } }
    pjoin(g, d, "jsrf");
    pjoin(m, g, "Media");
    if (is_dir(m)) { snprintf(out, LPATH, "%s", g); return 1; }
    pjoin(g, d, "game");
    pjoin(m, g, "Media");
    if (is_dir(m)) { snprintf(out, LPATH, "%s", g); return 1; }
    return 0;
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
    s_from_launcher = 0;                        /* chosen here: this one, for now */
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
    js_scan(s_cur.buf, s_cur.size, &s_snd);     /* m7: its sounds */
    v3d_load(s_cur.buf, s_cur.size);            /* m5: its models */
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
    if (k >= 0) { stop_sound(); s_sel_snd = -1; }   /* m7 */
    if (k < 0 || k >= s_cur.ntex) return;
    t = &s_cur.tex[k];
    px = (uint8_t *)malloc((size_t)t->w * t->h * 4);
    if (jt_decode(&s_cur, t, px)) { free(px); return; }
    free(s_tex_rgba);                           /* m5: kept as it is, for the 3D view */
    s_tex_rgba = (uint8_t *)malloc((size_t)t->w * t->h * 4);
    if (s_tex_rgba) memcpy(s_tex_rgba, px, (size_t)t->w * t->h * 4);
    s_tex_ver++;
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

/* rgba (the texture's size) into texture k, the file written in place
 * after its original is kept, the game's cached copy too; -1 failed */
/* the file as it now is in s_cur written over the game's (the original
 * kept first, once) and its cached copy; done: what was done, for the message */
static int store_file(const char *done)
{
    char out[LPATH], dir[LPATH], bak[LPATH], *s;
    game_path(out, s_files[s_sel_file].rel);
    backup_path(bak, s_files[s_sel_file].rel);
    snprintf(dir, sizeof dir, "%s", bak);
    if ((s = strrchr(dir, SEP[0]))) { *s = 0; make_dirs(dir); }
    if (!is_file(bak) && !copy_file(out, bak)) {        /* the original kept first, once */
        snprintf(s_msg, sizeof s_msg, "Couldn't keep the original in %s: nothing changed", bak);
        return -1;
    }
    if (!write_file(out, s_cur.buf, s_cur.size)) {
        snprintf(s_msg, sizeof s_msg, "Couldn't write %s", out);
        return -1;
    }
    s_mat_dirty = 0;                            /* m10: material edits are in the file now */
    {
        char cp[LPATH];
        cache_path(cp, s_files[s_sel_file].rel);
        s_cur_modded = 1;
        if (is_file(cp) && !write_cache(cp, s_cur.buf, s_cur.size))
            snprintf(s_msg, sizeof s_msg, "%s, but the game's cached copy %s couldn't be: delete it by hand", done, cp);
        else
            snprintf(s_msg, sizeof s_msg, "%s in %s (the original is in mod_backups)", done, out);
    }
    return 0;
}

static int store_texture(int k, const uint8_t *px, const char *note)
{
    char done[300];
    if (jt_encode(&s_cur, &s_cur.tex[k], px)) {
        snprintf(s_msg, sizeof s_msg, "Couldn't encode the texture.");
        return -1;
    }
    snprintf(done, sizeof done, "Texture %d replaced%s", k, note);
    return store_file(done);
}

/* ---- m8: a model for Blender: .obj, .mtl and its texture as a PNG, in
 * model_exports/ in the game folder ---- */

static void model_base(char *base, size_t n, const JMesh *m)
{
    char *p;
    snprintf(base, n, "%s", s_files[s_sel_file].rel + (strncmp(s_files[s_sel_file].rel, "Media/", 6) ? 0 : 6));
    for (p = base; *p; p++) if (*p == '/' || *p == '.') *p = '_';
    snprintf(base + strlen(base), n - strlen(base), "_model%d", m->index);
}

static void export_model(const JMesh *m)
{
    char base[600], dir[LPATH], obj[LPATH], mtl[LPATH], png[LPATH], nobj[700], nmtl[700], npng[700];
    int k, tk = -1;
    model_base(base, sizeof base, m);
    pjoin(dir, s_game, "model_exports");
    make_dirs(dir);
    snprintf(nobj, sizeof nobj, "%s.obj", base);
    snprintf(nmtl, sizeof nmtl, "%s.mtl", base);
    snprintf(npng, sizeof npng, "%s.png", base);
    pjoin(obj, dir, nobj);
    pjoin(mtl, dir, nmtl);
    pjoin(png, dir, npng);
    for (k = 0; k < s_cur.ntex; k++) if (m->texid && s_cur.tex[k].id == m->texid) { tk = k; break; }
    if (tk >= 0) {                              /* its texture (the painting's, while painting it) */
        const JTex *t = &s_cur.tex[tk];
        int w = t->w, h = t->h;
        uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
        const uint8_t *src = px;
        if (paint_on() && tk == s_sel_tex) { src = paint_image(); paint_size(&w, &h); }
        else if (!px || jt_decode(&s_cur, t, px)) tk = -1;
        if (tk >= 0 && !stbi_write_png(png, w, h, 4, src, w * 4)) tk = -1;
        free(px);
    }
    if (jm_write_obj(m, obj, mtl, nmtl, tk >= 0 ? npng : NULL, base))
        snprintf(s_msg, sizeof s_msg, "Couldn't write %s", obj);
    else
        snprintf(s_msg, sizeof s_msg, "Model %d saved as %s (%d triangles%s)", m->index, obj, m->nt,
                 tk >= 0 ? ", with its texture" : ", its texture isn't in this file");
}

/* ---- m9: a model replaced from an .obj (the exported one, edited, or one
 * dropped on the window) ---- */

static void import_model(SDL_Renderer *ren, const JMesh *m, const char *path)
{
    char in[LPATH], err[400], done[500];
    uint8_t *nb;
    size_t ns;
    int idx = m->index, tk = s_sel_tex;
    if (path) snprintf(in, sizeof in, "%s", path);
    else {
        char base[600], nobj[700], dir[LPATH];
        model_base(base, sizeof base, m);
        snprintf(nobj, sizeof nobj, "%s.obj", base);
        pjoin(dir, s_game, "model_exports");
        pjoin(in, dir, nobj);
    }
    if (!is_file(in)) { snprintf(s_msg, sizeof s_msg, "No %s: Export OBJ first, or drop an .obj on the window.", in); return; }
    if (paint_dirty()) { snprintf(s_msg, sizeof s_msg, "Save or close the painting first."); return; }
    if (paint_on()) paint_stop();               /* nothing unsaved: closed for the new file */
    if (jm_import_obj(s_cur.buf, s_cur.size, m, in, &nb, &ns, err, sizeof err)) { snprintf(s_msg, sizeof s_msg, "%s", err); return; }
    snprintf(done, sizeof done, "Model %d replaced (%s)", idx, err);
    {   /* the file is the new one now */
        char fp[1024];
        snprintf(fp, sizeof fp, "%s", s_cur.path);
        jf_free(&s_cur);
        jf_parse(&s_cur, fp, nb, ns);
        js_free(&s_snd);
        js_scan(s_cur.buf, s_cur.size, &s_snd);
        v3d_load(s_cur.buf, s_cur.size);
    }
    store_file(done);
    show_texture(ren, tk >= 0 && tk < s_cur.ntex ? tk : 0);
}

/* ---- m10: the material editor, under the 3D view: the shown model's
 * materials (colour, shading kind, the other numbers), edited in place,
 * previewed on the model, saved to the game file ---- */

#define MAT_H 372.0f

static const struct { const char *name; uint32_t v; } SHADING[] = {   /* the kinds JSRF ModTool names */
    { "Standard", 1328128 }, { "Skin", 1360896 }, { "Lambert", 16711680 }, { "Metal", 1122978 },
    { "Metal, reflective", 6726306 }, { "Metal, reflective 2", 6693602 }, { "Semi-transparent", 4612770 },
    { "Self-illuminated", 524287 }, { "UV scroll X (ticking)", 4661255 }, { "UV scroll X (sliding)", 459263 } };
#define NSHADING ((int)(sizeof SHADING / sizeof SHADING[0]))

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void wr32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

static void material_panel(struct nk_context *ctx, SDL_Renderer *ren, float row)
{
    const JMesh *m = v3d_current();
    size_t at = 0;
    int n = 0, i;
    char t[96];
    if (!m || jm_materials(s_cur.buf, s_cur.size, m, &at, &n)) { v3d_set_materials(NULL, 0, -1); n = 0; }
    nk_layout_row_dynamic(ctx, row, 1);
    snprintf(t, sizeof t, "%s Materials (%d)%s", s_mat_open ? "-" : "+", n, s_mat_dirty ? ", not saved" : "");
    if (nk_button_label(ctx, t)) s_mat_open = !s_mat_open;
    if (!n) { if (s_mat_open) { nk_layout_row_dynamic(ctx, row, 1); nk_label(ctx, "No materials found for this model.", NK_TEXT_LEFT); } return; }
    if (s_mat_sel >= n) s_mat_sel = 0;
    {   /* (tests) MT_MATS=k: the editor open on material k; MT_MATCOL=RRGGBB: its colour, once */
        static int once;
        if (!once && getenv("MT_MATS")) {
            once = 1;
            s_mat_open = 1;
            s_mat_sel = atoi(getenv("MT_MATS")) < n ? atoi(getenv("MT_MATS")) : 0;
            if (getenv("MT_MATCOL")) {
                unsigned v = (unsigned)strtoul(getenv("MT_MATCOL"), NULL, 16);
                uint8_t *e = s_cur.buf + at + (size_t)s_mat_sel * 20;
                e[2] = (uint8_t)(v >> 16); e[1] = (uint8_t)(v >> 8); e[0] = (uint8_t)v;
                s_mat_dirty = 1;
            }
        }
    }
    v3d_set_materials(s_cur.buf + at, n, s_mat_open && s_mat_hl ? s_mat_sel : -1);
    if (!s_mat_open) return;
    {
        uint8_t *e = s_cur.buf + at + (size_t)s_mat_sel * 20, before[20];
        struct nk_colorf c;
        uint32_t kind = rd32(e + 4);
        int k, known = -1, v3 = (int)rd32(e + 8), v4 = (int)rd32(e + 12);
        float v5;
        memcpy(before, e, 20);
        memcpy(&v5, e + 16, 4);
        for (k = 0; k < NSHADING; k++) if (SHADING[k].v == kind) known = k;
        /* which one, and its triangles shown */
        nk_layout_row_template_begin(ctx, row);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 100);
        nk_layout_row_template_end(ctx);
        snprintf(t, sizeof t, "Material %d", s_mat_sel);
        if (nk_combo_begin_label(ctx, t, nk_vec2(nk_widget_width(ctx), 300))) {
            nk_layout_row_dynamic(ctx, row - 6, 1);
            for (i = 0; i < n; i++) {
                char it[48];
                snprintf(it, sizeof it, "Material %d", i);
                if (nk_combo_item_label(ctx, it, NK_TEXT_LEFT)) s_mat_sel = i;
            }
            nk_combo_end(ctx);
        }
        {
            nk_bool hl = (nk_bool)s_mat_hl;
            nk_checkbox_label(ctx, "Highlight", &hl);
            s_mat_hl = hl;
        }
        /* its colour (stored B, G, R, A): multiplies the texture */
        c.r = e[2] / 255.0f; c.g = e[1] / 255.0f; c.b = e[0] / 255.0f; c.a = e[3] / 255.0f;
        nk_layout_row_dynamic(ctx, 96, 1);
        c = nk_color_picker(ctx, c, NK_RGBA);
        nk_layout_row_dynamic(ctx, row - 4, 2);
        {
            int r = (int)(c.r * 255 + 0.5f), g = (int)(c.g * 255 + 0.5f), b = (int)(c.b * 255 + 0.5f), a = (int)(c.a * 255 + 0.5f);
            r = nk_propertyi(ctx, "#R:", 0, r, 255, 1, 1);
            g = nk_propertyi(ctx, "#G:", 0, g, 255, 1, 1);
            b = nk_propertyi(ctx, "#B:", 0, b, 255, 1, 1);
            a = nk_propertyi(ctx, "#A:", 0, a, 255, 1, 1);
            e[2] = (uint8_t)r; e[1] = (uint8_t)g; e[0] = (uint8_t)b; e[3] = (uint8_t)a;
        }
        /* its shading kind */
        nk_layout_row_template_begin(ctx, row);
        nk_layout_row_template_push_static(ctx, 70);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, "Shading", NK_TEXT_LEFT);
        if (known >= 0) snprintf(t, sizeof t, "%s", SHADING[known].name);
        else snprintf(t, sizeof t, "Other (%u)", (unsigned)kind);
        if (nk_combo_begin_label(ctx, t, nk_vec2(nk_widget_width(ctx), 330))) {
            nk_layout_row_dynamic(ctx, row - 6, 1);
            for (k = 0; k < NSHADING; k++)
                if (nk_combo_item_label(ctx, SHADING[k].name, NK_TEXT_LEFT)) wr32(e + 4, SHADING[k].v);
            nk_combo_end(ctx);
        }
        /* the rest, as they are: what they do isn't known */
        nk_layout_row_dynamic(ctx, row - 4, 2);
        v3 = nk_propertyi(ctx, "#Value 3:", -2147483647, v3, 2147483647, 1, 1);
        v4 = nk_propertyi(ctx, "#Value 4:", -2147483647, v4, 2147483647, 1, 1);
        v5 = nk_propertyf(ctx, "#Value 5:", -100000.0f, v5, 100000.0f, 1.0f, 0.5f);
        nk_label_colored(ctx, "3-5: use not known", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        wr32(e + 8, (uint32_t)v3);
        wr32(e + 12, (uint32_t)v4);
        memcpy(e + 16, &v5, 4);
        if (memcmp(before, e, 20)) s_mat_dirty = 1;
        nk_layout_row_dynamic(ctx, row, s_mat_dirty ? 2 : 1);
        if (s_mat_dirty) {
            if (nk_button_label(ctx, "Save materials")) {
                char done[96];
                snprintf(done, sizeof done, "Materials of model %d saved", m->index);
                store_file(done);
            }
            if (nk_button_label(ctx, "Revert")) {
                int tk = s_sel_tex;
                s_mat_dirty = 0;
                open_file(s_sel_file);
                show_texture(ren, tk >= 0 && tk < s_cur.ntex ? tk : 0);
                snprintf(s_msg, sizeof s_msg, "Material changes thrown away.");
            }
        } else {
            nk_label_colored(ctx, "Colour multiplies the texture.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        }
    }
}

/* ---- m7: sounds ---- */

static void snd_path(char *out, const char *rel, int n)   /* where a sound is exported */
{
    char nm[600], *p;
    snprintf(nm, sizeof nm, "sound_edits" SEP "%s", rel + (strncmp(rel, "Media/", 6) ? 0 : 6));
    for (p = nm + 12; *p; p++) if (*p == '/' || *p == '.') *p = '_';
    snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "_%d.wav", n);
    pjoin(out, s_game, nm);
}

static void show_sound(int k)
{
    stop_sound();
    free(s_snd_pcm);
    s_snd_pcm = NULL;
    s_snd_frames = 0;
    s_sel_snd = k;
    if (k < 0 || k >= s_snd.n) return;
    if (s_preview) SDL_DestroyTexture(s_preview);
    s_preview = NULL;
    s_sel_tex = -1;
    if (js_decode(s_cur.buf, &s_snd.s[k], &s_snd_pcm, &s_snd_frames)) {
        s_snd_pcm = NULL;
        s_snd_frames = 0;
    }
}

static void play_sound(void)
{
    SDL_AudioSpec want, have;
    const JSnd *s;
    stop_sound();
    if (s_sel_snd < 0 || !s_snd_pcm) return;
    s = &s_snd.s[s_sel_snd];
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        snprintf(s_msg, sizeof s_msg, "No sound output: %s", SDL_GetError());
        return;
    }
    SDL_zero(want);
    want.freq = s->rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)s->ch;
    want.samples = 1024;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* SDL converts to what the device takes */
    if (!s_audio) { snprintf(s_msg, sizeof s_msg, "No sound output: %s", SDL_GetError()); return; }
    SDL_QueueAudio(s_audio, s_snd_pcm, (Uint32)s_snd_frames * (Uint32)s->ch * 2);
    SDL_PauseAudioDevice(s_audio, 0);
}

static int sound_playing(void) { return s_audio && SDL_GetQueuedAudioSize(s_audio) > 0; }

static int export_sound(int k, int quiet)
{
    char out[LPATH], dir[LPATH], *p;
    int16_t *pcm;
    int fr;
    const JSnd *s = &s_snd.s[k];
    snd_path(out, s_files[s_sel_file].rel, k);
    snprintf(dir, sizeof dir, "%s", out);
    if ((p = strrchr(dir, SEP[0]))) { *p = 0; make_dirs(dir); }
    if (js_decode(s_cur.buf, s, &pcm, &fr)) {
        if (!quiet) snprintf(s_msg, sizeof s_msg, "Sound %d is %s: it can't be read (only PCM and Xbox ADPCM).", k, js_fmt_name(s->fmt));
        return -1;
    }
    if (js_write_wav(out, pcm, fr, s->ch, s->rate)) {
        free(pcm);
        snprintf(s_msg, sizeof s_msg, "Couldn't write %s", out);
        return -1;
    }
    free(pcm);
    if (!quiet) snprintf(s_msg, sizeof s_msg, "Sound %d saved as %s", k, out);
    return 0;
}

static void export_all_sounds(void)
{
    int k, ok = 0;
    char out[LPATH];
    for (k = 0; k < s_snd.n; k++) ok += !export_sound(k, 1);
    snd_path(out, s_files[s_sel_file].rel, 0);
    snprintf(s_msg, sizeof s_msg, "%d of %d sounds saved as WAV files, like %s", ok, s_snd.n, out);
}

static void import_sound(int k, const char *wav)
{
    char in[LPATH], err[300], done[300];
    int16_t *pcm;
    int fr, ch, rate;
    uint8_t *nb;
    size_t ns;
    const JSnd *s = &s_snd.s[k];
    if (wav) snprintf(in, sizeof in, "%s", wav);
    else snd_path(in, s_files[s_sel_file].rel, k);
    if (!is_file(in)) { snprintf(s_msg, sizeof s_msg, "No %s: Export it first, or drop a WAV file on the window.", in); return; }
    if (js_read_wav(in, &pcm, &fr, &ch, &rate, err, sizeof err)) { snprintf(s_msg, sizeof s_msg, "%s", err); return; }
    if (js_replace(s_cur.buf, s_cur.size, s, pcm, fr, ch, rate, &nb, &ns)) {
        free(pcm);
        snprintf(s_msg, sizeof s_msg, "Sound %d is %s %d-bit: it can't be replaced.", k, js_fmt_name(s->fmt), s->bits);
        return;
    }
    snprintf(done, sizeof done, "Sound %d replaced (%.2f s, made %d Hz %s %s)", k, (double)fr / rate, s->rate,
             s->ch == 1 ? "mono" : "stereo", js_fmt_name(s->fmt));
    free(pcm);
    {   /* the file is the new one now */
        char path[1024];
        snprintf(path, sizeof path, "%s", s_cur.path);
        stop_sound();
        jf_free(&s_cur);
        jf_parse(&s_cur, path, nb, ns);
        js_free(&s_snd);
        js_scan(s_cur.buf, s_cur.size, &s_snd);
        v3d_load(s_cur.buf, s_cur.size);
    }
    store_file(done);
    show_sound(k < s_snd.n ? k : 0);
}

static void import_texture(SDL_Renderer *ren, int k, const char *png)
{
    char in[LPATH], note[200] = "";
    const JTex *t = &s_cur.tex[k];
    int w, h, c, grown = 0;
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
    /* m2: a bigger texture (Miggas): square, 512, 1024, 2048 or 4096, in a
     * .dat (graffiti keep their size: the game knows them by it) */
    if ((w != t->w || h != t->h) && w == h && t->box && w > t->w
        && (w == 512 || w == 1024 || w == 2048 || w == 4096)) {
        int ow = t->w;
        if (jt_grow(&s_cur, k, w)) {
            stbi_image_free(px);
            snprintf(s_msg, sizeof s_msg, "Couldn't make room for a %dx%d texture: nothing changed.", w, h);
            open_file(s_sel_file);
            show_texture(ren, k);
            return;
        }
        t = &s_cur.tex[k];
        grown = 1;
        snprintf(note, sizeof note, " (now %dx%d, was %dx%d)", w, h, ow, ow);
    } else if (w != t->w || h != t->h) {
        uint8_t *r = resize(px, w, h, t->w, t->h);
        stbi_image_free(px);
        px = r;
        if (!t->box)
            snprintf(note, sizeof note, " (resized from %dx%d: graffiti keep their size)", w, h);
        else if (w != h || w > 4096 || (w != 512 && w != 1024 && w != 2048 && w != 4096) || w < t->w)
            snprintf(note, sizeof note, " (resized from %dx%d: a bigger texture must be square, "
                     "512, 1024, 2048 or 4096)", w, h);
        else
            snprintf(note, sizeof note, " (resized from %dx%d)", w, h);
    }
    if (store_texture(k, px, note) && grown) open_file(s_sel_file);
    free(px);
    show_texture(ren, k);
}

/* m4: the painter, on the picked texture */
static void start_paint(SDL_Renderer *ren)
{
    const JTex *t = &s_cur.tex[s_sel_tex];
    uint8_t *px = (uint8_t *)malloc((size_t)t->w * t->h * 4);
    if (!px || jt_decode(&s_cur, t, px) || paint_start(ren, px, t->w, t->h))
        snprintf(s_msg, sizeof s_msg, "Couldn't open the texture for painting.");
    else
        paint_allow_resize(t->box && t->w == t->h ? 4096 : 0);   /* m4c: a .dat's (square) textures can grow */
    free(px);
}

/* m4: the painting into the texture (m4c: made its size first when resized) */
static void save_paint(SDL_Renderer *ren)
{
    int pw, ph, grown = 0;
    char note[64] = " (painted)";
    paint_size(&pw, &ph);
    if (pw != s_cur.tex[s_sel_tex].w) {     /* m4c: resized: the texture made that size first */
        int ow = s_cur.tex[s_sel_tex].w;
        if (jt_grow(&s_cur, s_sel_tex, pw)) {
            snprintf(s_msg, sizeof s_msg, "Couldn't make room for a %dx%d texture: nothing saved.", pw, ph);
            open_file(s_sel_file);
            pw = -1;
        } else {
            grown = 1;
            snprintf(note, sizeof note, " (painted, now %dx%d, was %dx%d)", pw, ph, ow, ow);
        }
    }
    if (pw > 0 && !store_texture(s_sel_tex, paint_image(), note)) {
        paint_saved();
        show_texture(ren, s_sel_tex);
    } else if (pw > 0 && grown) open_file(s_sel_file);
}

/* m4: may the picked file or texture change? not with unsaved painting */
static int may_leave_paint(void)
{
    if (s_mat_dirty) {                          /* m10 */
        snprintf(s_msg, sizeof s_msg, "Unsaved material changes: Save materials, or Revert them.");
        return 0;
    }
    if (paint_dirty()) {
        snprintf(s_msg, sizeof s_msg, "Unsaved painting: Save it to the game, or Close it (twice) to throw it away.");
        return 0;
    }
    paint_stop();
    return 1;
}

/* ---- m10: every modded file back to its original (the files in
 * mod_backups/), their cached copies too ---- */

typedef struct { char dir[LPATH]; char rel[512]; int *n, *bad; int dry; } RWalk;

static void restore_walk(const char *dir, const char *rel, int *n, int *bad, int dry);

static void restore_cb(const char *name, bool isdir, void *u)
{
    const RWalk *w = (const RWalk *)u;
    char p[LPATH], r[512];
    if (name[0] == '.') return;
    pjoin(p, w->dir, name);
    snprintf(r, sizeof r, "%s%s%s", w->rel, w->rel[0] ? "/" : "", name);
    if (isdir) { restore_walk(p, r, w->n, w->bad, w->dry); return; }
    (*w->n)++;
    if (w->dry) return;
    {
        char g[LPATH], cp[LPATH];
        game_path(g, r);
        if (!copy_file(p, g)) { (*w->bad)++; return; }
        cache_path(cp, r);
        if (is_file(cp)) {                      /* the game's cached copy, padded as it reads it */
            size_t n = 0;
            char *b = read_file(p, &n);
            if (b) write_cache(cp, (const uint8_t *)b, n);
            free(b);
        }
        remove_file(p);
    }
}

static void restore_walk(const char *dir, const char *rel, int *n, int *bad, int dry)
{
    RWalk w;
    snprintf(w.dir, sizeof w.dir, "%s", dir);
    snprintf(w.rel, sizeof w.rel, "%s", rel);
    w.n = n;
    w.bad = bad;
    w.dry = dry;
    list_dir(dir, restore_cb, &w);
}

/* the modded files (counted), or (go) put back */
static int restore_all(int go, int *bad)
{
    char d[LPATH];
    int n = 0, b = 0;
    pjoin(d, s_game, "mod_backups");
    if (!is_dir(d)) return 0;
    restore_walk(d, "", &n, &b, !go);
    if (bad) *bad = b;
    return n;
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
        if (is_file(cp)) {                              /* the game's cached copy too, padded */
            size_t n = 0;
            char *b = read_file(bak, &n);
            if (b) write_cache(cp, (const uint8_t *)b, n);
            free(b);
        }
        remove_file(bak);
        snprintf(s_msg, sizeof s_msg, "Back to the original: %s", p);
    } else snprintf(s_msg, sizeof s_msg, "Couldn't put the original back from %s", bak);
    {
        int sk = s_sel_snd;
        open_file(s_sel_file);
        if (sk >= 0) show_sound(sk < s_snd.n ? sk : 0);   /* m7 */
        else show_texture(ren, k < s_cur.ntex ? k : 0);
    }
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

/* the list's order: tab, section, the name shown */
static int cmp_view(const void *a, const void *b)
{
    const Entry *x = &s_files[*(const int *)a], *y = &s_files[*(const int *)b];
    int c;
    if (x->cat != y->cat) return x->cat - y->cat;
    if (x->sec != y->sec) return x->sec - y->sec;
    if ((c = SDL_strcasecmp(x->secname, y->secname))) return c;
    return SDL_strcasecmp(x->label, y->label);
}

static char *s_dropped;                  /* a PNG dropped on the window */

/* m5: narrower lists, the texture, the 3D view on the right (Miggas) */
#define LW_FILES 240.0f
#define LW_TEX 180.0f

static void draw(struct nk_context *ctx, SDL_Renderer *ren, int ww, int wh)
{
    const float row = 30, top = 128;
    const float tex_w = ((float)ww - LW_FILES - LW_TEX) * 0.55f;
    int busy = SDL_AtomicGet(&s_scan_busy), i;
    if (SDL_AtomicGet(&s_scan_done)) {
        SDL_AtomicSet(&s_scan_done, 0);
        SDL_LockMutex(s_mx);
        qsort(s_files, (size_t)s_nfiles, sizeof(Entry), cmp_entry);
        SDL_UnlockMutex(s_mx);
        snprintf(s_msg, sizeof s_msg, "%s%s%d files with textures or sounds (of %d looked at).", s_restore_msg,
                 s_restore_msg[0] ? " " : "", s_nfiles, SDL_AtomicGet(&s_scan_seen));
        s_restore_msg[0] = 0;
    }
    if (s_dropped) {
        size_t dl = strlen(s_dropped);
        if (dl > 4 && !SDL_strcasecmp(s_dropped + dl - 4, ".obj")) {   /* m9: a model, for the one shown */
            if (s_sel_file >= 0 && v3d_current()) import_model(ren, v3d_current(), s_dropped);
            else snprintf(s_msg, sizeof s_msg, "Pick the model in the 3D view first, then drop the .obj on the window.");
        } else if (dl > 4 && !SDL_strcasecmp(s_dropped + dl - 4, ".wav")) {   /* m7: a sound */
            if (s_sel_file >= 0 && s_sel_snd >= 0) import_sound(s_sel_snd, s_dropped);
            else snprintf(s_msg, sizeof s_msg, "Pick a sound first, then drop the WAV file on it.");
        } else if (paint_on()) snprintf(s_msg, sizeof s_msg, "Close the painter first, then drop the image.");
        else if (s_sel_file >= 0 && s_sel_tex >= 0) import_texture(ren, s_sel_tex, s_dropped);
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
        nk_layout_row_template_push_static(ctx, 120);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, "Game folder", NK_TEXT_LEFT);
        if (s_game[0] && s_from_launcher) nk_labelf(ctx, NK_TEXT_LEFT, "%s  (the launcher's)", s_game);
        else nk_label(ctx, s_game[0] ? s_game : "(none: choose the folder with default.xbe and Media)", NK_TEXT_LEFT);
        if (nk_button_label(ctx, "Choose...")) {
            char d[LPATH];
            if (pick_folder(s_game, d)) set_game(d);
        }
        if (nk_button_label(ctx, busy ? "..." : "Rescan")) { start_scan(); s_restore_ask = 0; }
        if (nk_button_label(ctx, s_restore_ask ? "Sure? Again" : "Restore all") && !busy && s_game[0]) {   /* m10 */
            int n = restore_all(0, NULL), bad = 0;
            if (!n) { snprintf(s_msg, sizeof s_msg, "Nothing to restore: no file is modded."); s_restore_ask = 0; }
            else if (!s_restore_ask) {
                s_restore_ask = 1;
                snprintf(s_msg, sizeof s_msg, "Restore all puts back the original of the %d modded file%s (every texture, sound "
                         "and model change in them). Press it again to do it.", n, n == 1 ? "" : "s");
            } else if (paint_dirty()) {
                snprintf(s_msg, sizeof s_msg, "Save or close the painting first.");
            } else {
                char msg[200];
                s_restore_ask = 0;
                paint_stop();
                close_file();
                s_sel_file = -1;
                n = restore_all(1, &bad);
                if (bad) snprintf(msg, sizeof msg, "%d file%s put back; %d couldn't be (still in mod_backups).", n - bad, n - bad == 1 ? "" : "s", bad);
                else snprintf(msg, sizeof msg, "All %d modded file%s put back to the original.", n, n == 1 ? "" : "s");
                start_scan();
                snprintf(s_restore_msg, sizeof s_restore_msg, "%s", msg);
            }
        }
        {                                           /* m3: the tabs, with their counts */
            int cnt[CAT_N] = { 0 }, c;
            SDL_LockMutex(s_mx);
            for (i = 0; i < s_nfiles; i++) cnt[s_files[i].cat]++;
            SDL_UnlockMutex(s_mx);
            nk_layout_row_dynamic(ctx, row, NTABS);
            for (c = 0; c < NTABS; c++) {
                char t[48];
                int on = TABS[c] == s_tab;
                snprintf(t, sizeof t, "%s (%d)", CAT_NAMES[TABS[c]], cnt[TABS[c]]);
                if (nk_selectable_label(ctx, t, NK_TEXT_CENTERED, &on) && on) s_tab = TABS[c];
            }
        }
        nk_layout_row_dynamic(ctx, row - 4, 1);
        if (busy) nk_labelf(ctx, NK_TEXT_LEFT, "Looking through the game files... %d", SDL_AtomicGet(&s_scan_seen));
        else nk_label_wrap(ctx, s_msg);
    }
    nk_end(ctx);

    /* the files: the tab's, in collapsible sections */
    if (nk_begin(ctx, "Files", nk_rect(0, top, LW_FILES, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        static int *view;
        static int capview;
        int nview = 0, k;
        nk_layout_row_dynamic(ctx, row, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, s_filter, sizeof s_filter, nk_filter_default);
        SDL_LockMutex(s_mx);
        if (capview < s_nfiles) {
            capview = s_nfiles + 64;
            view = (int *)realloc(view, sizeof(int) * (size_t)capview);
        }
        for (i = 0; i < s_nfiles; i++) {
            const Entry *e = &s_files[i];
            if (e->cat != s_tab) continue;
            if (s_filter[0] && !SDL_strcasestr(e->rel, s_filter) && !SDL_strcasestr(e->label, s_filter)) continue;
            view[nview++] = i;
        }
        if (nview) qsort(view, (size_t)nview, sizeof(int), cmp_view);
        for (i = 0; i < nview; i = k) {
            const Entry *e0 = &s_files[view[i]];
            char head[128], hash[160];
            int n = 0;
            for (k = i; k < nview && s_files[view[k]].cat == e0->cat && s_files[view[k]].sec == e0->sec
                        && !strcmp(s_files[view[k]].secname, e0->secname); k++) n++;
            if (e0->cat == CAT_GRAF && e0->sec <= 4)
                snprintf(head, sizeof head, "%s, %d x %d (%d)", e0->secname, e0->w, e0->h, n);
            else
                snprintf(head, sizeof head, "%s (%d)", e0->secname, n);
            snprintf(hash, sizeof hash, "%d/%d/%s", s_tab, e0->cat, e0->secname);
            /* open at first, but graffiti and All's (many files) closed */
            if (nk_tree_push_hashed(ctx, NK_TREE_TAB, head,
                                    s_tab == CAT_GRAF ? NK_MINIMIZED : NK_MAXIMIZED,
                                    hash, (int)strlen(hash), 0)) {
                int q;
                for (q = i; q < k; q++) {
                    const Entry *e = &s_files[view[q]];
                    char lb[160];
                    int sel = view[q] == s_sel_file;
                    nk_layout_row_dynamic(ctx, row - 6, 1);
                    snprintf(lb, sizeof lb, "%s  (%d)", e->label, e->ntex ? e->ntex : e->nsnd);
                    if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel && !busy && may_leave_paint()) {
                        open_file(view[q]);
                        if (s_cur.ntex) show_texture(ren, getenv("MT_TEX") && atoi(getenv("MT_TEX")) < s_cur.ntex ? atoi(getenv("MT_TEX")) : 0);
                        else if (s_snd.n) show_sound(getenv("MT_SND") && atoi(getenv("MT_SND")) < s_snd.n ? atoi(getenv("MT_SND")) : 0);   /* m7 */
                    }
                }
                nk_tree_pop(ctx);
            }
        }
        if (!nview && s_nfiles) {
            nk_layout_row_dynamic(ctx, row, 1);
            nk_label(ctx, "Nothing here.", NK_TEXT_LEFT);
        }
        SDL_UnlockMutex(s_mx);
    }
    nk_end(ctx);

    /* its textures */
    if (nk_begin(ctx, s_cur.ntex || !s_snd.n ? "Textures" : "Sounds", nk_rect(LW_FILES, top, LW_TEX, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, row - 6, 1);
        for (i = 0; i < s_cur.ntex; i++) {
            const JTex *t = &s_cur.tex[i];
            int sel = i == s_sel_tex;
            char lb[64];
            snprintf(lb, sizeof lb, "%d: %dx%d %s", i, t->w, t->h, jt_fmt_name(t->fmt));
            if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel && may_leave_paint())
                show_texture(ren, i);
        }
        if (s_snd.n && s_cur.ntex) nk_label_colored(ctx, "Sounds", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        for (i = 0; i < s_snd.n; i++) {             /* m7: its sounds */
            const JSnd *sn = &s_snd.s[i];
            int sel = i == s_sel_snd;
            char lb[64];
            if (sn->frames) snprintf(lb, sizeof lb, "%d: %.2f s, %d kHz", i, (double)sn->frames / sn->rate, (sn->rate + 500) / 1000);
            else snprintf(lb, sizeof lb, "%d: %s", i, js_fmt_name(sn->fmt));
            if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel && may_leave_paint())
                show_sound(i);
        }
    }
    nk_end(ctx);

    /* m7: the sound */
    if (s_sel_file >= 0 && s_sel_snd >= 0 && s_sel_snd < s_snd.n) {
        if (nk_begin(ctx, "Sound", nk_rect(LW_FILES + LW_TEX, top, tex_w, (float)wh - top), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            const JSnd *sn = &s_snd.s[s_sel_snd];
            int playing = sound_playing();
            struct nk_rect wb;
            nk_layout_row_dynamic(ctx, row - 6, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "%s, sound %d", s_files[s_sel_file].rel, s_sel_snd);
            if (sn->frames)
                nk_labelf(ctx, NK_TEXT_LEFT, "%.2f s, %d Hz, %s, %s", (double)sn->frames / sn->rate, sn->rate,
                          sn->ch == 1 ? "mono" : sn->ch == 2 ? "stereo" : "multichannel", js_fmt_name(sn->fmt));
            else
                nk_labelf(ctx, NK_TEXT_LEFT, "%s (format 0x%X): can't be played or replaced", js_fmt_name(sn->fmt), sn->fmt);
            nk_label_colored(ctx, s_cur_modded ? "Modded (original in mod_backups)" : "Original",
                             NK_TEXT_LEFT, s_cur_modded ? nk_rgb(240, 175, 30) : nk_rgb(150, 150, 160));
            nk_layout_row_dynamic(ctx, row, 3);
            if (nk_button_label(ctx, playing ? "Stop" : "Play")) { if (playing) stop_sound(); else play_sound(); }
            if (nk_button_label(ctx, "Export WAV")) export_sound(s_sel_snd, 0);
            if (nk_button_label(ctx, "Import WAV")) import_sound(s_sel_snd, NULL);
            nk_layout_row_dynamic(ctx, row, 3);
            if (nk_button_label(ctx, "Export all")) export_all_sounds();
            if (nk_button_label(ctx, "Edits folder")) {
                char d[LPATH];
                pjoin(d, s_game, "sound_edits");
                make_dirs(d);
                open_folder(d);
            }
            if (s_cur_modded) {
                if (nk_button_label(ctx, "Undo all (file)")) revert_file(ren);
            } else nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, row - 8, 1);
            nk_label_colored(ctx, "Import: the exported WAV, or drop one here.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            nk_label_colored(ctx, "Any WAV: made the game's rate and channels.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            /* the waveform: each column's lowest and highest sample, where it's playing */
            nk_layout_row_dynamic(ctx, 220, 1);
            if (nk_widget(&wb, ctx) != NK_WIDGET_INVALID) {
                struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
                int cols = (int)wb.w, x, c, ch = sn->ch;
                float mid = wb.y + wb.h / 2;
                nk_fill_rect(cv, wb, 4, nk_rgb(24, 24, 30));
                nk_stroke_line(cv, wb.x, mid, wb.x + wb.w, mid, 1, nk_rgb(60, 60, 70));
                if (s_snd_pcm && s_snd_frames > 0 && cols > 0)
                    for (x = 0; x < cols; x++) {
                        int f0 = (int)((long long)s_snd_frames * x / cols), f1 = (int)((long long)s_snd_frames * (x + 1) / cols), f;
                        int lo = 0, hi = 0;
                        if (f1 <= f0) f1 = f0 + 1;
                        if (f1 > s_snd_frames) f1 = s_snd_frames;
                        for (f = f0; f < f1; f++)
                            for (c = 0; c < ch; c++) {
                                int v = s_snd_pcm[(size_t)f * ch + c];
                                if (v < lo) lo = v;
                                if (v > hi) hi = v;
                            }
                        nk_stroke_line(cv, wb.x + x + 0.5f, mid - hi * (wb.h / 2 - 4) / 32768.0f,
                                       wb.x + x + 0.5f, mid - lo * (wb.h / 2 - 4) / 32768.0f, 1, nk_rgb(240, 175, 30));
                    }
                if (playing && s_snd_frames > 0) {
                    Uint32 left = SDL_GetQueuedAudioSize(s_audio) / (2u * (Uint32)ch);
                    float px = wb.x + wb.w * (float)(s_snd_frames - (int)left) / (float)s_snd_frames;
                    nk_stroke_line(cv, px, wb.y, px, wb.y + wb.h, 2, nk_rgb(255, 255, 255));
                }
            }
        }
        nk_end(ctx);
    } else {
    /* the texture */
    if (nk_begin(ctx, "Texture", nk_rect(LW_FILES + LW_TEX, top, tex_w, (float)wh - top),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | (paint_on() ? NK_WINDOW_NO_SCROLLBAR : 0))) {
        if (paint_on() && s_sel_file >= 0 && s_sel_tex >= 0 && s_sel_tex < s_cur.ntex) {
            /* m4: painting */
            static int close_asked;
            int r;
            nk_layout_row_dynamic(ctx, row - 6, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "Painting %s, texture %d%s", s_files[s_sel_file].rel, s_sel_tex,
                      paint_dirty() ? " (not saved)" : "");
            r = paint_panel(ctx, row, (float)wh - top - 7 * row - 40);
            if (r == PAINT_SAVE && paint_dirty()) {
                save_paint(ren);
            } else if (r == PAINT_CLOSE) {
                if (paint_dirty() && !close_asked) {
                    close_asked = 1;
                    snprintf(s_msg, sizeof s_msg, "Not saved: Close again to throw the painting away.");
                } else {
                    close_asked = 0;
                    paint_stop();
                    s_msg[0] = 0;
                }
            } else if (r == PAINT_SAVE) close_asked = 0;
        } else if (s_sel_file >= 0 && s_sel_tex >= 0 && s_sel_tex < s_cur.ntex) {
            const JTex *t = &s_cur.tex[s_sel_tex];
            float avail = tex_w - 56, side = (float)wh - top - 9 * row - 40;
            float sz = avail < side ? avail : side;
            nk_layout_row_dynamic(ctx, row - 6, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "%s, texture %d", s_files[s_sel_file].rel, s_sel_tex);
            nk_labelf(ctx, NK_TEXT_LEFT, "%dx%d %s, %d level%s%s%s", t->w, t->h, jt_fmt_name(t->fmt), t->levels,
                      t->levels == 1 ? "" : "s", t->alpha ? ", transparency" : "", t->swizzled ? ", swizzled" : "");
            nk_label_colored(ctx, s_cur_modded ? "Modded (original in mod_backups)" : "Original",
                             NK_TEXT_LEFT, s_cur_modded ? nk_rgb(240, 175, 30) : nk_rgb(150, 150, 160));
            /* m5: two rows that share the narrower panel */
            nk_layout_row_dynamic(ctx, row, 3);
            if (nk_button_label(ctx, "Paint")) start_paint(ren);
            if (nk_button_label(ctx, "Export PNG")) export_texture(s_sel_tex);
            if (nk_button_label(ctx, "Import PNG")) import_texture(ren, s_sel_tex, NULL);
            nk_layout_row_dynamic(ctx, row, 2);
            if (nk_button_label(ctx, "Edits folder")) {
                char d[LPATH];
                pjoin(d, s_game, "texture_edits");
                make_dirs(d);
                open_folder(d);
            }
            if (s_cur_modded) {
                if (nk_button_label(ctx, "Undo all (file)")) revert_file(ren);
            } else nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, row - 8, 1);
            nk_label_colored(ctx, "Import: the exported PNG, or drop one here.", NK_TEXT_LEFT,
                             nk_rgb(150, 150, 160));
            if (s_preview && sz > 32) {
                /* m1d: its own shape (graffiti aren't all square, Miggas), at most 2x */
                struct nk_image im = nk_image_ptr(s_preview);
                float k = avail / (float)s_pw, kh = side / (float)s_ph;
                if (kh < k) k = kh;
                if (k > 2.0f) k = 2.0f;
                nk_layout_row_static(ctx, (float)s_ph * k, (int)((float)s_pw * k), 1);
                {
                    struct nk_rect ib = nk_widget_bounds(ctx);
                    nk_image(ctx, im);
                    v3d_draw_uv(nk_window_get_canvas(ctx), ib.x, ib.y, ib.w, ib.h);   /* m5b: Wires */
                }
            }
        } else {
            nk_layout_row_dynamic(ctx, row, 1);
            nk_label(ctx, s_nfiles ? "Pick a file, then a texture." : "Choose the game folder.", NK_TEXT_LEFT);
        }
    }
    nk_end(ctx);
    }

    /* m5: the 3D view, wearing the picked texture (the painting while painting) */
    if (nk_begin(ctx, "3D view", nk_rect(LW_FILES + LW_TEX + tex_w, top, (float)ww - LW_FILES - LW_TEX - tex_w, (float)wh - top),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        if (s_sel_file >= 0 && s_sel_tex >= 0 && s_sel_tex < s_cur.ntex) {
            int pw = s_cur.tex[s_sel_tex].w, ph = s_cur.tex[s_sel_tex].h;
            const uint8_t *px = s_tex_rgba;
            unsigned ver = s_tex_ver;
            if (paint_on()) {
                px = paint_image();
                paint_size(&pw, &ph);
                ver = paint_serial();
            }
            v3d_panel(ctx, ren, s_cur.tex[s_sel_tex].id, px, pw, ph, ver, row,
                      (float)wh - top - 76 - row - (s_mat_open ? MAT_H : 0));   /* m10b: room for the Materials button (m10c: a little more) */
            material_panel(ctx, ren, row);              /* m10 */
            if (v3d_take_paint_request() && !paint_on()) start_paint(ren);   /* m6: Paint ticked on the model */
            {
                const JMesh *em;
                if (v3d_take_export_request(&em)) export_model(em);              /* m8 */
                if (v3d_take_import_request(&em)) import_model(ren, em, NULL);   /* m9 */
            }
        } else {
            nk_layout_row_dynamic(ctx, row, 1);
            nk_label(ctx, s_sel_snd >= 0 ? "Sounds have no 3D view." : "Pick a texture.", NK_TEXT_LEFT);
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
    const char *shot = NULL, *shot_pick = NULL, *test_rel = NULL, *test_png = NULL;
    int i, test_n = 0, paint_test = 0, vsync = 0, restore_test = 0;
    SDL_SetMainReady();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(s_game, sizeof s_game, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];   /* a screenshot, then quit (tests) */
        else if (!strcmp(argv[i], "--pick") && i + 1 < argc) shot_pick = argv[++i];   /* ... of this file */
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) s_tab = atoi(argv[++i]);     /* ... on this tab */
        else if (!strcmp(argv[i], "--restore-test")) restore_test = 1;   /* Restore all, then the shot (tests) */
        else if (!strcmp(argv[i], "--paint-test")) paint_test = 1;   /* ... after a stroke painted (tests) */
        else if (!strcmp(argv[i], "--test") && i + 3 < argc) {   /* tests: import PNG into REL's texture N, or undo (N -1) */
            test_rel = argv[++i];
            test_n = atoi(argv[++i]);
            test_png = argv[++i];
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    s_mx = SDL_CreateMutex();
    if (!s_game[0]) {                           /* m11: the launcher's game folder when it's beside us */
        char g[LPATH];
        if (launcher_game(g)) { snprintf(s_game, sizeof s_game, "%s", g); s_from_launcher = 1; }
        else load_settings();
    }
    win = SDL_CreateWindow("JSRF Mod Tool " MODTOOL_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    paint_set_overlay(v3d_draw_uv);                 /* m5b: Wires over the painting */
    if (getenv("MT_WIRE")) v3d_set_wire(1);         /* (tests) */
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    ctx = nk_sdl_init(win, ren);
    load_font(ctx, 18);
    style(ctx);
    if (restore_test && s_game[0]) {
        int bad = 0, n = restore_all(1, &bad);
        printf("restored %d, failed %d\n", n, bad);
    }
    if (s_game[0]) start_scan();
    {
        SDL_RendererInfo ri;
        if (!SDL_GetRendererInfo(ren, &ri) && (ri.flags & SDL_RENDERER_PRESENTVSYNC)) vsync = 1;
    }
    while (running) {
        SDL_Event e;
        int ww, wh;
        Uint64 f0 = SDL_GetPerformanceCounter();
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
        if (shot && !SDL_AtomicGet(&s_scan_busy) && !SDL_AtomicGet(&s_scan_done) && s_nfiles && s_sel_file < 0) {
            int k = 0;
            if (shot_pick) for (k = 0; k < s_nfiles - 1 && strcmp(s_files[k].rel, shot_pick); k++) ;
            open_file(k);
            if (s_cur.ntex) show_texture(ren, getenv("MT_TEX") && atoi(getenv("MT_TEX")) < s_cur.ntex ? atoi(getenv("MT_TEX")) : 0);
            else if (s_snd.n) show_sound(getenv("MT_SND") && atoi(getenv("MT_SND")) < s_snd.n ? atoi(getenv("MT_SND")) : 0);   /* m7 */
        }
        SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        if (paint_test && shot && s_preview) {             /* a stroke by the mouse, then saved */
            static int f;
            SDL_Event ev;
            memset(&ev, 0, sizeof ev);
            static int sx0 = 700, sy0 = 420, sx1 = 1100, sy1 = 620;   /* MT_STROKE=x0,y0,x1,y1 */
            if (f == 0 && getenv("MT_STROKE")) sscanf(getenv("MT_STROKE"), "%d,%d,%d,%d", &sx0, &sy0, &sx1, &sy1);
            f++;
            if (f == 1) {
                start_paint(ren);
                if (getenv("MT_RESIZE")) paint_resize(atoi(getenv("MT_RESIZE")));
                if (getenv("MT_PAINT3D")) v3d_set_paint(1);   /* m6: the stroke on the model */
            } else if (f == 3) {
                ev.type = SDL_MOUSEMOTION; ev.motion.x = sx0; ev.motion.y = sy0; SDL_PushEvent(&ev);
            } else if (f == 4) {
                ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.state = SDL_PRESSED;
                ev.button.x = sx0; ev.button.y = sy0; SDL_PushEvent(&ev);
            } else if (f > 4 && f < 30) {
                ev.type = SDL_MOUSEMOTION; ev.motion.x = sx0 + (sx1 - sx0) * (f - 4) / 25; ev.motion.y = sy0 + (sy1 - sy0) * (f - 4) / 25;
                ev.motion.state = SDL_BUTTON_LMASK; SDL_PushEvent(&ev);
            } else if (f == 30) {
                ev.type = SDL_MOUSEBUTTONUP; ev.button.button = SDL_BUTTON_LEFT; ev.button.state = SDL_RELEASED;
                ev.button.x = sx1; ev.button.y = sy1; SDL_PushEvent(&ev);
            } else if (f == 34) {
                if (paint_dirty()) save_paint(ren);
                printf("%s\n", s_msg);
            } else if (f >= 36 && f < 40 && !getenv("MT_CLICK")) {   /* zoomed in, to see it stays in its box */
                ev.type = SDL_MOUSEWHEEL; ev.wheel.y = 1; ev.wheel.preciseY = 1.0f; SDL_PushEvent(&ev);
            } else if (f >= 36 && f <= 38 && getenv("MT_CLICK")) {   /* the mouse there ("x,y"), a click */
                int cx = 0, cy = 0;
                sscanf(getenv("MT_CLICK"), "%d,%d", &cx, &cy);
                if (f == 36) {
                    ev.type = SDL_MOUSEMOTION; ev.motion.x = cx; ev.motion.y = cy; SDL_PushEvent(&ev);
                    goto present;
                }
                ev.type = f == 37 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.state = f == 37 ? SDL_PRESSED : SDL_RELEASED;
                ev.button.x = cx; ev.button.y = cy;
                SDL_PushEvent(&ev);
            }
            if (getenv("MT_SHOTF") && f == atoi(getenv("MT_SHOTF"))) {
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32, SDL_PIXELFORMAT_ABGR8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ABGR8888, sf->pixels, sf->pitch);   /* this frame, drawn */
                stbi_write_png(shot, ww, wh, 4, sf->pixels, sf->pitch);
                SDL_FreeSurface(sf);
                break;
            }
            if (f < 42) goto present;
        }
        if (shot && (s_preview || s_sel_snd >= 0)) {   /* m7: or a sound */
            static int frames;
            if (++frames == 5) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32, SDL_PIXELFORMAT_ABGR8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ABGR8888, s->pixels, s->pitch);
                stbi_write_png(shot, ww, wh, 4, s->pixels, s->pitch);
                SDL_FreeSurface(s);
                running = 0;
            }
        }
    present:
        SDL_RenderPresent(ren);
        if (!running) break;
        /* m5c: the screen's refresh paces the frames; a fixed 10 ms on top of
         * it made every other refresh missed (about 30 fps, choppy painting,
         * Miggas). Without vsync, about 60 frames a second. */
        if (SDL_AtomicGet(&s_scan_busy)) SDL_Delay(30);
        else if (!vsync) {
            double ms = (double)(SDL_GetPerformanceCounter() - f0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            if (ms < 15.0) SDL_Delay((Uint32)(16.0 - ms));
        }
    }
    close_file();
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
