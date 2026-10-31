/* JSRF launcher: one program to set up, build, update, configure and play
 * the recompiled Jet Set Radio Future, on Windows and Linux (Steam Deck).
 * Nothing has to be installed first: the tools it needs (Python, the Zig
 * C compiler, CMake) are portable copies it downloads into tools/.
 *
 * The window is drawn with Nuklear (third_party/nuklear) on SDL2. */
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "ini.h"
#include "jobs.h"
#include "util.h"
#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { TAB_PLAY, TAB_SETTINGS, TAB_BUILD, TAB_UPDATE, TAB_COUNT };
static const char *const TAB_NAMES[TAB_COUNT] = { "Play", "Settings", "Build", "Update" };

static int s_tab = TAB_PLAY;
static int s_win_h = 800;             /* the window's height, for the drop-down lists */
static Ini s_game_ini;
static char s_game_dir_edit[LPATH];
static bool s_regen;
static unsigned s_log_seen;
static char s_message[512];
static float s_row = 34;              /* row height, from the font */

static const struct nk_color C_OK   = { 120, 220, 120, 255 };
static const struct nk_color C_BAD  = { 240, 130, 110, 255 };
static const struct nk_color C_DIM  = { 165, 165, 175, 255 };
static const struct nk_color C_HEAD = { 255, 205, 60, 255 };

static void load_game_ini(void)
{
    char p[LPATH];
    game_ini(p);
    ini_free(&s_game_ini);
    ini_load(&s_game_ini, p);
}

static void set_game_dir(const char *dir)
{
    paths_set_game(dir);
    snprintf(s_game_dir_edit, sizeof s_game_dir_edit, "%s", g_paths.game);
    load_game_ini();
}

/* a dropped default.xbe (or its folder) sets the game folder */
static void dropped(const char *p)
{
    char d[LPATH];
    snprintf(d, sizeof d, "%s", p);
    if (is_file(d)) {
        char *s1 = strrchr(d, '/'), *s2 = strrchr(d, '\\');
        char *s = s1 > s2 ? s1 : s2;
        if (s) *s = 0;
    }
    if (is_dir(d)) {
        set_game_dir(d);
        snprintf(s_message, sizeof s_message, "Game folder set to %s", d);
    }
}

/* ---------------------------------------------------------------------- */
static void status_line(struct nk_context *ctx, const char *what, bool ok, const char *yes, const char *no)
{
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 2);
    nk_layout_row_push(ctx, 0.28f);
    nk_label(ctx, what, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.72f);
    nk_label_colored(ctx, ok ? yes : no, NK_TEXT_LEFT, ok ? C_OK : C_BAD);
    nk_layout_row_end(ctx);
}

static bool button(struct nk_context *ctx, const char *label, bool enabled)
{
    if (!enabled) nk_widget_disable_begin(ctx);
    bool r = nk_button_label(ctx, label) && enabled;
    if (!enabled) nk_widget_disable_end(ctx);
    return r;
}

static void spacer(struct nk_context *ctx, float h)
{
    nk_layout_row_dynamic(ctx, h, 1);
    nk_spacing(ctx, 1);
}

/* Play's Build: generate (if needed) and build (if needed) */
static void start_build(void)
{
    if (job_start(JOB_ALL, false)) s_tab = TAB_BUILD;
}

static void tab_play(struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label(ctx, "Game folder (with default.xbe):", NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
    nk_layout_row_push(ctx, 0.70f);
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
                                                 s_game_dir_edit, sizeof s_game_dir_edit, nk_filter_default);
    nk_layout_row_push(ctx, 0.14f);
    bool use = nk_button_label(ctx, "Use");
    nk_layout_row_push(ctx, 0.16f);
    if (nk_button_label(ctx, "Open folder")) {
        make_dirs(g_paths.game);
        open_folder(g_paths.game);
    }
    nk_layout_row_end(ctx);
    if ((use || (ev & NK_EDIT_COMMITED)) && strcmp(s_game_dir_edit, g_paths.game)) set_game_dir(s_game_dir_edit);
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label_colored(ctx, "Or drop default.xbe (or the folder) onto this window.", NK_TEXT_LEFT, C_DIM);

    spacer(ctx, 10);
    bool xbe = have_xbe(), exe = have_game_exe(), tools = have_tools();
    status_line(ctx, "Game files", xbe, "default.xbe found", "default.xbe not found in this folder");
    status_line(ctx, "Generated code", have_gen(), "there", "not generated yet: press Build");
    status_line(ctx, "Recompiled game", exe, "built", "not built yet: press Build");
    status_line(ctx, "Project source", g_paths.have_source,
                "downloaded", "not downloaded yet: Update -> Download");
    status_line(ctx, "Build tools", tools, "ready", "downloaded on the first build (about 150 MB)");

    spacer(ctx, 16);
    nk_layout_row_dynamic(ctx, s_row * 1.8f, 2);
    if (button(ctx, "PLAY", exe && !job_busy())) {
        if (!play()) snprintf(s_message, sizeof s_message, "Could not start the game.");
        else snprintf(s_message, sizeof s_message, "Game started.");
    }
    if (button(ctx, exe ? "Build again" : "Build", xbe && g_paths.have_source && !job_busy()))
        start_build();
}

/* ---------------------------------------------------------------------- */
static int text_lines(struct nk_context *ctx, const char *s, float width)
{
    const struct nk_user_font *f = ctx->style.font;
    float w = f->width(f->userdata, f->height, s, (int)strlen(s));
    int n = (int)(w / (width > 50 ? width - 20 : 50)) + 1;
    return n < 1 ? 1 : n;
}

/* ---- Settings: the common settings as controls, each writing the
 * jsrf.ini lines it stands for; "All settings" is every line ---- */
enum { SET_GAMEPLAY, SET_VIDEO, SET_INPUT, SET_AUDIO, SET_DEBUG, SET_ALL, SET_COUNT };
static const char *const SET_NAMES[SET_COUNT] = { "Gameplay", "Video", "Input", "Audio", "Debug", "All settings" };
static int s_set_tab = SET_GAMEPLAY;

static Ini *gi(void) { return &s_game_ini; }

/* the value of a key now, or def when it is off */
static const char *get_or(const char *key, const char *def)
{
    const char *v = ini_get(gi(), key);
    return v && *v ? v : def;
}

static void put(const char *key, const char *val, const char *section)
{
    ini_put(gi(), key, val, section);
}

static void off(const char *key) { ini_off(gi(), key); }

/* the description, under its option */
static void desc(struct nk_context *ctx, float width, const char *text, struct nk_color c)
{
    nk_layout_row_dynamic(ctx, (s_row - 8) * text_lines(ctx, text, width), 1);
    nk_label_colored_wrap(ctx, text, c);
    spacer(ctx, 2);
}

/* a name on the left, the control on the right */
static void opt_row(struct nk_context *ctx, const char *name)
{
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 2);
    nk_layout_row_push(ctx, 0.34f);
    nk_label(ctx, name, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.66f);
}

static int combo(struct nk_context *ctx, const char *name, const char *const *items, int n, int sel)
{
    opt_row(ctx, name);
    /* every item, or what fits under the box (it opens downwards and the
     * window cuts it off): the rest scrolls */
    struct nk_rect b = nk_widget_bounds(ctx);
    float want = n * (s_row + ctx->style.window.spacing.y + 2) + 2 * ctx->style.window.popup_padding.y + 4;
    float room = (float)s_win_h - (b.y + b.h) - 8;
    if (want > room && room > s_row * 2) want = room;
    int r = nk_combo(ctx, items, n, sel, (int)s_row, nk_vec2(b.w, want));
    nk_layout_row_end(ctx);
    return r;
}

static bool check(struct nk_context *ctx, const char *name, bool on)
{
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_bool v = on;
    nk_checkbox_label(ctx, name, &v);
    return v != 0;
}

static int find(const char *const *vals, int n, const char *v)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(vals[i], v)) return i;
    return -1;
}

static void panel_gameplay(struct nk_context *ctx, float w)
{
    bool on = !strcmp(get_or("RECOMP_SKIP_INTRO", "0"), "1");
    bool nv = check(ctx, "Skip intros", on);
    if (nv != on) { if (nv) put("RECOMP_SKIP_INTRO", "1", "game"); else off("RECOMP_SKIP_INTRO"); }
    desc(ctx, w, "Skips the logos at start.", C_DIM);
}

static void panel_video(struct nk_context *ctx, float w)
{
    /* resolution scale: RECOMP_GL_SCALE */
    {
        static const char *const items[] = { "1x (640x480)", "2x (1280x960)", "3x (1920x1440)", "4x (2560x1920)" };
        int cur = atoi(get_or("RECOMP_GL_SCALE", "1"));
        if (cur < 1) cur = 1;
        if (cur > 4) cur = 4;
        int sel = combo(ctx, "Resolution scale", items, 4, cur - 1);
        if (sel != cur - 1) {
            char v[4];
            snprintf(v, sizeof v, "%d", sel + 1);
            put("RECOMP_GL_SCALE", v, "display");
        }
        desc(ctx, w, "The resolution the game is drawn at. Higher is sharper and smoother; 2x is best for the Steam Deck.", C_DIM);
    }
    /* widescreen: RECOMP_WIDESCREEN (auto, 1 = 16:9), or RECOMP_ASPECT=w:h,
     * which wins over it */
    {
        static const char *const items[] = { "Disabled (4:3)", "Auto (your screen's shape)", "16:9", "16:10", "21:9", "32:9" };
        static const char *const aspects[] = { "", "", "", "16:10", "21:9", "32:9" };
        const char *asp = ini_get(gi(), "RECOMP_ASPECT");
        const char *ws = ini_get(gi(), "RECOMP_WIDESCREEN");
        int cur = 0;
        if (asp && *asp) { cur = find(aspects, 6, asp); if (cur < 3) cur = 0; }
        else if (ws && !strcmp(ws, "auto")) cur = 1;
        else if (ws && ws[0] == '1') cur = 2;
        int sel = combo(ctx, "Widescreen", items, 6, cur);
        if (sel != cur) {
            off("RECOMP_ASPECT");
            off("RECOMP_WIDESCREEN");
            if (sel == 1) put("RECOMP_WIDESCREEN", "auto", "display");
            else if (sel == 2) put("RECOMP_WIDESCREEN", "1", "display");
            else if (sel >= 3) put("RECOMP_ASPECT", aspects[sel], "display");
        }
        desc(ctx, w, "Auto uses your screen's shape. The HUD moves to the screen edges; a few menus stay 4:3.", C_DIM);
    }
    /* window: RECOMP_FULLSCREEN (a borderless full-screen window), and on
     * Windows RECOMP_GL_PRESENT=dxgi on top of it */
    {
#ifdef _WIN32
        static const char *const items[] = { "Windowed", "Borderless window", "Fullscreen" };
        enum { W_WIN, W_BORDERLESS, W_FULL, W_N = 3 };
#else
        static const char *const items[] = { "Windowed", "Fullscreen" };
        enum { W_WIN, W_FULL, W_N = 2, W_BORDERLESS = 99 };
#endif
        bool fs = !strcmp(get_or("RECOMP_FULLSCREEN", "0"), "1");
        bool dx = !strcmp(get_or("RECOMP_GL_PRESENT", ""), "dxgi");
        int cur = !fs ? W_WIN : (dx && W_BORDERLESS != 99) ? W_BORDERLESS : W_FULL;
        int sel = combo(ctx, "Window", items, W_N, cur);
        if (sel != cur) {
            if (sel == W_WIN) { off("RECOMP_FULLSCREEN"); off("RECOMP_GL_PRESENT"); }
            else if (sel == W_FULL) { put("RECOMP_FULLSCREEN", "1", "display"); off("RECOMP_GL_PRESENT"); }
            else { put("RECOMP_FULLSCREEN", "1", "display"); put("RECOMP_GL_PRESENT", "dxgi", "display"); }
        }
#ifdef _WIN32
        desc(ctx, w, "F11 or Alt+Enter switches while playing. Borderless window presents through DXGI and may lower performance.", C_DIM);
#else
        desc(ctx, w, "F11 or Alt+Enter switches while playing.", C_DIM);
#endif
    }
    /* anisotropic filtering: RECOMP_GL_ANISO (off = 1) */
    {
        static const char *const items[] = { "Disabled", "2x", "4x", "8x", "16x" };
        static const char *const vals[] = { "", "2", "4", "8", "16" };
        int cur = find(vals, 5, get_or("RECOMP_GL_ANISO", ""));
        if (cur < 0) cur = 0;
        int sel = combo(ctx, "Anisotropic filtering", items, 5, cur);
        if (sel != cur) { if (sel == 0) off("RECOMP_GL_ANISO"); else put("RECOMP_GL_ANISO", vals[sel], "display"); }
        desc(ctx, w, "Sharper textures on surfaces seen at an angle (roads, walls). Costs very little.", C_DIM);
    }
    /* anti-aliasing: RECOMP_GL_AA */
    {
        static const char *const items[] = { "Disabled", "FXAA", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra" };
        static const char *const vals[] = { "", "fxaa", "smaa_low", "smaa_medium", "smaa_high", "smaa_ultra" };
        int cur = find(vals, 6, get_or("RECOMP_GL_AA", ""));
        if (cur < 0) cur = 0;
        int sel = combo(ctx, "Anti-aliasing", items, 6, cur);
        if (sel != cur) { if (sel == 0) off("RECOMP_GL_AA"); else put("RECOMP_GL_AA", vals[sel], "display"); }
        desc(ctx, w, "Smooths jagged edges. FXAA is the lightest, SMAA looks better. A higher resolution scale smooths edges too.", C_DIM);
    }
}

static void panel_input(struct nk_context *ctx, float w)
{
    {
        static const char *const items[] = { "1", "2", "3", "4" };
        int cur = atoi(get_or("RECOMP_USB_PADS", "1"));
        if (cur < 1) cur = 1;
        if (cur > 4) cur = 4;
        int sel = combo(ctx, "Gamepads", items, 4, cur - 1);
        if (sel != cur - 1) put("RECOMP_USB_PADS", items[sel], "input");
        desc(ctx, w, "Number of players. With 2 or more, players join in the order they press a button.", C_DIM);
    }
    {
        int cur = atoi(get_or("RECOMP_RUMBLE", "100")), v = cur;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
        nk_layout_row_push(ctx, 0.34f);
        nk_label(ctx, "Rumble strength", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.54f);
        nk_slider_int(ctx, 0, &v, 100, 5);
        nk_layout_row_push(ctx, 0.12f);
        char t[16];
        snprintf(t, sizeof t, v ? "%d%%" : "Off", v);
        nk_label(ctx, t, NK_TEXT_RIGHT);
        nk_layout_row_end(ctx);
        if (v != cur) { char s2[8]; snprintf(s2, sizeof s2, "%d", v); put("RECOMP_RUMBLE", s2, "input"); }
        desc(ctx, w, "Controller vibration, in percent. 0 turns it off.", C_DIM);
    }
}

static void panel_audio(struct nk_context *ctx, float w)
{
    float cur = (float)atof(get_or("RECOMP_APU_GAIN", "0.5")), v = cur;
    if (v < 0) v = 0;
    if (v > 1.5f) v = 1.5f;
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
    nk_layout_row_push(ctx, 0.34f);
    nk_label(ctx, "Gain", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.54f);
    nk_slider_float(ctx, 0.0f, &v, 1.5f, 0.05f);
    nk_layout_row_push(ctx, 0.12f);
    char t[16];
    snprintf(t, sizeof t, "%.2f", v);
    nk_label(ctx, t, NK_TEXT_RIGHT);
    nk_layout_row_end(ctx);
    if (v < cur - 0.001f || v > cur + 0.001f) {
        char s2[16];
        snprintf(s2, sizeof s2, "%.2f", v);
        put("RECOMP_APU_GAIN", s2, "audio");
    }
    desc(ctx, w, "Master volume. 0.50 is the default; above 1.00 loud sounds may be limited.", C_DIM);
}

/* Logging: RECOMP_LOG=stderr.txt with quiet off; off: no log, quiet on.
 * Performance logs: RECOMP_PROFILE=1, which needs the log. */
static void panel_debug(struct nk_context *ctx, float w)
{
    bool logging = ini_get(gi(), "RECOMP_LOG") && *ini_get(gi(), "RECOMP_LOG");
    bool prof = !strcmp(get_or("RECOMP_PROFILE", "0"), "1");
    bool nl = check(ctx, "Logging", logging);
    desc(ctx, w, "Writes stderr.txt in the game folder, for bug reports. Turns quiet mode off, so the game runs a little slower.", C_DIM);
    bool np = check(ctx, "Performance logs", prof);
    desc(ctx, w, "Slows the game down. Adds where the time goes to the log every 10 seconds. Turns Logging on.", C_BAD);
    if (np && !prof) nl = true;            /* needs the log */
    if (!nl && logging) np = false;        /* no log, nothing to write it to */
    if (nl != logging) {
        if (nl) { put("RECOMP_LOG", "stderr.txt", "files"); off("RECOMP_QUIET"); }
        else { off("RECOMP_LOG"); put("RECOMP_QUIET", "1", "game"); }
    }
    if (np != prof) { if (np) put("RECOMP_PROFILE", "1", "files"); else off("RECOMP_PROFILE"); }
}

/* every line of jsrf.ini, ticked = on */
static void panel_all(struct nk_context *ctx, float width)
{
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label_colored(ctx, "Every line of jsrf.ini. Ticked = on.", NK_TEXT_LEFT, C_DIM);
    for (int i = 0; i < s_game_ini.n; i++) {
        IniLine *L = &s_game_ini.l[i];
        switch (L->kind) {
        case L_SECTION:
            spacer(ctx, 6);
            nk_layout_row_dynamic(ctx, s_row, 1);
            nk_label_colored(ctx, L->show, NK_TEXT_LEFT, C_HEAD);
            break;
        case L_DESC:
            nk_layout_row_dynamic(ctx, (s_row - 8) * text_lines(ctx, L->show, width), 1);
            nk_label_colored_wrap(ctx, L->show, C_DIM);
            break;
        case L_SETTING: {
            nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 2);
            nk_layout_row_push(ctx, 0.45f);
            nk_bool on = L->on;
            if (nk_checkbox_label(ctx, L->key, &on)) ini_set_on(&s_game_ini, i, on != 0);
            nk_layout_row_push(ctx, 0.55f);
            char before[sizeof L->val];
            memcpy(before, L->val, sizeof before);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, L->val, sizeof L->val, nk_filter_default);
            if (strcmp(before, L->val) && !L->on) ini_set_on(&s_game_ini, i, true);
            nk_layout_row_end(ctx);
            break;
        }
        default:
            break;
        }
    }
}

static void tab_settings(struct nk_context *ctx, float h)
{
    char p[LPATH];
    game_ini(p);
    if (!s_game_ini.loaded) {
        nk_layout_row_dynamic(ctx, s_row, 1);
        nk_label(ctx, "No jsrf.ini in the game folder yet.", NK_TEXT_LEFT);
        nk_label_colored(ctx, "It is made when the game is built, or now from the project's template:",
                         NK_TEXT_LEFT, C_DIM);
        nk_layout_row_dynamic(ctx, s_row * 1.4f, 3);
        char tmpl[LPATH];
        pjoin3(tmpl, g_paths.source, "jsrf_recomp", "portable");
        pjoin(tmpl, tmpl, "jsrf.ini");
        if (button(ctx, "Create jsrf.ini", is_file(tmpl) && is_dir(g_paths.game))) {
            copy_file(tmpl, p);
            load_game_ini();
        }
        return;
    }
    bool dirty = ini_dirty(&s_game_ini);
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row * 1.2f, 4);
    nk_layout_row_push(ctx, 0.16f);
    if (button(ctx, "Save", dirty)) {
        if (ini_save(&s_game_ini)) snprintf(s_message, sizeof s_message, "Saved %s", p);
        else snprintf(s_message, sizeof s_message, "Could not save %s", p);
    }
    nk_layout_row_push(ctx, 0.16f);
    if (button(ctx, "Undo changes", dirty)) ini_revert(&s_game_ini);
    nk_layout_row_push(ctx, 0.16f);
    if (button(ctx, "Reload", true)) load_game_ini();
    nk_layout_row_push(ctx, 0.52f);
    nk_label_colored(ctx, dirty ? "Not saved yet." : "Steam launch options override these.",
                     NK_TEXT_RIGHT, dirty ? C_HEAD : C_DIM);
    nk_layout_row_end(ctx);

    /* the sections */
    nk_layout_row_dynamic(ctx, s_row * 1.1f, SET_COUNT);
    for (int i = 0; i < SET_COUNT; i++) {
        nk_bool sel = s_set_tab == i;
        if (nk_selectable_label(ctx, SET_NAMES[i], NK_TEXT_CENTERED, &sel)) s_set_tab = i;
    }

    nk_layout_row_dynamic(ctx, h - s_row * 2.3f - 24, 1);
    if (nk_group_begin(ctx, "settings", 0)) {
        float width = nk_window_get_content_region(ctx).w;
        switch (s_set_tab) {
        case SET_GAMEPLAY: panel_gameplay(ctx, width); break;
        case SET_VIDEO:    panel_video(ctx, width); break;
        case SET_INPUT:    panel_input(ctx, width); break;
        case SET_AUDIO:    panel_audio(ctx, width); break;
        case SET_DEBUG:    panel_debug(ctx, width); break;
        case SET_ALL:      panel_all(ctx, width); break;
        }
        nk_group_end(ctx);
    }
}

/* ---------------------------------------------------------------------- */
static void log_view(struct nk_context *ctx, float h)
{
    nk_layout_row_dynamic(ctx, h, 1);
    const char **lines;
    int n = log_lock(&lines);
    int rh = (int)(s_row - 10);
    struct nk_list_view view;
    if (nk_list_view_begin(ctx, &view, "log", 0, rh, n)) {
        nk_layout_row_dynamic(ctx, (float)rh, 1);
        for (int i = view.begin; i < view.end && i < n; i++) {
            const char *l = lines[i];
            bool head = !strncmp(l, "== ", 3);
            bool err = strstr(l, "error") || strstr(l, "Failed") || strstr(l, "FAILED");
            nk_label_colored(ctx, head ? l + 3 : l, NK_TEXT_LEFT,
                             head ? C_HEAD : err ? C_BAD : nk_rgb(215, 215, 220));
        }
        nk_list_view_end(&view);
    }
    log_unlock();
    /* new output: follow it */
    if (log_serial() != s_log_seen) {
        s_log_seen = log_serial();
        nk_group_set_scroll(ctx, "log", 0, (nk_uint)(n * (rh + (int)ctx->style.window.spacing.y)));
    }
}

/* launcher_log.txt in the program that opens text files */
static void open_log_in_editor(void)
{
    char p[LPATH];
    pjoin(p, g_paths.base, "launcher_log.txt");
    open_folder(p);
}

static void tab_build(struct nk_context *ctx, float h)
{
    float used = 0;
#ifdef _WIN32
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
    nk_layout_row_push(ctx, 0.18f);
    nk_label(ctx, "Compiler", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.42f);
    static const char *const comp[] = { "Auto (Visual Studio if installed)", "Zig (built-in)", "Visual Studio" };
    int c = compiler_pref();
    int nc = nk_combo(ctx, comp, 3, c, (int)s_row, nk_vec2(420, s_row * 4));
    if (nc != c) compiler_set(nc);
    nk_layout_row_push(ctx, 0.40f);
    nk_label_colored(ctx, have_msvc() ? "Visual Studio found" : "Visual Studio not installed: Zig is used",
                     NK_TEXT_LEFT, C_DIM);
    nk_layout_row_end(ctx);
    used += s_row + 4;
#endif
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_bool rg = s_regen;
    nk_checkbox_label(ctx, "Even if nothing changed (normally each button checks first and skips)", &rg);
    s_regen = rg != 0;
    used += s_row + 4;

    bool busy = job_busy();
    bool ready = !busy && have_xbe() && g_paths.have_source;
    nk_layout_row_dynamic(ctx, s_row * 1.4f, 6);
    if (button(ctx, "Generate code", ready)) job_start(JOB_GENERATE, s_regen);
    if (button(ctx, "Build", ready)) job_start(JOB_BUILD, s_regen);
    if (button(ctx, "Download tools", !busy && !have_tools())) job_start(JOB_TOOLS, false);
    if (button(ctx, "Cancel", busy)) job_cancel();
    if (button(ctx, "Clear log", !busy)) log_clear();
    if (button(ctx, "Open log file", true)) open_log_in_editor();
    used += s_row * 1.4f + 4;
    if (!have_xbe()) {
        nk_layout_row_dynamic(ctx, s_row, 1);
        nk_label_colored(ctx, "Set the game folder (with default.xbe) in Play first.", NK_TEXT_LEFT, C_BAD);
        used += s_row + 4;
    }
    log_view(ctx, h - used - 8);
}

static void tab_update(struct nk_context *ctx, float h)
{
    float used = 0;
    nk_layout_row_dynamic(ctx, s_row, 1);
    char t[600];
    {
        const char *id = source_id();
        if (!g_paths.have_source) snprintf(t, sizeof t, "The project is not downloaded yet.");
        else if (*id) snprintf(t, sizeof t, "Project version: %.10s", id);
        else snprintf(t, sizeof t, "Project version: unknown");
        size_t tl = strlen(t);
        snprintf(t + tl, sizeof t - tl, "   (branch: %s, set in launcher.ini)", update_branch());
        nk_label(ctx, t, NK_TEXT_LEFT);
        const char *r = update_remote();
        if (*r && strcmp(r, id))
            nk_label_colored(ctx, "A newer version is available.", NK_TEXT_LEFT, C_HEAD);
        else
            nk_label_colored(ctx, "Update only downloads the project (your game files, jsrf.ini and saves are kept). Then Build.",
                             NK_TEXT_LEFT, C_DIM);
        used += 2 * (s_row + 4);
        bool busy = job_busy();
        nk_layout_row_dynamic(ctx, s_row * 1.4f, 5);
        if (button(ctx, "Check for updates", !busy)) job_start(JOB_CHECK, false);
        if (button(ctx, g_paths.have_source ? "Update" : "Download", !busy)) {
            job_start(JOB_UPDATE, false);
        }
        if (button(ctx, "Cancel", busy)) job_cancel();
        if (button(ctx, "Clear log", !busy)) log_clear();
        if (button(ctx, "Open log file", true)) open_log_in_editor();
        used += s_row * 1.4f + 4;
    }
    log_view(ctx, h - used - 8);
}

/* ---------------------------------------------------------------------- */
static void load_font(struct nk_context *ctx, float size)
{
    static const char *const fonts[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
        NULL
    };
    struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *font = NULL;
    for (int i = 0; fonts[i] && !font; i++)
        if (is_file(fonts[i])) font = nk_font_atlas_add_from_file(atlas, fonts[i], size, NULL);
    if (!font) font = nk_font_atlas_add_default(atlas, size, NULL);
    nk_sdl_font_stash_end();
    nk_style_set_font(ctx, &font->handle);
    s_row = size + 14;
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
    ctx->style.window.padding = nk_vec2(16, 12);
    ctx->style.window.spacing = nk_vec2(8, 6);
    ctx->style.button.rounding = 6;
}

/* jsrf_launcher --tools | --generate | --build | --all | --update | --check
 * [--force]: the job without the window, its log printed */
static void open_log_file(void)
{
    char p[LPATH];
    pjoin(p, g_paths.base, "launcher_log.txt");
    log_file_open(p);
}

static int headless(int job, bool regen)
{
    log_init();
    paths_init();
    open_log_file();
    if (!job_start(job, regen)) return 1;
    int shown = 0;
    for (;;) {
        bool busy = job_busy();
        const char **lines;
        int n = log_lock(&lines);
        if (n < shown) shown = 0;
        /* the last line may still be growing: printed once it is done */
        int upto = busy ? n - 1 : n;
        for (; shown < upto; shown++) printf("%s\n", lines[shown]);
        log_unlock();
        fflush(stdout);
        if (!busy) break;
        SDL_Delay(100);
    }
    return job_result() == 1 ? 0 : 1;
}

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    int job = JOB_NONE;
    bool regen = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tools")) job = JOB_TOOLS;
        else if (!strcmp(argv[i], "--generate")) job = JOB_GENERATE;
        else if (!strcmp(argv[i], "--build")) job = JOB_BUILD;
        else if (!strcmp(argv[i], "--all")) job = JOB_ALL;
        else if (!strcmp(argv[i], "--update")) job = JOB_UPDATE;
        else if (!strcmp(argv[i], "--check")) job = JOB_CHECK;
        else if (!strcmp(argv[i], "--force")) regen = true;
    }
    if (job != JOB_NONE) {
#ifdef _WIN32
        /* a window program: print to the console it was started from */
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
#endif
        SDL_Init(0);
        int r = headless(job, regen);
        SDL_Quit();
        return r;
    }
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    log_init();
    paths_init();
    open_log_file();
    snprintf(s_game_dir_edit, sizeof s_game_dir_edit, "%s", g_paths.game);
    load_game_ini();

    char title[128];
    snprintf(title, sizeof title, "Jet Set Radio Future - launcher %s", LAUNCHER_VERSION);
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 800, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    struct nk_context *ctx = nk_sdl_init(win, ren);
    load_font(ctx, 20);
    style(ctx);

    logf_("Launcher folder: %s", g_paths.base);
    logf_("Project: %s%s", g_paths.source, g_paths.have_source ? "" : " (not downloaded yet)");
    logf_("Game folder: %s", g_paths.game);

    int last_result = -1;
    bool running = true;
    while (running) {
        SDL_Event e;
        nk_input_begin(ctx);
        /* A tap (touchscreen, or a quick click) is the pointer arriving,
         * the press and the release all at once, and the UI only sees a
         * button pressed if each comes in its own frame: the pointer moves
         * in this frame, the press is kept for the next, and a press ends
         * its frame so the release comes in the one after. */
        static SDL_Event pending;
        static bool have_pending;
        bool got;
        if (have_pending) { e = pending; have_pending = false; got = true; }
        else got = SDL_WaitEventTimeout(&e, job_busy() ? 50 : 250);   /* idle, but follow a job's log */
        if (got) {
            do {
                if (e.type == SDL_MOUSEBUTTONDOWN && !have_pending &&
                    (e.button.x != ctx->input.mouse.pos.x || e.button.y != ctx->input.mouse.pos.y)) {
                    nk_input_motion(ctx, e.button.x, e.button.y);
                    pending = e;
                    have_pending = true;
                    break;
                }
                if (e.type == SDL_QUIT) running = false;
                else if (e.type == SDL_DROPFILE) { dropped(e.drop.file); SDL_free(e.drop.file); }
                else nk_sdl_handle_event(&e);
                if (e.type == SDL_MOUSEBUTTONDOWN) break;
            } while (SDL_PollEvent(&e));
        }
        nk_sdl_handle_grab();
        nk_input_end(ctx);

        /* a finished build: the game folder has new files */
        int r = job_busy() ? -1 : job_result();
        if (r != last_result) {
            last_result = r;
            if (r >= 0) load_game_ini();
        }

        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        s_win_h = h;
        if (nk_begin(ctx, "main", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_begin(ctx, NK_DYNAMIC, s_row * 1.3f, TAB_COUNT + 1);
            nk_layout_row_push(ctx, 0.32f);
            nk_label_colored(ctx, "JET SET RADIO FUTURE", NK_TEXT_LEFT, C_HEAD);
            for (int i = 0; i < TAB_COUNT; i++) {
                nk_layout_row_push(ctx, 0.17f);
                nk_bool sel = s_tab == i;
                if (nk_selectable_label(ctx, TAB_NAMES[i], NK_TEXT_CENTERED, &sel)) s_tab = i;
            }
            nk_layout_row_end(ctx);
            spacer(ctx, 4);

            float body = (float)h - s_row * 1.3f - s_row - 64;
            nk_layout_row_dynamic(ctx, body, 1);
            if (nk_group_begin(ctx, "body", NK_WINDOW_NO_SCROLLBAR)) {
                float inner = body - 20;
                switch (s_tab) {
                case TAB_PLAY:     tab_play(ctx); break;
                case TAB_SETTINGS: tab_settings(ctx, inner); break;
                case TAB_BUILD:    tab_build(ctx, inner); break;
                case TAB_UPDATE:   tab_update(ctx, inner); break;
                }
                nk_group_end(ctx);
            }

            nk_layout_row_dynamic(ctx, s_row, 1);
            const char *st = job_busy() ? job_status() : s_message[0] ? s_message : job_status();
            nk_label_colored(ctx, st, NK_TEXT_LEFT, job_busy() ? C_HEAD : C_DIM);
        }
        nk_end(ctx);

        SDL_SetRenderDrawColor(ren, 28, 28, 34, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_RenderPresent(ren);
    }
    if (job_busy()) {
        job_cancel();
        while (job_busy()) SDL_Delay(20);
    }
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    return main(__argc, __argv);
}
#endif
