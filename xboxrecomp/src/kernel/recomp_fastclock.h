/* v169: a cheap clock for the executor's own timing figures.
 *
 * The executor reads the clock about ten times a draw for its [PERF]
 * figures (vertices, set-up, the GL draw, texture checks), and under Proton
 * QueryPerformanceCounter is a call into Wine: 139 ns each on a Steam Deck
 * (run 168's "clock costs" line), against 12 ns for rdtsc. At 60,000 draws
 * a second that is roughly a tenth of the executor's time spent reading
 * clocks.
 *
 * fast_qpc() returns QueryPerformanceCounter units worked out from rdtsc.
 * For its first half second it simply calls QueryPerformanceCounter, then
 * fixes the rate between the two and switches to rdtsc; the values are
 * anchored to the same starting point, so a duration that straddles the
 * switch is still right. Only for durations: frame pacing keeps the real
 * QueryPerformanceCounter. RECOMP_FAST_CLOCK=0 keeps it everywhere.
 *
 * Static per file that includes it: each file calibrates its own copy. */
#ifndef RECOMP_FASTCLOCK_H
#define RECOMP_FASTCLOCK_H
#include <stdint.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

static volatile double s_fc_ratio;   /* QPC ticks per rdtsc tick, once known */
static uint64_t s_fc_tsc0;
static int64_t  s_fc_q0, s_fc_need;
static int      s_fc_mode = -1;

static int64_t fc_slow(void)
{
    LARGE_INTEGER q;
    uint64_t t = __rdtsc();
    QueryPerformanceCounter(&q);
    if (s_fc_mode < 0) {
        const char *e = getenv("RECOMP_FAST_CLOCK");
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_fc_q0 = q.QuadPart;
        s_fc_tsc0 = t;
        s_fc_need = f.QuadPart / 2;
        s_fc_mode = !(e && e[0] == '0');
    } else if (s_fc_mode && q.QuadPart - s_fc_q0 >= s_fc_need && t > s_fc_tsc0) {
        s_fc_ratio = (double)(q.QuadPart - s_fc_q0) / (double)(t - s_fc_tsc0);
    }
    return q.QuadPart;
}

static inline int64_t fast_qpc(void)
{
    double r = s_fc_ratio;
    if (r > 0.0)
        return s_fc_q0 + (int64_t)((double)(__rdtsc() - s_fc_tsc0) * r);
    return fc_slow();
}

/* drop-in for QueryPerformanceCounter(&q) in timing-only code */
static inline void fast_qpc_li(LARGE_INTEGER *q) { q->QuadPart = fast_qpc(); }
#endif
