/* nv2a_tm.h: TM_NOW(), the clock of the renderer's [PERF] timings, in
 * SDL_GetPerformanceCounter's units. Shared by the renderer and its OpenGL
 * backend; each file keeps its own calibration (values from either are on
 * the same scale and starting point, durations only). */
#ifndef NV2A_TM_H
#define NV2A_TM_H
#include <stdint.h>
#include <stdlib.h>
#include <SDL.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>               /* recomp_fastclock.h: QueryPerformanceCounter */
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(_WIN32)
/* v169: rdtsc-based, in the same units (SDL's counter is QueryPerformanceCounter
 * here, ~140 ns a read under Proton); see recomp_fastclock.h */
#include "recomp_fastclock.h"
#define TM_NOW() ((uint64_t)fast_qpc())
#else
/* v258: rdtsc on Linux too, in SDL's units. SDL's counter is clock_gettime,
 * read about ten times a draw for the [PERF] figures: 3% of the executor on
 * the Deck (Miggas's profile, 99th Street). As recomp_fastclock.h: the first
 * half second from SDL's counter, then the rate between the two is fixed and
 * rdtsc is used, from the same starting point. Durations only. */
#include <x86intrin.h>
static volatile double s_tmf_ratio;
static uint64_t s_tmf_tsc0, s_tmf_q0;
static uint64_t tm_now_slow(void)
{
    uint64_t t = __rdtsc(), q = SDL_GetPerformanceCounter();
    if (!s_tmf_q0) { s_tmf_q0 = q; s_tmf_tsc0 = t; }
    else if (q - s_tmf_q0 >= SDL_GetPerformanceFrequency() / 2 && t > s_tmf_tsc0
             && !getenv("RECOMP_FAST_CLOCK_OFF"))
        s_tmf_ratio = (double)(q - s_tmf_q0) / (double)(t - s_tmf_tsc0);
    return q;
}
static inline uint64_t tm_now_fast(void)
{
    double r = s_tmf_ratio;
    if (r > 0.0) return s_tmf_q0 + (uint64_t)((double)(__rdtsc() - s_tmf_tsc0) * r);
    return tm_now_slow();
}
#define TM_NOW() tm_now_fast()
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
