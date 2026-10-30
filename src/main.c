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

static void start_build(void)
{
    if (job_start(JOB_BUILD, s_regen)) { s_tab = TAB_BUILD; s_regen = false; }
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
    nk_label_colored(ctx, dirty ? "Not saved yet." : "Ticked = on. Steam launch options override this file.",
                     NK_TEXT_RIGHT, dirty ? C_HEAD : C_DIM);
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, h - s_row * 1.2f - 12, 1);
    if (nk_group_begin(ctx, "ini", 0)) {
        float width = nk_window_get_content_region(ctx).w;
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
    nk_checkbox_label(ctx, "Translate the game's code again (normally only after an update)", &rg);
    s_regen = rg != 0;
    used += s_row + 4;

    bool busy = job_busy();
    nk_layout_row_dynamic(ctx, s_row * 1.4f, 4);
    if (button(ctx, "Build", !busy && have_xbe() && g_paths.have_source)) start_build();
    if (button(ctx, "Download tools", !busy && !have_tools())) job_start(JOB_TOOLS, false);
    if (button(ctx, "Cancel", busy)) job_cancel();
    if (button(ctx, "Clear log", !busy)) log_clear();
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
            nk_label_colored(ctx, "Updating keeps your game files, jsrf.ini and saves, and builds the new version.",
                             NK_TEXT_LEFT, C_DIM);
        used += 2 * (s_row + 4);
        bool busy = job_busy();
        nk_layout_row_dynamic(ctx, s_row * 1.4f, 4);
        if (button(ctx, "Check for updates", !busy)) job_start(JOB_CHECK, false);
        if (button(ctx, g_paths.have_source ? "Update" : "Download", !busy)) {
            job_start(JOB_UPDATE, false);
        }
        if (button(ctx, "Cancel", busy)) job_cancel();
        if (button(ctx, "Clear log", !busy)) log_clear();
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

/* jsrf_launcher --tools | --build [--regenerate] | --update | --check:
 * the job without the window, its log printed */
static int headless(int job, bool regen)
{
    log_init();
    paths_init();
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
        else if (!strcmp(argv[i], "--build")) job = JOB_BUILD;
        else if (!strcmp(argv[i], "--update")) job = JOB_UPDATE;
        else if (!strcmp(argv[i], "--check")) job = JOB_CHECK;
        else if (!strcmp(argv[i], "--regenerate")) regen = true;
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
