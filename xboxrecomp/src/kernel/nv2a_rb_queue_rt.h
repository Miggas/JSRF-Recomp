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

typedef struct { uint32_t op, len; uint64_t pad; } RbqHdr;   /* 16 bytes */

static const RbOps *q_in;                /* the backend underneath */
static int q_on;                         /* the thread is running */
static uint8_t *q_buf;
static uint64_t q_wl;                    /* written (writer's own) */
/* each on a cache line of its own: one side writes, the other reads */
static _Alignas(64) _Atomic uint64_t q_w;     /* published to the reader */
static _Alignas(64) _Atomic uint64_t q_done;  /* made by the reader */
static _Alignas(64) _Atomic int q_sleep;
static _Alignas(64) _Atomic int q_dwait;
static SDL_sem *q_wake, *q_dsem;

/* counters (the writer's; the reader's busy time is atomic) */
static _Atomic uint64_t q_busy_t;        /* the reader making calls, TM ticks */
static uint64_t q_n_async, q_bytes, q_n_drain, q_drain_t, q_n_space;
static uint64_t q_drain_by[RB_NCALLS_Q], q_drain_t_by[RB_NCALLS_Q];

static void q_exec(const RbqHdr *h);     /* generated */

static int q_thread(void *u)
{
    uint64_t r = 0;
    (void)u;
    for (;;) {
        uint64_t w = atomic_load_explicit(&q_w, memory_order_acquire);
        if (r == w) {
            int spin;
            for (spin = 0; spin < 4000; spin++) {      /* ~20-60 us */
                RBQ_PAUSE();
                if (atomic_load_explicit(&q_w, memory_order_acquire) != r)
                    break;
            }
            if (atomic_load_explicit(&q_w, memory_order_acquire) != r)
                continue;
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

static void q_publish(void)
{
    q_unpub = 0;
    atomic_store_explicit(&q_w, q_wl, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst); /* the store before the look at q_sleep */
    if (atomic_load_explicit(&q_sleep, memory_order_relaxed) && atomic_exchange(&q_sleep, 0))
        SDL_SemPost(q_wake);
}

/* wait until the reader has made every call written so far */
static void q_drain(int op)
{
    uint64_t t0;
    int spin;
    if (q_unpub)
        q_publish();
    if (atomic_load_explicit(&q_done, memory_order_acquire) == q_wl)
        return;
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
        q_publish();
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

#endif
