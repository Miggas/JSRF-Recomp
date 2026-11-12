/* The 3D view: see view3d.h */
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

#include "model.h"
#include "view3d.h"
#include "paint.h"

static struct {
    JModels ms;
    int loaded, cur, all;
    uint32_t for_tex;                   /* the texture the pick was made for */
    float yaw, pitch, dist, pan[3];
    int cam_set;
    SDL_Texture *rt;
    int rw, rh;
    uint8_t *fb;
    float *zb;
    int need;
    const uint8_t *last_px;
    unsigned last_ver;
    int last_cur;
    int wire, own;                      /* m5b: the edges shown; the model shown wears the texture */
    uint32_t *edges;                    /* m5c: the shown model's edges, each once (pairs) */
    int nedges, edges_for;
    float *uvb;                         /* m6: each pixel's texture coordinates (where zb > 0) */
    int paint, want_paint;              /* m6: painting on the model; asked to start the painter */
    int stroking, had;                  /* m6: a stroke; its last sample hit the model */
    float lsx, lsy, lu, lv;             /* m6: the last sample: on the screen, on the texture */
    int want_export;                    /* m8: Export pressed (the model's index + 1) */
    int want_import;                    /* m9: Import pressed (the same) */
    float mcol[64][3];                  /* m10: each material's colour, multiplying the texture */
    int nmcol, hl;                      /* m10: how many; the one highlighted (-1 none) */
} V;

static int cmp_edge(const void *a, const void *b)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

/* m5c: an edge two triangles share drawn once, not twice */
static void build_edges(int cur)
{
    const JMesh *m = &V.ms.m[cur];
    int i, e, n = 0, k;
    free(V.edges);
    V.edges = (uint32_t *)malloc(sizeof(uint32_t) * 6 * (size_t)m->nt + 1);
    V.nedges = 0;
    V.edges_for = cur;
    if (!V.edges) return;
    for (i = 0; i < m->nt; i++)
        for (e = 0; e < 3; e++) {
            uint32_t a = m->ix[i * 3 + e], b = m->ix[i * 3 + (e + 1) % 3];
            V.edges[n * 2] = a < b ? a : b;
            V.edges[n * 2 + 1] = a < b ? b : a;
            n++;
        }
    qsort(V.edges, (size_t)n, sizeof(uint32_t) * 2, cmp_edge);
    for (i = 0, k = 0; i < n; i++)
        if (!k || V.edges[i * 2] != V.edges[(k - 1) * 2] || V.edges[i * 2 + 1] != V.edges[(k - 1) * 2 + 1]) {
            V.edges[k * 2] = V.edges[i * 2];
            V.edges[k * 2 + 1] = V.edges[i * 2 + 1];
            k++;
        }
    V.nedges = k;
}

void v3d_free(void)
{
    jm_free(&V.ms);
    free(V.edges);
    V.edges = NULL;
    V.nedges = 0;
    V.edges_for = -1;
    V.loaded = 0;
    V.cur = -1;
}

void v3d_load(const uint8_t *buf, size_t size)
{
    v3d_free();
    jm_load(buf, size, &V.ms);
    V.loaded = 1;
    V.cur = -1;
    V.for_tex = 0;
    V.need = 1;
    V.nmcol = 0;                                /* m10 */
    V.hl = -1;
}

/* m5: only a model known to wear it (HUD files hold hundreds of models
 * whose textures aren't known: guessing showed the wrong ones) */
static int wears(const JMesh *m, uint32_t texid) { return texid && m->texid == texid; }

/* ---- drawing, in software ---- */

typedef struct { float sx, sy, iz, u, v, l; int ok; } SV;   /* a vertex on the screen */

static void raster(const SV *a, const SV *b, const SV *c, const uint8_t *tex, int tw, int th, const float *tint)
{
    float minx = a->sx, maxx = a->sx, miny = a->sy, maxy = a->sy, area;
    int x0, x1, y0, y1, x, y;
    minx = fminf(minx, fminf(b->sx, c->sx));
    maxx = fmaxf(maxx, fmaxf(b->sx, c->sx));
    miny = fminf(miny, fminf(b->sy, c->sy));
    maxy = fmaxf(maxy, fmaxf(b->sy, c->sy));
    x0 = (int)floorf(minx); x1 = (int)ceilf(maxx); y0 = (int)floorf(miny); y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > V.rw - 1) x1 = V.rw - 1;
    if (y1 > V.rh - 1) y1 = V.rh - 1;
    area = (b->sx - a->sx) * (c->sy - a->sy) - (b->sy - a->sy) * (c->sx - a->sx);
    if (fabsf(area) < 1e-6f) return;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ((b->sx - px) * (c->sy - py) - (b->sy - py) * (c->sx - px)) / area;
            float w1 = ((c->sx - px) * (a->sy - py) - (c->sy - py) * (a->sx - px)) / area;
            float w2 = 1.0f - w0 - w1, iz, u, v, l;
            size_t i = (size_t)y * V.rw + x;
            uint8_t *o;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            iz = w0 * a->iz + w1 * b->iz + w2 * c->iz;
            if (iz <= V.zb[i]) continue;
            u = (w0 * a->u * a->iz + w1 * b->u * b->iz + w2 * c->u * c->iz) / iz;   /* perspective-correct */
            v = (w0 * a->v * a->iz + w1 * b->v * b->iz + w2 * c->v * c->iz) / iz;
            l = w0 * a->l + w1 * b->l + w2 * c->l;
            o = V.fb + i * 4;
            if (tex) {
                int tx = (int)floorf((u - floorf(u)) * tw), ty = (int)floorf((v - floorf(v)) * th);
                const uint8_t *t;
                if (tx >= tw) tx = tw - 1;
                if (ty >= th) ty = th - 1;
                t = tex + ((size_t)ty * tw + tx) * 4;
                if (t[3] < 128) continue;       /* see-through parts */
                o[0] = (uint8_t)(t[0] * l * tint[0] > 255 ? 255 : t[0] * l * tint[0]);
                o[1] = (uint8_t)(t[1] * l * tint[1] > 255 ? 255 : t[1] * l * tint[1]);
                o[2] = (uint8_t)(t[2] * l * tint[2] > 255 ? 255 : t[2] * l * tint[2]);
            } else {
                o[0] = (uint8_t)(170 * l * tint[0] > 255 ? 255 : 170 * l * tint[0]);
                o[1] = (uint8_t)(170 * l * tint[1] > 255 ? 255 : 170 * l * tint[1]);
                o[2] = (uint8_t)(170 * l * tint[2] > 255 ? 255 : 170 * l * tint[2]);
            }
            o[3] = 255;
            V.zb[i] = iz;
            if (V.uvb) { V.uvb[i * 2] = u; V.uvb[i * 2 + 1] = v; }   /* m6 */
        }
}

/* m5b: an edge, 1 pixel, over what is in front of it only (a little
 * slack so the triangle's own surface doesn't hide it) */
static void edge(const SV *a, const SV *b)
{
    float dx = b->sx - a->sx, dy = b->sy - a->sy;
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy))), k;
    if (n > 4096) return;
    for (k = 0; k <= n; k++) {
        float s = n ? (float)k / n : 0, iz = a->iz + (b->iz - a->iz) * s;
        int x = (int)(a->sx + dx * s), y = (int)(a->sy + dy * s);
        size_t i;
        uint8_t *o;
        if (x < 0 || y < 0 || x >= V.rw || y >= V.rh) continue;
        i = (size_t)y * V.rw + x;
        if (iz < V.zb[i] * 0.995f) continue;
        o = V.fb + i * 4;
        o[0] = (uint8_t)((o[0] + 3 * 80) / 4);
        o[1] = (uint8_t)((o[1] + 3 * 230) / 4);
        o[2] = (uint8_t)((o[2] + 3 * 255) / 4);
    }
}

static void render(const JMesh *m, const uint8_t *tex, int tw, int th)
{
    float eye[3], f[3], r[3], u[3], tgt[3], len, focal;
    SV *sv;
    int i, y;
    for (y = 0; y < V.rh; y++)                  /* the background: dark, a little lighter at the top */
        for (i = 0; i < V.rw; i++) {
            uint8_t *o = V.fb + ((size_t)y * V.rw + i) * 4;
            uint8_t g = (uint8_t)(34 - 14 * y / (V.rh ? V.rh : 1));
            o[0] = g; o[1] = g; o[2] = (uint8_t)(g + 4); o[3] = 255;
        }
    memset(V.zb, 0, sizeof(float) * (size_t)V.rw * V.rh);
    if (!m) return;
    for (i = 0; i < 3; i++) tgt[i] = m->c[i] + V.pan[i];
    eye[0] = tgt[0] + sinf(V.yaw) * cosf(V.pitch) * V.dist * m->r;
    eye[1] = tgt[1] + sinf(V.pitch) * V.dist * m->r;
    eye[2] = tgt[2] + cosf(V.yaw) * cosf(V.pitch) * V.dist * m->r;
    for (i = 0; i < 3; i++) f[i] = tgt[i] - eye[i];
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (i = 0; i < 3; i++) f[i] /= len;
    r[0] = -f[2]; r[1] = 0; r[2] = f[0];        /* f x (0, 1, 0) */
    len = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (len < 1e-6f) { r[0] = 1; len = 1; }
    r[0] /= len; r[2] /= len;
    u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
    focal = (float)V.rh * 0.5f / tanf(0.35f);  /* about 40 degrees high */
    sv = (SV *)malloc(sizeof(SV) * (size_t)m->nv);
    for (i = 0; i < m->nv; i++) {
        const float *p = m->p + i * 3;
        float d[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
        float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], yy = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        sv[i].ok = z > m->r * 0.01f;
        sv[i].iz = sv[i].ok ? 1.0f / z : 0;
        sv[i].sx = (float)V.rw * 0.5f + x * focal * sv[i].iz;
        sv[i].sy = (float)V.rh * 0.5f - yy * focal * sv[i].iz;
        sv[i].u = m->t[i * 2];
        sv[i].v = m->t[i * 2 + 1];
        if (m->has_n) {                         /* lit from the camera, both faces */
            const float *n = m->n + i * 3;
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), dd;
            dd = nl > 0 ? -(n[0] * f[0] + n[1] * f[1] + n[2] * f[2]) / nl : 1;
            sv[i].l = 0.45f + 0.75f * fabsf(dd);
        } else sv[i].l = 1.0f;
    }
    for (i = 0; i < m->nt; i++) {
        const uint32_t *t = m->ix + i * 3;
        float tint[3] = { 1, 1, 1 };
        int mt = m->tmat ? m->tmat[i] : 0;
        if (!sv[t[0]].ok || !sv[t[1]].ok || !sv[t[2]].ok) continue;
        if (mt >= 0 && mt < V.nmcol) { tint[0] = V.mcol[mt][0]; tint[1] = V.mcol[mt][1]; tint[2] = V.mcol[mt][2]; }
        if (V.nmcol && V.hl >= 0 && mt != V.hl) { tint[0] *= 0.3f; tint[1] *= 0.3f; tint[2] *= 0.3f; }   /* m10: the others dimmed */
        raster(&sv[t[0]], &sv[t[1]], &sv[t[2]], tex, tw, th, tint);
    }
    if (V.wire && V.edges && V.edges_for >= 0 && &V.ms.m[V.edges_for] == m)   /* m5b: the edges, where they're seen */
        for (i = 0; i < V.nedges; i++) {
            const SV *a = &sv[V.edges[i * 2]], *b = &sv[V.edges[i * 2 + 1]];
            if (a->ok && b->ok) edge(a, b);
        }
    free(sv);
}

/* ---- m6: painting on the model ---- */

/* the texture coordinates under screen pixel (x, y) of the view; 0 none */
static int sample(int x, int y, float *u, float *v)
{
    size_t i;
    if (!V.uvb || x < 0 || y < 0 || x >= V.rw || y >= V.rh) return 0;
    i = (size_t)y * V.rw + x;
    if (V.zb[i] <= 0) return 0;
    *u = V.uvb[i * 2];
    *v = V.uvb[i * 2 + 1];
    return 1;
}

/* the stroke along the screen from the last sample to (x, y), a sample
 * every 2 pixels: each joins the last on the texture, unless they are far
 * apart there (a seam: the surface continues somewhere else on the
 * texture) or the cursor left the model in between */
static void stroke_screen(float x, float y, int tw, int th)
{
    float dx = x - V.lsx, dy = y - V.lsy, d = sqrtf(dx * dx + dy * dy);
    int n = (int)(d / 2.0f) + 1, k;
    float far = fmaxf(0.04f, 3.0f * paint_brush_size() / (float)(tw > th ? tw : th));   /* of the texture's width */
    for (k = 1; k <= n; k++) {
        float sx = V.lsx + dx * (float)k / (float)n, sy = V.lsy + dy * (float)k / (float)n, u, v;
        if (!sample((int)sx, (int)sy, &u, &v)) { V.had = 0; continue; }
        {
            float du = (u - floorf(u)) - (V.lu - floorf(V.lu)), dv = (v - floorf(v)) - (V.lv - floorf(V.lv));
            int jump = !V.had || sqrtf(du * du + dv * dv) > far;
            paint_ext_to(u, v, jump);
        }
        V.lu = u;
        V.lv = v;
        V.had = 1;
    }
    V.lsx = x;
    V.lsy = y;
}

int v3d_take_paint_request(void)
{
    int r = V.want_paint;
    V.want_paint = 0;
    return r;
}

/* ---- the panel ---- */

void v3d_panel(struct nk_context *ctx, SDL_Renderer *ren, uint32_t texid, const uint8_t *rgba, int tw, int th,
               unsigned ver, float row, float h)
{
    struct nk_input *in = &ctx->input;
    char name[64];
    int i, n = 0;
    struct nk_rect b;
    const JMesh *m;
    V.own = 0;
    if (!V.loaded || !V.ms.n) {
        nk_layout_row_dynamic(ctx, row, 1);
        nk_label(ctx, V.loaded ? "No 3D models in this file." : "Pick a file.", NK_TEXT_LEFT);
        return;
    }
    for (i = 0; i < V.ms.n; i++) n += wears(&V.ms.m[i], texid);
    if (V.for_tex != texid || V.cur < 0 || V.cur >= V.ms.n) {   /* the biggest model wearing it */
        int best = -1;
        for (i = 0; i < V.ms.n; i++)
            if (wears(&V.ms.m[i], texid) && (best < 0 || V.ms.m[i].nt > V.ms.m[best].nt)) best = i;
        V.cur = best >= 0 ? best : 0;
        V.for_tex = texid;
        V.cam_set = 0;
        V.need = 1;
    }
    if (!V.cam_set) {
        V.yaw = 3.14159265f;
        V.pitch = 0.15f;
        V.dist = 2.6f;
        V.pan[0] = V.pan[1] = V.pan[2] = 0;
        V.cam_set = 1;
        V.need = 1;
    }
    m = &V.ms.m[V.cur];

    /* which model */
    nk_layout_row_template_begin(ctx, row);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 52);
    nk_layout_row_template_push_static(ctx, 72);
    nk_layout_row_template_push_static(ctx, 64);
    nk_layout_row_template_end(ctx);
    snprintf(name, sizeof name, "Model %d%s", m->index, wears(m, texid) ? "" : " (other)");
    if (nk_combo_begin_label(ctx, name, nk_vec2(nk_widget_width(ctx), 300))) {
        nk_layout_row_dynamic(ctx, row - 6, 1);
        for (i = 0; i < V.ms.n; i++) {
            char t[64];
            if (!V.all && !wears(&V.ms.m[i], texid)) continue;
            snprintf(t, sizeof t, "Model %d%s", V.ms.m[i].index,
                     wears(&V.ms.m[i], texid) ? "" : " (other)");
            if (nk_combo_item_label(ctx, t, NK_TEXT_LEFT)) { V.cur = i; V.cam_set = 0; }
        }
        nk_combo_end(ctx);
    }
    {
        nk_bool all = (nk_bool)V.all;
        nk_checkbox_label(ctx, "All", &all);
        V.all = all;
        all = (nk_bool)V.wire;
        nk_checkbox_label(ctx, "Wires", &all);       /* m5b, Miggas's idea */
        if ((int)all != V.wire) { V.wire = all; V.need = 1; }
    }
    if (nk_button_label(ctx, "Reset")) V.cam_set = 0;
    V.own = wears(m, texid);
    /* m6: Paint on the model (starts the painter when it isn't open) */
    nk_layout_row_template_begin(ctx, row);
    nk_layout_row_template_push_static(ctx, 64);
    nk_layout_row_template_push_static(ctx, 112);
    nk_layout_row_template_push_static(ctx, 112);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    {
        nk_bool pm = (nk_bool)V.paint;
        int can = V.own;
        if (!can) { pm = 0; nk_widget_disable_begin(ctx); }
        nk_checkbox_label(ctx, "Paint", &pm);
        if (!can) nk_widget_disable_end(ctx);
        if (can && (int)pm != V.paint) {
            V.paint = pm;
            if (V.paint && !paint_on()) V.want_paint = 1;
        }
        if (!can) V.paint = 0;
    }
    if (nk_button_label(ctx, "Export OBJ")) V.want_export = V.cur + 1;   /* m8: for Blender */
    if (nk_button_label(ctx, "Import OBJ")) V.want_import = V.cur + 1;   /* m9: back from it */
    nk_label_colored(ctx, !n ? "None known to wear it."
                     : V.paint ? "Turn: middle."
                               : "Drag: turn. Right: move.",
                     NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    if (!n && !V.all) { V.own = 0; return; }

    /* the view */
    nk_layout_row_dynamic(ctx, h - 2 * row, 1);
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return;
    {
        int w = (int)b.w, hh = (int)b.h, hover = nk_input_is_mouse_hovering_rect(in, b);
        if (w < 16) w = 16;
        if (hh < 16) hh = 16;
        if (w > 1600) w = 1600;
        if (hh > 1600) hh = 1600;
        if (w != V.rw || hh != V.rh || !V.rt) {
            if (V.rt) SDL_DestroyTexture(V.rt);
            V.rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, hh);
            free(V.fb);
            free(V.zb);
            free(V.uvb);
            V.fb = (uint8_t *)malloc((size_t)w * hh * 4);
            V.zb = (float *)malloc(sizeof(float) * (size_t)w * hh);
            V.uvb = (float *)malloc(sizeof(float) * 2 * (size_t)w * hh);
            V.rw = w;
            V.rh = hh;
            V.need = 1;
        }
        int painting = V.paint && V.own && paint_on();
        if (V.paint && !paint_on() && !V.want_paint) V.paint = 0;   /* the painter was closed */
        if (painting) {                       /* m6: the left button paints */
            float mx = in->mouse.pos.x - b.x, my = in->mouse.pos.y - b.y, u, v;
            int tool = paint_tool();
            if (hover && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT) && sample((int)mx, (int)my, &u, &v)) {
                if (tool == PAINT_T_PICKER) paint_ext_pick(u, v);
                else if (tool == PAINT_T_FILL) paint_ext_fill(u, v);
                else {
                    paint_ext_begin(u, v);
                    V.stroking = 1;
                    V.had = 1;
                    V.lu = u; V.lv = v;
                    V.lsx = mx; V.lsy = my;
                }
            } else if (V.stroking && in->mouse.buttons[NK_BUTTON_LEFT].down) {
                if (mx != V.lsx || my != V.lsy) stroke_screen(mx, my, tw, th);
            } else if (V.stroking) {
                V.stroking = 0;
                paint_ext_end();
            }
        } else if (V.stroking) {
            V.stroking = 0;
            paint_ext_end();
        }
        if (nk_input_has_mouse_click_down_in_rect(in, painting ? NK_BUTTON_MIDDLE : NK_BUTTON_LEFT, b, nk_true)
            && (in->mouse.delta.x || in->mouse.delta.y)) {
            V.yaw -= in->mouse.delta.x * 0.01f;
            V.pitch += in->mouse.delta.y * 0.01f;
            if (V.pitch > 1.5f) V.pitch = 1.5f;
            if (V.pitch < -1.5f) V.pitch = -1.5f;
            V.need = 1;
        }
        if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_RIGHT, b, nk_true) && (in->mouse.delta.x || in->mouse.delta.y)) {
            float k = V.dist * m->r * 0.0025f, cy = cosf(V.yaw), sy = sinf(V.yaw);
            /* along the screen: right (cos yaw, 0, -sin yaw), up */
            V.pan[0] -= in->mouse.delta.x * k * cy;
            V.pan[2] += in->mouse.delta.x * k * sy;
            V.pan[1] += in->mouse.delta.y * k;
            V.need = 1;
        }
        if (hover && in->mouse.scroll_delta.y != 0) {
            V.dist *= in->mouse.scroll_delta.y > 0 ? 0.88f : 1.14f;
            if (V.dist < 0.3f) V.dist = 0.3f;
            if (V.dist > 20) V.dist = 20;
            in->mouse.scroll_delta.y = 0;
            V.need = 1;
        }
        if (rgba != V.last_px || ver != V.last_ver || V.cur != V.last_cur) V.need = 1;
        if (V.wire && (!V.edges || V.edges_for != V.cur)) build_edges(V.cur);
        if (V.need && V.rt && V.fb && V.zb) {
            const int own = wears(m, texid);
            render(m, own ? rgba : NULL, tw, th);
            SDL_UpdateTexture(V.rt, NULL, V.fb, V.rw * 4);
            V.need = 0;
            V.last_px = rgba;
            V.last_ver = ver;
            V.last_cur = V.cur;
        }
        if (V.rt) {
            struct nk_image im = nk_image_ptr(V.rt);
            nk_draw_image(nk_window_get_canvas(ctx), nk_rect(b.x, b.y, (float)V.rw, (float)V.rh), &im, nk_rgb(255, 255, 255));
        }
        if (painting && hover && (paint_tool() == PAINT_T_BRUSH || paint_tool() == PAINT_T_ERASER)) {
            /* m6: the brush's size on the model where the cursor is: the
             * texture's pixels per screen pixel there, from the neighbours */
            int x = (int)(in->mouse.pos.x - b.x), y = (int)(in->mouse.pos.y - b.y);
            float u0, v0, u1, v1, u2, v2;
            if (sample(x, y, &u0, &v0) && sample(x + 1, y, &u1, &v1) && sample(x, y + 1, &u2, &v2)) {
                float a = sqrtf((u1 - u0) * (u1 - u0) * tw * tw + (v1 - v0) * (v1 - v0) * th * th);
                float c = sqrtf((u2 - u0) * (u2 - u0) * tw * tw + (v2 - v0) * (v2 - v0) * th * th);
                float per = (a + c) * 0.5f, r = per > 1e-4f ? paint_brush_size() * 0.5f / per : 4;
                if (r < 2) r = 2;
                if (r > 300) r = 300;
                nk_stroke_circle(nk_window_get_canvas(ctx), nk_rect(in->mouse.pos.x - r, in->mouse.pos.y - r, 2 * r, 2 * r),
                                 1.5f, nk_rgba(255, 255, 255, 200));
            }
        }
    }
}

void v3d_draw_uv(struct nk_command_buffer *cv, float x, float y, float w, float h)
{
    const JMesh *m;
    int i;
    if (!V.wire || !V.own || V.cur < 0 || V.cur >= V.ms.n) return;
    if (!V.edges || V.edges_for != V.cur) build_edges(V.cur);
    if (!V.edges) return;
    m = &V.ms.m[V.cur];
    for (i = 0; i < V.nedges; i++) {
        const float *a = m->t + V.edges[i * 2] * 2, *b = m->t + V.edges[i * 2 + 1] * 2;
        float ou = floorf(fminf(a[0], b[0])), ov = floorf(fminf(a[1], b[1]));   /* a repeating texture: its first copy */
        nk_stroke_line(cv, x + (a[0] - ou) * w, y + (a[1] - ov) * h, x + (b[0] - ou) * w, y + (b[1] - ov) * h, 1.0f,
                       nk_rgba(80, 230, 255, 170));
    }
}

void v3d_set_wire(int on) { V.wire = on; V.need = 1; }
void v3d_set_paint(int on) { V.paint = on; }

int v3d_take_export_request(const JMesh **m)
{
    int k = V.want_export - 1;
    V.want_export = 0;
    if (k < 0 || k >= V.ms.n) return 0;
    *m = &V.ms.m[k];
    return 1;
}

int v3d_take_import_request(const JMesh **m)
{
    int k = V.want_import - 1;
    V.want_import = 0;
    if (k < 0 || k >= V.ms.n) return 0;
    *m = &V.ms.m[k];
    return 1;
}

const JMesh *v3d_current(void)
{
    return V.loaded && V.cur >= 0 && V.cur < V.ms.n ? &V.ms.m[V.cur] : NULL;
}

void v3d_set_materials(const uint8_t *mats, int n, int hl)
{
    float c[64][3];
    int i;
    if (n > 64) n = 64;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {                   /* B, G, R, A */
        c[i][0] = mats[i * 20 + 2] / 255.0f;
        c[i][1] = mats[i * 20 + 1] / 255.0f;
        c[i][2] = mats[i * 20 + 0] / 255.0f;
    }
    if (n != V.nmcol || hl != V.hl || memcmp(c, V.mcol, sizeof(float) * 3 * (size_t)n)) {
        memcpy(V.mcol, c, sizeof(float) * 3 * (size_t)n);
        V.nmcol = n;
        V.hl = hl;
        V.need = 1;
    }
}
