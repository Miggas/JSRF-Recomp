/* nv2a_rb_queue_rt.h: the recording thread's queue (vk13), included by the
 * generated nv2a_rb_queue.c.
 *
 * The renderer's backend calls that return nothing and need nothing back
 * (state, uniforms, bindings, draws, clears, copies) are written into a
 * ring with their arguments -- and a copy of the data their pointers point
 * at -- and made by a thread of their own, in order. Every other call first
 * waits until that thread has made all the calls before it ("drain"), then
 * runs on the caller's thread as it always did. So the backend is used by
 * one thread at a time, and what a call sees is what it saw before.
 *
 * One writer (the renderer's thread), one reader (the recording thread).
 * Positions are byte counts that only grow; the ring index is pos % size. */
#ifndef NV2A_RB_QUEUE_RT_H
#define NV2A_RB_QUEUE_RT_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <SDL.h>
#include "nv2a_tm.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <immintrin.h>
#define RBQ_PAUSE() _mm_pause()
#else
#define RBQ_PAUSE() ((void)0)
#endif

#define RBQ_SIZE      (16u << 20)        /* the ring */
#define RBQ_MAX_COPY  (1u << 20)         /* larger data: the call waits and runs here */
#define RBQ_OP_WRAP   0xFFFFu
#define RBQ_WAKE      (64u << 10)        /* a sleeping reader is woken for this much */

typedef struct { uint32_t op, len; uint64_t pad; } RbqHdr;   /* 16 bytes */

static const RbOps *q_in;                /* the backend underneath */
static int q_on;                         /* the thread is running */
static uint8_t *q_buf;
static uint64_t q_wl;                    /* written (writer's own) */
/* each on a cache line of its own: one side writes, the other reads */
static _Alignas(64) _Atomic uint64_t q_w;     /* published to the reader */
static _Alignas(64) _Atomic uint64_t q_done;  /* made by the reader */
static _Alignas(64) _Atomic int q_sleep;
static _Atomic uint64_t q_sleep_at;           /* where the reader was when it slept */
static _Alignas(64) _Atomic int q_dwait;
static SDL_sem *q_wake, *q_dsem;

/* counters (the writer's; the reader's busy time is atomic) */
static _Atomic uint64_t q_busy_t;        /* the reader making calls, TM ticks */
static uint64_t q_n_async, q_bytes, q_n_drain, q_drain_t, q_n_space;
static uint64_t q_drain_by[RB_NCALLS_Q], q_drain_t_by[RB_NCALLS_Q];

static void q_exec(const RbqHdr *h);     /* generated */
static void q_exec_own(const RbqHdr *h);  /* below: the calls written here */

static int q_thread(void *u)
{
    uint64_t r = 0;
    (void)u;
    {   /* RECOMP_PROFILE samples this thread too ([PROF] VK recording) */
        extern void (*g_thread_watch_hook)(const char *name);
        if (g_thread_watch_hook)
            g_thread_watch_hook("VK recording");
    }
    for (;;) {
        uint64_t w = atomic_load_explicit(&q_w, memory_order_acquire);
        if (r == w) {
            int spin;
            /* vk14: hardly a spin, then sleep until there is RBQ_WAKE to do
             * or something waits (vk13 spun ~50 us each time: 85% of the
             * thread's time on the Deck, a core's worth of power) */
            for (spin = 0; spin < 32; spin++) {
                RBQ_PAUSE();
                if (atomic_load_explicit(&q_w, memory_order_acquire) != r)
                    break;
            }
            if (atomic_load_explicit(&q_w, memory_order_acquire) != r)
                continue;
            atomic_store_explicit(&q_sleep_at, r, memory_order_relaxed);
            atomic_store(&q_sleep, 1);
            if (atomic_load(&q_w) != r) {
                if (atomic_exchange(&q_sleep, 0))
                    continue;
                SDL_SemWait(q_wake);             /* the writer posted: take it */
                continue;
            }
            SDL_SemWait(q_wake);
            continue;
        }
        {
            uint64_t t0 = TM_NOW();
            unsigned k = 0;
            while (r != w) {
                const RbqHdr *h = (const RbqHdr *)(q_buf + (r % RBQ_SIZE));
                if (h->op == RBQ_OP_WRAP)
                    r += RBQ_SIZE - (r % RBQ_SIZE);
                else {
                    q_exec(h);
                    r += h->len;
                }
                if (++k % 64 == 0)              /* not every call: the writer reads it */
                    atomic_store_explicit(&q_done, r, memory_order_release);
            }
            atomic_store_explicit(&q_done, r, memory_order_release);
            atomic_fetch_add_explicit(&q_busy_t, TM_NOW() - t0, memory_order_relaxed);
        }
        if (atomic_load(&q_dwait) && atomic_load(&q_w) == r && atomic_exchange(&q_dwait, 0))
            SDL_SemPost(q_dsem);
    }
    return 0;
}

static unsigned q_unpub;                  /* calls written, not yet published */

/* the calls written so far to the reader; a sleeping reader is woken when
 * there is RBQ_WAKE to do (fewer wake-ups: each costs both threads a system
 * call), or always (force: something waits for it) */
static void q_publish_w(int force)
{
    q_unpub = 0;
    atomic_store_explicit(&q_w, q_wl, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst); /* the store before the look at q_sleep */
    if (atomic_load_explicit(&q_sleep, memory_order_relaxed)
            && (force || q_wl - atomic_load_explicit(&q_sleep_at, memory_order_relaxed) >= RBQ_WAKE)
            && atomic_exchange(&q_sleep, 0))
        SDL_SemPost(q_wake);
}
#define q_publish() q_publish_w(1)

/* wait until the reader has made every call written so far */
static void q_drain(int op)
{
    uint64_t t0;
    int spin;
    if (atomic_load_explicit(&q_done, memory_order_acquire) == q_wl)
        return;
    q_publish();                                /* and wake the reader, whatever is left */
    t0 = TM_NOW();
    q_n_drain++;
    if (op >= 0 && op < RB_NCALLS_Q) q_drain_by[op]++;
    for (spin = 0; spin < 20000; spin++) {
        RBQ_PAUSE();
        if (atomic_load_explicit(&q_done, memory_order_acquire) == q_wl)
            goto done;
    }
    atomic_store(&q_dwait, 1);
    if (atomic_load(&q_done) == q_wl) {
        if (!atomic_exchange(&q_dwait, 0))
            SDL_SemWait(q_dsem);                 /* the reader posted: take it */
    } else
        SDL_SemWait(q_dsem);
done:
    {
        uint64_t dt = TM_NOW() - t0;
        q_drain_t += dt;
        if (op >= 0 && op < RB_NCALLS_Q) q_drain_t_by[op] += dt;
    }
}

/* room for len bytes (a multiple of 16) at the write position */
static uint64_t q_lim = RBQ_SIZE;          /* the writer may write up to here
                                            * (q_done + size when last looked:
                                            * not on every call, the reader
                                            * keeps writing q_done) */
static void q_room(uint64_t len)
{
    if (q_wl + len <= q_lim)
        return;
    q_lim = atomic_load_explicit(&q_done, memory_order_acquire) + RBQ_SIZE;
    if (q_wl + len <= q_lim)
        return;
    q_n_space++;
    q_publish();
    while (q_wl + len > (q_lim = atomic_load_explicit(&q_done, memory_order_acquire) + RBQ_SIZE))
        SDL_Delay(0);
}

static void *q_begin(uint32_t op, size_t body)
{
    uint64_t len = ((uint64_t)sizeof(RbqHdr) + body + 15) & ~(uint64_t)15;
    uint64_t pos = q_wl % RBQ_SIZE;
    RbqHdr *h;
    if (pos + len > RBQ_SIZE) {                 /* doesn't fit before the end: wrap */
        q_room(RBQ_SIZE - pos);
        ((RbqHdr *)(q_buf + pos))->op = RBQ_OP_WRAP;
        q_wl += RBQ_SIZE - pos;
        pos = 0;
    }
    q_room(len);
    h = (RbqHdr *)(q_buf + pos);
    h->op = op;
    h->len = (uint32_t)len;
    q_wl += len;                                /* published by q_end, once written */
    q_n_async++;
    q_bytes += len;
    return h + 1;
}

/* published every 8 calls (and before any wait): the reader keeps up
 * without a fence on every call */
static inline void q_end(void)
{
    if (++q_unpub >= 8)
        q_publish_w(0);
}

static int q_start(void)
{
    SDL_Thread *t;
    const char *e = getenv("RECOMP_VK_RECORD_THREAD");
    if (e && e[0] == '0')
        return 0;
    q_buf = (uint8_t *)malloc(RBQ_SIZE);
    q_wake = SDL_CreateSemaphore(0);
    q_dsem = SDL_CreateSemaphore(0);
    if (!q_buf || !q_wake || !q_dsem)
        return 0;
    t = SDL_CreateThread(q_thread, "vk record", NULL);
    if (!t)
        return 0;
    SDL_DetachThread(t);
    q_on = 1;
    return 1;
}


/* ── vk17: calls answered on the renderer's side ────────────────────────── */

/* The scissor the renderer last set (rb_scissor_get asked for it twice a
 * frame, and each time waited for the recording thread): mirrored from the
 * calls that change the backend's own copy. -1s: not known (as there). */
static int q_sc[4] = { -1, -1, -1, -1 };
static void q_sc_note(int x, int y, int w, int h) { q_sc[0] = x; q_sc[1] = y; q_sc[2] = w; q_sc[3] = h; }
static void q_sc_forget(void) { q_sc[0] = q_sc[1] = q_sc[2] = q_sc[3] = -1; }
static void q_scissor_get(int sc[4])
{
    if (!q_on) { q_in->scissor_get(sc); return; }
    memcpy(sc, q_sc, sizeof q_sc);
}

/* Fences made in the queue, in order with the draws (rb_fence waited for
 * the recording thread, 2000 times every 10 s in the busiest places: the
 * vertex rings change segment). The renderer gets a stand-in at once; the
 * recording thread fills in the backend's fence when it gets there. A wait
 * on one the backend already knows passed (a frame or two old: what the
 * rings wait for) needs nothing of the recording thread; any other waits
 * for it to catch up and asks the backend as before. */
#define Q_OP_FENCE      (RB_NCALLS_Q + 0)
#define Q_OP_FENCE_DEL  (RB_NCALLS_Q + 1)
#define Q_OP_RESOLVE    (RB_NCALLS_Q + 2)
#define Q_ALREADY_SIGNALED 0x911Au           /* GL_ALREADY_SIGNALED */
typedef struct { void *real; _Atomic int made; } QFence;
typedef struct { QFence *f; uint32_t cond, flags; } QA_qfence;
static uint64_t q_n_fence_fast;

static void *q_fence(uint32_t cond, uint32_t flags)
{
    QFence *f = (QFence *)malloc(sizeof *f);
    QA_qfence *a;
    if (!f)
        return NULL;
    if (!q_on) {
        f->real = q_in->fence(cond, flags);
        atomic_store(&f->made, 1);
        return f;
    }
    f->real = NULL;
    atomic_store(&f->made, 0);
    a = (QA_qfence *)q_begin(Q_OP_FENCE, sizeof *a);
    a->f = f; a->cond = cond; a->flags = flags;
    q_end();
    return f;
}
static uint32_t q_fence_wait(void *h, uint32_t flags, uint64_t timeout_ns)
{
    QFence *f = (QFence *)h;
    if (!f)
        return Q_ALREADY_SIGNALED;
    if (atomic_load_explicit(&f->made, memory_order_acquire) && q_in->fence_signaled(f->real)) {
        q_n_fence_fast++;
        return Q_ALREADY_SIGNALED;
    }
    if (q_on) q_drain(Q_IX_fence_wait);
    return q_in->fence_wait(f->real, flags, timeout_ns);
}
static int q_fence_signaled(void *h)
{
    QFence *f = (QFence *)h;
    return f && atomic_load_explicit(&f->made, memory_order_acquire) && q_in->fence_signaled(f->real);
}
static void q_fence_delete(void *h)
{
    QFence *f = (QFence *)h;
    QA_qfence *a;
    if (!f)
        return;
    if (!q_on) {
        q_in->fence_delete(f->real);
        free(f);
        return;
    }
    /* after the fence is made, in the queue's order */
    a = (QA_qfence *)q_begin(Q_OP_FENCE_DEL, sizeof *a);
    a->f = f; a->cond = a->flags = 0;
    q_end();
}

/* vk17b: rb_resolve answers with the target that holds the resolved image
 * -- always the same one (the backend makes it the first time) -- so after
 * the first, it is known here and the resolve itself goes in the queue (it
 * waited for the recording thread twice a frame with MSAA). */
typedef struct { uint32_t fbo, tex; int w, h; } QA_qresolve;
/* v312h: rb_program_ready, asked for at most draws while a shader is being
 * built in the background, waited each time for the recording thread to
 * catch up (an HD 620 laptop: 0.5 a draw, 1-2 ms each, so 3-14 fps while
 * its one builder worked through 40 shaders). Its answer of 0 can only
 * change once a build finishes, so while rb_program_gen stays the same, a
 * handle last answered 0 is answered 0 again here, without waiting. */
#define Q_PR_N 512u
static struct { uint32_t id, gen; } q_pr[Q_PR_N];
static int q_program_ready(uint32_t *id)
{
    uint32_t was = *id, k = (was * 2654435761u) >> 23, g;
    int r;
    if (!q_on)
        return q_in->program_ready(id);
    g = q_in->program_gen();             /* before the wait: a build done meanwhile is asked again */
    if (was && q_pr[k].id == was && q_pr[k].gen == g)
        return 0;
    q_drain(Q_IX_program_ready);
    r = q_in->program_ready(id);
    if (r == 0) { q_pr[k].id = was; q_pr[k].gen = g; }
    else if (q_pr[k].id == was) q_pr[k].id = 0;
    return r;
}

static uint32_t q_resolve_fbo;
static uint32_t q_resolve(uint32_t fbo, uint32_t tex, int w, int h)
{
    QA_qresolve *a;
    if (!q_on || !q_resolve_fbo) {
        if (q_on) q_drain(Q_IX_resolve);
        return q_resolve_fbo = q_in->resolve(fbo, tex, w, h);
    }
    a = (QA_qresolve *)q_begin(Q_OP_RESOLVE, sizeof *a);
    a->fbo = fbo; a->tex = tex; a->w = w; a->h = h;
    q_end();
    return q_resolve_fbo;
}

static void q_exec_own(const RbqHdr *h)
{
    const QA_qfence *a = (const QA_qfence *)(h + 1);
    switch (h->op) {
    case Q_OP_RESOLVE: {
        const QA_qresolve *r = (const QA_qresolve *)(h + 1);
        q_in->resolve(r->fbo, r->tex, r->w, r->h);
        break;
    }
    case Q_OP_FENCE:
        a->f->real = q_in->fence(a->cond, a->flags);
        atomic_store_explicit(&a->f->made, 1, memory_order_release);
        break;
    case Q_OP_FENCE_DEL:
        q_in->fence_delete(a->f->real);
        free(a->f);
        break;
    default:
        break;
    }
}

#endif
