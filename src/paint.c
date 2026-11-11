/* The texture painter: see paint.h */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"

#include "paint.h"

enum { T_BRUSH, T_ERASER, T_PICKER, T_FILL, T_N };

typedef struct { int x, y, w, h; uint8_t *before, *after; } Patch;
enum { UNDO_MAX = 64 };

static struct {
    int on, w, h, dirty;
    uint8_t *img, *orig, *before, *mask;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int ux0, uy0, ux1, uy1;                 /* the display's part to redo (x1, y1 past the end) */
    int tool, tol;
    float size, hard, opac;
    struct nk_colorf col;
    float zoom, ox, oy;                     /* the image's top-left in the canvas */
    int fit;
    int stroking;
    float lx, ly;
    int bx0, by0, bx1, by1;                 /* the stroke's box */
    Patch undo[UNDO_MAX], redo[UNDO_MAX];
    int nundo, nredo;
    char status[160];
    int resize_max;                         /* m4c */
    unsigned serial;                        /* m5: changes with the pixels */
    void (*overlay)(struct nk_command_buffer *, float, float, float, float);   /* m5b */
} P;

static void touch(int x0, int y0, int x1, int y1)   /* [x0, x1) x [y0, y1) changed */
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > P.w) x1 = P.w;
    if (y1 > P.h) y1 = P.h;
    if (x0 >= x1 || y0 >= y1) return;
    if (P.ux1 <= P.ux0) { P.ux0 = x0; P.uy0 = y0; P.ux1 = x1; P.uy1 = y1; return; }
    if (x0 < P.ux0) P.ux0 = x0;
    if (y0 < P.uy0) P.uy0 = y0;
    if (x1 > P.ux1) P.ux1 = x1;
    if (y1 > P.uy1) P.uy1 = y1;
}

static void upload(void)                    /* the changed part, over a checkerboard */
{
    int w = P.ux1 - P.ux0, h = P.uy1 - P.uy0, x, y;
    uint8_t *buf;
    SDL_Rect r;
    if (w <= 0 || h <= 0 || !P.tex) return;
    buf = (uint8_t *)malloc((size_t)w * h * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t *p = P.img + ((size_t)(P.uy0 + y) * P.w + P.ux0 + x) * 4;
            uint8_t *o = buf + ((size_t)y * w + x) * 4;
            int bg = (((P.ux0 + x) / 8) ^ ((P.uy0 + y) / 8)) & 1 ? 150 : 100, a = p[3], c;
            for (c = 0; c < 3; c++) o[c] = (uint8_t)((p[c] * a + bg * (255 - a)) / 255);
            o[3] = 255;
        }
    P.serial++;
    r.x = P.ux0; r.y = P.uy0; r.w = w; r.h = h;
    SDL_UpdateTexture(P.tex, &r, buf, w * 4);
    free(buf);
    P.ux0 = P.uy0 = P.ux1 = P.uy1 = 0;
}

static void free_patch(Patch *p) { free(p->before); free(p->after); memset(p, 0, sizeof *p); }

static void clear_stack(Patch *s, int *n)
{
    int i;
    for (i = 0; i < *n; i++) free_patch(&s[i]);
    *n = 0;
}

static void push(Patch *s, int *n, Patch p)
{
    if (*n == UNDO_MAX) {                   /* the oldest goes */
        free_patch(&s[0]);
        memmove(s, s + 1, sizeof(Patch) * (UNDO_MAX - 1));
        (*n)--;
    }
    s[(*n)++] = p;
}

static uint8_t *cut(const uint8_t *src, int x, int y, int w, int h)
{
    uint8_t *o = (uint8_t *)malloc((size_t)w * h * 4);
    int r;
    for (r = 0; r < h; r++) memcpy(o + (size_t)r * w * 4, src + ((size_t)(y + r) * P.w + x) * 4, (size_t)w * 4);
    return o;
}

static void paste(uint8_t *dst, const uint8_t *src, int x, int y, int w, int h)
{
    int r;
    for (r = 0; r < h; r++) memcpy(dst + ((size_t)(y + r) * P.w + x) * 4, src + (size_t)r * w * 4, (size_t)w * 4);
}

/* the change in the box [x0, x1) x [y0, y1) since P.before: an undo step */
static void record(int x0, int y0, int x1, int y1)
{
    Patch p;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > P.w) x1 = P.w;
    if (y1 > P.h) y1 = P.h;
    if (x0 >= x1 || y0 >= y1) return;
    p.x = x0; p.y = y0; p.w = x1 - x0; p.h = y1 - y0;
    p.before = cut(P.before, p.x, p.y, p.w, p.h);
    p.after = cut(P.img, p.x, p.y, p.w, p.h);
    push(P.undo, &P.nundo, p);
    clear_stack(P.redo, &P.nredo);
    P.dirty = 1;
}

static void undo_redo(int redo)
{
    Patch p;
    if (redo ? !P.nredo : !P.nundo) return;
    p = redo ? P.redo[--P.nredo] : P.undo[--P.nundo];
    paste(P.img, redo ? p.after : p.before, p.x, p.y, p.w, p.h);
    touch(p.x, p.y, p.x + p.w, p.y + p.h);
    push(redo ? P.undo : P.redo, redo ? &P.nundo : &P.nredo, p);
    P.dirty = 1;
}

int paint_start(SDL_Renderer *ren, const uint8_t *rgba, int w, int h)
{
    size_t n = (size_t)w * h * 4;
    paint_stop();
    P.img = (uint8_t *)malloc(n);
    P.orig = (uint8_t *)malloc(n);
    P.before = (uint8_t *)malloc(n);
    P.mask = (uint8_t *)malloc((size_t)w * h);
    if (!P.img || !P.orig || !P.before || !P.mask) { paint_stop(); return -1; }
    memcpy(P.img, rgba, n);
    memcpy(P.orig, rgba, n);
    P.w = w;
    P.h = h;
    P.ren = ren;
    P.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!P.tex) { paint_stop(); return -1; }
    SDL_SetTextureScaleMode(P.tex, SDL_ScaleModeNearest);   /* pixels, not a blur, when zoomed in */
    touch(0, 0, w, h);
    upload();
    if (P.size <= 0) {                      /* the tools as they were last time, else these */
        P.size = 8;
        P.hard = 0.8f;
        P.opac = 1.0f;
        P.tol = 32;
        P.col.r = 1; P.col.g = 0.2f; P.col.b = 0.2f; P.col.a = 1;
    }
    P.fit = 1;
    P.on = 1;
    P.dirty = 0;
    P.status[0] = 0;
    P.resize_max = 4096;
    return 0;
}

void paint_stop(void)
{
    clear_stack(P.undo, &P.nundo);
    clear_stack(P.redo, &P.nredo);
    free(P.img); free(P.orig); free(P.before); free(P.mask);
    P.img = P.orig = P.before = P.mask = NULL;
    if (P.tex) SDL_DestroyTexture(P.tex);
    P.tex = NULL;
    P.on = P.dirty = P.stroking = 0;
}

int paint_on(void) { return P.on; }
int paint_dirty(void) { return P.on && P.dirty; }
void paint_saved(void) { P.dirty = 0; }
const uint8_t *paint_image(void) { return P.img; }
void paint_size(int *w, int *h) { *w = P.w; *h = P.h; }
void paint_allow_resize(int max) { P.resize_max = max; }
unsigned paint_serial(void) { return P.serial; }

/* m4c: the picture k times bigger, each pixel a k x k block (integer
 * scaling, nothing blurred); the original's copy too, for the eraser. The
 * history goes: a resize is not undone (Close throws it away). */
static uint8_t *scale_up(const uint8_t *src, int w, int h, int k)
{
    uint8_t *d = (uint8_t *)malloc((size_t)w * k * h * k * 4);
    int x, y, W = w * k;
    if (!d) return NULL;
    for (y = 0; y < h * k; y++)
        for (x = 0; x < W; x++)
            memcpy(d + ((size_t)y * W + x) * 4, src + ((size_t)(y / k) * w + x / k) * 4, 4);
    return d;
}

int paint_resize(int k)
{
    int w = P.w * k, h = P.h * k;
    uint8_t *img, *orig, *before, *mask;
    SDL_Texture *tex;
    if (w > P.resize_max || h > P.resize_max) return -1;
    img = scale_up(P.img, P.w, P.h, k);
    orig = scale_up(P.orig, P.w, P.h, k);
    before = (uint8_t *)malloc((size_t)w * h * 4);
    mask = (uint8_t *)malloc((size_t)w * h);
    tex = SDL_CreateTexture(P.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!img || !orig || !before || !mask || !tex) {
        free(img); free(orig); free(before); free(mask);
        if (tex) SDL_DestroyTexture(tex);
        return -1;
    }
    free(P.img); free(P.orig); free(P.before); free(P.mask);
    SDL_DestroyTexture(P.tex);
    P.img = img; P.orig = orig; P.before = before; P.mask = mask; P.tex = tex;
    SDL_SetTextureScaleMode(P.tex, SDL_ScaleModeNearest);
    P.w = w;
    P.h = h;
    clear_stack(P.undo, &P.nundo);
    clear_stack(P.redo, &P.nredo);
    P.ux0 = P.uy0 = P.ux1 = P.uy1 = 0;
    touch(0, 0, w, h);
    upload();
    P.fit = 1;
    P.dirty = 1;
    return 0;
}

/* ---- the tools ---- */

static void stamp(float cx, float cy)
{
    float r = P.size * 0.5f, inner = r * P.hard;
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    int x, y;
    uint8_t target[4];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > P.w) x1 = P.w;
    if (y1 > P.h) y1 = P.h;
    target[0] = (uint8_t)(P.col.r * 255 + 0.5f);
    target[1] = (uint8_t)(P.col.g * 255 + 0.5f);
    target[2] = (uint8_t)(P.col.b * 255 + 0.5f);
    target[3] = (uint8_t)(P.col.a * 255 + 0.5f);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy), a;
            size_t i = (size_t)y * P.w + x;
            int m, c;
            if (r < 0.75f) a = ((int)floorf(cx) == x && (int)floorf(cy) == y) ? 1.0f : 0.0f;   /* one pixel */
            else if (d <= inner) a = 1.0f;
            else if (d >= r) a = 0.0f;
            else a = 1.0f - (d - inner) / (r - inner);
            m = (int)(a * 255 + 0.5f);
            if (m <= P.mask[i]) continue;   /* a stroke doesn't build up over itself */
            P.mask[i] = (uint8_t)m;
            {
                float t = (float)m / 255.0f * P.opac;
                const uint8_t *b = P.before + i * 4, *g = P.tool == T_ERASER ? P.orig + i * 4 : target;
                uint8_t *o = P.img + i * 4;
                for (c = 0; c < 4; c++) o[c] = (uint8_t)(b[c] + (g[c] - b[c]) * t + 0.5f);
            }
            if (x < P.bx0) P.bx0 = x;
            if (y < P.by0) P.by0 = y;
            if (x + 1 > P.bx1) P.bx1 = x + 1;
            if (y + 1 > P.by1) P.by1 = y + 1;
        }
    touch(x0, y0, x1, y1);
}

static void stroke_to(float x, float y)
{
    float dx = x - P.lx, dy = y - P.ly, d = sqrtf(dx * dx + dy * dy);
    float step = P.size * 0.2f < 0.5f ? 0.5f : P.size * 0.2f;
    int n = (int)(d / step), i;
    for (i = 1; i <= n; i++) stamp(P.lx + dx * i / (float)(n + 1), P.ly + dy * i / (float)(n + 1));
    stamp(x, y);
    P.lx = x;
    P.ly = y;
}

static void fill_at(int sx, int sy)
{
    uint8_t seed[4], target[4];
    int *stack, n = 0, cap = 1024, bx0 = sx, by0 = sy, bx1 = sx + 1, by1 = sy + 1;
    uint8_t *seen;
    if (sx < 0 || sy < 0 || sx >= P.w || sy >= P.h) return;
    memcpy(P.before, P.img, (size_t)P.w * P.h * 4);
    memcpy(seed, P.img + ((size_t)sy * P.w + sx) * 4, 4);
    target[0] = (uint8_t)(P.col.r * 255 + 0.5f);
    target[1] = (uint8_t)(P.col.g * 255 + 0.5f);
    target[2] = (uint8_t)(P.col.b * 255 + 0.5f);
    target[3] = (uint8_t)(P.col.a * 255 + 0.5f);
    seen = (uint8_t *)calloc((size_t)P.w * P.h, 1);
    stack = (int *)malloc(sizeof(int) * (size_t)cap);
    stack[n++] = sy * P.w + sx;
    seen[sy * P.w + sx] = 1;
    while (n) {
        int p = stack[--n], x = p % P.w, y = p / P.w, k, c;
        uint8_t *o = P.img + (size_t)p * 4;
        for (c = 0; c < 4; c++) o[c] = (uint8_t)(o[c] + (target[c] - o[c]) * P.opac + 0.5f);
        if (x < bx0) bx0 = x;
        if (y < by0) by0 = y;
        if (x + 1 > bx1) bx1 = x + 1;
        if (y + 1 > by1) by1 = y + 1;
        for (k = 0; k < 4; k++) {
            int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3), q;
            const uint8_t *b;
            if (nx < 0 || ny < 0 || nx >= P.w || ny >= P.h) continue;
            q = ny * P.w + nx;
            if (seen[q]) continue;
            b = P.before + (size_t)q * 4;
            if (abs(b[0] - seed[0]) + abs(b[1] - seed[1]) + abs(b[2] - seed[2]) + abs(b[3] - seed[3]) > P.tol * 4) continue;
            seen[q] = 1;
            if (n == cap) { cap *= 2; stack = (int *)realloc(stack, sizeof(int) * (size_t)cap); }
            stack[n++] = q;
        }
    }
    free(seen);
    free(stack);
    record(bx0, by0, bx1, by1);
    touch(bx0, by0, bx1, by1);
}

/* ---- m4b: icons, as paint programs have them, drawn as shapes ---- */

enum { I_BRUSH, I_ERASER, I_PICKER, I_FILL, I_UNDO, I_REDO, I_FIT };

static void draw_icon(struct nk_command_buffer *cv, struct nk_rect r, int icon, struct nk_color c)
{
    const float PI = 3.14159265f;
    float s = (r.w < r.h ? r.w : r.h) * 0.62f, cx = r.x + r.w / 2, cy = r.y + r.h / 2, t = s * 0.1f;
    if (t < 1.5f) t = 1.5f;
    switch (icon) {
    case I_BRUSH:                                           /* handle, ferrule, the bristles' tip */
        nk_stroke_line(cv, cx + 0.48f * s, cy - 0.48f * s, cx + 0.02f * s, cy - 0.02f * s, t, c);
        nk_fill_circle(cv, nk_rect(cx - 0.30f * s, cy - 0.06f * s, 0.36f * s, 0.36f * s), c);
        nk_fill_triangle(cv, cx - 0.28f * s, cy + 0.06f * s, cx - 0.06f * s, cy + 0.28f * s, cx - 0.50f * s, cy + 0.50f * s, c);
        break;
    case I_ERASER: {                                        /* a block on its side, its rubber end filled */
        float ux = 0.7071f, uy = -0.7071f, vx = 0.7071f, vy = 0.7071f, L = 0.48f * s, W = 0.22f * s;
        float p[8], q[8];
        p[0] = cx - ux * L - vx * W; p[1] = cy - uy * L - vy * W;
        p[2] = cx + ux * L - vx * W; p[3] = cy + uy * L - vy * W;
        p[4] = cx + ux * L + vx * W; p[5] = cy + uy * L + vy * W;
        p[6] = cx - ux * L + vx * W; p[7] = cy - uy * L + vy * W;
        q[0] = p[0]; q[1] = p[1];
        q[2] = cx - ux * L * 0.1f - vx * W; q[3] = cy - uy * L * 0.1f - vy * W;
        q[4] = cx - ux * L * 0.1f + vx * W; q[5] = cy - uy * L * 0.1f + vy * W;
        q[6] = p[6]; q[7] = p[7];
        nk_fill_polygon(cv, q, 4, c);
        nk_stroke_polygon(cv, p, 4, t * 0.8f, c);
        break;
    }
    case I_PICKER:                                          /* the dropper's tube and bulb */
        nk_stroke_line(cv, cx - 0.46f * s, cy + 0.46f * s, cx + 0.12f * s, cy - 0.12f * s, t, c);
        nk_stroke_line(cv, cx - 0.02f * s, cy - 0.30f * s, cx + 0.30f * s, cy + 0.02f * s, t, c);
        nk_fill_circle(cv, nk_rect(cx + 0.08f * s, cy - 0.46f * s, 0.38f * s, 0.38f * s), c);
        break;
    case I_FILL: {                                          /* a tipped bucket and a drop */
        float p[8];
        p[0] = cx - 0.42f * s; p[1] = cy - 0.05f * s;
        p[2] = cx - 0.05f * s; p[3] = cy - 0.42f * s;
        p[4] = cx + 0.30f * s; p[5] = cy - 0.07f * s;
        p[6] = cx - 0.07f * s; p[7] = cy + 0.30f * s;
        nk_stroke_polygon(cv, p, 4, t * 0.9f, c);
        nk_fill_triangle(cv, cx + 0.30f * s, cy - 0.07f * s, cx + 0.12f * s, cy - 0.25f * s, cx + 0.42f * s, cy + 0.12f * s, c);
        nk_fill_circle(cv, nk_rect(cx + 0.30f * s, cy + 0.20f * s, 0.24f * s, 0.24f * s), c);
        break;
    }
    case I_UNDO:
    case I_REDO: {                                          /* a curved arrow */
        float ex = icon == I_UNDO ? cx - 0.34f * s : cx + 0.34f * s, ey = cy + 0.12f * s;
        nk_stroke_arc(cv, cx, cy + 0.12f * s, 0.34f * s, PI, 2 * PI, t, c);
        nk_fill_triangle(cv, ex - 0.20f * s, ey - 0.02f * s, ex + 0.20f * s, ey - 0.02f * s, ex, ey + 0.30f * s, c);
        break;
    }
    case I_FIT: {                                           /* four corners */
        float a = 0.42f * s, b = 0.18f * s;
        nk_stroke_line(cv, cx - a, cy - a, cx - a + b, cy - a, t, c); nk_stroke_line(cv, cx - a, cy - a, cx - a, cy - a + b, t, c);
        nk_stroke_line(cv, cx + a, cy - a, cx + a - b, cy - a, t, c); nk_stroke_line(cv, cx + a, cy - a, cx + a, cy - a + b, t, c);
        nk_stroke_line(cv, cx - a, cy + a, cx - a + b, cy + a, t, c); nk_stroke_line(cv, cx - a, cy + a, cx - a, cy + a - b, t, c);
        nk_stroke_line(cv, cx + a, cy + a, cx + a - b, cy + a, t, c); nk_stroke_line(cv, cx + a, cy + a, cx + a, cy + a - b, t, c);
        break;
    }
    }
}

/* a button with an icon (selected: a tool's, -1 a plain button); its name
 * as a tooltip */
static int icon_button(struct nk_context *ctx, int icon, int selected, int enabled, const char *tip)
{
    struct nk_rect r = nk_widget_bounds(ctx);
    int hit, on = selected > 0;
    if (selected >= 0) hit = nk_selectable_label(ctx, " ", NK_TEXT_CENTERED, &on) && on;   /* Nuklear wants a label */
    else hit = nk_button_label(ctx, " ");
    draw_icon(nk_window_get_canvas(ctx), r, icon, enabled ? nk_rgb(235, 235, 240) : nk_rgb(95, 95, 105));
    if (nk_input_is_mouse_hovering_rect(&ctx->input, r)) nk_tooltip(ctx, tip);
    return hit && enabled;
}

/* m4b: a setting as the launcher shows them: its name, a slider, its value */
static void slider_i(struct nk_context *ctx, const char *name, int *v, int lo, int hi, const char *fmt)
{
    char t[24];
    nk_label(ctx, name, NK_TEXT_LEFT);
    nk_slider_int(ctx, lo, v, hi, 1);
    snprintf(t, sizeof t, fmt, *v);
    nk_label(ctx, t, NK_TEXT_RIGHT);
}

static void slider_pct(struct nk_context *ctx, const char *name, float *v, float lo)
{
    char t[24];
    nk_label(ctx, name, NK_TEXT_LEFT);
    nk_slider_float(ctx, lo, v, 1.0f, 0.01f);
    snprintf(t, sizeof t, "%d%%", (int)(*v * 100 + 0.5f));
    nk_label(ctx, t, NK_TEXT_RIGHT);
}

void paint_set_overlay(void (*fn)(struct nk_command_buffer *, float, float, float, float)) { P.overlay = fn; }

static void canvas_draw(struct nk_command_buffer *cv, struct nk_rect b, int hover, float mx, float my, float ix, float iy)
{
    struct nk_image im = nk_image_ptr(P.tex);
    nk_fill_rect(cv, b, 0, nk_rgb(16, 16, 20));
    nk_draw_image(cv, nk_rect(b.x + P.ox, b.y + P.oy, P.w * P.zoom, P.h * P.zoom), &im, nk_rgb(255, 255, 255));
    if (P.overlay) P.overlay(cv, b.x + P.ox, b.y + P.oy, P.w * P.zoom, P.h * P.zoom);
    if (hover && (P.tool == T_BRUSH || P.tool == T_ERASER)) {   /* the brush's outline */
        float r = P.size * 0.5f * P.zoom;
        if (r < 2) r = 2;
        nk_stroke_circle(cv, nk_rect(mx - r, my - r, 2 * r, 2 * r), 1.0f, nk_rgb(255, 255, 255));
    }
    if (hover && ix >= 0 && iy >= 0 && ix < P.w && iy < P.h)
        snprintf(P.status, sizeof P.status, "x %d, y %d", (int)ix, (int)iy);
    else
        snprintf(P.status, sizeof P.status, "Wheel: zoom. Right drag: move");
}

/* ---- the panel ---- */

int paint_panel(struct nk_context *ctx, float row, float canvas_h)
{
    struct nk_input *in = &ctx->input;
    struct nk_rect b;
    int ret = PAINT_NONE, i;
    if (!P.on) return PAINT_CLOSE;

    /* the tools, the colour */
    nk_layout_row_template_begin(ctx, row + 6);
    for (i = 0; i < T_N; i++) nk_layout_row_template_push_static(ctx, row + 10);
    nk_layout_row_template_push_static(ctx, 70);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, row + 10);
    nk_layout_row_template_push_static(ctx, row + 10);
    nk_layout_row_template_push_static(ctx, row + 10);
    nk_layout_row_template_end(ctx);
    for (i = 0; i < T_N; i++) {
        static const int icons[T_N] = { I_BRUSH, I_ERASER, I_PICKER, I_FILL };
        static const char *const tips[T_N] = { "Brush", "Eraser: back to the original", "Picker: a colour from the texture",
                                                 "Fill" };
        if (icon_button(ctx, icons[i], P.tool == i ? 1 : 0, 1, tips[i])) P.tool = i;
    }
    if (nk_combo_begin_color(ctx, nk_rgb_cf(P.col), nk_vec2(260, 330))) {
        nk_layout_row_dynamic(ctx, 180, 1);
        P.col = nk_color_picker(ctx, P.col, NK_RGBA);
        nk_layout_row_dynamic(ctx, row - 6, 1);
        P.col.r = nk_propertyf(ctx, "#R:", 0, P.col.r, 1, 0.01f, 0.005f);
        P.col.g = nk_propertyf(ctx, "#G:", 0, P.col.g, 1, 0.01f, 0.005f);
        P.col.b = nk_propertyf(ctx, "#B:", 0, P.col.b, 1, 0.01f, 0.005f);
        P.col.a = nk_propertyf(ctx, "#A:", 0, P.col.a, 1, 0.01f, 0.005f);
        nk_combo_end(ctx);
    }
    nk_spacing(ctx, 1);
    if (icon_button(ctx, I_UNDO, -1, P.nundo > 0, "Undo (Ctrl+Z)")) undo_redo(0);
    if (icon_button(ctx, I_REDO, -1, P.nredo > 0, "Redo (Ctrl+Y)")) undo_redo(1);
    if (icon_button(ctx, I_FIT, -1, 1, "Fit the whole texture")) P.fit = 1;

    /* their settings */
    nk_layout_row_template_begin(ctx, row - 4);
    nk_layout_row_template_push_static(ctx, 78);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 50);
    nk_layout_row_template_push_static(ctx, 12);
    nk_layout_row_template_push_static(ctx, 78);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 50);
    nk_layout_row_template_end(ctx);
    {
        int sz = (int)P.size;
        slider_i(ctx, "Size", &sz, 1, 256, "%d px");
        P.size = (float)sz;
    }
    nk_spacing(ctx, 1);
    slider_pct(ctx, "Hardness", &P.hard, 0.0f);
    slider_pct(ctx, "Opacity", &P.opac, 0.01f);
    nk_spacing(ctx, 1);
    slider_i(ctx, "Fill range", &P.tol, 0, 255, "%d");

    /* undo, the view, save */
    /* m5: in two rows, for the narrower panel beside the 3D view */
    nk_layout_row_template_begin(ctx, row - 8);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 140);
    nk_layout_row_template_end(ctx);
    nk_label_colored(ctx, P.status, NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    nk_labelf(ctx, NK_TEXT_RIGHT, "%d x %d, %d%%", P.w, P.h, (int)(P.zoom * 100 + 0.5f));
    nk_layout_row_dynamic(ctx, row, 3);
    {   /* m4c: integer scaling, x2 x4 x8, greyed past 4096 (or always, for graffiti) */
        struct nk_rect r = nk_widget_bounds(ctx);
        if (!P.resize_max) nk_widget_disable_begin(ctx);
        if (nk_combo_begin_label(ctx, "Resize", nk_vec2(200, 4 * row + 10))) {
            static const int ks[3] = { 2, 4, 8 };
            nk_layout_row_dynamic(ctx, row - 4, 1);
            for (i = 0; i < 3; i++) {
                char t[48];
                int w = P.w * ks[i], h = P.h * ks[i], ok = w <= P.resize_max && h <= P.resize_max;
                snprintf(t, sizeof t, "x%d   %d x %d", ks[i], w, h);
                if (!ok) nk_widget_disable_begin(ctx);
                if (nk_combo_item_label(ctx, t, NK_TEXT_LEFT) && ok) {
                    if (paint_resize(ks[i])) snprintf(P.status, sizeof P.status, "Not enough memory to resize.");
                }
                if (!ok) nk_widget_disable_end(ctx);
            }
            nk_combo_end(ctx);
        }
        if (!P.resize_max) {
            nk_widget_disable_end(ctx);
            if (nk_input_is_mouse_hovering_rect(in, r)) nk_tooltip(ctx, "Graffiti keep their size");
        }
    }
    if (nk_button_label(ctx, P.dirty ? "Save" : "Saved")) ret = PAINT_SAVE;
    if (nk_button_label(ctx, "Close")) ret = PAINT_CLOSE;
    if (nk_input_is_key_pressed(in, NK_KEY_TEXT_UNDO)) undo_redo(0);
    if (nk_input_is_key_pressed(in, NK_KEY_TEXT_REDO)) undo_redo(1);

    /* the canvas */
    nk_layout_row_dynamic(ctx, canvas_h, 1);
    if (nk_widget(&b, ctx) != NK_WIDGET_INVALID) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        int hover = nk_input_is_mouse_hovering_rect(in, b);
        float mx = in->mouse.pos.x, my = in->mouse.pos.y;
        float ix, iy;
        if (P.fit) {
            float k = (b.w - 8) / (float)P.w, kh = (b.h - 8) / (float)P.h;
            P.zoom = k < kh ? k : kh;
            P.ox = (b.w - P.w * P.zoom) / 2;
            P.oy = (b.h - P.h * P.zoom) / 2;
            P.fit = 0;
        }
        if (hover && in->mouse.scroll_delta.y != 0) {           /* zoom, around the mouse */
            float z = P.zoom * (in->mouse.scroll_delta.y > 0 ? 1.25f : 0.8f);
            if (z < 0.05f) z = 0.05f;
            if (z > 32) z = 32;
            P.ox = mx - b.x - (mx - b.x - P.ox) * z / P.zoom;
            P.oy = my - b.y - (my - b.y - P.oy) * z / P.zoom;
            P.zoom = z;
            in->mouse.scroll_delta.y = 0;
        }
        if (hover && (in->mouse.buttons[NK_BUTTON_RIGHT].down || in->mouse.buttons[NK_BUTTON_MIDDLE].down)) {
            P.ox += in->mouse.delta.x;                          /* pan */
            P.oy += in->mouse.delta.y;
        }
        ix = (mx - b.x - P.ox) / P.zoom;
        iy = (my - b.y - P.oy) / P.zoom;
        if (hover && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) {   /* this frame's press, not a held one */
            if (P.tool == T_PICKER) {
                int px = (int)floorf(ix), py = (int)floorf(iy);
                if (px >= 0 && py >= 0 && px < P.w && py < P.h) {
                    const uint8_t *p = P.img + ((size_t)py * P.w + px) * 4;
                    P.col.r = p[0] / 255.0f; P.col.g = p[1] / 255.0f; P.col.b = p[2] / 255.0f; P.col.a = p[3] / 255.0f;
                }
            } else if (P.tool == T_FILL) {
                fill_at((int)floorf(ix), (int)floorf(iy));
            } else {
                memcpy(P.before, P.img, (size_t)P.w * P.h * 4);
                memset(P.mask, 0, (size_t)P.w * P.h);
                P.bx0 = P.by0 = 1 << 30;
                P.bx1 = P.by1 = -1;
                P.stroking = 1;
                P.lx = ix;
                P.ly = iy;
                stamp(ix, iy);
            }
        } else if (P.stroking && in->mouse.buttons[NK_BUTTON_LEFT].down) {
            if (ix != P.lx || iy != P.ly) stroke_to(ix, iy);
        } else if (P.stroking) {
            P.stroking = 0;
            record(P.bx0, P.by0, P.bx1, P.by1);
        }
        upload();
        {   /* m4b: drawn inside the canvas only, the zoomed image not over the tools */
            struct nk_rect wc = cv->clip, c;
            float x1 = b.x + b.w < wc.x + wc.w ? b.x + b.w : wc.x + wc.w, y1 = b.y + b.h < wc.y + wc.h ? b.y + b.h : wc.y + wc.h;
            c.x = b.x > wc.x ? b.x : wc.x;
            c.y = b.y > wc.y ? b.y : wc.y;
            c.w = x1 - c.x > 0 ? x1 - c.x : 0;
            c.h = y1 - c.y > 0 ? y1 - c.y : 0;
            nk_push_scissor(cv, c);
            canvas_draw(cv, b, hover, mx, my, ix, iy);
            nk_push_scissor(cv, wc);
        }
    }
    return ret;
}
