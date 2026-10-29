/* v172: the GL thread, second part (after the generated nv2a_gl_thread.h):
 * virtual fences, name pools, the thread itself. See nv2a_gl_thread_rt.h. */
#ifndef NV2A_GL_THREAD_RT2_H
#define NV2A_GL_THREAD_RT2_H
static void gt_noop(void *arg) { (void)arg; }
/* ── virtual fences ─────────────────────────────────────────────────── */
#define GT_NSYNC 1024
enum { GTS_FREE = 0, GTS_QUEUED, GTS_MADE, GTS_SIGNALED };
typedef struct { GLsync real; SDL_atomic_t state; } GtVSync;
static GtVSync s_gt_sync[GT_NSYNC];
static uint32_t s_gt_sync_cur;
static GtVSync *s_gt_pend[GT_NSYNC];           /* GL thread: made, not signalled */
static int s_gt_npend;
static SDL_atomic_t s_gt_fwaiting;             /* the writer waits for a fence */

static int gt_is_virtual(GLsync s)
{
    return (const GtVSync *)(const void *)s >= s_gt_sync
        && (const GtVSync *)(const void *)s < s_gt_sync + GT_NSYNC;
}

typedef struct { GtVSync *v; GLenum cond; GLbitfield flags; } GtFenceCmd;
static void gt_del_real(void *s) { glad_glDeleteSync((GLsync)s); }
typedef struct { GLsync s; GLbitfield flags; GLuint64 timeout; GLenum r; } GtRealWait;
static void gt_wait_real(void *a)
{
    GtRealWait *w = (GtRealWait *)a;
    w->r = glad_glClientWaitSync(w->s, w->flags, w->timeout);
}
static GLsync gtv_FenceSync(GLenum cond, GLbitfield flags)
{
    GtVSync *v = NULL;
    GtFenceCmd *c;
    uint32_t i;
    for (;;) {
        for (i = 0; i < GT_NSYNC; i++) {
            GtVSync *e = &s_gt_sync[(s_gt_sync_cur + i) % GT_NSYNC];
            if (GT_LD(&e->state) == GTS_FREE) {
                v = e;
                s_gt_sync_cur = (uint32_t)(e - s_gt_sync) + 1;
                break;
            }
        }
        if (v)
            break;
        /* all in use (should not happen: a few per ring): let the GL
         * thread catch up */
        gt_call_sync(gt_noop, NULL);
    }
    GT_ST(&v->state, GTS_QUEUED);
    c = (GtFenceCmd *)gt_begin(GT_FENCE, sizeof *c);
    c->v = v; c->cond = cond; c->flags = flags;
    gt_end();
    return (GLsync)(void *)v;
}

static void gtv_DeleteSync(GLsync s)
{
    if (!s)
        return;
    if (!gt_is_virtual(s)) {
        gt_call_sync(gt_del_real, (void *)s);   /* made before the GL thread started */
        return;
    }
    {
        GtVSync **c = (GtVSync **)gt_begin(GT_DELSYNC, sizeof *c);
        *c = (GtVSync *)(void *)s;
        gt_end();
    }
}

static int gt_signaled(void *c)
{
    return GT_LD(&((GtVSync *)c)->state) == GTS_SIGNALED;
}

static GLenum gtv_ClientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout)
{
    GtVSync *v;
    if (!gt_is_virtual(s)) {
        GtRealWait a;
        a.s = s; a.flags = flags; a.timeout = timeout;
        gt_call_sync(gt_wait_real, &a);
        return a.r;
    }
    v = (GtVSync *)(void *)s;
    if (GT_LD(&v->state) == GTS_SIGNALED)
        return GL_ALREADY_SIGNALED;
    if (!timeout)
        return GL_TIMEOUT_EXPIRED;
    GT_ST(&s_gt_fwaiting, 1);
    gt_wake_reader();
    gt_pwait_until(gt_signaled, v);
    GT_ST(&s_gt_fwaiting, 0);
    return GL_CONDITION_SATISFIED;
}

/* GL thread: the fences made and not yet signalled, oldest first (they
 * signal in order, so the first one not done ends the look). With wait_ns,
 * the writer waits for one: commands are flushed and the oldest waited for. */
static void gt_poll_fences(GLuint64 wait_ns)
{
    int done = 0;
    while (done < s_gt_npend) {
        GtVSync *v = s_gt_pend[done];
        GLenum r = glad_glClientWaitSync(v->real, wait_ns ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                         done == 0 ? wait_ns : 0);
        if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED || r == GL_WAIT_FAILED) {
            GT_ST(&v->state, GTS_SIGNALED);
            done++;
        } else {
            break;
        }
    }
    if (done) {
        memmove(s_gt_pend, s_gt_pend + done, (size_t)(s_gt_npend - done) * sizeof *s_gt_pend);
        s_gt_npend -= done;
        gt_wake_writer();
    }
}

/* ── name pools ─────────────────────────────────────────────────────── */
typedef struct { GLuint n[64]; int have; } GtPool;
typedef struct { GtPool *pool; int kind; } GtFill;
static GtPool s_gtp[6];
static void gt_fill_pool(void *arg)
{
    GtFill *f = (GtFill *)arg;
    switch (f->kind) {
    case 0: glad_glGenTextures(64, f->pool->n); break;
    case 1: glad_glGenFramebuffers(64, f->pool->n); break;
    case 2: glad_glGenRenderbuffers(64, f->pool->n); break;
    case 3: glad_glGenBuffers(64, f->pool->n); break;
    case 4: glad_glGenVertexArrays(64, f->pool->n); break;
    default: glad_glGenQueries(64, f->pool->n); break;
    }
    f->pool->have = 64;
}
static void gt_gen(int kind, GLsizei n, GLuint *out)
{
    GtPool *p = &s_gtp[kind];
    GLsizei i;
    for (i = 0; i < n; i++) {
        if (!p->have) {
            GtFill f;
            f.pool = p; f.kind = kind;
            gt_call_sync(gt_fill_pool, &f);
        }
        out[i] = p->n[--p->have];
    }
}
static void gtv_GenTextures(GLsizei n, GLuint *o)      { gt_gen(0, n, o); }
static void gtv_GenFramebuffers(GLsizei n, GLuint *o)  { gt_gen(1, n, o); }
static void gtv_GenRenderbuffers(GLsizei n, GLuint *o) { gt_gen(2, n, o); }
static void gtv_GenBuffers(GLsizei n, GLuint *o)       { gt_gen(3, n, o); }
static void gtv_GenVertexArrays(GLsizei n, GLuint *o)  { gt_gen(4, n, o); }
static void gtv_GenQueries(GLsizei n, GLuint *o)       { gt_gen(5, n, o); }

/* ── the GL thread ──────────────────────────────────────────────────── */
static void gt_replay_custom(uint32_t id, uint8_t *p)
{
    switch (id) {
    case GT_FENCE: {
        GtFenceCmd *c = (GtFenceCmd *)p;
        c->v->real = glad_glFenceSync(c->cond, c->flags);
        GT_ST(&c->v->state, GTS_MADE);
        if (s_gt_npend < GT_NSYNC)
            s_gt_pend[s_gt_npend++] = c->v;
        break;
    }
    case GT_DELSYNC: {
        GtVSync *v = *(GtVSync **)p;
        int i;
        for (i = 0; i < s_gt_npend; i++)
            if (s_gt_pend[i] == v) {
                memmove(s_gt_pend + i, s_gt_pend + i + 1, (size_t)(s_gt_npend - i - 1) * sizeof *s_gt_pend);
                s_gt_npend--;
                break;
            }
        if (v->real)
            glad_glDeleteSync(v->real);
        v->real = NULL;
        GT_ST(&v->state, GTS_FREE);
        break;
    }
    case GT_CALL: {
        gt_fn fn;
        memcpy(&fn, p, sizeof fn);
        fn(p + 8);
        break;
    }
    case GT_FRAME:
        SDL_AtomicIncRef(&s_gt_fdone);
        gt_wake_writer();
        break;
    case GT_SYNC: {
        GtSyncCmd *c = (GtSyncCmd *)p;
        gt_replay_sync(c->fid, c->args, c->ret);
        GT_ST(c->done, 1);
        gt_wake_writer();
        break;
    }
    case GT_CALLSYNC: {
        GtCallSync *c = (GtCallSync *)p;
        c->fn(c->arg);
        GT_ST(c->done, 1);
        gt_wake_writer();
        break;
    }
    default: {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] GL thread: unknown command %u\n", id);
    }
    }
}

static void gt_replay_sync_custom(uint32_t id, void *args, void *ret)
{
    (void)args; (void)ret;
    fprintf(stderr, "[GL] GL thread: unknown call %u\n", id);
}

static SDL_Window *s_gt_win;
static SDL_GLContext s_gt_ctx;
static SDL_atomic_t s_gt_started;

static int SDLCALL gt_thread(void *unused)
{
    uint32_t r = 0, n = 0;
    (void)unused;
    t_gt_mode = 0;
    if (SDL_GL_MakeCurrent(s_gt_win, s_gt_ctx) != 0) {
        fprintf(stderr, "[GL] GL thread: cannot take the context (%s)\n", SDL_GetError());
        GT_ST(&s_gt_started, -1);
        return 0;
    }
    GT_ST(&s_gt_started, 1);
#if defined(_WIN32)
    {
        /* RECOMP_PROFILE samples this thread too ([PROF] NV2A GL thread) */
        extern void (*g_thread_watch_hook)(const char *name);
        if (g_thread_watch_hook)
            g_thread_watch_hook("NV2A GL");
    }
#endif
    for (;;) {
        uint32_t w = (uint32_t)GT_LD(&s_gt_w);
        if (r == w) {
            int i;
            /* idle. A writer waiting for a fence: wait for the GPU here */
            if (s_gt_npend && GT_LD(&s_gt_fwaiting)) {
                gt_poll_fences(1000000u);
                continue;
            }
            if (s_gt_npend)
                gt_poll_fences(0);
            gt_wake_writer();
            /* v179: spin a few microseconds (RECOMP_GL_THREAD_SPIN, default
             * 3), not 400 pauses, before sleeping */
            {
                uint64_t t0 = __rdtsc();
                for (i = 0; (uint32_t)GT_LD(&s_gt_w) == r; i++) {
                    gt_pause();
                    if ((i & 15) == 15 && __rdtsc() - t0 > s_gt_spin_tsc)
                        break;
                }
            }
            if ((uint32_t)GT_LD(&s_gt_w) != r)
                continue;
            GT_ST(&s_gt_cwait, 1);
            if ((uint32_t)GT_LD(&s_gt_w) == r && !GT_LD(&s_gt_fwaiting))
                SDL_SemWaitTimeout(s_gt_csem, 20);
            GT_ST(&s_gt_cwait, 0);
            continue;
        }
        {
            uint8_t *h = s_gt_buf + (r & (GT_RING_BYTES - 1));
            uint32_t id = ((uint32_t *)h)[0], size = ((uint32_t *)h)[1];
            if (id != GT_WRAP)
                gt_replay(id, h + 8);
            r += size;
            GT_ST(&s_gt_r, (int)r);
            if ((++n & 255) == 0 && s_gt_npend)
                gt_poll_fences(0);
            gt_wake_writer();
        }
    }
    return 0;
}

/* called on the thread that made the context, once it is set up; from then
 * on that thread queues its GL calls */
static int gt_start(SDL_Window *win, SDL_GLContext ctx)
{
    const char *e = getenv("RECOMP_GL_THREAD"), *l = getenv("RECOMP_GL_THREAD_LAG");
    SDL_Thread *th;
    int i;
    if (e && e[0] == '0') {
        fprintf(stderr, "[GL] GL calls made on the executor (RECOMP_GL_THREAD=0)\n");
        return 0;
    }
    if (l)
        s_gt_lag = atoi(l) < 0 ? 0 : atoi(l);
    s_gt_buf = (uint8_t *)malloc(GT_RING_BYTES);
    s_gt_csem = SDL_CreateSemaphore(0);
    s_gt_psem = SDL_CreateSemaphore(0);
    if (!s_gt_buf || !s_gt_csem || !s_gt_psem)
        return 0;
    {
        /* v179: the idle spin's length in rdtsc ticks */
        /* v243b: 50 us under Wine/Proton. A wake-up there is a slow system
         * call; with 3 us the GL thread fell asleep between almost every
         * draw (~10 us apart on the Deck) and the executor woke it each time
         * (46% of its samples, Deck run in 99th Street: 38 fps; 50 us: 47-49,
         * 200 us: 46-47). */
        const char *sp = getenv("RECOMP_GL_THREAD_SPIN");
        int wine = 0;
#if defined(_WIN32)
        {
            void *nt = SDL_LoadObject("ntdll.dll");
            wine = nt && SDL_LoadFunction(nt, "wine_get_version") != NULL;
            if (nt) SDL_UnloadObject(nt);
        }
#endif
        int us = sp ? atoi(sp) : (wine ? 50 : 3);
        Uint64 q0 = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        uint64_t t0 = __rdtsc(), dt;
        SDL_Delay(10);
        dt = __rdtsc() - t0;
        q0 = SDL_GetPerformanceCounter() - q0;
        if (us < 0) us = 0;
        if (q0 && f)
            s_gt_spin_tsc = (uint64_t)((double)dt * (double)f / (double)q0 * us / 1e6);
        fprintf(stderr, "[GL] GL thread idle spin %d us (%llu ticks)\n", us,
                (unsigned long long)s_gt_spin_tsc);
    }
    s_gt_win = win;
    s_gt_ctx = ctx;
    glad_glFlush();
    SDL_GL_MakeCurrent(win, NULL);
    th = SDL_CreateThread(gt_thread, "NV2A GL", NULL);
    if (!th) {
        SDL_GL_MakeCurrent(win, ctx);
        return 0;
    }
    SDL_DetachThread(th);
    for (i = 0; i < 5000 && !GT_LD(&s_gt_started); i++)
        SDL_Delay(1);
    if (GT_LD(&s_gt_started) != 1) {
        fprintf(stderr, "[GL] GL thread did not start -- GL calls stay on the executor\n");
        SDL_GL_MakeCurrent(win, ctx);
        return 0;
    }
    s_gt_on = 1;
    t_gt_mode = 1;
    fprintf(stderr, "[GL] GL calls made on a thread of their own, at most %d frame(s) queued"
            " (RECOMP_GL_THREAD=0 for the executor)\n", s_gt_lag);
    return 1;
}

typedef struct { uint32_t want; } GtFrames;
static int gt_frames_ok(void *c)
{
    return (int32_t)((uint32_t)GT_LD(&s_gt_fdone) - ((GtFrames *)c)->want) >= 0;
}

/* at a frame's end: mark it, and keep at most s_gt_lag frames queued */
static void gt_frame_end(void)
{
    GtFrames f;
    if (!s_gt_on || t_gt_mode != 1)
        return;
    gt_begin(GT_FRAME, 0);
    gt_end();
    s_gt_frec++;
    f.want = s_gt_frec - (uint32_t)s_gt_lag;
    gt_pwait_until(gt_frames_ok, &f);
}

/* everything queued so far made */
static void gt_drain(void)
{
    if (s_gt_on && t_gt_mode == 1)
        gt_call_sync(gt_noop, NULL);
}
#endif
