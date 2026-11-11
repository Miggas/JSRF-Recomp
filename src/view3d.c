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
} V;

void v3d_free(void)
{
    jm_free(&V.ms);
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
}

/* m5: only a model known to wear it (HUD files hold hundreds of models
 * whose textures aren't known: guessing showed the wrong ones) */
static int wears(const JMesh *m, uint32_t texid) { return texid && m->texid == texid; }

/* ---- drawing, in software ---- */

typedef struct { float sx, sy, iz, u, v, l; int ok; } SV;   /* a vertex on the screen */

static void raster(const SV *a, const SV *b, const SV *c, const uint8_t *tex, int tw, int th)
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
                o[0] = (uint8_t)(t[0] * l > 255 ? 255 : t[0] * l);
                o[1] = (uint8_t)(t[1] * l > 255 ? 255 : t[1] * l);
                o[2] = (uint8_t)(t[2] * l > 255 ? 255 : t[2] * l);
            } else {
                o[0] = o[1] = o[2] = (uint8_t)(170 * l > 255 ? 255 : 170 * l);
            }
            o[3] = 255;
            V.zb[i] = iz;
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
        if (!sv[t[0]].ok || !sv[t[1]].ok || !sv[t[2]].ok) continue;
        raster(&sv[t[0]], &sv[t[1]], &sv[t[2]], tex, tw, th);
    }
    if (V.wire)                                 /* m5b: the edges, where they're seen */
        for (i = 0; i < m->nt; i++) {
            const uint32_t *t = m->ix + i * 3;
            int e;
            if (!sv[t[0]].ok || !sv[t[1]].ok || !sv[t[2]].ok) continue;
            for (e = 0; e < 3; e++) edge(&sv[t[e]], &sv[t[(e + 1) % 3]]);
        }
    free(sv);
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
    nk_layout_row_dynamic(ctx, row - 8, 1);
    nk_label_colored(ctx, n ? "Drag: turn. Right drag: move. Wheel: zoom."
                            : "No model known to wear it. All: look anyway.",
                     NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    V.own = wears(m, texid);
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
            V.fb = (uint8_t *)malloc((size_t)w * hh * 4);
            V.zb = (float *)malloc(sizeof(float) * (size_t)w * hh);
            V.rw = w;
            V.rh = hh;
            V.need = 1;
        }
        if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, b, nk_true) && (in->mouse.delta.x || in->mouse.delta.y)) {
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
    }
}

void v3d_draw_uv(struct nk_command_buffer *cv, float x, float y, float w, float h)
{
    const JMesh *m;
    int i, e;
    if (!V.wire || !V.own || V.cur < 0 || V.cur >= V.ms.n) return;
    m = &V.ms.m[V.cur];
    for (i = 0; i < m->nt; i++) {
        const uint32_t *t = m->ix + i * 3;
        float u[3], v[3], ou, ov;
        for (e = 0; e < 3; e++) { u[e] = m->t[t[e] * 2]; v[e] = m->t[t[e] * 2 + 1]; }
        ou = floorf(fminf(u[0], fminf(u[1], u[2])));   /* a repeating texture: its first copy */
        ov = floorf(fminf(v[0], fminf(v[1], v[2])));
        for (e = 0; e < 3; e++) {
            int f = (e + 1) % 3;
            nk_stroke_line(cv, x + (u[e] - ou) * w, y + (v[e] - ov) * h, x + (u[f] - ou) * w, y + (v[f] - ov) * h, 1.0f,
                           nk_rgba(80, 230, 255, 170));
        }
    }
}

void v3d_set_wire(int on) { V.wire = on; V.need = 1; }
