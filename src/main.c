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
/* v1q: every clickable widget reports its box (third_party/nuklear/nuklear.h,
 * marked "jsrf launcher"), for moving between them with a controller */
static void focus_hook(float x, float y, float w, float h, int kind, float frac, float step);
#define NK_FOCUS_HOOK(r, kind, frac, step) focus_hook((r).x, (r).y, (r).w, (r).h, kind, frac, step)
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
#include "settings_other.h"   /* v2n: Settings -> Other settings */
#include "util.h"
#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { TAB_PLAY, TAB_SETTINGS, TAB_BUILD, TAB_UPDATE, TAB_LAUNCHER, TAB_DEBUG, TAB_COUNT };
static const char *const TAB_NAMES[TAB_COUNT] = { "Play", "Settings", "Build", "Update", "Launcher", "Debug" };

static int s_tab = TAB_PLAY;
static int s_win_h = 800;             /* the window's height, for the drop-down lists */
static Ini s_game_ini;
static char s_game_dir_edit[LPATH];
static bool s_regen;
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
    req_refresh();
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
/* a green check, a red X, or a small spinner while it is being made */
enum { M_NO, M_OK, M_WORKING };
static void mark(struct nk_context *ctx, int m)
{
    struct nk_rect r;
    if (!nk_widget(&r, ctx)) return;
    struct nk_command_buffer *cb = nk_window_get_canvas(ctx);
    float sz = r.h * 0.55f, cx = r.x + sz * 0.7f, cy = r.y + r.h / 2;
    if (m == M_WORKING) {
        float a = (float)(SDL_GetTicks() % 1000) / 1000.0f * 6.2831853f;
        nk_stroke_arc(cb, cx, cy, sz * 0.45f, a, a + 4.2f, 3.0f, C_HEAD);
    } else if (m == M_OK) {
        nk_stroke_line(cb, cx - sz * 0.45f, cy, cx - sz * 0.1f, cy + sz * 0.35f, 3.0f, C_OK);
        nk_stroke_line(cb, cx - sz * 0.1f, cy + sz * 0.35f, cx + sz * 0.5f, cy - sz * 0.4f, 3.0f, C_OK);
    } else {
        nk_stroke_line(cb, cx - sz * 0.4f, cy - sz * 0.4f, cx + sz * 0.4f, cy + sz * 0.4f, 3.0f, C_BAD);
        nk_stroke_line(cb, cx - sz * 0.4f, cy + sz * 0.4f, cx + sz * 0.4f, cy - sz * 0.4f, 3.0f, C_BAD);
    }
}

/* One requirement: the mark, its name, what it is. When missing, the line
 * is a link to where it is fixed: true when clicked. */
static bool req_line_m(struct nk_context *ctx, const char *what, int m, const char *yes, const char *no);
static bool req_line(struct nk_context *ctx, const char *what, bool ok, const char *yes, const char *no)
{
    return req_line_m(ctx, what, ok ? M_OK : M_NO, yes, no);
}

/* m: M_OK shows yes, M_NO no as a link, M_WORKING yes in yellow */
static bool req_line_m(struct nk_context *ctx, const char *what, int m, const char *yes, const char *no)
{
    bool ok = m != M_NO;
    /* the whole line is the link: the X, the name and the text */
    bool clicked = false;
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
    nk_layout_row_push(ctx, 0.04f);
    if (!ok && nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT)) clicked = true;
    mark(ctx, m);
    nk_layout_row_push(ctx, 0.24f);
    if (!ok && nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT)) clicked = true;
    nk_label(ctx, what, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.72f);
    if (ok) {
        nk_label_colored(ctx, yes, NK_TEXT_LEFT, m == M_WORKING ? C_HEAD : C_DIM);
    } else {
        bool hover = nk_widget_is_hovered(ctx);
        if (nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT)) clicked = true;
        char t[300];
        snprintf(t, sizeof t, "%s  >", no);
        nk_label_colored(ctx, t, NK_TEXT_LEFT, hover ? C_HEAD : C_BAD);
    }
    nk_layout_row_end(ctx);
    return clicked;
}

static void choose_game_folder(void)
{
    char d[LPATH];
    if (job_busy()) return;
    if (pick_folder(g_paths.game, d)) {
        set_game_dir(d);
        snprintf(s_message, sizeof s_message, "Game folder set to %s", d);
    }
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

/* Play's Build: generate (if needed) and build (if needed); v2i: it stays
 * on Play, where the progress shows (Miggas) */
static void start_build(void)
{
    job_start(JOB_ALL, false);
}

/* v2i: the running step and how far it is. A step whose length is not known
 * (a download, a check) gets a bar that just moves. */
static float progress_bar(struct nk_context *ctx)
{
    if (!job_busy()) return 0;
    float p = log_progress();
    nk_size cur;
    char t[96];
    if (p >= 0) {
        cur = (nk_size)(p * 1000.0f + 0.5f);
        snprintf(t, sizeof t, "%s  %d%%", job_phase(), (int)(p * 100.0f + 0.5f));
    } else {
        cur = (nk_size)((SDL_GetTicks() / 2) % 1000);
        snprintf(t, sizeof t, "%s...", job_phase());
    }
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 2);
    nk_layout_row_push(ctx, 0.36f);
    nk_label(ctx, t, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.64f);
    nk_progress(ctx, &cur, 1000, NK_FIXED);
    nk_layout_row_end(ctx);
    return s_row + 4;
}

/* v1q: controller navigation. The d-pad (or the left stick) moves a
 * highlight between the clickable widgets -- the nearest one in that
 * direction -- and A clicks it; on a slider left / right change the value.
 * B closes an open list, the right stick scrolls, LB / RB change tab, Start
 * plays. The highlight shows while the controller is in use; the mouse works
 * as before. Widgets report their boxes while the frame is built
 * (focus_hook); the next frame's input picks from them. */
typedef struct { float x, y, w, h, frac, step; int kind, popup; nk_uint *offy; } FocusT;
#define FOCUS_MAX 512
static FocusT s_ft[2][FOCUS_MAX];            /* this frame's, last frame's */
static int s_ftn[2], s_ftcur;
static struct nk_context *s_fctx;
static bool s_pad_mode, s_pad_play, s_quit_after_play, s_pad_busy, s_pad_release;
static bool s_have_focus, s_pad_click_now, s_rep_from_stick;
static FocusT s_focus;                       /* the highlighted widget */
static int s_rep_dir = -1, s_stick_dir = -1;  /* a held direction repeats */
static int s_pad_slide;                       /* -1 / +1 for the highlighted slider */
static Uint32 s_rep_next;
static void pad_repeat_start(int dir) { s_rep_dir = dir; s_rep_next = SDL_GetTicks() + 450; }
static void pad_repeat_stop(void) { s_rep_dir = -1; }
static SDL_GameController *s_pads[4];

/* v1s: a scrolling list (the settings): up / down walk through it and it
 * scrolls along; only from its top does up leave it (for the tab buttons).
 * Its widgets out of view don't report themselves, so when none is left in
 * that direction the list scrolls and the move is tried again next frame. */
typedef struct { nk_uint *offy; struct nk_rect clip; float bottom; } ScrollR;
static ScrollR s_sr[2][4];
static int s_srn[2];
static int s_pend_dir = -1, s_pend_n;

/* called inside a group, after its last row */
static void scroll_region_mark(struct nk_context *ctx)
{
    struct nk_panel *l = ctx->current ? ctx->current->layout : NULL;
    if (!l || !l->offset_y || s_srn[s_ftcur] >= 4) return;
    ScrollR *r = &s_sr[s_ftcur][s_srn[s_ftcur]++];
    r->offy = l->offset_y;
    r->clip = l->clip;
    r->bottom = l->at_y + l->row.height;
}

static ScrollR *scroll_region_of(const FocusT *f)
{
    int prev = s_ftcur ^ 1;
    if (!f->offy || f->popup) return NULL;
    for (int i = 0; i < s_srn[prev]; i++)
        if (s_sr[prev][i].offy == f->offy) return &s_sr[prev][i];
    return NULL;
}

static void focus_hook(float x, float y, float w, float h, int kind, float frac, float step)
{
    FocusT *t;
    /* (and not the scroll bars: a list's bar reports its empty part) */
    if (s_ftn[s_ftcur] >= FOCUS_MAX || w < 14 || h < 4) return;
    /* a slider's own +/- buttons and similar come in again inside it: keep one */
    t = &s_ft[s_ftcur][s_ftn[s_ftcur]];
    t->x = x; t->y = y; t->w = w; t->h = h; t->kind = kind; t->frac = frac; t->step = step;
    t->popup = s_fctx && s_fctx->current && s_fctx->current->layout
               && (s_fctx->current->layout->type & NK_PANEL_SET_POPUP);
    t->offy = s_fctx && s_fctx->current && s_fctx->current->layout
              ? s_fctx->current->layout->offset_y : NULL;
    s_ftn[s_ftcur]++;
}

/* last frame's targets, only the open list's if one is open */
static int focus_targets(FocusT **out)
{
    int prev = s_ftcur ^ 1, n = s_ftn[prev], np = 0, i;
    static FocusT tmp[FOCUS_MAX];
    for (i = 0; i < n; i++) if (s_ft[prev][i].popup) tmp[np++] = s_ft[prev][i];
    if (np) { *out = tmp; return np; }
    *out = s_ft[prev];
    return n;
}

static float cxf(const FocusT *t) { return t->x + t->w / 2; }
static float cyf(const FocusT *t) { return t->y + t->h / 2; }

/* keep the highlight on the same widget as the frame changes */
static void focus_follow(int ww, int wh)
{
    FocusT *t;
    int n = focus_targets(&t), i, best = -1;
    float bd = 1e30f;
    float fx = s_have_focus ? cxf(&s_focus) : ww * 0.27f, fy = s_have_focus ? cyf(&s_focus) : wh * 0.7f;
    for (i = 0; i < n; i++) {
        float dx = cxf(&t[i]) - fx, dy = cyf(&t[i]) - fy, d = dx * dx + dy * dy;
        if (s_have_focus && t[i].popup != s_focus.popup) d += 1e8f;   /* into a list that opened */
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) {
        static FocusT before_list;               /* where a list was opened from */
        if (!s_focus.popup && t[best].popup) before_list = s_focus;
        if (s_focus.popup && !t[best].popup && s_have_focus) {
            /* the list closed: back to its header, the nearest to where it was opened */
            float bd2 = 1e30f;
            for (i = 0; i < n; i++) {
                float dx = cxf(&t[i]) - cxf(&before_list), dy = cyf(&t[i]) - cyf(&before_list);
                if (dx * dx + dy * dy < bd2) { bd2 = dx * dx + dy * dy; best = i; }
            }
        }
        s_focus = t[best];
        s_have_focus = true;
    } else s_have_focus = false;
}

/* the nearest widget in a direction (0 left, 1 right, 2 up, 3 down) */
static int focus_best(int dir, FocusT *t, int n, const ScrollR *only);

/* scroll a list by d (clamped); the highlight moves with its widget */
static void scroll_region_by(ScrollR *r, float d)
{
    float maxd = r->bottom - (r->clip.y + r->clip.h);
    if (d > 0 && d > maxd) d = maxd;
    if (d < 0 && -d > (float)*r->offy) d = -(float)*r->offy;
    if (d > -1 && d < 1) return;
    *r->offy = (nk_uint)((float)*r->offy + d);
    s_focus.y -= d;
}

static bool focus_move(int dir)
{
    FocusT *t;
    int n = focus_targets(&t), best;
    ScrollR *r = dir >= 2 ? scroll_region_of(&s_focus) : NULL;
    if (r) {
        best = focus_best(dir, t, n, r);
        if (best >= 0) {
            s_focus = t[best];
            /* all of it in view */
            float top = r->clip.y, bot = r->clip.y + r->clip.h;
            if (s_focus.y + s_focus.h > bot) scroll_region_by(r, s_focus.y + s_focus.h - bot + 6);
            else if (s_focus.y < top) scroll_region_by(r, s_focus.y - top - 6);
            return true;
        }
        if (dir == 3 && r->bottom > r->clip.y + r->clip.h + 1) {
            scroll_region_by(r, s_row * 1.5f);
            s_pend_dir = dir;
            return true;
        }
        if (dir == 2 && *r->offy > 0) {
            scroll_region_by(r, -s_row * 1.5f);
            s_pend_dir = dir;
            return true;
        }
    }
    best = focus_best(dir, t, n, NULL);
    if (best < 0) return false;
    s_focus = t[best];
    return true;
}

/* the nearest widget in a direction; only: just that list's, in view */
static int focus_best(int dir, FocusT *t, int n, const ScrollR *only)
{
    int i, best = -1;
    float bd = 1e30f, fx = cxf(&s_focus), fy = cyf(&s_focus);
    for (i = 0; i < n; i++) {
        if (only && (t[i].offy != only->offy || t[i].y + t[i].h <= only->clip.y + 2
                     || t[i].y >= only->clip.y + only->clip.h - 2)) continue;
        float dx = cxf(&t[i]) - fx, dy = cyf(&t[i]) - fy, along, across;
        switch (dir) {
        case 0: along = -dx; across = dy; break;
        case 1: along = dx;  across = dy; break;
        case 2: along = -dy; across = dx; break;
        default: along = dy; across = dx; break;
        }
        if (along < 4) continue;
        /* overlapping in the other axis counts as straight ahead */
        if (dir >= 2) {
            float ox = NK_MIN(t[i].x + t[i].w, s_focus.x + s_focus.w) - NK_MAX(t[i].x, s_focus.x);
            if (ox > 0) across = 0;
        } else {
            /* left / right stay in the row */
            float oy = NK_MIN(t[i].y + t[i].h, s_focus.y + s_focus.h) - NK_MAX(t[i].y, s_focus.y);
            if (oy <= 0) continue;
            across = 0;
        }
        {
            float d = along + 2.5f * (across < 0 ? -across : across);
            if (d < bd) { bd = d; best = i; }
        }
    }
    return best;
}

/* a click at (x, y): pressed now, released next frame */
static void pad_click(struct nk_context *ctx, float x, float y)
{
    nk_input_motion(ctx, (int)x, (int)y);
    nk_input_button(ctx, NK_BUTTON_LEFT, (int)x, (int)y, 1);
    s_pad_release = true;
    s_pad_click_now = true;
}

static void pad_dir(struct nk_context *ctx, int dir)
{
    s_pad_mode = true;
    if (!s_have_focus) return;
    if (s_focus.kind == 1 && dir < 2) {      /* a slider: a step down or up */
        s_pad_slide = dir ? 1 : -1;          /* taken by pad_slider_* as it is drawn */
        return;
    }
    if (!focus_move(dir) && dir >= 2)        /* nothing further: scroll the page */
        nk_input_scroll(ctx, nk_vec2(0, dir == 2 ? 1.0f : -1.0f));
    nk_input_motion(ctx, (int)cxf(&s_focus), (int)cyf(&s_focus));   /* hover it, as a mouse would */
}

/* sliders that the d-pad can step (Nuklear's own only move when dragged by
 * the knob): when this one is the highlighted widget, a step of it */
static bool pad_slider_here(struct nk_context *ctx)
{
    struct nk_rect b = nk_widget_bounds(ctx);
    float fx = s_focus.x + s_focus.w / 2, fy = s_focus.y + s_focus.h / 2;
    return s_pad_slide && s_have_focus && s_focus.kind == 1
        && fx >= b.x && fx <= b.x + b.w && fy >= b.y && fy <= b.y + b.h;
}
static void pad_slider_int(struct nk_context *ctx, int min, int *v, int max, int step)
{
    if (pad_slider_here(ctx)) {
        *v += s_pad_slide * step;
        if (*v < min) *v = min;
        if (*v > max) *v = max;
        s_pad_slide = 0;
    }
    nk_slider_int(ctx, min, v, max, step);
}
static void pad_slider_float(struct nk_context *ctx, float min, float *v, float max, float step)
{
    if (pad_slider_here(ctx)) {
        *v += (float)s_pad_slide * step;
        if (*v < min) *v = min;
        if (*v > max) *v = max;
        s_pad_slide = 0;
    }
    nk_slider_float(ctx, min, v, max, step);
}

static bool s_checked_start;   /* v1w: the check for updates at start has been made */

/* v1w: the check for updates (at start) doesn't hold up PLAY or Build: Build
 * stops it first */
static bool free_or_checking(void) { return !job_busy() || job_current() == JOB_CHECK; }
static void stop_check(void)
{
    if (job_current() != JOB_CHECK) return;
    job_cancel();
    for (int i = 0; i < 100 && job_busy(); i++) SDL_Delay(20);
}

static void tab_play(struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label(ctx, "Game folder (with default.xbe):", NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
    nk_layout_row_push(ctx, 0.70f);
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
                                                 s_game_dir_edit, sizeof s_game_dir_edit, nk_filter_default);
    nk_layout_row_push(ctx, 0.15f);
    if (button(ctx, "Choose folder", !job_busy())) choose_game_folder();
    nk_layout_row_push(ctx, 0.15f);
    if (nk_button_label(ctx, "Open folder")) {
        make_dirs(g_paths.game);
        open_folder(g_paths.game);
    }
    nk_layout_row_end(ctx);
    if ((ev & NK_EDIT_COMMITED) && strcmp(s_game_dir_edit, g_paths.game)) set_game_dir(s_game_dir_edit);
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label_colored(ctx, "Type the folder and press Enter, choose it, or drop default.xbe onto this window.",
                     NK_TEXT_LEFT, C_DIM);

    spacer(ctx, 10);
    /* in the order they are needed; a missing one links to where it is fixed */
    bool xbe = have_xbe(), exe = have_game_exe(), tools = have_tools();
    char xbe_ok[128];
    {
        const char *rn = xbe ? xbe_release() : NULL;
        snprintf(xbe_ok, sizeof xbe_ok, "%s found%s%s", game_xbe_name(), rn ? ": " : "", rn ? rn : "");
    }
    if (req_line(ctx, "Game files", xbe, xbe_ok, "default.xbe not found here: choose the game folder"))
        choose_game_folder();
    if (req_line(ctx, "Project source", g_paths.have_source, "downloaded", "not downloaded yet: go to Update"))
        s_tab = TAB_UPDATE;
    if (req_line(ctx, "Build tools", tools, "ready", "not downloaded yet: go to Build (about 150 MB)"))
        s_tab = TAB_BUILD;
    {
        /* from the last check (req_refresh), or what the running job is doing */
        bool busy = job_busy();
        const char *ph = job_phase();
        int g = req_gen(), b = req_build();
        if (busy && !strcmp(ph, "Generating code")) {
            req_line_m(ctx, "Generated code", M_WORKING, "being generated...", "");
        } else if (req_line_m(ctx, "Generated code", g == 2 ? M_OK : M_NO, "up to date",
                              g == 1 ? "out of date (the project or default.xbe changed): go to Build"
                                     : "not generated yet: go to Build")) {
            s_tab = TAB_BUILD;
        }
        if (busy && !strcmp(ph, "Building game")) {
            req_line_m(ctx, "Recompiled game", M_WORKING, "being built...", "");
        } else if (req_line_m(ctx, "Recompiled game", b == 2 ? M_OK : M_NO, "up to date",
                              b == 1 ? "older than the code: go to Build" : "not built yet: go to Build")) {
            s_tab = TAB_BUILD;
        }
    }

    /* v2i: Build's progress, here on Play */
    if (job_busy()) {
        spacer(ctx, 4);
        progress_bar(ctx);
    }
    {
        /* v1w: what the check at start found (or is finding); the line is a
         * button to the Update tab */
        const char *r = update_remote(), *ln = launcher_newer();
        bool game_new = g_paths.have_source && *r && strcmp(r, source_id());
        bool checking = job_current() == JOB_CHECK;
        spacer(ctx, 6);
        nk_layout_row_dynamic(ctx, s_row * 1.2f, 1);
        if (game_new || *ln) {
            char t[160];
            if (game_new && *ln) snprintf(t, sizeof t, "Updates available: the game and launcher %s  >", ln + 9);
            else if (*ln) snprintf(t, sizeof t, "Update available: launcher %s  >", ln + 9);
            else snprintf(t, sizeof t, "Update available: the game  >");
            struct nk_style_button keep = ctx->style.button;
            ctx->style.button.text_normal = C_HEAD;
            ctx->style.button.text_hover = C_HEAD;
            ctx->style.button.border_color = C_HEAD;
            ctx->style.button.border = 2.0f;
            if (nk_button_label(ctx, t)) s_tab = TAB_UPDATE;
            ctx->style.button = keep;
        } else if (checking) {
            nk_label_colored(ctx, "Checking for updates...", NK_TEXT_LEFT, C_DIM);
        } else if (s_checked_start && g_paths.have_source && *r) {
            nk_label_colored(ctx, "The game and the launcher are up to date.", NK_TEXT_LEFT, C_DIM);
        } else {
            nk_spacing(ctx, 1);
        }
    }

    spacer(ctx, 10);
    nk_layout_row_dynamic(ctx, s_row * 1.8f, 2);
    if (button(ctx, "PLAY", exe && free_or_checking()) || (s_pad_play && exe && free_or_checking())) {
        if (!play()) snprintf(s_message, sizeof s_message, "Could not start the game.");
        else {
            snprintf(s_message, sizeof s_message, "Game started.");
            /* v1p: the launcher closes behind it; v2i: unless set to stay */
            s_quit_after_play = close_on_play_pref();
        }
    }
    if (button(ctx, exe ? "Build again" : "Build", xbe && g_paths.have_source && free_or_checking())) {
        stop_check();
        start_build();
    }
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
 * jsrf.ini lines it stands for; "Other settings" is every other setting (settings_other.h) ---- */
enum { SET_GAMEPLAY, SET_VIDEO, SET_INPUT, SET_AUDIO, SET_DEBUG, SET_ALL, SET_COUNT };
static const char *const SET_NAMES[SET_COUNT] = { "Gameplay", "Video", "Input", "Audio", "Debug", "Other settings" };
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
    /* v2a: the game's language (the game's v271) */
    {
        static const char *const items[] = { "English", "French", "German", "Spanish", "Italian", "Japanese" };
        static const char *const vals[] = { "", "french", "german", "spanish", "italian", "japanese" };
        int cur = find(vals, 6, get_or("RECOMP_LANGUAGE", ""));
        if (cur < 0) cur = 0;
        int sel = combo(ctx, "Language", items, 6, cur);
        if (sel != cur) { if (sel) put("RECOMP_LANGUAGE", vals[sel], "game"); else off("RECOMP_LANGUAGE"); }
        desc(ctx, w, "The game's text and menus. PAL (Europe and its Sega GT 2002 + JSRF disc): English, French, "
                     "German, Spanish, Italian. Every other release: English or Japanese.", C_DIM);
    }
}

static void panel_video(struct nk_context *ctx, float w)
{
    /* resolution scale: RECOMP_GL_SCALE */
    {
        static const char *const items[] = { "1x (640x480)", "2x (1280x960)", "3x (1920x1440)", "4x (2560x1920)",
                                             "5x (3200x2400)", "6x (3840x2880)" };   /* v2p: 5x, v2q: 6x for 4K (Miggas) */
        enum { NS = sizeof items / sizeof items[0] };
        int cur = atoi(get_or("RECOMP_GL_SCALE", "1"));
        if (cur < 1) cur = 1;
        if (cur > NS) cur = NS;
        int sel = combo(ctx, "Resolution scale", items, NS, cur - 1);
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
        pad_slider_int(ctx, 0, &v, 100, 5);
        nk_layout_row_push(ctx, 0.12f);
        char t[16];
        snprintf(t, sizeof t, v ? "%d%%" : "Off", v);
        nk_label(ctx, t, NK_TEXT_RIGHT);
        nk_layout_row_end(ctx);
        if (v != cur) { char s2[8]; snprintf(s2, sizeof s2, "%d", v); put("RECOMP_RUMBLE", s2, "input"); }
        desc(ctx, w, "Controller vibration, in percent. 0 turns it off.", C_DIM);
    }
    /* v1v: the modern camera (the game's v265) */
    spacer(ctx, 6);
    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_label_colored(ctx, "Camera", NK_TEXT_LEFT, C_HEAD);
    {
        bool on = strcmp(get_or("RECOMP_CAM_ORBIT", "1"), "0") != 0;
        bool n = check(ctx, "Modern 3rd person camera", on);
        if (n != on) { if (n) off("RECOMP_CAM_ORBIT"); else put("RECOMP_CAM_ORBIT", "0", "input"); }
        desc(ctx, w, "The right stick turns the camera around the skater. While skating it swings back "
                     "behind a second after you let go; standing still it stays until you move. "
                     "Off: the original camera.", C_DIM);
        if (!n) nk_widget_disable_begin(ctx);
        {
            bool y = !strcmp(get_or("RECOMP_CAM_INVERT_Y", "0"), "1");
            bool ny = check(ctx, "Camera invert Up <> Down", y);
            if (ny != y) { if (ny) put("RECOMP_CAM_INVERT_Y", "1", "input"); else off("RECOMP_CAM_INVERT_Y"); }
            bool x = !strcmp(get_or("RECOMP_CAM_INVERT_X", "0"), "1");
            bool nx = check(ctx, "Camera invert Left <> Right", x);
            if (nx != x) { if (nx) put("RECOMP_CAM_INVERT_X", "1", "input"); else off("RECOMP_CAM_INVERT_X"); }
        }
        {
            float cur = (float)atof(get_or("RECOMP_CAM_SENS", "1.0")), v;
            if (!(cur >= 0.3f)) cur = cur > 0.0f ? 0.3f : 1.0f;
            if (cur > 2.0f) cur = 2.0f;
            v = cur;
            nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 3);
            nk_layout_row_push(ctx, 0.34f);
            nk_label(ctx, "Camera sensitivity", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 0.54f);
            pad_slider_float(ctx, 0.3f, &v, 2.0f, 0.1f);
            nk_layout_row_push(ctx, 0.12f);
            v = (float)(int)(v * 10.0f + 0.5f) / 10.0f;
            char t[16];
            snprintf(t, sizeof t, "%.1f", v);
            nk_label(ctx, t, NK_TEXT_RIGHT);
            nk_layout_row_end(ctx);
            if (v < cur - 0.01f || v > cur + 0.01f) {
                if (v > 0.99f && v < 1.01f) off("RECOMP_CAM_SENS");
                else put("RECOMP_CAM_SENS", t, "input");
            }
        }
        if (!n) nk_widget_disable_end(ctx);
        desc(ctx, w, "These settings are only for the modern 3rd person camera.", C_DIM);
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
    pad_slider_float(ctx, 0.0f, &v, 1.5f, 0.05f);
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
#ifdef _WIN32
    desc(ctx, w, "Writes stderr.txt in the game folder, for bug reports, with the debug file (jsrf_recomp.pdb) "
         "so it can name the game's functions. Turns quiet mode off, so the game runs a little slower.", C_DIM);
    if (logging && !ini_dirty(gi()) && debug_files_missing())
        desc(ctx, w, "jsrf_recomp.pdb is missing: the log can't name the game's functions. Build again to restore it.", C_BAD);
#else
    desc(ctx, w, "Writes stderr.txt in the game folder, for bug reports. Turns quiet mode off, so the game runs a little slower.", C_DIM);
#endif
    bool np = check(ctx, "Performance logs", prof);
    desc(ctx, w, "Slows the game down. Adds where the time goes to the log every 10 seconds. Turns Logging on.", C_BAD);
    if (np && !prof) nl = true;            /* needs the log */
    if (!nl && logging) np = false;        /* no log, nothing to write it to */
    if (nl != logging) {
        if (nl) { put("RECOMP_LOG", "stderr.txt", "files"); off("RECOMP_QUIET"); }
        else { off("RECOMP_LOG"); put("RECOMP_QUIET", "1", "game"); }
    }
    if (np != prof) { if (np) put("RECOMP_PROFILE", "1", "files"); else off("RECOMP_PROFILE"); }
    /* v2k: chapter select (the game's v276, RECOMP_CHAPTER_SELECT=<chapter>[:<mission>]):
     * each chapter's opening (mission 96, the default) and its Garage
     * (mission 0; chapter 5's is 10) */
    {
        static const char *const items[] = { "Off",
            "Chapter 1: opening", "Chapter 1: Garage", "Chapter 2: opening", "Chapter 2: Garage",
            "Chapter 3: opening", "Chapter 3: Garage", "Chapter 4: opening", "Chapter 4: Garage",
            "Chapter 5: opening", "Chapter 5: Garage", "Chapter 6: opening", "Chapter 6: Garage",
            "Chapter 7: opening", "Chapter 7: Garage", "Chapter 8: opening", "Chapter 8: Garage" };
        static const char *const vals[] = { "",
            "1", "1:0", "2", "2:0", "3", "3:0", "4", "4:0",
            "5", "5:10", "6", "6:0", "7", "7:0", "8", "8:0" };
        enum { N = sizeof vals / sizeof vals[0] };
        const char *v = ini_get(gi(), "RECOMP_CHAPTER_SELECT");
        int cur = (v && *v) ? find(vals, N, v) : 0;
        if (cur < 0) cur = 0;   /* a value typed by hand: shown as Off until changed */
        int sel = combo(ctx, "Chapter select", items, N, cur);
        if (sel != cur) { if (sel) put("RECOMP_CHAPTER_SELECT", vals[sel], "tools"); else off("RECOMP_CHAPTER_SELECT"); }
        desc(ctx, w, "New game starts from selected chapter", C_DIM);
        desc(ctx, w, "The chapter's earlier events are skipped, and the game's next save point saves the new "
                     "chapter over your progress: back up the UDATA folder first.", C_BAD);
    }
}

/* every line of jsrf.ini, ticked = on */
/* v2n: Other settings (Miggas): every setting the game reads that the tabs
 * above don't show (settings_other.h), each with what it does. Ticked =
 * the line is on in jsrf.ini; the value is edited in place. */
static int line_of(const char *key, bool on_only)
{
    int first = -1;
    for (int i = 0; i < s_game_ini.n; i++) {
        IniLine *L = &s_game_ini.l[i];
        if (L->kind != L_SETTING || strcmp(L->key, key)) continue;
        if (L->on) return i;
        if (first < 0) first = i;
    }
    return on_only ? -1 : first;
}

static void panel_all(struct nk_context *ctx, float width)
{
    static char vals[OTHER_SETTINGS_N][256];
    static bool init[OTHER_SETTINGS_N];
    nk_layout_row_dynamic(ctx, (s_row - 8) * 2, 1);
    nk_label_colored_wrap(ctx, "Changes in here could break the game, proceed at your own risk.", C_BAD);
    desc(ctx, width, "Every other setting the game reads. Ticked = on; the value next to it is what it's set to.", C_DIM);
    for (int k = 0; k < OTHER_SETTINGS_N; k++) {
        const OtherSetting *o = &OTHER_SETTINGS[k];
        if (!o->key) {
            spacer(ctx, 6);
            nk_layout_row_dynamic(ctx, s_row, 1);
            nk_label_colored(ctx, o->text, NK_TEXT_LEFT, C_HEAD);
            continue;
        }
        int li = line_of(o->key, false);
        IniLine *L = li >= 0 ? &s_game_ini.l[li] : NULL;
        bool on = L && L->on;
        char *val;
        size_t cap;
        if (L) { val = L->val; cap = sizeof L->val; }      /* the line's own value, on or not */
        else {
            if (!init[k]) { snprintf(vals[k], sizeof vals[k], "%s", o->val); init[k] = true; }
            val = vals[k]; cap = sizeof vals[k];
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, s_row, 2);
        nk_layout_row_push(ctx, 0.55f);
        nk_bool t = on;
        bool toggled = nk_checkbox_label(ctx, o->key, &t);
        nk_layout_row_push(ctx, 0.45f);
        char before[256];
        snprintf(before, sizeof before, "%s", val);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, val, (int)cap, nk_filter_default);
        bool edited = strcmp(before, val) != 0;
        nk_layout_row_end(ctx);
        if (toggled) {
            if (t) { if (L) ini_set_on(&s_game_ini, li, true); else put(o->key, val, "other"); }
            else off(o->key);
        } else if (edited && !on) {
            if (L) ini_set_on(&s_game_ini, li, true); else put(o->key, val, "other");
        }
        desc(ctx, width, o->text, C_DIM);
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
        if (ini_save(&s_game_ini)) {
            /* Logging on: the debug file goes next to the game; off: away */
            if (debug_files_sync() < 0)
                snprintf(s_message, sizeof s_message, "Saved. jsrf_recomp.pdb is missing: build again to restore it.");
            else
                snprintf(s_message, sizeof s_message, "Saved %s", p);
        } else {
            snprintf(s_message, sizeof s_message, "Could not save %s", p);
        }
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
    /* v2i: Other settings (was All settings) only with the launcher's debug menu on (Miggas) */
    int nset = debug_menu_pref() ? SET_COUNT : SET_ALL;
    if (s_set_tab >= nset) s_set_tab = SET_GAMEPLAY;
    nk_layout_row_dynamic(ctx, s_row * 1.1f, nset);
    for (int i = 0; i < nset; i++) {
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
        scroll_region_mark(ctx);
        nk_group_end(ctx);
    }
}

/* ---------------------------------------------------------------------- */
/* The log. steps: only the steps (the "== " lines and each job's start),
 * for the normal tabs; the Debug tab shows everything. */
static void log_view(struct nk_context *ctx, float h, bool steps)
{
    static const char **pick;
    static int pick_cap;
    nk_layout_row_dynamic(ctx, h, 1);
    const char **lines;
    int n = log_lock(&lines);
    if (steps) {
        if (pick_cap < n) { pick_cap = n + 64; pick = realloc(pick, sizeof(char *) * pick_cap); }
        int m = 0;
        for (int i = 0; i < n; i++)
            if (!strncmp(lines[i], "== ", 3) || !strncmp(lines[i], "---- ", 5)) pick[m++] = lines[i];
        lines = pick;
        n = m;
    }
    const char *id = steps ? "steps" : "log";
    int rh = (int)(s_row - 10);
    struct nk_list_view view;
    if (nk_list_view_begin(ctx, &view, id, 0, rh, n)) {
        nk_layout_row_dynamic(ctx, (float)rh, 1);
        for (int i = view.begin; i < view.end && i < n; i++) {
            const char *l = lines[i];
            bool head = !strncmp(l, "== ", 3);
            bool err = strstr(l, "error") || strstr(l, "Failed") || strstr(l, "FAILED");
            nk_label_colored(ctx, head ? l + 3 : l, NK_TEXT_LEFT,
                             err ? C_BAD : head ? C_HEAD : (steps ? C_DIM : nk_rgb(215, 215, 220)));
        }
        nk_list_view_end(&view);
    }
    log_unlock();
    /* new output: follow it */
    static unsigned seen[2];
    if (log_serial() != seen[steps]) {
        seen[steps] = log_serial();
        nk_group_set_scroll(ctx, id, 0, (nk_uint)(n * (rh + (int)ctx->style.window.spacing.y)));
    }
}

/* launcher_log.txt in the program that opens text files */
static void open_log_in_editor(void)
{
    char p[LPATH];
    pjoin(p, g_paths.base, "launcher_log.txt");
    open_folder(p);
}

/* after a failed job: where the details are */
static float failed_hint(struct nk_context *ctx)
{
    if (job_busy() || job_result() != 0) return 0;
    nk_layout_row_begin(ctx, NK_DYNAMIC, s_row * 1.2f, 2);
    nk_layout_row_push(ctx, 0.70f);
    nk_label_colored(ctx, "It failed: the details are in the log file.", NK_TEXT_LEFT, C_BAD);
    nk_layout_row_push(ctx, 0.30f);
    if (nk_button_label(ctx, "Open log file")) open_log_in_editor();
    nk_layout_row_end(ctx);
    return s_row * 1.2f + 4;
}

static void tab_build(struct nk_context *ctx, float h)
{
    float used = 0;
    bool busy = job_busy();
    bool ready = !busy && have_xbe() && g_paths.have_source;
    nk_layout_row_dynamic(ctx, s_row * 1.4f, 3);
    if (button(ctx, "Generate code", ready)) job_start(JOB_GENERATE, s_regen);
    if (button(ctx, "Build", ready)) job_start(JOB_BUILD, s_regen);
    if (button(ctx, "Cancel", busy)) job_cancel();
    used += s_row * 1.4f + 4;
    used += progress_bar(ctx);
    nk_layout_row_dynamic(ctx, s_row, 1);
    if (!have_xbe())
        nk_label_colored(ctx, "Choose the game folder (with default.xbe) in Play first.", NK_TEXT_LEFT, C_BAD);
    else if (!g_paths.have_source)
        nk_label_colored(ctx, "Download the project in Update first.", NK_TEXT_LEFT, C_BAD);
    else
        nk_label_colored(ctx, "Each button checks first and skips what is already up to date. "
                         "The first time also downloads the tools (about 150 MB).", NK_TEXT_LEFT, C_DIM);
    used += s_row + 4;
    used += failed_hint(ctx);
    log_view(ctx, h - used - 8, true);
}

/* v2i: the launcher's own settings (launcher.ini), saved at once (Miggas) */
static void tab_launcher(struct nk_context *ctx, float h)
{
    (void)h;
    float w = nk_window_get_content_region(ctx).w;
    {
        bool on = close_on_play_pref();
        bool n = check(ctx, "Close the launcher when the game starts", on);
        if (n != on) launcher_pref_set("CLOSE_ON_PLAY", n ? "1" : "0");
        desc(ctx, w, "Off: the launcher stays open while you play.", C_DIM);
    }
    {
        static const char *const items[] = { "main (tested releases)", "test (newest, less tested)" };
        static const char *const vals[] = { "main", "test" };
        int cur = !strcmp(update_branch(), "test") ? 1 : 0;
        int sel = combo(ctx, "Project branch", items, 2, cur);
        if (sel != cur && !job_busy()) {
            launcher_pref_set("BRANCH", vals[sel]);
            job_start(JOB_CHECK, false);      /* what that branch has now */
        }
        desc(ctx, w, "Which version of the project Update downloads. After changing it, Update and then Build.", C_DIM);
    }
    {
        bool on = debug_menu_pref();
        bool n = check(ctx, "Debug", on);
        if (n != on) launcher_pref_set("DEBUG_MENU", n ? "1" : "0");
        desc(ctx, w, "Shows the Debug tab (the tools, the checks, the whole log) and Settings > Other settings "
                     "(every other setting the game reads).", C_DIM);
    }
}

static void tab_update(struct nk_context *ctx, float h)
{
    float used = 0;
    nk_layout_row_dynamic(ctx, s_row, 1);
    char t[600];
    const char *id = source_id();
    if (!g_paths.have_source) snprintf(t, sizeof t, "The project is not downloaded yet.");
    else if (*id) snprintf(t, sizeof t, "Project version: %.10s", id);
    else snprintf(t, sizeof t, "Project version: unknown");
    size_t tl = strlen(t);
    snprintf(t + tl, sizeof t - tl, "   (branch: %s, set in Launcher)", update_branch());
    nk_label(ctx, t, NK_TEXT_LEFT);
    const char *r = update_remote();
    if (*r && strcmp(r, id))
        nk_label_colored(ctx, "A newer version is available.", NK_TEXT_LEFT, C_HEAD);
    else
        nk_label_colored(ctx, "Update only downloads the project (your game files, jsrf.ini and saves are kept). Then Build.",
                         NK_TEXT_LEFT, C_DIM);
    used += 2 * (s_row + 4);
    bool busy = job_busy();
    nk_layout_row_dynamic(ctx, s_row * 1.4f, 3);
    if (button(ctx, "Check for updates", !busy)) job_start(JOB_CHECK, false);
    if (button(ctx, g_paths.have_source ? "Update" : "Download", !busy)) job_start(JOB_UPDATE, false);
    if (button(ctx, "Cancel", busy)) job_cancel();
    used += s_row * 1.4f + 4;
    used += progress_bar(ctx);
    {
        /* v1r: the launcher itself */
        const char *ln = launcher_newer();
        char lt[160];
        nk_layout_row_begin(ctx, NK_DYNAMIC, s_row * 1.4f, 2);
        nk_layout_row_push(ctx, 0.66f);
        if (*ln) {
            snprintf(lt, sizeof lt, "Launcher %s is available (this one is %s).", ln + 9, LAUNCHER_VERSION);
            nk_label_colored(ctx, lt, NK_TEXT_LEFT, C_HEAD);
        } else {
            snprintf(lt, sizeof lt, "Launcher %s. Check for updates also looks for a newer launcher.", LAUNCHER_VERSION);
            nk_label_colored(ctx, lt, NK_TEXT_LEFT, C_DIM);
        }
        nk_layout_row_push(ctx, 0.34f);
        if (button(ctx, "Update launcher", *ln && !busy)) job_start(JOB_SELF, false);
        nk_layout_row_end(ctx);
        used += s_row * 1.4f + 4;
    }
    used += failed_hint(ctx);
    log_view(ctx, h - used - 8, true);
}

/* the debug menu (DEBUG_MENU=1 in launcher.ini): the tools, the checks, the
 * helpers for a running game, and the whole log */
static Uint32 s_trace_until;     /* trace.flag goes again at this time */

static void flag_file(const char *name)
{
    char p[LPATH];
    pjoin(p, g_paths.game, name);
    write_file(p, "", 0);
}

static void tab_debug(struct nk_context *ctx, float h)
{
    float used = 0;
    bool busy = job_busy();
    bool src = !busy && g_paths.have_source;

    nk_layout_row_dynamic(ctx, s_row, 1);
    nk_bool rg = s_regen;
    nk_checkbox_label(ctx, "Force Generate/Build", &rg);
    s_regen = rg != 0;
    used += s_row + 4;

    nk_layout_row_dynamic(ctx, s_row * 1.3f, 4);
    if (button(ctx, "Download tools", !busy && !have_tools())) job_start(JOB_TOOLS, false);
    if (button(ctx, "Check generated code", src)) job_start_tool(TOOL_CHECK_GEN);
    if (button(ctx, "Check default.xbe", src && have_xbe())) job_start_tool(TOOL_CHECK_XBE);
    if (button(ctx, "Compare cache", src)) job_start_tool(TOOL_COMPARE_CACHE);
    used += s_row * 1.3f + 4;

    bool exe = have_game_exe();
    nk_layout_row_dynamic(ctx, s_row * 1.3f, 3);
    if (button(ctx, "Play, stop after 30 s", exe && !busy)) {
        if (play_watchdog(30)) snprintf(s_message, sizeof s_message, "Game started: it stops after 30 s, with a dump in stderr.txt.");
    }
    if (button(ctx, "Sample a hang", exe)) {
        flag_file("sample.flag");
        snprintf(s_message, sizeof s_message, "Sampling the game: [SAMPLE] lines in stderr.txt in a few seconds.");
    }
    if (button(ctx, "Trace one frame", exe)) {
        flag_file("trace.flag");
        s_trace_until = SDL_GetTicks() + 5000;
        snprintf(s_message, sizeof s_message, "Tracing one frame: [FTRACE] lines in stderr.txt.");
    }
    used += s_row * 1.3f + 4;
    nk_layout_row_dynamic(ctx, s_row - 6, 1);
    nk_label_colored(ctx, "The last two work on a running game with Logging on (Settings -> Debug).", NK_TEXT_LEFT, C_DIM);
    used += s_row - 2;

    nk_layout_row_dynamic(ctx, s_row * 1.2f, 3);
    if (button(ctx, "Cancel", busy)) job_cancel();
    if (button(ctx, "Clear log", !busy)) log_clear();
    if (button(ctx, "Open log file", true)) open_log_in_editor();
    used += s_row * 1.2f + 4;
    log_view(ctx, h - used - 8, false);
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
        else if (!strcmp(argv[i], "--self-update")) job = JOB_SELF;   /* v1r */
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
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    log_init();
    paths_init();
    open_log_file();
    launcher_cleanup_old();                  /* v1r: what a self-update left */
    snprintf(s_game_dir_edit, sizeof s_game_dir_edit, "%s", g_paths.game);
    load_game_ini();
    req_refresh();

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
    s_fctx = ctx;
    load_font(ctx, 20);
    style(ctx);

    logf_("Launcher folder: %s", g_paths.base);
    logf_("Project: %s%s", g_paths.source, g_paths.have_source ? "" : " (not downloaded yet)");
    logf_("Game folder: %s", g_paths.game);
    /* v1w: look for a newer game and launcher at once (no answer: nothing shown) */
    if (job_start(JOB_CHECK, false)) s_checked_start = true;

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
        else got = SDL_WaitEventTimeout(&e, s_pad_busy ? 16 : job_busy() ? 33 : 250);   /* idle, but follow a job's log */
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
                else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                    for (int k = 0; k < 4; k++)
                        if (!s_pads[k]) { s_pads[k] = SDL_GameControllerOpen(e.cdevice.which); break; }
                } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                    for (int k = 0; k < 4; k++)
                        if (s_pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[k]))
                                         == e.cdevice.which) {
                            SDL_GameControllerClose(s_pads[k]);
                            s_pads[k] = NULL;
                        }
                } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    int ntab = debug_menu_pref() ? TAB_COUNT : TAB_DEBUG;
                    s_pad_mode = true;
                    switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  pad_dir(ctx, 0); pad_repeat_start(0); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: pad_dir(ctx, 1); pad_repeat_start(1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    pad_dir(ctx, 2); pad_repeat_start(2); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  pad_dir(ctx, 3); pad_repeat_start(3); break;
                    case SDL_CONTROLLER_BUTTON_A:
                        if (s_have_focus && s_focus.kind != 1) pad_click(ctx, cxf(&s_focus), cyf(&s_focus));
                        break;
                    case SDL_CONTROLLER_BUTTON_B: {
                        FocusT *t;
                        int n = focus_targets(&t);
                        if (n && t[0].popup) pad_click(ctx, 2, 2);   /* outside: the list closes */
                        break;
                    }
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  s_tab = (s_tab + ntab - 1) % ntab; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: s_tab = (s_tab + 1) % ntab; break;
                    case SDL_CONTROLLER_BUTTON_START: s_tab = TAB_PLAY; s_pad_play = true; break;
                    default: break;
                    }
                    break;                   /* a press is its own frame */
                } else if (e.type == SDL_CONTROLLERBUTTONUP) {
                    pad_repeat_stop();
                } else {
                    if (e.type == SDL_MOUSEMOTION) s_pad_mode = false;   /* the mouse again */
                    nk_sdl_handle_event(&e);
                }
                if (e.type == SDL_MOUSEBUTTONDOWN) break;
            } while (SDL_PollEvent(&e));
        }
        /* v1q: the release of a controller click; the left stick as a
         * d-pad; held directions repeat; the right stick scrolls */
        if (s_pad_release && !s_pad_click_now) {
            nk_input_button(ctx, NK_BUTTON_LEFT, ctx->input.mouse.pos.x, ctx->input.mouse.pos.y, 0);
            s_pad_release = false;
        }
        s_pad_click_now = false;
        {
            Uint32 now = SDL_GetTicks();
            int stick = -1;
            float sy = 0;
            for (int k = 0; k < 4; k++) {
                SDL_GameController *g = s_pads[k];
                if (!g) continue;
                int lx = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX);
                int ly = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
                int ry = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_RIGHTY);
                if (abs(lx) > 20000 || abs(ly) > 20000)
                    stick = abs(lx) > abs(ly) ? (lx < 0 ? 0 : 1) : (ly < 0 ? 2 : 3);
                if (abs(ry) > 7000) sy = (ry > 0 ? ry - 7000 : ry + 7000) / 25767.0f;
            }
            if (stick != s_stick_dir) {
                s_stick_dir = stick;
                if (stick >= 0) { pad_dir(ctx, stick); pad_repeat_start(stick); }
                else if (s_rep_from_stick) pad_repeat_stop();
                s_rep_from_stick = stick >= 0;
            } else if (s_rep_dir >= 0 && SDL_TICKS_PASSED(now, s_rep_next)) {
                pad_dir(ctx, s_rep_dir);
                s_rep_next = now + 110;
            }
            if (sy) { nk_input_scroll(ctx, nk_vec2(0, -sy * 0.5f)); s_pad_mode = true; }
            /* v1s: a list scrolled to show more: the move again */
            if (s_pend_dir >= 0 && s_pend_n++ < 12) { int d = s_pend_dir; s_pend_dir = -1; pad_dir(ctx, d); }
            else { s_pend_dir = -1; s_pend_n = 0; }
            s_pad_busy = s_rep_dir >= 0 || sy != 0 || s_pad_release || s_pend_dir >= 0;
        }
        nk_sdl_handle_grab();
        nk_input_end(ctx);

        /* a finished build: the game folder has new files */
        int r = job_busy() ? -1 : job_result();
        if (r != last_result) {
            last_result = r;
            if (r >= 0) { load_game_ini(); paths_refresh_source(); req_refresh(); }
        }

        if (s_trace_until && SDL_TICKS_PASSED(SDL_GetTicks(), s_trace_until)) {
            /* a later Trace one frame can trace another moment */
            char p[LPATH];
            pjoin(p, g_paths.game, "trace.flag");
            remove_file(p);
            s_trace_until = 0;
        }

        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        s_win_h = h;
        if (nk_begin(ctx, "main", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
            int ntab = debug_menu_pref() ? TAB_COUNT : TAB_DEBUG;
            if (s_tab >= ntab) s_tab = TAB_PLAY;
            nk_layout_row_begin(ctx, NK_DYNAMIC, s_row * 1.3f, ntab + 1);
            nk_layout_row_push(ctx, 0.32f);
            nk_label_colored(ctx, "JET SET RADIO FUTURE", NK_TEXT_LEFT, C_HEAD);
            for (int i = 0; i < ntab; i++) {
                nk_layout_row_push(ctx, 0.68f / ntab);
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
                case TAB_LAUNCHER: tab_launcher(ctx, inner); break;
                case TAB_DEBUG:    tab_debug(ctx, inner); break;
                }
                nk_group_end(ctx);
            }

            if (job_busy()) {
                /* a turning arc while a job runs, the step, then the detail */
                nk_layout_row_begin(ctx, NK_STATIC, s_row, 2);
                nk_layout_row_push(ctx, s_row);
                struct nk_rect r;
                if (nk_widget(&r, ctx)) {
                    struct nk_command_buffer *cb = nk_window_get_canvas(ctx);
                    float a = (float)(SDL_GetTicks() % 1000) / 1000.0f * 6.2831853f;
                    float rad = r.h * 0.32f;
                    nk_stroke_arc(cb, r.x + r.w / 2, r.y + r.h / 2, rad, 0, 6.2831853f, 3.0f, nk_rgb(60, 60, 72));
                    nk_stroke_arc(cb, r.x + r.w / 2, r.y + r.h / 2, rad, a, a + 4.2f, 3.0f, C_HEAD);
                }
                nk_layout_row_push(ctx, (float)w - s_row - 48);
                char t[400];
                snprintf(t, sizeof t, "%s: %s", job_phase(), job_status());
                nk_label_colored(ctx, t, NK_TEXT_LEFT, C_HEAD);
                nk_layout_row_end(ctx);
            } else {
                nk_layout_row_dynamic(ctx, s_row, 1);
                nk_label_colored(ctx, s_message[0] ? s_message : job_status(), NK_TEXT_LEFT, C_DIM);
            }
        }
        nk_end(ctx);

        SDL_SetRenderDrawColor(ren, 28, 28, 34, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        if (s_pad_mode && s_have_focus) {
            /* v1q: the highlighted widget, a yellow frame around it */
            SDL_Rect r = { (int)s_focus.x - 3, (int)s_focus.y - 3, (int)s_focus.w + 6, (int)s_focus.h + 6 };
            SDL_SetRenderDrawColor(ren, 245, 200, 40, 255);
            for (int k = 0; k < 3; k++) {
                SDL_RenderDrawRect(ren, &r);
                r.x++; r.y++; r.w -= 2; r.h -= 2;
            }
        }
        SDL_RenderPresent(ren);
        s_pad_play = false;
        s_pad_slide = 0;
        if (s_quit_after_play) running = false;
        if (launcher_restart_wanted() && !job_busy()) {
            /* v1r: the new launcher, started from the same place */
            char me[LPATH];
            if (self_exe(me) && launch(me, g_paths.base)) running = false;
        }
        /* v1q: this frame's widgets become the ones the next input picks from */
        s_ftcur ^= 1;
        s_ftn[s_ftcur] = 0;
        s_srn[s_ftcur] = 0;
        focus_follow(w, h);
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
