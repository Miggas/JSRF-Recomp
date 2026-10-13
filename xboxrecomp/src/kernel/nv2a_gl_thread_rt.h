/* v172: the GL thread.
 *
 * The NV2A executor used to make every OpenGL call itself, and under Proton
 * each one is a crossing into Wine's unix side and the Mesa driver: about
 * 1.3 s of every 10 in the busy street, plus the present. Now everything in
 * nv2a_gl.c still runs on the executor at the moment it ran before -- state
 * decisions, texture checks and decoding, vertex copies into the rings -- but
 * the GL calls it makes are written into a queue (a byte ring, one writer and
 * one reader) and made by a thread of their own that owns the context.
 *
 *  - A call with no result is queued: its arguments by value, the memory it
 *    reads (uniform values, texture pixels) copied in with it, so the caller
 *    may reuse that memory at once.
 *  - A call with a result, or one that writes memory (glGet*, glReadPixels,
 *    glMapBufferRange...), waits for the GL thread to reach it and make it.
 *    None of these are on the per-draw path.
 *  - Fences are virtual: glFenceSync returns at once, the GL thread makes the
 *    real fence when it gets there and watches it; glClientWaitSync waits
 *    for that. The vertex and index rings' reuse goes through these, so the
 *    queue can never run more than a ring ahead of the GPU.
 *  - glGen* names come from pools refilled 64 at a time.
 *  - The queue holds at most RECOMP_GL_THREAD_LAG frames (default 0 since v173): at a
 *    frame's end the executor waits until the GL thread has finished the
 *    frame before the last, so input lag cannot build up.
 *
 * Neither side spins for long: a waiting thread sleeps on a semaphore and is
 * woken by the other (a few microseconds of pause first, as a wake-up costs
 * more than that).
 *
 * RECOMP_GL_THREAD=0 makes the calls on the executor as before. */
#ifndef NV2A_GL_THREAD_RT_H
#define NV2A_GL_THREAD_RT_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define GT_TLS __declspec(thread)
#else
#include <x86intrin.h>
#define GT_TLS __thread
#endif

#define GT_AL8(n) (((size_t)(n) + 7u) & ~(size_t)7u)
#define GT_RING_BYTES (64u << 20)            /* power of two */
#define GT_INLINE_MAX (GT_RING_BYTES / 4)

/* 1: this thread queues its GL calls; 0: calls GL itself */
static GT_TLS int t_gt_mode;
static int s_gt_on;                           /* the GL thread is running */

enum { GT_WRAP = 1, GT_SYNC = 2, GT_FENCE = 3, GT_DELSYNC = 4, GT_CALL = 5,
       GT_FRAME = 6, GT_CALLSYNC = 7 };

static uint8_t *s_gt_buf;
static SDL_atomic_t s_gt_w, s_gt_r;           /* published positions (mod 2^32) */
static uint32_t s_gt_wl;                       /* writer's own position */
static uint32_t s_gt_cmd;                      /* start of the command being written */
static SDL_atomic_t s_gt_cwait, s_gt_pwait;    /* reader / writer asleep */
static SDL_sem *s_gt_csem, *s_gt_psem;
static SDL_atomic_t s_gt_fdone;                /* frames the GL thread has finished */
static uint32_t s_gt_frec;                     /* frames queued */
static int s_gt_lag = 0;   /* v173: 0 (runs 172b on the Deck: as smooth as 1, ~6 ms from finished to shown against ~24) */
static uint64_t s_gt_n_sync, s_gt_n_pwait, s_gt_n_cmds;  /* [PERF] */
static uint64_t s_gt_spin_tsc = 10000;   /* v179: idle spin, rdtsc ticks (set in gt_start) */

static void gt_replay(uint32_t id, uint8_t *p);
static void gt_replay_sync(uint32_t id, void *args, void *ret);
static void gt_replay_custom(uint32_t id, uint8_t *p);
static void gt_replay_sync_custom(uint32_t id, void *args, void *ret);
static GLsync gtv_FenceSync(GLenum cond, GLbitfield flags);
static void gtv_DeleteSync(GLsync s);
static GLenum gtv_ClientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout);
static void gtv_GenTextures(GLsizei n, GLuint *o);
static void gtv_GenFramebuffers(GLsizei n, GLuint *o);
static void gtv_GenRenderbuffers(GLsizei n, GLuint *o);
static void gtv_GenBuffers(GLsizei n, GLuint *o);
static void gtv_GenVertexArrays(GLsizei n, GLuint *o);
static void gtv_GenQueries(GLsizei n, GLuint *o);

static void gt_pause(void) { _mm_pause(); }

/* v179: the queue's flags read and written inline. SDL_AtomicGet/Set are
 * calls into SDL2.dll (a locked instruction each, even for a read), made for
 * every command on both sides: ~18% of the GL thread's samples in run 178's
 * profile were its idle loop and SDL_AtomicGet. A read is now a plain load
 * (x86 loads are acquire); a write stays a full barrier (xchg), which the
 * sleep / wake-up handshake needs -- each side publishes, then checks
 * whether the other went to sleep. */
#if defined(_MSC_VER) && !defined(__clang__)
#define GT_LD(a) ((int)(*(volatile long *)&(a)->value))
#define GT_ST(a, v) ((void)_InterlockedExchange((volatile long *)&(a)->value, (long)(v)))
#else
#define GT_LD(a) __atomic_load_n(&(a)->value, __ATOMIC_ACQUIRE)
#define GT_ST(a, v) __atomic_store_n(&(a)->value, (int)(v), __ATOMIC_SEQ_CST)
#endif

/* the writer waits until cond() holds: a short pause loop, then sleep until
 * the GL thread posts (it posts after each command while a writer waits;
 * the timeout only covers a lost wake-up) */
static void gt_pwait_until(int (*cond)(void *), void *ctx)
{
    int i;
    if (cond(ctx))
        return;
    s_gt_n_pwait++;
    for (i = 0; i < 200; i++) {
        gt_pause();
        if (cond(ctx))
            return;
    }
    for (;;) {
        GT_ST(&s_gt_pwait, 1);
        if (cond(ctx))
            break;
        SDL_SemWaitTimeout(s_gt_psem, 2);
        if (cond(ctx))
            break;
    }
    GT_ST(&s_gt_pwait, 0);
}

static void gt_wake_reader(void)
{
    if (GT_LD(&s_gt_cwait)) {
        GT_ST(&s_gt_cwait, 0);
        SDL_SemPost(s_gt_csem);
    }
}

static void gt_wake_writer(void)
{
    if (GT_LD(&s_gt_pwait)) {
        GT_ST(&s_gt_pwait, 0);
        SDL_SemPost(s_gt_psem);
    }
}

typedef struct { uint32_t need; } GtSpace;
static int gt_space_ok(void *c)
{
    uint32_t used = s_gt_wl - (uint32_t)GT_LD(&s_gt_r);
    return GT_RING_BYTES - used >= ((GtSpace *)c)->need;
}

/* room for a command of `payload` bytes; returns where its payload goes */
static void *gt_begin(uint32_t id, size_t payload)
{
    uint32_t total = (uint32_t)(8 + GT_AL8(payload));
    uint32_t at = s_gt_wl & (GT_RING_BYTES - 1);
    uint32_t to_end = GT_RING_BYTES - at;
    GtSpace sp;
    uint8_t *h;
    if (total > to_end) {
        /* the rest of the ring is skipped (a WRAP the reader steps over) */
        sp.need = to_end + total;
        gt_pwait_until(gt_space_ok, &sp);
        h = s_gt_buf + at;
        ((uint32_t *)h)[0] = GT_WRAP;
        ((uint32_t *)h)[1] = to_end;
        s_gt_wl += to_end;
        at = 0;
    } else {
        sp.need = total;
        gt_pwait_until(gt_space_ok, &sp);
    }
    h = s_gt_buf + at;
    ((uint32_t *)h)[0] = id;
    ((uint32_t *)h)[1] = total;
    s_gt_cmd = total;
    return h + 8;
}

static void gt_end(void)
{
    s_gt_wl += s_gt_cmd;
    s_gt_n_cmds++;
    GT_ST(&s_gt_w, (int)s_gt_wl);
    gt_wake_reader();
}

static int gt_flag_set(void *c) { return GT_LD((SDL_atomic_t *)c) != 0; }

/* a call made by the GL thread while the writer waits */
typedef struct { uint32_t fid; void *args; void *ret; SDL_atomic_t *done; } GtSyncCmd;
static void gt_sync(uint32_t fid, void *args, void *ret)
{
    SDL_atomic_t done;
    GtSyncCmd *c;
    GT_ST(&done, 0);
    c = (GtSyncCmd *)gt_begin(GT_SYNC, sizeof *c);
    c->fid = fid; c->args = args; c->ret = ret; c->done = &done;
    gt_end();
    s_gt_n_sync++;
    gt_pwait_until(gt_flag_set, &done);
}

/* run fn(arg) on the GL thread: queued with a copy of the n bytes at arg */
typedef void (*gt_fn)(void *arg);
static void gt_call(gt_fn fn, const void *arg, size_t n)
{
    uint8_t *p;
    if (!s_gt_on || t_gt_mode != 1) {
        uint8_t tmp[256];
        void *a = n <= sizeof tmp ? tmp : malloc(n);
        memcpy(a, arg, n);
        fn(a);
        if (a != tmp) free(a);
        return;
    }
    p = (uint8_t *)gt_begin(GT_CALL, 8 + n);
    memcpy(p, &fn, sizeof fn);
    memcpy(p + 8, arg, n);
    gt_end();
}

/* ...and wait for it (arg is used in place) */
typedef struct { gt_fn fn; void *arg; SDL_atomic_t *done; } GtCallSync;
static void gt_call_sync(gt_fn fn, void *arg)
{
    SDL_atomic_t done;
    GtCallSync *c;
    if (!s_gt_on || t_gt_mode != 1) {
        fn(arg);
        return;
    }
    GT_ST(&done, 0);
    c = (GtCallSync *)gt_begin(GT_CALLSYNC, sizeof *c);
    c->fn = fn; c->arg = arg; c->done = &done;
    gt_end();
    s_gt_n_sync++;
    gt_pwait_until(gt_flag_set, &done);
}

/* bytes a glTex(Sub)Image2D reads at unpack alignment 4; -1 when not known */
static size_t gt_tex_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
    size_t bpp = 0, row;
    if ((format == GL_BGRA || format == GL_RGBA)
            && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV))
        bpp = 4;
    if (!bpp || w < 0 || h < 0)
        return (size_t)-1;
    if (!w || !h)
        return 0;
    row = (size_t)w * bpp;
    return (size_t)(h - 1) * ((row + 3) & ~(size_t)3) + row;
}

#endif
