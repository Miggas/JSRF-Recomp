/*
 * jsrf_camwall.c -- v312z: the camera kept out of walls (Idea by Miggas; the
 * way of it follows JSRF-PC's "default cam fix", spriteice/JSRF-PC: its
 * findings, our own code).
 *
 * The game's follow camera keeps its eye out of walls with one line, from
 * the point it looks at to the eye: where the line meets a surface, the eye
 * goes there. But what the screen shows starts at the near plane, a little
 * ahead of the eye, and as wide as the view (wider in widescreen). So a wall
 * running along the view, the ground under it or a half pipe's curve can
 * cut into the picture while the line stays clear, and the inside of the
 * wall, or what is behind it, shows. The right stick's camera (v265) turns
 * the eye after the game placed it, with no wall test at all.
 *
 * After the camera's publication (sub_000A28B0, which runs the follow
 * camera's work-out) and the right stick's turn, in the follow modes, the
 * rectangle where the view starts is tested with the game's own line test
 * (the one its camera uses: sub_00018260, then sub_000194B0 for the nearest
 * hit): lines from the middle of that rectangle to its corners. When a
 * surface cuts across it, the eye first moves sideways, away from the
 * surface (a few units turn the view a few degrees), and if that doesn't
 * clear it, comes in towards the point looked at until the corners are
 * clear. The eye's own line is tested too: for the right stick's eye always,
 * for the game's only where what it meets hides the whole view (a surface
 * the eye has gone behind, which the game's own test, run when the eye
 * crosses a surface, can miss). Coming in is at once; going back out waits a
 * third of a second and then eases, so a wall going by doesn't shake the
 * view. Only the eye that is drawn (+0x44) is moved, after the game is done
 * with it; the game works its camera out again from its own state each
 * frame, so its smoothing and the controls' heading are as before. The
 * distance the skater's fade reads (+0x230) is set to the new one, so the
 * skater fades when the eye comes very near, as the game's own camera does.
 *
 * RECOMP_CAM_WALLS=0 turns it off; RECOMP_CAM_WALLS_LOG=1 logs what it does,
 * once a second at most.
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#else
#include "win32_compat.h"
#endif
#include "gen/recomp_types.h"

extern size_t xbox_GetMappedSize(void);
extern double xbox_WidescreenK(void);
extern void sub_00018260(void);     /* a line against the collision world */
extern void sub_000194B0(void);     /* the nearest hit of that test */

/* the camera (class 0x1CD518) */
#define CW_EYE          0x44u       /* the eye drawn from (the publication's) */
#define CW_TARGET       0x5Cu       /* the point it looks at */
#define CW_TEST_ARG     0xBCu       /* what the camera's wall test passes first */
#define CW_MODE         0x208u
#define CW_DISTANCE     0x230u      /* the eye's distance, read by the skater's fade */
/* the wall test, as the follow camera makes it */
#define CW_WORLD        0x0022FCE8u /* the collision world (the test's this) */
#define CW_STOPS        0x33B7u     /* what it stops at */
#define CW_HIT_A        0x2Cu       /* the hit point, in a hit from the record's first list */
#define CW_HIT_B        0x50u       /* ...and from its second */
/* the graphics device the camera is set on: where the view starts and how
 * wide it is (the title sets both once, at start) */
#define CW_DEV_VTABLE   0x001E0F00u
#define CW_DEV_NEAR     0x40u
#define CW_DEV_TAN      0x54u       /* tan of half the 4:3 view's width */

#define CW_HOLD   20                /* frames before going back out */
#define CW_EASE   0.1               /* then this share of the way a frame */
#define CW_TRIES  3

static int cw_on = -1, cw_log;

static int cw_enabled(void)
{
    if (cw_on < 0) {
        const char *e = getenv("RECOMP_CAM_WALLS");
        cw_on = !(e && e[0] == '0');
        e = getenv("RECOMP_CAM_WALLS_LOG");
        cw_log = e && e[0] == '1';
        fprintf(stderr, "[CAMWALL] camera kept out of walls: %s\n",
                cw_on ? "on (RECOMP_CAM_WALLS=0 for off)" : "off (RECOMP_CAM_WALLS=0)");
    }
    return cw_on;
}

static int cw_ok(uint32_t va, uint32_t n)
{
    uint64_t end = (uint64_t)va + n;
    return va >= 0x10000u && end <= xbox_GetMappedSize();
}

/* ---- the game's line test ------------------------------------------------ */

/* Scratch on the guest stack: the hit's pointer, the hit record, the ends. */
#define SCR_RESULT  0x00u
#define SCR_RECORD  0x40u
#define SCR_A       0xC0u
#define SCR_B       0xCCu
#define SCR_SIZE    0x100u

static uint32_t cw_call(void (*fn)(void), uint32_t this_, int n, const uint32_t *args)
{
    uint32_t r[10] = { g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp,
                       (uint32_t)g_df };
    double fp[8];
    int top = g_fp_top, cmp = g_fp_cmp, i;
    uint16_t cc = g_fp_cc, cw = g_fp_control_word;
    uint32_t ret;
    memcpy(fp, g_fp_stack, sizeof fp);
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0;                   /* a return address for its ret */
    g_ecx = this_;
    fn();
    ret = g_eax;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_esp = r[3]; g_ebx = r[4];
    g_esi = r[5]; g_edi = r[6]; g_ebp = r[7]; g_seh_ebp = r[8]; g_df = (int)r[9];
    memcpy(g_fp_stack, fp, sizeof fp);
    g_fp_top = top; g_fp_cmp = cmp; g_fp_cc = cc; g_fp_control_word = cw;
    return ret;
}

typedef struct { uint32_t cam, scr; int calls; } CwCtx;

/* How much of the line from a to b is open: 1 clear, else the share up to
 * the nearest surface. */
static double cw_line(CwCtx *c, const double a[3], const double b[3])
{
    uint32_t args[6], hit, second;
    float pa[3], pb[3];
    double ab2 = 0, along = 0;
    int k;
    c->calls++;
    for (k = 0; k < 3; k++) { pa[k] = (float)a[k]; pb[k] = (float)b[k]; }
    memcpy((void *)XBOX_PTR(c->scr + SCR_A), pa, 12);
    memcpy((void *)XBOX_PTR(c->scr + SCR_B), pb, 12);
    args[0] = c->cam + CW_TEST_ARG;
    args[1] = c->scr + SCR_A;
    args[2] = c->scr + SCR_B;
    args[3] = c->scr + SCR_RECORD;
    args[4] = CW_STOPS;
    args[5] = 0xFFFFFFFFu;
    if (!cw_call(sub_00018260, CW_WORLD, 6, args))
        return 1.0;                     /* nothing in the way */
    MEM32(c->scr + SCR_RESULT) = 0;
    args[0] = c->scr + SCR_RECORD;
    args[1] = c->scr + SCR_RESULT;
    second = cw_call(sub_000194B0, CW_WORLD, 2, args);
    hit = MEM32(c->scr + SCR_RESULT);
    if (!cw_ok(hit, CW_HIT_B + 12))
        return 1.0;
    {
        const float *p = (const float *)XBOX_PTR(hit + (second ? CW_HIT_B : CW_HIT_A));
        for (k = 0; k < 3; k++) {
            ab2 += (b[k] - a[k]) * (b[k] - a[k]);
            along += (p[k] - a[k]) * (b[k] - a[k]);
        }
    }
    if (!(ab2 > 0))
        return 1.0;
    along /= ab2;
    return along < 0 ? 0 : along > 1 ? 1 : along;
}

/* ---- the view ------------------------------------------------------------ */

/* Where the view starts: `ahead` in front of the eye, `hw` by `hh` either
 * side. The camera device is looked for in memory once (its vtable first,
 * then plausible values), and checked each time after. */
static uint32_t cw_dev;

static int cw_dev_fields(uint32_t d, float *near_, float *tan_)
{
    float n, t;
    if (!cw_ok(d, CW_DEV_TAN + 4) || MEM32(d) != CW_DEV_VTABLE)
        return 0;
    n = MEMF(d + CW_DEV_NEAR);
    t = MEMF(d + CW_DEV_TAN);
    if (!(n > 0.001f && n < 100.0f && t > 0.05f && t < 10.0f))
        return 0;
    *near_ = n;
    *tan_ = t;
    return 1;
}

static int cw_view(double *ahead, double *hw, double *hh)
{
    static DWORD next_scan;
    float n, t;
    double k;
    if (!cw_dev_fields(cw_dev, &n, &t)) {
        DWORD now = GetTickCount();
        uint32_t va, end = (uint32_t)xbox_GetMappedSize();
        if ((int32_t)(now - next_scan) < 0)
            return 0;
        next_scan = now + 2000;
        cw_dev = 0;
        for (va = 0x10000u; va + CW_DEV_TAN + 4 <= end; va += 4)
            if (MEM32(va) == CW_DEV_VTABLE && cw_dev_fields(va, &n, &t)) {
                cw_dev = va;
                break;
            }
        if (!cw_dev)
            return 0;
        fprintf(stderr, "[CAMWALL] camera device %08X: the view starts %.2f ahead, %.0f degrees across (4:3)\n",
                cw_dev, n, 2.0 * atan(t) * 57.29578);
    }
    k = xbox_WidescreenK();
    if (!(k > 0.2 && k < 1.0))
        k = 1.0;
    *ahead = n;
    *hw = n * t / k;
    *hh = n * t * 0.75;
    return 1;
}

/* ---- keeping the eye out ------------------------------------------------- */

typedef struct {
    double tgt[3], u[3], d;             /* the point looked at; eye - tgt; its length */
    double f[3], r[3], up[3];           /* forward, right, up */
    double ahead, hw, hh;
} CwView;

static int cw_frame(CwView *v, const float tgt[3], const float eye[3])
{
    double rl;
    int k;
    for (k = 0; k < 3; k++) {
        v->tgt[k] = tgt[k];
        v->u[k] = eye[k] - tgt[k];
    }
    v->d = sqrt(v->u[0] * v->u[0] + v->u[1] * v->u[1] + v->u[2] * v->u[2]);
    if (!(v->d > 1e-3))
        return 0;
    for (k = 0; k < 3; k++) v->f[k] = -v->u[k] / v->d;
    rl = sqrt(v->f[0] * v->f[0] + v->f[2] * v->f[2]);
    if (!(rl > 1e-6))
        return 0;                       /* looking straight up or down */
    v->r[0] = v->f[2] / rl; v->r[1] = 0; v->r[2] = -v->f[0] / rl;
    v->up[0] = v->f[1] * v->r[2] - v->f[2] * v->r[1];
    v->up[1] = v->f[2] * v->r[0] - v->f[0] * v->r[2];
    v->up[2] = v->f[0] * v->r[1] - v->f[1] * v->r[0];
    return 1;
}

/* A point of the view's start: the eye at share s of its distance, moved
 * across by p, then `ahead` forward and (x, y) of the rectangle. With
 * front = 0, at the eye's own place instead. */
static void cw_point(const CwView *v, double s, const double p[3], int front, double x, double y,
                     double out[3])
{
    int k;
    for (k = 0; k < 3; k++)
        out[k] = v->tgt[k] + s * v->u[k] + (p ? p[k] : 0) + (front ? v->ahead * v->f[k] : 0)
                 + x * v->r[k] + y * v->up[k];
}

static const double cw_cx[4] = { -1, 1, -1, 1 }, cw_cy[4] = { -1, -1, 1, 1 };

/* Lines from the middle of the view's start to its corners: the least of
 * them, each kept in t. */
static double cw_cuts(CwCtx *c, const CwView *v, double s, const double p[3], double t[4])
{
    double mid[3], cor[3], least = 1.0;
    int i;
    cw_point(v, s, p, 1, 0, 0, mid);
    for (i = 0; i < 4; i++) {
        cw_point(v, s, p, 1, cw_cx[i] * v->hw, cw_cy[i] * v->hh, cor);
        t[i] = cw_line(c, mid, cor);
        if (t[i] < least) least = t[i];
    }
    return least;
}

enum { BY_NONE, BY_EYE, BY_ACROSS, BY_IN, BY_FLOOR, BY_STUCK };
static const char *const cw_by_text[] = {
    "nothing in the way", "the eye's line", "moved across", "brought in",
    "as near as it may come", "nothing clears it (left as it was)" };

/* How far out the eye may be (a share of its distance) and how far across
 * it moves there. own_line: test the eye's line as it is (the right stick's
 * eye); else only when what it meets hides the whole view. */
static double cw_clear(CwCtx *c, const CwView *v, int own_line, double push[3], int *by)
{
    double t[4], p[3] = { 0, 0, 0 }, end[3], s0, floor_, reach;
    int i, k, n;
    push[0] = push[1] = push[2] = 0;
    *by = BY_NONE;
    for (k = 0; k < 3; k++) end[k] = v->tgt[k] + v->u[k];
    s0 = cw_line(c, v->tgt, end);
    if (s0 < 1 && !own_line) {
        /* The game put the eye here with its own test: leave it unless no
         * line beside the eye's, a view's width across, gets through either:
         * the eye is behind a surface wider than the view. */
        for (i = 0; i < 4 && s0 < 1; i++) {
            double a[3], b[3];
            for (k = 0; k < 3; k++) {
                double side = cw_cx[i] * v->hw * v->r[k] + cw_cy[i] * v->hh * v->up[k];
                a[k] = v->tgt[k] + side;
                b[k] = end[k] + v->ahead * v->f[k] + side;
            }
            if (cw_line(c, a, b) >= 1)
                s0 = 1;
        }
    }
    if (s0 < 1)
        *by = BY_EYE;
    /* Never so near that the view starts at or behind the point looked at. */
    reach = sqrt(v->ahead * v->ahead + v->hw * v->hw + v->hh * v->hh);
    floor_ = (v->ahead + reach) / v->d;
    if (floor_ >= 1)
        return 1;                       /* the game's own eye that near: left */
    if (s0 < floor_) {
        *by = BY_FLOOR;
        return floor_;
    }
    if (cw_cuts(c, v, s0, NULL, t) >= 1)
        return s0;
    /* Across: away from each corner that is cut, by how far short of it the
     * surface is. */
    for (n = 0; n < CW_TRIES; n++) {
        double step[3] = { 0, 0, 0 }, mid[3];
        for (i = 0; i < 4; i++)
            if (t[i] < 1)
                for (k = 0; k < 3; k++)
                    step[k] -= (1 - t[i]) * (cw_cx[i] * v->hw * v->r[k] + cw_cy[i] * v->hh * v->up[k]);
        if (step[0] == 0 && step[1] == 0 && step[2] == 0)
            break;                      /* cut alike on both sides */
        for (k = 0; k < 3; k++) p[k] += step[k];
        cw_point(v, s0, p, 1, 0, 0, mid);
        if (cw_line(c, v->tgt, mid) < 1)
            break;                      /* the move itself goes into something */
        if (cw_cuts(c, v, s0, p, t) >= 1) {
            memcpy(push, p, sizeof p);
            *by = BY_ACROSS;
            return s0;
        }
    }
    /* In: until lines from the point looked at to the corners are clear. */
    {
        double s = s0;
        for (n = 0; n < CW_TRIES && s >= floor_; n++) {
            double open = 1.0, cor[3];
            for (i = 0; i < 4; i++) {
                double o;
                cw_point(v, s, NULL, 1, cw_cx[i] * v->hw, cw_cy[i] * v->hh, cor);
                o = cw_line(c, v->tgt, cor);
                if (o < open) open = o;
            }
            if (open >= 1) {
                *by = BY_IN;
                return s;
            }
            s = s * open - (1 - open) * reach / v->d;
        }
    }
    *by = BY_STUCK;
    return s0;
}

/* ---- from frame to frame ------------------------------------------------- */

typedef struct {
    uint32_t cam, mode, seen;
    double share, push[3];
    int share_wait, push_wait;
} CwKeep;

#define CW_CAMS 4
static CwKeep cw_keep[CW_CAMS];

static CwKeep *cw_state(uint32_t cam, uint32_t mode)
{
    static uint32_t now;
    int i, old = 0;
    now++;
    for (i = 0; i < CW_CAMS; i++) {
        if (cw_keep[i].cam == cam && cw_keep[i].seen) break;
        if (cw_keep[i].seen < cw_keep[old].seen) old = i;
    }
    if (i == CW_CAMS) {
        i = old;
        cw_keep[i].cam = cam;
        cw_keep[i].mode = ~mode;
    }
    if (cw_keep[i].mode != mode || now - cw_keep[i].seen > 30) {
        /* a new mode, or not seen for half a second: start again */
        cw_keep[i].mode = mode;
        cw_keep[i].share = 1;
        cw_keep[i].share_wait = CW_HOLD;
        memset(cw_keep[i].push, 0, sizeof cw_keep[i].push);
        cw_keep[i].push_wait = CW_HOLD;
    }
    cw_keep[i].seen = now;
    return &cw_keep[i];
}

/* In at once; out after the hold, by degrees. */
static double cw_ease_share(CwKeep *w, double want)
{
    if (want < w->share) {
        w->share = want;
        w->share_wait = 0;
    } else if (w->share_wait < CW_HOLD) {
        w->share_wait++;
    } else {
        w->share += (want - w->share) * CW_EASE;
        if (want - w->share < 1e-4) w->share = want;
    }
    return w->share;
}

/* Across the same way: last frame's move kept while it still clears the
 * view (or what is needed now, when it doesn't), then eased to what is
 * needed. */
static void cw_ease_push(CwCtx *c, CwKeep *w, const CwView *v, double s, const double need[3],
                         double out[3])
{
    double t[4], diff = 0;
    int k, needed = need[0] != 0 || need[1] != 0 || need[2] != 0;
    for (k = 0; k < 3; k++) diff += fabs(need[k] - w->push[k]);
    if (diff < 1e-6) {
        w->push_wait = CW_HOLD;
    } else if (needed && cw_cuts(c, v, s, w->push, t) < 1) {
        memcpy(w->push, need, sizeof w->push);   /* last frame's no longer clears it */
        w->push_wait = 0;
    } else if (w->push_wait < CW_HOLD) {
        w->push_wait++;
    } else {
        for (k = 0; k < 3; k++) w->push[k] += (need[k] - w->push[k]) * CW_EASE;
        if (fabs(w->push[0]) + fabs(w->push[1]) + fabs(w->push[2]) < 1e-4 && !needed)
            memset(w->push, 0, sizeof w->push);
    }
    memcpy(out, w->push, sizeof w->push);
}

/* ---- the hook ------------------------------------------------------------ */

/* After the camera's publication (and the right stick's turn): the drawn
 * eye kept out of walls. own_line: the eye was turned by the right stick,
 * so its own line is tested as it is. Returns 1 when the eye moved. */
int jsrf_camwall_apply(uint32_t cam, int own_line)
{
    float *eye, *tgt;
    CwView v;
    CwCtx c;
    CwKeep *w;
    double need[3], push[3], open, s, there;
    uint32_t mode;
    int by, k;
    if (!cw_enabled() || !cw_ok(cam, 0x240u))
        return 0;
    mode = MEM32(cam + CW_MODE);
    if (mode != 3u && mode != 4u && mode != 5u && mode != 9u)
        return 0;                       /* the follow camera's modes only */
    eye = (float *)XBOX_PTR(cam + CW_EYE);
    tgt = (float *)XBOX_PTR(cam + CW_TARGET);
    if (!cw_frame(&v, tgt, eye) || !cw_view(&v.ahead, &v.hw, &v.hh))
        return 0;
    w = cw_state(cam, mode);
    c.cam = cam;
    c.calls = 0;
    g_esp -= SCR_SIZE;
    c.scr = g_esp;
    open = cw_clear(&c, &v, own_line, need, &by);
    s = cw_ease_share(w, open);
    if (s < open) {
        /* still in from before: what the view needs there */
        double scaled[3];
        CwView vs = v;
        for (k = 0; k < 3; k++) vs.u[k] = v.u[k] * s;
        vs.d = v.d * s;
        there = cw_clear(&c, &vs, 1, scaled, &by);
        if (there < 1) s = cw_ease_share(w, s * there);
        memcpy(need, scaled, sizeof need);
    }
    cw_ease_push(&c, w, &v, s, need, push);
    g_esp += SCR_SIZE;
    if (s >= 1 && push[0] == 0 && push[1] == 0 && push[2] == 0)
        return 0;
    for (k = 0; k < 3; k++)
        eye[k] = (float)(v.tgt[k] + v.u[k] * s + push[k]);
    {
        double dx = eye[0] - tgt[0], dy = eye[1] - tgt[1], dz = eye[2] - tgt[2];
        MEMF(cam + CW_DISTANCE) = (float)sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (cw_log) {
        static DWORD next;
        DWORD now = GetTickCount();
        if ((int32_t)(now - next) >= 0) {
            next = now + 1000;
            fprintf(stderr, "[CAMWALL] camera %08X mode %u: eye at %.0f%% of %.1f, %.2f across, %s%s (%d line tests)\n",
                    cam, mode, s * 100, v.d,
                    sqrt(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]),
                    cw_by_text[by], s > open ? ", easing back out" : "", c.calls);
        }
    }
    return 1;
}
