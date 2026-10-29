/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#elif !defined(InterlockedIncrement)
#include "win32_compat.h"   /* v244: InterlockedIncrement is a function there */
#endif
#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
/* lowest limiter gain since the last APU report (1 = never engaged) */
float g_apu_lim_min = 1.0f;

/* v83: RECOMP_APU_CAPTURE=1 records what the mixdown produces, so a crackle
 * can be looked at instead of guessed at. Four 16-bit channels at 48 kHz:
 *   1, 2  the final output, left and right (what XAudio2 is given)
 *   3     the effects alone: centre, crosstalk and reverb bins (3D voices)
 *   4     markers: +voices started, -voices ended, at the frame they did
 * written as 30-second files apu_capture_0.wav and apu_capture_1.wav, taking
 * turns, so the last 30-60 s are always on disk. Headers are kept valid
 * after every second, so a file is playable even if the game is killed. */
extern int g_apu_vp_starts, g_apu_vp_ends;
#define CAP_SEG_FRAMES (30 * 1500)
static FILE *s_cap;
static int s_cap_on = -1, s_cap_slot;
static uint32_t s_cap_frames;

static void cap_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 32u * 8u, v32;
    uint16_t v16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v32 = 36 + data; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v32 = 48000; fwrite(&v32, 4, 1, f); v32 = 48000 * 8; fwrite(&v32, 4, 1, f);
    v16 = 8; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void cap_frame(const int16_t (*out)[2], const float *fx)
{
    int16_t buf[NUM_SAMPLES_PER_FRAME][4];
    int i, st = g_apu_vp_starts, en = g_apu_vp_ends;
    g_apu_vp_starts = g_apu_vp_ends = 0;
    if (s_cap_on < 0) {
        const char *e = getenv("RECOMP_APU_CAPTURE");
        s_cap_on = e && e[0] && e[0] != '0';
        {   /* v199: the audio debug build captures too, unless told not to */
            extern int apu_audio_debug(void);
            if (!e && apu_audio_debug()) s_cap_on = 1;
        }
        if (s_cap_on)
            fprintf(stderr, "[APU] capturing the output to apu_capture_0.wav /"
                    " apu_capture_1.wav (30 s each, alternating)\n");
    }
    if (!s_cap_on)
        return;
    if (!s_cap || s_cap_frames >= CAP_SEG_FRAMES) {
        char name[32];
        if (s_cap) {
            cap_header(s_cap, s_cap_frames);
            fclose(s_cap);
            s_cap_slot ^= 1;
        }
        snprintf(name, sizeof name, "apu_capture_%d.wav", s_cap_slot);
        s_cap = fopen(name, "wb");
        s_cap_frames = 0;
        if (!s_cap) {
            s_cap_on = 0;
            return;
        }
        cap_header(s_cap, 0);
        {   /* v199: where this file starts, against the log's t= */
            extern long long apu_dbg_ms(void);
            fprintf(stderr, "[APU] capture %s starts at t=%lld\n", name, apu_dbg_ms());
        }
    }
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float x = fx[i] * 32767.0f;
        if (x > 32767.0f) x = 32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        buf[i][0] = out[i][0];
        buf[i][1] = out[i][1];
        buf[i][2] = (int16_t)x;
        buf[i][3] = 0;
    }
    buf[0][3] = (int16_t)(st * 1000 > 32000 ? 32000 : st * 1000);
    buf[16][3] = (int16_t)(en * 1000 > 32000 ? -32000 : -en * 1000);
    fwrite(buf, sizeof buf, 1, s_cap);
    if (++s_cap_frames % 1500 == 0) {
        cap_header(s_cap, s_cap_frames);
        fflush(s_cap);
    }
}

static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}


/* ============================================================
 * v115: the I3DL2 reverb, recreated natively.
 *
 * DirectSound runs the listener reverb on the GP DSP from dsstdfx.bin, whose
 * code is encrypted; here the DSP is not emulated at all. Instead the title's
 * own IDirectSound_SetI3DL2Listener call is caught (recomp_manual.c) and its
 * parameters drive a reverb built here from the I3DL2 model: room level and
 * its high-frequency cut, early reflections (level, delay), late reverb
 * (level, delay, decay time, HF decay ratio, diffusion, density). Its input
 * is mixbin 10, DirectSound's I3DL2 send, which used to be mixed in dry.
 * Not bit-exact to the Xbox; the same character, following the same
 * numbers. RECOMP_APU_REVERB=0 turns it off (bin 10 dry, as before);
 * RECOMP_APU_REVERB_GAIN scales the wet output (default 1).
 * ============================================================ */
typedef struct {
    float room, room_hf, rolloff, decay, decay_hf, refl, refl_delay,
          rev, rev_delay, diffusion, density, hf_ref;
} I3dl2Params;
static I3dl2Params s_i3_pending;
static volatile LONG s_i3_ver;          /* bumped by every set (v244: LONG, 32-bit on Linux too) */
static LONG s_i3_used = -1;

void mcpx_apu_set_i3dl2(const float p[12])
{
    memcpy(&s_i3_pending, p, sizeof s_i3_pending);
    InterlockedIncrement(&s_i3_ver);
}

#define RV_FS 48000.0f
#define RV_PRE (48000 / 2)               /* up to 0.5 s before the late part */
#define RV_LINES 4
#define RV_LMAX 6000
static struct {
    int on, active;
    float wet_gain, decay_scale;
    float g_room, a_room, lp_room;        /* level and one-pole HF cut */
    float g_refl, g_rev;
    int   refl_tap[4], rev_tap;
    float pre[RV_PRE]; int pre_w;
    float ap_buf[2][1024]; int ap_len[2], ap_w[2]; float ap_g;
    float line[RV_LINES][RV_LMAX]; int len[RV_LINES], w[RV_LINES];
    float fb[RV_LINES], damp_a[RV_LINES], damp_y[RV_LINES];
} s_rv;

static float mb_gain(float mb)
{
    if (mb <= -10000.0f) return 0.0f;
    return powf(10.0f, mb / 2000.0f);
}

/* one-pole lowpass y += a (x - y): the a whose gain at f is g (0 < g <= 1) */
static float onepole_for(float g, float f)
{
    float w = 2.0f * 3.14159265f * f / RV_FS, lo = 0.0001f, hi = 1.0f;
    int k;
    if (g >= 0.999f) return 1.0f;
    if (g < 0.001f) g = 0.001f;
    for (k = 0; k < 40; k++) {
        float a = 0.5f * (lo + hi), b = 1.0f - a;
        float mag = a / sqrtf(1.0f - 2.0f * b * cosf(w) + b * b);
        if (mag > g) hi = a; else lo = a;
    }
    return 0.5f * (lo + hi);
}

static void rv_configure(const I3dl2Params *p)
{
    static const int base[RV_LINES] = {1553, 1709, 1867, 2017};
    float dens = p->density < 0 ? 0 : (p->density > 100 ? 1 : p->density / 100.0f);
    float diff = p->diffusion < 0 ? 0 : (p->diffusion > 100 ? 1 : p->diffusion / 100.0f);
    float decay = (p->decay < 0.1f ? 0.1f : p->decay) * s_rv.decay_scale;
    float hfr = p->decay_hf < 0.1f ? 0.1f : p->decay_hf;
    float hfref = p->hf_ref > 20.0f ? p->hf_ref : 5000.0f;
    float rd = p->refl_delay < 0 ? 0 : (p->refl_delay > 0.3f ? 0.3f : p->refl_delay);
    float vd = p->rev_delay < 0 ? 0 : (p->rev_delay > 0.1f ? 0.1f : p->rev_delay);
    int i;
    s_rv.g_room = mb_gain(p->room);
    s_rv.a_room = onepole_for(mb_gain(p->room_hf), hfref);
    s_rv.g_refl = mb_gain(p->refl);
    s_rv.g_rev = mb_gain(p->rev);
    for (i = 0; i < 4; i++) {
        static const float spread[4] = {1.0f, 1.21f, 1.47f, 1.73f};
        int t = (int)(rd * spread[i] * RV_FS) + 1;
        s_rv.refl_tap[i] = t < RV_PRE - 1 ? t : RV_PRE - 1;
    }
    s_rv.rev_tap = (int)((rd + vd) * RV_FS) + 1;
    if (s_rv.rev_tap > RV_PRE - 1) s_rv.rev_tap = RV_PRE - 1;
    s_rv.ap_len[0] = 225 + (int)(diff * 331); s_rv.ap_len[1] = 341 + (int)(diff * 443);
    s_rv.ap_g = 0.35f + 0.35f * diff;
    for (i = 0; i < RV_LINES; i++) {
        float L = (float)base[i] * (0.6f + 0.8f * dens), g, ghf;
        int n = (int)L;
        if (n >= RV_LMAX) n = RV_LMAX - 1;
        s_rv.len[i] = n;
        g = powf(10.0f, -3.0f * (float)n / (RV_FS * decay));
        ghf = powf(10.0f, -3.0f * (float)n / (RV_FS * decay * hfr));
        s_rv.fb[i] = g;
        s_rv.damp_a[i] = onepole_for(ghf / g > 1.0f ? 1.0f : ghf / g, hfref);
    }
    s_rv.active = s_rv.g_room > 0.0f && (s_rv.g_refl > 0.0f || s_rv.g_rev > 0.0f);
    {
        static unsigned shown;
        if (shown++ < 40)
            fprintf(stderr, "[APU-RV] I3DL2 listener: room %.0f mB (HF %.0f), decay %.2f s (used %.2f s)"
                    " (HF x%.2f), reflections %.0f mB at %.3f s, reverb %.0f mB at +%.3f s,"
                    " diffusion %.0f%%, density %.0f%%, HF ref %.0f Hz -> %s\n",
                    p->room, p->room_hf, p->decay, decay, p->decay_hf, p->refl, p->refl_delay,
                    p->rev, p->rev_delay, p->diffusion, p->density, p->hf_ref,
                    s_rv.active ? "reverb on" : "reverb off");
    }
}

/* one sample of reverb input -> wet left/right */
static void rv_process(float x, float *ol, float *or_)
{
    float pin, late_in, a, y[RV_LINES], s4 = 0.0f, l, r;
    int i, k;
    s_rv.lp_room += s_rv.a_room * (x * s_rv.g_room - s_rv.lp_room);
    pin = s_rv.lp_room;
    s_rv.pre[s_rv.pre_w] = pin;
#define PRE_AT(d) s_rv.pre[(s_rv.pre_w - (d) + RV_PRE) % RV_PRE]
    l = s_rv.g_refl * (0.80f * PRE_AT(s_rv.refl_tap[0]) + 0.55f * PRE_AT(s_rv.refl_tap[2]));
    r = s_rv.g_refl * (0.75f * PRE_AT(s_rv.refl_tap[1]) + 0.50f * PRE_AT(s_rv.refl_tap[3]));
    late_in = PRE_AT(s_rv.rev_tap);
#undef PRE_AT
    s_rv.pre_w = (s_rv.pre_w + 1) % RV_PRE;
    /* diffusion: two allpasses */
    a = late_in;
    for (k = 0; k < 2; k++) {
        int n = s_rv.ap_len[k], rd = (s_rv.ap_w[k] - n + 1024) % 1024;
        float d = s_rv.ap_buf[k][rd], v = a + s_rv.ap_g * d;
        s_rv.ap_buf[k][s_rv.ap_w[k]] = v;
        a = d - s_rv.ap_g * v;
        s_rv.ap_w[k] = (s_rv.ap_w[k] + 1) % 1024;
    }
    /* four-line feedback delay network, Householder feedback */
    for (i = 0; i < RV_LINES; i++) {
        int rd = (s_rv.w[i] - s_rv.len[i] + RV_LMAX) % RV_LMAX;
        float v = s_rv.line[i][rd];
        s_rv.damp_y[i] += s_rv.damp_a[i] * (v - s_rv.damp_y[i]);
        y[i] = s_rv.damp_y[i] * s_rv.fb[i];
        s4 += y[i];
    }
    s4 *= 0.5f;
    for (i = 0; i < RV_LINES; i++) {
        s_rv.line[i][s_rv.w[i]] = y[i] - s4 + a;
        s_rv.w[i] = (s_rv.w[i] + 1) % RV_LMAX;
    }
    l += s_rv.g_rev * 0.6f * (y[0] - y[2] + 0.5f * y[1]);
    r += s_rv.g_rev * 0.6f * (y[1] - y[3] + 0.5f * y[2]);
    *ol = l * s_rv.wet_gain;
    *or_ = r * s_rv.wet_gain;
}

static int rv_enabled(void)
{
    if (!s_rv.on) {
        const char *e = getenv("RECOMP_APU_REVERB");
        s_rv.on = (e && e[0] == '0') ? -1 : 1;
        e = getenv("RECOMP_APU_REVERB_GAIN");
        s_rv.wet_gain = e ? (float)atof(e) : 1.0f;
        /* v128: measured on the console (digital out, sewer facility, the
         * title's "sewer pipe" listener, decay 2.81 s): the tail falls
         * 5.5 dB/s, RT60 about 11 s in every band up to 1 kHz, four times
         * the nominal decay, with the same high-frequency shape. Here it
         * fell 22 dB/s, exactly the nominal 2.8 s. So the decay time is
         * scaled by 4; RECOMP_APU_REVERB_DECAY sets another factor. */
        e = getenv("RECOMP_APU_REVERB_DECAY");
        s_rv.decay_scale = e ? (float)atof(e) : 4.0f;
        if (!(s_rv.decay_scale > 0.05f && s_rv.decay_scale < 20.0f))
            s_rv.decay_scale = 4.0f;
        fprintf(stderr, "[APU-RV] decay time x%.2f (RECOMP_APU_REVERB_DECAY)\n", s_rv.decay_scale);
    }
    if (s_rv.on < 0)
        return 0;
    {
        long v = s_i3_ver;
        if (v != s_i3_used) {
            I3dl2Params p;
            memcpy(&p, &s_i3_pending, sizeof p);
            s_i3_used = v;
            if (v > 0)
                rv_configure(&p);
        }
    }
    return s_i3_used > 0;      /* only once the title has set a listener */
}
void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    /* v110: how much each mixbin carries, every 10 s (15,000 frames): which
     * bins hold the reverb and effect sends, and when they are active. */
    {
        static double e[NUM_MIXBINS];
        static unsigned frames;
        static int on = -1;
        int b, j;
        if (on < 0) {
            const char *v = getenv("RECOMP_APU_GP_LOG");
            on = v && v[0] == '1';                 /* v116: off unless asked */
        }
        if (on) {
            for (b = 0; b < NUM_MIXBINS; b++)
                for (j = 0; j < NUM_SAMPLES_PER_FRAME; j++)
                    e[b] += (double)mixbins[b][j] * mixbins[b][j];
            if (++frames >= 15000) {
                fprintf(stderr, "[APU-BINS] rms per mixbin over 10 s:");
                for (b = 0; b < NUM_MIXBINS; b++) {
                    double r = sqrt(e[b] / ((double)frames * NUM_SAMPLES_PER_FRAME));
                    if (r > 1e-5)
                        fprintf(stderr, " %d=%.4f", b, r);
                    e[b] = 0;
                }
                fputc('\n', stderr);
                frames = 0;
            }
        }
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        static float gain = -1.0f, ceil_lv = 0.9f, env = 1.0f, rel = 0.0f;
        static int use_lim = 1;
        float bl[NUM_SAMPLES_PER_FRAME], br[NUM_SAMPLES_PER_FRAME];
        float fxs[NUM_SAMPLES_PER_FRAME];
        float peak = 0.0f;
        if (gain < 0.0f) {
            const char *e = getenv("RECOMP_APU_GAIN");
            gain = e ? (float)atof(e) : 0.5f;
            if (gain < 0.0f) gain = 0.0f;
            e = getenv("RECOMP_APU_LIMITER");
            use_lim = !(e && e[0] == '0');
            /* release time constant ~150 ms, evaluated once per 32-sample
             * block (0.667 ms at 48 kHz) */
            rel = expf(-((float)NUM_SAMPLES_PER_FRAME / 48000.0f) / 0.150f);
        }
        int use_rv = rv_enabled();
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                /* By what each bin is (DSMIXBIN_*), not every bin at full
                 * level. Adding all 32 counted a 3D voice several times --
                 * its crosstalk pairs, its reverb send, the effect sends --
                 * so sound effects stayed loud when the music was turned
                 * down (run 59).
                 *   0/1 front L/R, 4/5 back L/R          full
                 *   2 centre                             0.7 to both
                 *   6-9 crosstalk (3D voices), front/back 0.5
                 *   10 I3DL2 reverb send                 0.25 to both
                 *   3 LFE, 11+ effect sends              dropped (the GP
                 *   programs that would return them are not run). */
                float c = mixbins[2][i] * 0.7f, rv = mixbins[10][i] * 0.25f;
                float wl = rv, wr = rv;
                if (use_rv) {
                    /* v115: bin 10 goes through the reverb, wet only */
                    if (s_rv.active)
                        rv_process(mixbins[10][i], &wl, &wr);
                    else
                        wl = wr = 0.0f;
                }
                left  = mixbins[0][i] + mixbins[4][i]
                      + 0.5f * (mixbins[6][i] + mixbins[8][i]) + c + wl;
                right = mixbins[1][i] + mixbins[5][i]
                      + 0.5f * (mixbins[7][i] + mixbins[9][i]) + c + wr;
                fxs[i] = (0.25f * (mixbins[6][i] + mixbins[7][i] + mixbins[8][i]
                                   + mixbins[9][i]) + c + 0.5f * (wl + wr)) * gain;
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
                fxs[i] = 0.0f;
            }
            left *= gain;
            right *= gain;
            bl[i] = left; br[i] = right;
            {
                float al = fabsf(left), ar = fabsf(right);
                if (al > peak) peak = al;
                if (ar > peak) peak = ar;
            }
        }
        /* Master level, then a limiter (run 80). The tanh knee above 0.8
         * that was here bent every loud sample individually: with many
         * effects at once the mix sat in the knee (loudest sample 29295 =
         * 0.89, run 79 with no underruns and no starved voices), and a
         * per-sample curve on a sum of voices is distortion -- the crackle
         * heard on 99th street. This instead turns the whole mix down
         * smoothly when the block peak would pass 0.9 (the gain ramps to
         * the new level across the 32 samples, then recovers over ~150 ms),
         * so the waveform keeps its shape. RECOMP_APU_LIMITER=0 restores
         * the knee. */
        {
            float target = 1.0f;
            if (use_lim && peak * env > ceil_lv)
                target = ceil_lv / peak;
            else if (use_lim) {
                target = 1.0f - (1.0f - env) * rel;   /* release toward 1 */
                if (peak * target > ceil_lv) target = ceil_lv / peak;
            }
            if (use_lim && target > 1.0f) target = 1.0f;
            float g0 = env, step = use_lim ? (target - g0) / NUM_SAMPLES_PER_FRAME : 0.0f;
            if (!use_lim) g0 = 1.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float g = g0 + step * (float)(i + 1);
                float left = bl[i] * g, right = br[i] * g;
                if (!use_lim) {
                    if (left > 0.8f) left = 0.8f + 0.2f * tanhf((left - 0.8f) / 0.2f);
                    if (left < -0.8f) left = -0.8f - 0.2f * tanhf((-left - 0.8f) / 0.2f);
                    if (right > 0.8f) right = 0.8f + 0.2f * tanhf((right - 0.8f) / 0.2f);
                    if (right < -0.8f) right = -0.8f - 0.2f * tanhf((-right - 0.8f) / 0.2f);
                }
                /* Clamp to [-1, 1] range (only the first samples of a
                 * sudden transient can still reach it) */
                if (left > 1.0f) left = 1.0f;
                if (left < -1.0f) left = -1.0f;
                if (right > 1.0f) right = 1.0f;
                if (right < -1.0f) right = -1.0f;
                /* Write (not accumulate) into the frame buffer; each of
                 * the 8 sub-frames writes its own 32-sample slice. */
                d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
                d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
            }
            if (use_lim) env = target;
            g_apu_lim_min = (env < g_apu_lim_min) ? env : g_apu_lim_min;
        }
        cap_frame((const int16_t (*)[2])&d->monitor.frame_buf[off], fxs);
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
