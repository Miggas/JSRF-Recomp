/*
 * main_linux.c - Jet Set Radio Future, native Linux build (v244)
 *
 * The same start-up as main.c (the Windows build), without what only
 * Windows has: the dbghelp crash reports and the copy of saves from the old
 * %LOCALAPPDATA% place. The stall sampler (v256) and RECOMP_PROFILE (v257)
 * are here too.
 *
 * Faults are handled as on Windows: win32_compat.c turns SIGSEGV into the
 * vectored exception the handler below takes, so the APU and the USB host
 * controller registers (pages that fault on purpose) are answered by the
 * same decoders.
 *
 * Layout: the executable sits in the game folder next to default.xbe and
 * jsrf.ini (where the launcher puts it), or game/default.xbe under the
 * working folder.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>
#include <elf.h>
#include <fcntl.h>

#include <xbox/xboxrecomp.h>
#include "jsrf_version.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

#define YOUR_GAME_ENTRY_POINT 0x00148023u
extern void XAPILIB_mainCRTStartup(void);
extern int recomp_dispatch_init(void);

static char g_xbe_path[PATH_MAX] = "game/default.xbe";
static char g_game_dir[PATH_MAX] = "game";
static int  g_portable;
static int  g_ini_count;
static char g_ini_path[PATH_MAX];

/* ── Stall sampler (v256, as main.c's on Windows) ─────────────
 * If no frame has been flipped for 4 s (after the first one), the game
 * thread is sampled three times, and again every 20 s while it lasts; also
 * when sample.flag appears (the launcher's Sample a hang). Each sample: where
 * the thread is in the recomp's own code (a signal to the thread takes its
 * instruction pointer and host call chain; names need -rdynamic, see the
 * CMakeLists) and the game's own call chain, the return addresses on its
 * guest stack. RECOMP_STALL_SAMPLE=0 turns it off. A freeze on the Steam Deck
 * left nothing to go on without it. */
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
static pthread_t g_main_pt;
static volatile uint32_t *g_main_esp;    /* the game thread's TLS g_esp */
#define SAMPLE_SIG (SIGRTMIN + 7)
#define SAMPLE_MAX 24
static void *volatile s_smp_frames[SAMPLE_MAX];
static volatile int s_smp_n, s_smp_done;
static void *volatile s_smp_rip;

static void sample_sig(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    s_smp_rip = (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
    s_smp_n = backtrace((void **)s_smp_frames, SAMPLE_MAX);
    __atomic_store_n(&s_smp_done, 1, __ATOMIC_RELEASE);
}

static void print_host_addr(const char *pre, void *a)
{
    Dl_info di;
    if (dladdr(a, &di) && di.dli_sname)
        fprintf(stderr, "%s%s+0x%lX\n", pre, di.dli_sname,
                (unsigned long)((uintptr_t)a - (uintptr_t)di.dli_saddr));
    else if (dladdr(a, &di) && di.dli_fname)
        fprintf(stderr, "%s%s+0x%lX\n", pre, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1
                : di.dli_fname, (unsigned long)((uintptr_t)a - (uintptr_t)di.dli_fbase));
    else
        fprintf(stderr, "%s%p\n", pre, a);
}

static void sample_main_thread(int k, const char *why)
{
    uint32_t esp = g_main_esp ? *g_main_esp : 0;
    int i, waited;
    fprintf(stderr, "\n[SAMPLE %d] game thread (%s, guest esp=0x%08X):\n", k + 1, why, esp);
    s_smp_done = 0;
    if (pthread_kill(g_main_pt, SAMPLE_SIG) == 0) {
        for (waited = 0; waited < 200 && !__atomic_load_n(&s_smp_done, __ATOMIC_ACQUIRE); waited++)
            usleep(1000);
        if (s_smp_done) {
            print_host_addr("  RIP ", s_smp_rip);
            fprintf(stderr, "  host stack, innermost first:\n");
            for (i = 1; i < s_smp_n; i++)
                print_host_addr("    ", s_smp_frames[i]);
        } else {
            fprintf(stderr, "  (the thread did not answer the signal)\n");
        }
    }
    if (g_xbox_mem_offset && esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp);
        int shown = 0;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 512 && shown < 24; i++)
            if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, sp[i]);
                shown++;
            }
    }
    fflush(stderr);
}

static unsigned long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}

static void *sampler_thread(void *unused)
{
    extern unsigned long xbox_Nv2aLastFlipMs(void);
    unsigned long last_sampled = 0;
    int k;
    (void)unused;
    for (;;) {
        usleep(500 * 1000);
        {
            unsigned long lf = xbox_Nv2aLastFlipMs(), now = (unsigned long)GetTickCount();
            extern volatile LONG g_xbox_game_held;   /* v259b: the quit window / shader build */
            if (lf && now - lf > 4000 && !g_xbox_game_held
                    && (!last_sampled || now_ms() - last_sampled > 20000)) {
                last_sampled = now_ms();
                fprintf(stderr, "\n[STALL] no frame for %lu ms: sampling the game thread\n", now - lf);
                for (k = 0; k < 3; k++) { sample_main_thread(k, "stall"); usleep(700 * 1000); }
            } else if (lf && now - lf <= 4000) {
                last_sampled = 0;
            }
        }
        if (access("sample.flag", F_OK) == 0) {
            unlink("sample.flag");
            fprintf(stderr, "\n[SAMPLE] sample.flag found: sampling the game thread\n");
            for (k = 0; k < 3; k++) { sample_main_thread(k, "sample.flag"); usleep(700 * 1000); }
        }
    }
    return NULL;
}

/* from the game thread, before the game starts */
static void sampler_start(void)
{
    const char *e = getenv("RECOMP_STALL_SAMPLE");
    struct sigaction sa;
    pthread_t t;
    void *warm[2];
    if (e && e[0] == '0')
        return;
    g_main_pt = pthread_self();
    g_main_esp = &g_esp;
    backtrace(warm, 2);                    /* loads the unwinder now, not in the handler */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SAMPLE_SIG, &sa, NULL);
    unlink("sample.flag");
    if (pthread_create(&t, NULL, sampler_thread, NULL) == 0)
        pthread_detach(t);
}

/* ── RECOMP_PROFILE=1 (v257, as main.c's on Windows) ─────────
 * About 1000 times a second each watched thread -- the game thread, the
 * NV2A executor and the GL thread (they register through
 * g_thread_watch_hook) -- is sent a signal that records where it is. Every
 * 10 s the top 20 functions of each go to stderr as [PROF] lines (for the
 * game thread also those of frames past 18 ms), and the counts restart.
 * Where the time goes on the Steam Deck, where the optimizations start. */
#define PROF_SIG (SIGRTMIN + 8)
typedef struct { uintptr_t lo, hi; const char *name; } ExeFn;
static uintptr_t s_text_lo, s_text_hi;   /* the game's code, for the handler */
#define PROF_THREADS 4
#define PROF_RING 8192                     /* samples kept between drains */
typedef struct {
    pthread_t pt;
    char name[32];
    void *volatile ring[PROF_RING];
    void *volatile caller[PROF_RING];     /* v257b: [rsp], who called a leaf */
    volatile unsigned char slow[PROF_RING];
    volatile unsigned w;                   /* written by the signal handler */
    unsigned r;
} ProfThread;
static ProfThread s_prof[PROF_THREADS];
static volatile int s_prof_n;
static int s_prof_on;
extern volatile uint64_t g_last_flip_qpc;
static volatile int s_prof_slow_now;

static void prof_sig(int sig, siginfo_t *si, void *uc)
{
    pthread_t me = pthread_self();
    int k, n = __atomic_load_n(&s_prof_n, __ATOMIC_ACQUIRE);
    (void)sig; (void)si;
    for (k = 0; k < n; k++)
        if (pthread_equal(s_prof[k].pt, me)) {
            unsigned w = s_prof[k].w;
            const greg_t *g = ((ucontext_t *)uc)->uc_mcontext.gregs;
            s_prof[k].ring[w % PROF_RING] = (void *)g[REG_RIP];
            /* outside the game's code (libc's memcpy and the like): the first
             * return address into it on the stack, the caller (at a leaf it
             * is the top word) */
            {
                const uintptr_t *sp = (const uintptr_t *)(uintptr_t)g[REG_RSP];
                uintptr_t rip = (uintptr_t)g[REG_RIP], c = 0;
                int i;
                if (rip < s_text_lo || rip >= s_text_hi)
                    for (i = 0; i < 64; i++)
                        if (sp[i] >= s_text_lo && sp[i] < s_text_hi) { c = sp[i]; break; }
                s_prof[k].caller[w % PROF_RING] = (void *)c;
            }
            s_prof[k].slow[w % PROF_RING] = (unsigned char)(k == 0 && s_prof_slow_now);
            __atomic_store_n(&s_prof[k].w, w + 1, __ATOMIC_RELEASE);
            return;
        }
}

static void prof_add_thread(const char *name)
{
    int k;
    if (!s_prof_on) return;
    k = __atomic_load_n(&s_prof_n, __ATOMIC_ACQUIRE);
    if (k >= PROF_THREADS) return;
    s_prof[k].pt = pthread_self();
    snprintf(s_prof[k].name, sizeof s_prof[k].name, "%s", name);
    __atomic_store_n(&s_prof_n, k + 1, __ATOMIC_RELEASE);
}
static void prof_watch_hook(const char *name) { prof_add_thread(name); }

typedef struct { const char *name; unsigned all, slow; } ProfFn;
#define PROF_FN 4096
static ProfFn s_fn[PROF_THREADS][PROF_FN];
static int s_nfn[PROF_THREADS];
static unsigned s_tot[PROF_THREADS], s_tot_slow;

/* v257b: the game's own functions by name, static ones included: from the
 * executable's symbol table (it is not position-independent, so the values
 * are the addresses). dladdr only knows the exported ones. */
static ExeFn *s_exefn;
static size_t s_exefn_n;
static char *s_exestr;
static int exefn_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const ExeFn *)a)->lo, y = ((const ExeFn *)b)->lo;
    return x < y ? -1 : x > y;
}
static void exe_symbols_load(void)
{
    int fd = open("/proc/self/exe", O_RDONLY);
    Elf64_Ehdr eh;
    Elf64_Shdr *sh = NULL;
    Elf64_Sym *sym = NULL;
    size_t i, nsym;
    if (fd < 0) return;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG)
            || eh.e_shentsize != sizeof(Elf64_Shdr))
        goto out;
    sh = (Elf64_Shdr *)malloc((size_t)eh.e_shnum * sizeof *sh);
    if (!sh || pread(fd, sh, (size_t)eh.e_shnum * sizeof *sh, (off_t)eh.e_shoff)
               != (ssize_t)((size_t)eh.e_shnum * sizeof *sh))
        goto out;
    for (i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB && sh[i].sh_link < eh.e_shnum) {
            const Elf64_Shdr *st = &sh[sh[i].sh_link];
            nsym = sh[i].sh_size / sizeof(Elf64_Sym);
            sym = (Elf64_Sym *)malloc(sh[i].sh_size);
            s_exestr = (char *)malloc(st->sh_size + 1);
            s_exefn = (ExeFn *)malloc(nsym * sizeof *s_exefn);
            if (!sym || !s_exestr || !s_exefn
                    || pread(fd, sym, sh[i].sh_size, (off_t)sh[i].sh_offset) != (ssize_t)sh[i].sh_size
                    || pread(fd, s_exestr, st->sh_size, (off_t)st->sh_offset) != (ssize_t)st->sh_size)
                goto out;
            s_exestr[st->sh_size] = 0;
            for (size_t j = 0; j < nsym; j++)
                if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC && sym[j].st_value && sym[j].st_name < st->sh_size) {
                    s_exefn[s_exefn_n].lo = (uintptr_t)sym[j].st_value;
                    s_exefn[s_exefn_n].hi = (uintptr_t)(sym[j].st_value + (sym[j].st_size ? sym[j].st_size : 1));
                    s_exefn[s_exefn_n].name = s_exestr + sym[j].st_name;
                    s_exefn_n++;
                }
            qsort(s_exefn, s_exefn_n, sizeof *s_exefn, exefn_cmp);
            if (s_exefn_n) {
                s_text_lo = s_exefn[0].lo;
                s_text_hi = s_exefn[s_exefn_n - 1].hi;
            }
            break;
        }
out:
    free(sym);
    free(sh);
    close(fd);
}
static const char *exe_name(uintptr_t a)
{
    size_t lo = 0, hi = s_exefn_n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_exefn[mid].lo <= a) lo = mid + 1; else hi = mid;
    }
    if (lo && a < s_exefn[lo - 1].hi) return s_exefn[lo - 1].name;
    return NULL;
}

/* names live as long as the program; library names with their caller are
 * kept in a small table so the same pair is the same pointer */
#define PROF_PAIRS 2048
static char s_pair[PROF_PAIRS][112];
static int s_pair_n;
static const char *prof_name2(void *a, void *caller)
{
    Dl_info di;
    const char *n = exe_name((uintptr_t)a), *lib, *c;
    char buf[112];
    int i;
    if (n) return n;
    if (dladdr(a, &di) && di.dli_sname) lib = di.dli_sname;
    else if (dladdr(a, &di) && di.dli_fname) {
        lib = strrchr(di.dli_fname, '/');
        lib = lib ? lib + 1 : di.dli_fname;
    } else lib = "?";
    c = exe_name((uintptr_t)caller);
    if (!c) return lib;
    snprintf(buf, sizeof buf, "%s <- %s", lib, c);
    for (i = 0; i < s_pair_n; i++)
        if (!strcmp(s_pair[i], buf)) return s_pair[i];
    if (s_pair_n == PROF_PAIRS) return lib;
    memcpy(s_pair[s_pair_n], buf, sizeof buf);
    return s_pair[s_pair_n++];
}

static void prof_drain(void)
{
    int k, n = __atomic_load_n(&s_prof_n, __ATOMIC_ACQUIRE);
    for (k = 0; k < n; k++) {
        unsigned w = __atomic_load_n(&s_prof[k].w, __ATOMIC_ACQUIRE);
        if (w - s_prof[k].r > PROF_RING) s_prof[k].r = w - PROF_RING;
        for (; s_prof[k].r != w; s_prof[k].r++) {
            const char *nm = prof_name2(s_prof[k].ring[s_prof[k].r % PROF_RING],
                                        s_prof[k].caller[s_prof[k].r % PROF_RING]);
            int slow = s_prof[k].slow[s_prof[k].r % PROF_RING], j;
            for (j = 0; j < s_nfn[k]; j++)
                if (s_fn[k][j].name == nm) break;
            if (j == s_nfn[k]) {
                if (j == PROF_FN) continue;
                s_fn[k][j].name = nm; s_fn[k][j].all = s_fn[k][j].slow = 0;
                s_nfn[k]++;
            }
            s_fn[k][j].all++;
            s_tot[k]++;
            if (slow) { s_fn[k][j].slow++; s_tot_slow++; }
        }
    }
}

static int fn_all(const void *a, const void *b) { return (int)((const ProfFn *)b)->all - (int)((const ProfFn *)a)->all; }
static int fn_slow(const void *a, const void *b) { return (int)((const ProfFn *)b)->slow - (int)((const ProfFn *)a)->slow; }

static void prof_report(void)
{
    int k, i, n = __atomic_load_n(&s_prof_n, __ATOMIC_ACQUIRE);
    for (k = 0; k < n; k++) {
        if (!s_tot[k]) continue;
        qsort(s_fn[k], (size_t)s_nfn[k], sizeof s_fn[k][0], fn_all);
        fprintf(stderr, "[PROF] %s thread, %u samples%s; top functions:\n", s_prof[k].name,
                s_tot[k], k == 0 ? "" : "");
        for (i = 0; i < s_nfn[k] && i < 20; i++)
            fprintf(stderr, "[PROF]   %5.1f%%  %s\n", s_fn[k][i].all * 100.0 / s_tot[k], s_fn[k][i].name);
        if (k == 0 && s_tot_slow) {
            qsort(s_fn[k], (size_t)s_nfn[k], sizeof s_fn[k][0], fn_slow);
            fprintf(stderr, "[PROF] game thread during frames past 18 ms (%u samples):\n", s_tot_slow);
            for (i = 0; i < s_nfn[k] && i < 20 && s_fn[k][i].slow; i++)
                fprintf(stderr, "[PROF]   %5.1f%%  %s\n", s_fn[k][i].slow * 100.0 / s_tot_slow, s_fn[k][i].name);
        }
        s_nfn[k] = 0; s_tot[k] = 0;
    }
    s_tot_slow = 0;
    fflush(stderr);
}

static void *prof_thread(void *unused)
{
    unsigned long last = now_ms();
    (void)unused;
    for (;;) {
        int k, n = __atomic_load_n(&s_prof_n, __ATOMIC_ACQUIRE);
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        s_prof_slow_now = g_last_flip_qpc
            && (uint64_t)(q.QuadPart - (LONGLONG)g_last_flip_qpc) * 1000 > (uint64_t)f.QuadPart * 18;
        for (k = 0; k < n; k++)
            pthread_kill(s_prof[k].pt, PROF_SIG);
        usleep(1000);
        if (now_ms() - last >= 250) prof_drain();
        if (now_ms() - last >= 10000) {
            prof_drain();
            prof_report();
            last = now_ms();
        }
    }
    return NULL;
}

/* from the game thread, before the game starts */
static void prof_start(void)
{
    extern void (*g_thread_watch_hook)(const char *name);
    const char *e = getenv("RECOMP_PROFILE");
    struct sigaction sa;
    pthread_t t;
    if (!e || atoi(e) <= 0)
        return;
    s_prof_on = 1;
    exe_symbols_load();
    fprintf(stderr, "[PROF] %zu functions named from the executable\n", s_exefn_n);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(PROF_SIG, &sa, NULL);
    prof_add_thread("game");
    g_thread_watch_hook = prof_watch_hook;
    fprintf(stderr, "[PROF] sampling the game, NV2A executor and GL threads (RECOMP_PROFILE)\n");
    if (pthread_create(&t, NULL, prof_thread, NULL) == 0)
        pthread_detach(t);
}

/* ── jsrf.ini: NAME=value lines, as main.c reads them ──────── */

static char *ini_trim(char *p)
{
    char *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (e - p >= 2 && ((p[0] == '"' && e[-1] == '"') || (p[0] == '\'' && e[-1] == '\''))) {
        e[-1] = 0;
        p++;
    }
    return p;
}

static void ini_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    static char own[128][64];
    static int n_own;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *k = ini_trim(line), *eq, *v;
        if (!*k || *k == ';' || *k == '#' || *k == '[')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = ini_trim(eq + 1);
        k = ini_trim(k);
        {
            char *c = strstr(v, " ;");
            if (!c) c = strstr(v, "\t;");
            if (c) { *c = 0; v = ini_trim(v); }
        }
        if (!*k)
            continue;
        {
            int i, mine = 0;
            for (i = 0; i < n_own; i++)
                if (!strcasecmp(own[i], k)) { mine = 1; break; }
            if (getenv(k) && !mine)
                continue;                  /* the environment wins */
            if (!*v)
                continue;
            if (!mine && n_own < (int)(sizeof own / sizeof own[0]))
                snprintf(own[n_own++], sizeof own[0], "%s", k);
            if (!mine)
                g_ini_count++;
        }
        setenv(k, v, 1);
    }
    fclose(f);
}

static void portable_setup(void)
{
    char dir[PATH_MAX], xbe[PATH_MAX + 16];
    char *slash;
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    struct stat st;
    if (n <= 0)
        return;
    dir[n] = 0;
    slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = 0;
    snprintf(xbe, sizeof xbe, "%s/default.xbe", dir);
    if (stat(xbe, &st) == 0) {
        g_portable = 1;
        if (chdir(dir) != 0) { /* keep going from where we are */ }
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s", xbe);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    }
    snprintf(g_ini_path, sizeof g_ini_path, "%s/jsrf.ini", dir);
    ini_load(g_ini_path);
    {
        /* stderr to the file RECOMP_LOG names (relative: in the game folder).
         * Always, when it is set: from Steam stderr goes to a pipe nobody
         * reads, so "only when nothing captured it", as on Windows, would
         * lose the log just where it is wanted. */
        const char *log = getenv("RECOMP_LOG");
        if (log && *log) {
            if (!freopen(log, "w", stderr)) { /* keep the old stderr */ }
        }
    }
}

/* ── faults: the APU and USB registers, then a report ─────── */

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
extern void xbox_OhciReport(void);

static volatile int g_fatal_once;

static LONG veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t xbox_va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        int is_write = ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0;
        if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                && apu_hook_handle_mmio(ep->ContextRecord, fault_addr, xbox_va, is_write))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (xbox_OhciOwnsAddress(xbox_va) && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* single steps and breakpoints are other handlers' business */
    if (ep->ExceptionRecord->ExceptionCode == 0x80000004u
            || ep->ExceptionRecord->ExceptionCode == 0x80000003u)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!__sync_lock_test_and_set(&g_fatal_once, 1)) {
        const CONTEXT *c = ep->ContextRecord;
        const uint8_t *code = (const uint8_t *)(uintptr_t)c->Rip;
        int i;
        fprintf(stderr, "[CRASH] exception 0x%08X at RIP=0x%llX, fault addr=0x%llX (%s)\n",
                (unsigned)ep->ExceptionRecord->ExceptionCode, (unsigned long long)c->Rip,
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n", g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
                (uint32_t)(ep->ExceptionRecord->ExceptionInformation[1]
                           - (uintptr_t)g_xbox_mem_offset));
        fprintf(stderr, "  host regs: rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n",
                (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
                (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
        fprintf(stderr, "  host regs: rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n",
                (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
                (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
        fprintf(stderr, "  code at RIP:");
        for (i = 0; i < 16; i++)
            fprintf(stderr, " %02X", code[i]);
        fprintf(stderr, "\n  (addr2line -e jsrf_recomp -f 0x... names the function)\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* main.c's thread profiler is Windows-only; the hook stays for the callers */
void recomp_prof_watch_this_thread(const char *name) { (void)name; }

static int load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || !(data = malloc((size_t)size))) {
        fclose(f);
        return 0;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return 1;
}

static int game_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    portable_setup();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Jet Set Radio Future - Static Recompilation (Linux) ===\n");
    fprintf(stderr, "[CONFIG] %s layout, game files in %s; %d setting(s) from %s\n",
            g_portable ? "portable" : "classic", g_game_dir, g_ini_count,
            g_ini_count ? g_ini_path : "no jsrf.ini");
    fprintf(stderr, "[CONFIG] build " JSRF_BUILD " (Linux), compiled " __DATE__ " " __TIME__ "\n");

    AddVectoredExceptionHandler(1, veh_handler);

    if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
        fprintf(stderr, "Failed to load default.xbe. Put this program in the game folder next"
                " to default.xbe, or the game files in game/ under the working folder.\n");
        return 1;
    }
    {
        const char *mb = getenv("RECOMP_RAM_MB");
        unsigned v = mb ? (unsigned)strtoul(mb, NULL, 0) : 128;
        if (v >= 64 && v <= 512) {
            extern void xbox_SetTotalRam(size_t bytes);
            xbox_SetTotalRam((size_t)v * 1024 * 1024);
        }
    }
    {
        const char *r = getenv("RECOMP_HEAP_RECLAIM");
        if (!r) setenv("RECOMP_HEAP_RECLAIM", "1", 1);
        else if (!strcmp(r, "0")) unsetenv("RECOMP_HEAP_RECLAIM");
        r = getenv("RECOMP_DEFER_APC");
        if (!r) setenv("RECOMP_DEFER_APC", "1", 1);
        else if (!strcmp(r, "0")) unsetenv("RECOMP_DEFER_APC");
    }
    {
        extern int apu_audio_debug(void);
        apu_audio_debug();
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Failed to initialize the Xbox memory layout"
                " (the address range it needs is taken).\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
    }
    xbox_OhciInit();
    xbox_kernel_init();
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        const char *sd = getenv("RECOMP_SAVE_DIR");
        char save[PATH_MAX + 32], gfull[PATH_MAX];
        if (!realpath(g_game_dir, gfull))
            snprintf(gfull, sizeof gfull, "%s", g_game_dir);
        if (sd && *sd && sd[0] == '/')
            snprintf(save, sizeof save, "%s", sd);
        else
            snprintf(save, sizeof save, "%s/%s", gfull, (sd && *sd) ? sd : "savegame");
        fprintf(stderr, "[CONFIG] saves in %s\n", save);
        xbox_path_init(gfull, save);
    }
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");
    xbox_WatchdogStart();
    sampler_start();                       /* v256 */
    prof_start();                          /* v257 */
    {
        extern void xbox_mmcss_game_thread(const char *who);
        xbox_mmcss_game_thread("main");
    }
    printf("Entry point: 0x%08X\nStarting game...\n", YOUR_GAME_ENTRY_POINT);
    XAPILIB_mainCRTStartup();

    fprintf(stderr, "[EXIT] game entry point returned (eax=0x%08X)\n", g_eax);
    xbox_OhciReport();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}

/* The game runs on a thread with a 256 MB stack (the Windows build's
 * /STACK): the main thread's stack is the system's 8 MB. */
static int s_rc;
static void *game_thread(void *unused) { (void)unused; s_rc = game_main(); return NULL; }

int main(int argc, char **argv)
{
    pthread_t t;
    pthread_attr_t a;
    (void)argc; (void)argv;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)256 << 20);
    if (pthread_create(&t, &a, game_thread, NULL) != 0)
        return game_main();
    pthread_join(t, NULL);
    return s_rc;
}
