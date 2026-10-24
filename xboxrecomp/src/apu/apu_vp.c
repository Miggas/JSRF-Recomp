/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "fpconv.h"

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

/* v199: RECOMP_AUDIO_DEBUG=1 -- the audio debug build's switch. Voice events
 * ([AVOICE]), every looping PCM buffer watched for replays ([RING]), the
 * frame thread's pacing ([APUT]), the game's ADX server timing ([ADX], in
 * recomp_manual.c) and the output captured to WAV (RECOMP_APU_CAPTURE), all
 * with t= milliseconds since start, so a crackle in the capture can be put
 * against the lines around it. */
int apu_audio_debug(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_AUDIO_DEBUG");
        v = e && e[0] == '1';
        if (v)
            fprintf(stderr, "[APU] audio debug on (RECOMP_AUDIO_DEBUG): voice events, buffer"
                    " watch, pacing, ADX timing, output capture\n");
    }
    return v;
}
long long apu_dbg_ms(void)
{
    static int64_t t0;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!t0) t0 = now;
    return (long long)(now - t0);
}
static long s_adbg_lines, s_adbg_dropped;
#define ADBG_MAX_LINES 40000
static int adbg_line(void)
{
    if (s_adbg_lines < ADBG_MAX_LINES) {
        s_adbg_lines++;
        return 1;
    }
    if (!s_adbg_dropped++)
        fprintf(stderr, "[AVOICE] line limit reached (%d); counting only from here\n", ADBG_MAX_LINES);
    return 0;
}
static uint8_t s_adbg_dry[MCPX_HW_MAX_VOICES];   /* a stream voice found no segment */
static int s_adbg_game_off;
static void adbg_voice(MCPXAPUState *d, const char *what, uint16_t v);

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

static void adbg_voice(MCPXAPUState *d, const char *what, uint16_t v)
{
    uint32_t f, ba, ebo, lbo, cbo, st;
    int pitch;
    const char *kind;
    if (!apu_audio_debug() || v >= MCPX_HW_MAX_VOICES || !adbg_line())
        return;
    f = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFFu);
    ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
    ebo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO);
    lbo = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE, NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    cbo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
    st = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFFu);
    pitch = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                    NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    kind = ((f >> 30) & 3) == 2 ? "adpcm" : ((f >> 28) & 3) == 1 ? "s16"
           : ((f >> 28) & 3) == 0 ? "u8" : "s24/32";
    fprintf(stderr, "[AVOICE] t=%lld %-7s v%-3u %s%s%s%s %s, ba %06X ebo %u lbo %u cbo %u,"
            " pitch %d (%.0f Hz), state %08X, fmt %08X\n", apu_dbg_ms(), what, v,
            (f & NV_PAVS_VOICE_CFG_FMT_STEREO) ? "stereo" : "mono",
            (f & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) ? " stream" : "",
            (f & NV_PAVS_VOICE_CFG_FMT_LOOP) ? " loop" : "",
            (f & NV_PAVS_VOICE_CFG_FMT_PERSIST) ? " persist" : "",
            kind, ba, ebo, lbo, cbo, pitch, 48000.0 * pow(2.0, pitch / 4096.0), st, f);
    /* another voice already playing the same memory: two copies of one sound
     * (or one sound over another's leftovers) heard at once */
    if (!strcmp(what, "on") && !(f & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE)) {
        uint16_t u;
        for (u = 0; u < MCPX_HW_MAX_VOICES; u++) {
            if (u == v || !voice_get_mask(d, u, NV_PAVS_VOICE_PAR_STATE,
                                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                continue;
            if (voice_get_mask(d, u, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_DATA_TYPE))
                continue;
            if (voice_get_mask(d, u, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA) == ba
                    && adbg_line())
                fprintf(stderr, "[AVOICE] t=%lld   v%u starts on buffer %06X that v%u is still"
                        " playing (at %u of %u)\n", apu_dbg_ms(), v, ba, u,
                        voice_get_mask(d, u, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, u, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO));
        }
    }
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    if (apu_audio_debug() && voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                            NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
        adbg_voice(d, s_adbg_game_off ? "off" : "ended", v);
    if (v < MCPX_HW_MAX_VOICES)
        s_adbg_dry[v] = 0;
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    qemu_mutex_lock(&d->lock);

    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }

    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);
        s_adbg_dry[selected_handle & 0xFF] = 0;
        adbg_voice(d, "on", (uint16_t)selected_handle);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
        adbg_voice(d, "release", (uint16_t)selected_handle);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        s_adbg_game_off = 1;
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        s_adbg_game_off = 0;
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        adbg_voice(d, (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) ? "pause" : "resume",
                   (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE));
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            d->set_irq = true;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

/* v69: per-voice statistics for the [VOICE] report (mcpx_apu_vp_report):
 * what each playing voice was asked for and what it delivered, so a sound
 * that comes out slow, crackling or stretched shows which of rate, missing
 * data, filter or level is responsible. */
static struct {
    uint32_t frames, out, short_frames, starved, src;
    int16_t  p; int8_t ps; float ef, ea, rate;
    uint32_t fmt, misc;
    int      lpf; int16_t fc; uint16_t q;
    uint8_t  bin[8]; uint16_t vol[8];
    float    peak, clipped;
    uint32_t ba; uint16_t hrtf;      /* v200: buffer and HRTF entry, for the report */
    uint16_t vmin[8], vmax[8];       /* v200: the level range over the period */
} s_vst[MCPX_HW_MAX_VOICES];

/* v174: the music watch (RECOMP_AUDIO_REPORT / QUIET off). JSRF streams its
 * music (CRI ADX) into a looping stereo 16-bit buffer, written once a frame
 * from the vsync thread while the voice plays it. On the Deck the music now
 * and then crackles for a whole map: seams in the data where the voice
 * reads. The VP notes each such voice's buffer and play position; after each
 * ADX server run (mcpx_apu_music_watch, from the vsync hook) the buffer is
 * compared with the previous run's copy to see where the new data went,
 * relative to where the voice was. Reported every 2 s with [BGM]. */
/* v199: every looping PCM voice, not just stereo 16-bit music: JSRF's
 * dialogue (Professor K) is streamed the same way into a mono loop, and it is
 * the one heard jumbled. bs = bytes a sample frame, kind bit 0 stereo, bit 1
 * 8-bit. [RING] lines (were [BGM]); with RECOMP_AUDIO_DEBUG also each moment
 * a voice plays past the newest data the title wrote -- it then replays the
 * previous lap of the loop, old words over new. */
#define MW_SLOTS 8
typedef struct { volatile uint32_t v, ba, ebo, cbo, sge, frame, bs, kind; } MwVoice;
static MwVoice s_mw[MW_SLOTS];
static volatile uint32_t s_mw_frame;
static void mw_note(uint32_t v, uint32_t ba, uint32_t ebo, uint32_t cbo, uint32_t sge,
                    uint32_t bs, uint32_t kind)
{
    int i, k = 0;
    for (i = 0; i < MW_SLOTS; i++) {
        if (s_mw[i].frame && s_mw[i].v == v) { k = i; break; }
        if (s_mw[i].frame < s_mw[k].frame) k = i;
    }
    s_mw[k].v = v; s_mw[k].ba = ba; s_mw[k].ebo = ebo; s_mw[k].sge = sge;
    s_mw[k].cbo = cbo; s_mw[k].bs = bs; s_mw[k].kind = kind;
    s_mw[k].frame = ++s_mw_frame;
}

static double mw_left(const uint8_t *buf, long ix, long n, uint32_t bs, uint32_t kind)
{
    const uint8_t *q;
    ix %= n;
    if (ix < 0) ix += n;
    q = buf + (size_t)ix * bs;
    if (kind & 2) return ((double)q[0] - 128.0) * 256.0;
    return (double)(int16_t)(q[0] | (q[1] << 8));
}

void mcpx_apu_music_watch(void)
{
    static uint8_t *prev[MW_SLOTS];
    static uint32_t prev_n[MW_SLOTS], prev_ba[MW_SLOTS], prev_cbo[MW_SLOTS], prev_v[MW_SLOTS];
    static uint32_t prev_bs[MW_SLOTS], prev_kind[MW_SLOTS];
    static uint8_t *cur;
    static size_t cur_cap;
    static long runs[MW_SLOTS], writes[MW_SLOTS], s0min[MW_SLOTS], s0max[MW_SLOTS], s0sum[MW_SLOTS];
    static long s1min[MW_SLOTS], s1max[MW_SLOTS], s1sum[MW_SLOTS];
    static long wsum[MW_SLOTS], asum[MW_SLOTS], over[MW_SLOTS];
    /* v175: is the new data continuous with the run before? */
    static long pend[MW_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static long contig[MW_SLOTS], gaps[MW_SLOTS], gmin[MW_SLOTS], gmax[MW_SLOTS];
    static long bseam[MW_SLOTS], iseam[MW_SLOTS];
    /* v199: fresh data still ahead of the voice; overruns of it */
    static long fresh[MW_SLOTS], lead_min[MW_SLOTS], overruns[MW_SLOTS], over_most[MW_SLOTS];
    static int in_over[MW_SLOTS];
    static int64_t last;
    int k, dbg = apu_audio_debug();
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    for (k = 0; k < MW_SLOTS; k++) {
        uint32_t v = s_mw[k].v, ba = s_mw[k].ba, n = s_mw[k].ebo + 1, sge = s_mw[k].sge;
        uint32_t cbo = s_mw[k].cbo, bs = s_mw[k].bs, kind = s_mw[k].kind, off, i;
        size_t bytes = (size_t)n * bs;
        if (!s_mw[k].frame || s_mw_frame - s_mw[k].frame > 3000 || n > (1u << 18) || !bs)
            continue;
        if (bytes > cur_cap) {
            free(cur);
            cur = (uint8_t *)malloc(bytes);
            cur_cap = cur ? bytes : 0;
            if (!cur) return;
        }
        for (off = 0; off < bytes; ) {
            uint32_t a2 = ba + off, chunk = TARGET_PAGE_SIZE - (a2 % TARGET_PAGE_SIZE);
            if (chunk > bytes - off) chunk = (uint32_t)(bytes - off);
            memcpy(cur + off, mcpx_apu_phys(get_data_ptr(sge, 0xFFFFFFFF, a2)), chunk);
            off += chunk;
        }
        if (prev[k] && prev_n[k] == n && prev_ba[k] == ba && prev_v[k] == v && prev_bs[k] == bs) {
            long cnt = 0, d0 = -1, d1 = -1;
            long adv = (long)((cbo + n - prev_cbo[k]) % n);   /* played since the last run */
            for (i = 0; i < n; i++)
                if (memcmp(cur + (size_t)i * bs, prev[k] + (size_t)i * bs, bs)) {
                    long dd = (long)((i + n - cbo) % n);   /* samples ahead of the voice */
                    if (d0 < 0 || dd < d0) d0 = dd;
                    if (dd > d1) d1 = dd;
                    cnt++;
                }
            runs[k]++;
            asum[k] += adv;
            fresh[k] -= adv;
            if (cnt) {
                long b2 = (long)((cbo + (uint32_t)d0) % n), e = (long)((cbo + (uint32_t)d1) % n);
                long len = d1 - d0 + 1, j;
                double acc = 0;
                int nacc = 0;
#define MW_L(ix) mw_left(cur, (ix), (long)n, bs, kind)
                for (j = 2; j < len; j++) {
                    long ix = (b2 + j) % (long)n;
                    acc += fabs(MW_L(ix) - 2 * MW_L(ix - 1) + MW_L(ix - 2));
                    nacc++;
                }
                if (pend[k] >= 0) {
                    long g = (long)((b2 - pend[k] - 1 + (long)n) % (long)n);
                    if (g == 0) {
                        double avg = nacc ? acc / nacc : 0;
                        double r = fabs(MW_L(b2) - 2 * MW_L(b2 - 1) + MW_L(b2 - 2));
                        double r1 = fabs(MW_L(b2 + 1) - 2 * MW_L(b2) + MW_L(b2 - 1));
                        contig[k]++;
                        if ((r > 6 * avg + 300 || r1 > 6 * avg + 300) && avg < 4000)
                            bseam[k]++;
                    } else {
                        if (g > (long)n / 2) g -= (long)n;     /* overlap: negative */
                        if (!gaps[k] || g < gmin[k]) gmin[k] = g;
                        if (!gaps[k] || g > gmax[k]) gmax[k] = g;
                        gaps[k]++;
                        if (dbg && adbg_line())
                            fprintf(stderr, "[RING] t=%lld voice %u: new data %s the last write by"
                                    " %ld samples (at %ld..%ld, voice at %u)\n", apu_dbg_ms(), v,
                                    g < 0 ? "overlaps" : "leaves a hole after", g < 0 ? -g : g,
                                    b2, e, cbo);
                    }
                }
                if (nacc > 8) {
                    double avg = acc / nacc;
                    for (j = 4; j < len - 2; j++) {
                        long ix = (b2 + j) % (long)n;
                        if (fabs(MW_L(ix) - 2 * MW_L(ix - 1) + MW_L(ix - 2)) > 8 * avg + 400) {
                            iseam[k]++;
                            break;
                        }
                    }
                }
#undef MW_L
                pend[k] = e;
                fresh[k] = d1 + 1;                 /* fresh data now ahead of the voice */
                if (!writes[k] || d0 < s0min[k]) s0min[k] = d0;
                if (!writes[k] || d0 > s0max[k]) s0max[k] = d0;
                if (!writes[k] || d1 < s1min[k]) s1min[k] = d1;
                if (!writes[k] || d1 > s1max[k]) s1max[k] = d1;
                s0sum[k] += d0; s1sum[k] += d1;
                wsum[k] += cnt;
                if (d0 == 0 || d1 >= (long)n - 64) over[k]++;   /* written at or just behind the voice */
                writes[k]++;
            }
            if (writes[k] && (runs[k] == 1 || fresh[k] < lead_min[k])) lead_min[k] = fresh[k];
            if (writes[k] && fresh[k] < 0) {
                /* the voice is past everything new: playing the last lap again */
                if (-fresh[k] > over_most[k]) over_most[k] = -fresh[k];
                if (!in_over[k]) {
                    in_over[k] = 1;
                    overruns[k]++;
                    if (dbg && adbg_line())
                        fprintf(stderr, "[RING] t=%lld voice %u: played past the newest data --"
                                " replaying the previous lap of its %u-sample loop\n",
                                apu_dbg_ms(), v, n);
                }
            } else if (in_over[k] && cnt) {
                in_over[k] = 0;
                if (dbg && adbg_line())
                    fprintf(stderr, "[RING] t=%lld voice %u: new data again (%ld samples ahead)\n",
                            apu_dbg_ms(), v, fresh[k]);
            }
        } else {
            free(prev[k]);
            prev[k] = (uint8_t *)malloc(bytes);
            pend[k] = -1;
            contig[k] = gaps[k] = bseam[k] = iseam[k] = 0;
            runs[k] = writes[k] = wsum[k] = asum[k] = over[k] = 0;
            s0sum[k] = s1sum[k] = 0;
            fresh[k] = lead_min[k] = overruns[k] = over_most[k] = 0;
            in_over[k] = 0;
            if (dbg && adbg_line())
                fprintf(stderr, "[RING] t=%lld watching voice %u: %s %s loop of %u samples at %06X\n",
                        apu_dbg_ms(), v, (kind & 1) ? "stereo" : "mono", (kind & 2) ? "u8" : "s16", n, ba);
        }
        if (prev[k])
            memcpy(prev[k], cur, bytes);
        prev_n[k] = n; prev_ba[k] = ba; prev_cbo[k] = cbo; prev_v[k] = v;
        prev_bs[k] = bs; prev_kind[k] = kind;
    }
    if (!last)
        last = now;
    if (now - last < 2000)
        return;
    last = now;
    for (k = 0; k < MW_SLOTS; k++) {
        if (!runs[k])
            continue;
        if (writes[k])
            fprintf(stderr, "[RING]   voice %u (%s loop of %u samples): %ld runs, %ld wrote; the new data"
                    " began %ld..%ld (avg %ld) and ended %ld..%ld (avg %ld) samples ahead of the voice;"
                    " %ld samples written and %ld played a run; %ld writes at or just behind the voice\n",
                    prev_v[k], (prev_kind[k] & 1) ? "stereo" : "mono", prev_n[k], runs[k], writes[k],
                    s0min[k], s0max[k], s0sum[k] / writes[k],
                    s1min[k], s1max[k], s1sum[k] / writes[k], wsum[k] / writes[k], asum[k] / runs[k], over[k]);
        else
            fprintf(stderr, "[RING]   voice %u (%s loop of %u samples): %ld runs, nothing written\n",
                    prev_v[k], (prev_kind[k] & 1) ? "stereo" : "mono", prev_n[k], runs[k]);
        if (writes[k])
            fprintf(stderr, "[RING]   voice %u: %ld writes followed on from the last one (%ld with a jump"
                    " where they joined), %ld did not (%ld..%ld samples apart); %ld writes with a jump"
                    " inside; fresh data ahead of the voice at least %ld; %ld times it played past the"
                    " newest data (by up to %ld samples)\n", prev_v[k], contig[k], bseam[k], gaps[k],
                    gmin[k], gmax[k], iseam[k], lead_min[k], overruns[k], over_most[k]);
        contig[k] = gaps[k] = bseam[k] = iseam[k] = 0;
        runs[k] = writes[k] = wsum[k] = asum[k] = over[k] = 0;
        s0sum[k] = s1sum[k] = 0;
        overruns[k] = over_most[k] = 0;
        lead_min[k] = fresh[k];
    }
}

static int voice_get_samples_inner(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested);
static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    int n = voice_get_samples_inner(d, v, samples, num_samples_requested);
    if (n > 0 && v < MCPX_HW_MAX_VOICES)
        s_vst[v].src += (uint32_t)n;
    return n;
}

static int voice_get_samples_inner(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            if (!s_adbg_dry[v]) {          /* v199: the title fell behind */
                s_adbg_dry[v] = 1;
                adbg_voice(d, "dry", (uint16_t)v);
            }
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        if (s_adbg_dry[v]) {
            s_adbg_dry[v] = 0;
            adbg_voice(d, "fed", (uint16_t)v);
        }
        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    /* v70: past the end, not at it. The loop above stops either after
     * playing sample `ebo` (cbo is then ebo + 1) or because the request was
     * filled, and when that left cbo == ebo the last sample had not been
     * played yet -- `>=` jumped to the loop start (or the next segment)
     * without it. One sample dropped per pass through a looping buffer is a
     * click each time a streamed track's ring wraps. */
    if (cbo > ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    if (!stream && loop && !adpcm && ebo >= 1023
            && (block_size == 4 || block_size == 2 || block_size == 1))   /* v199: mono, 8-bit too */
        mw_note(v, ba, ebo, cbo, d->regs[NV_PAPU_VPSGEADDR], (uint32_t)block_size,
                (stereo ? 1u : 0u) | (container_size == 1 ? 2u : 0u));
    return sample_count;
}

/* ============================================================
 * Voice resampling (simplified - no libsamplerate)
 *
 * Since libsamplerate is stubbed, we do a simple nearest-neighbor
 * resample. This gives us functional audio at the cost of quality.
 * ============================================================ */

/* Pitch. This used to fetch source samples one for one at 48 kHz whatever
 * the voice's pitch said, so every sound played at 48 kHz: a 22,050 Hz effect
 * came out more than twice as fast and an octave up, an 11,025 Hz one four
 * times (run 56: "accelerated, high pitched"). Now each voice keeps a small
 * buffer of source samples and a fractional read position, and the output is
 * linearly interpolated at `rate` (output samples per source sample, the
 * same meaning xemu's libsamplerate ratio has). A voice that is at exactly
 * 48 kHz takes the old path unchanged.
 *
 * Reading ahead by up to VRES_RAW samples moves the voice's hardware position
 * that much early (under 1.4 ms at 48 kHz); the title sees its buffer
 * consumed marginally sooner, which streaming code tolerates. */
#define VRES_RAW 64
static struct {
    float raw[VRES_RAW][2];
    int n, pos;
    float frac;
    float prev[2], cur[2];
    int primed;
    uint32_t last_frame;
} s_vres[MCPX_HW_MAX_VOICES];
static uint32_t s_vres_frame;
/* v82: each voice's per-bin level last frame, for the volume glide */
static float   s_vg[MCPX_HW_MAX_VOICES][8];
static uint8_t s_vg_bin[MCPX_HW_MAX_VOICES][8];
static uint32_t s_vg_frame[MCPX_HW_MAX_VOICES];
/* v83: voices that began / ended this frame, read (and reset) by the
 * capture in apu_dsp.c */
int g_apu_vp_starts, g_apu_vp_ends;
static uint32_t s_vg_steps, s_vg_mutes;   /* level changes > 1 dB, to/from silence */
static float    s_vg_big;                  /* biggest change in dB (audible ones) */

/* v70: source samples this frame still needs. The resampler used to fetch
 * VRES_RAW (64) at a time, moving the voice's play position up to 64
 * samples past what had been heard. A title streaming into a looping buffer
 * (CRI's ADX player) writes new data just behind the play position it reads
 * back, so samples fetched early were the previous pass's stale data:
 * fragments of old audio in the music, heard as crackle and drag (run 69,
 * title screen, 44.1 kHz stereo voice). Now each frame fetches only what it
 * consumes. RECOMP_APU_READAHEAD=1 restores the 64-sample read-ahead. */
static int s_vres_need;

static int vres_readahead(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("RECOMP_APU_READAHEAD") != NULL;
    return v;
}

static int vres_next(MCPXAPUState *d, uint16_t v, float out[2])
{
    if (s_vres[v].pos >= s_vres[v].n) {
        int want = VRES_RAW;
        if (!vres_readahead()) {
            want = s_vres_need > 0 ? s_vres_need : 1;
            if (want > VRES_RAW) want = VRES_RAW;
        }
        int c = voice_get_samples(d, v, s_vres[v].raw, want);
        if (c <= 0) {
            s_vres[v].n = s_vres[v].pos = 0;
            return 0;
        }
        s_vres[v].n = c;
        s_vres[v].pos = 0;
    }
    out[0] = s_vres[v].raw[s_vres[v].pos][0];
    out[1] = s_vres[v].raw[s_vres[v].pos][1];
    s_vres[v].pos++;
    s_vres_need--;
    return 1;
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    int sample_count = 0;
    float step;

    if (s_vres[v].last_frame + 1 != s_vres_frame && s_vres[v].last_frame != s_vres_frame) {
        /* Not processed last frame: a new sound, start clean. */
        s_vres[v].n = s_vres[v].pos = 0;
        s_vres[v].frac = 0.0f;
        s_vres[v].primed = 0;
    }
    s_vres[v].last_frame = s_vres_frame;

    if (!(rate > 0.0f) || (rate > 0.9999f && rate < 1.0001f && !s_vres[v].primed
                            && s_vres[v].pos >= s_vres[v].n)) {
        while (sample_count < requested_num) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) break;

            int count = voice_get_samples(d, v, &samples[sample_count],
                                          requested_num - sample_count);
            if (count < 0) break;
            if (count == 0) return -1;
            sample_count += count;
        }
        return sample_count;
    }

    step = 1.0f / rate;                       /* source samples per output sample */
    if (step > 32.0f) step = 32.0f;
    if (step < 1.0f / 32.0f) step = 1.0f / 32.0f;

    /* What this call will consume: two to prime, then one each time frac
     * passes a whole sample over the requested outputs. */
    s_vres_need = (int)(s_vres[v].frac + step * (float)requested_num) + 1
                + (s_vres[v].primed ? 0 : 2);

    if (!s_vres[v].primed) {
        if (!vres_next(d, v, s_vres[v].prev))
            return -1;
        if (!vres_next(d, v, s_vres[v].cur)) {
            s_vres[v].cur[0] = s_vres[v].prev[0];
            s_vres[v].cur[1] = s_vres[v].prev[1];
        }
        s_vres[v].frac = 0.0f;
        s_vres[v].primed = 1;
    }
    while (sample_count < requested_num) {
        float f;
        while (s_vres[v].frac >= 1.0f) {
            s_vres[v].prev[0] = s_vres[v].cur[0];
            s_vres[v].prev[1] = s_vres[v].cur[1];
            if (!vres_next(d, v, s_vres[v].cur)) {
                s_vres[v].primed = 0;         /* out of data: the voice ended */
                return sample_count ? sample_count : -1;
            }
            s_vres[v].frac -= 1.0f;
        }
        f = s_vres[v].frac;
        samples[sample_count][0] = s_vres[v].prev[0] + (s_vres[v].cur[0] - s_vres[v].prev[0]) * f;
        samples[sample_count][1] = s_vres[v].prev[1] + (s_vres[v].cur[1] - s_vres[v].prev[1]) * f;
        sample_count++;
        s_vres[v].frac += step;
    }
    return sample_count;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        int empty = 0, sample_count = 0, starved = 0;
        for (; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) break;           /* mixed below, faded */
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) { starved = 1; break; }
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) { starved = 1; break; }
            sample_count += count;
        }
        s_vst[v].frames++;
        s_vst[v].out += (uint32_t)sample_count;
        if (sample_count < NUM_SAMPLES_PER_FRAME) s_vst[v].short_frames++;
        if (starved && voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                      NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            s_vst[v].starved++;
    }
    s_vst[v].p = p; s_vst[v].ps = ps; s_vst[v].ef = ef_value; s_vst[v].rate = rate;
    s_vst[v].fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFFu);
    s_vst[v].ea = ea_value;

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) {
        /* The voice ran out during this frame. Its last samples used to be
         * dropped here, cutting the sound off mid-waveform -- the click at
         * the end of some effects (run 57). Mix them, faded to silence over
         * the frame (under 0.7 ms), so the sound ends without a step. */
        if (multipass)
            return;
        g_apu_vp_ends++;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float g = 1.0f - (float)i / (float)NUM_SAMPLES_PER_FRAME;
            samples[i][0] *= g;
            samples[i][1] *= g;
        }
    }

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
        s_vst[v].bin[i] = (uint8_t)bin[i];
        if (s_vst[v].frames <= 1 || vol[i] < s_vst[v].vmin[i]) s_vst[v].vmin[i] = vol[i];
        if (s_vst[v].frames <= 1 || vol[i] > s_vst[v].vmax[i]) s_vst[v].vmax[i] = vol[i];
        s_vst[v].vol[i] = vol[i];
    }

    s_vst[v].ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
    s_vst[v].hrtf = v < MCPX_HW_MAX_3D_VOICES
        ? (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                   NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE) : 0xFFFFu;

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    s_vst[v].lpf = lpf;
    s_vst[v].misc = (uint32_t)fmode;
    if (lpf) {
        s_vst[v].fc = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA,
                                              NV_PAVS_VOICE_TAR_FCA_FC0);
        s_vst[v].q = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCA,
                                              NV_PAVS_VOICE_TAR_FCA_FC1);
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                if (samples[i][ch] > 1.0f || samples[i][ch] < -1.0f)
                    s_vst[v].clipped += 1.0f;
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices */
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float a = fabsf(samples[i][0]), b2 = fabsf(samples[i][1]);
        if (a > s_vst[v].peak) s_vst[v].peak = a;
        if (b2 > s_vst[v].peak) s_vst[v].peak = b2;
    }

    /* Mix into bins.
     *
     * v82: each bin's level glides from last frame's to this frame's across
     * the 32 samples instead of stepping. The volumes read here are the
     * TARGET registers (TAR_VOL*), which DirectSound rewrites for every 3D
     * voice each time it recomputes positions; the MCPX ramps its current
     * level toward them, and jumping straight there puts a small step in the
     * waveform at every update. One voice's steps are hard to hear; a street
     * full of moving 3D sounds is a constant crackle (run 81: no underruns,
     * no starved voices, limiter idle, crackle still there). A voice that was
     * not playing last frame starts from silence, which also takes the click
     * off sounds that begin mid-waveform. RECOMP_APU_VOL_RAMP=0 steps as
     * before. */
    {
        static int ramp_on = -1;
        int fresh = s_vg_frame[v] + 1 != s_vres_frame;
        if (ramp_on < 0) {
            const char *e = getenv("RECOMP_APU_VOL_RAMP");
            ramp_on = !(e && e[0] == '0');
        }
        s_vg_frame[v] = s_vres_frame;
        if (fresh) g_apu_vp_starts++;
        for (int b = 0; b < 8; b++) {
            float g = ea_value;
            float hr;
            if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
                hr = (float)(1 << d->vp.hrtf_headroom);
            } else {
                hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            }
            g *= attenuate(vol[b]) / hr;
            if (!ramp_on) {
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    mixbins[bin[b]][i] += g * samples[i][b % channels];
                continue;
            }
            {
                float g0 = (fresh || s_vg_bin[v][b] != (uint8_t)bin[b]) ? 0.0f : s_vg[v][b];
                float dg = (g - g0) / (float)NUM_SAMPLES_PER_FRAME;
                if (fresh || s_vg_bin[v][b] != (uint8_t)bin[b]) {
                    /* a new route: only a brand-new voice fades in; a bin
                     * that moved mid-sound takes its level at once */
                    if (!fresh) { g0 = g; dg = 0.0f; }
                }
                if (!fresh && s_vg_bin[v][b] == (uint8_t)bin[b] && g0 != g) {
                    if (g0 <= 0.0f || g <= 0.0f) {
                        s_vg_mutes++;
                    } else {
                        float db = fabsf(20.0f * log10f(g / g0));
                        if (db > 1.0f) {
                            s_vg_steps++;
                            if (db > s_vg_big) s_vg_big = db;
                        }
                    }
                }
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    mixbins[bin[b]][i] += (g0 + dg * (float)(i + 1)) * samples[i][b % channels];
                s_vg[v][b] = g;
                s_vg_bin[v][b] = (uint8_t)bin[b];
            }
        }
    }

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
    s_vres_frame++;

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* Process voice directly (single-threaded) */
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
            }
            d->regs[current] = d->regs[next];
        }
    }

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

/* The [VOICE] report: every voice that played since the last one. Called
 * from the frame thread (apu_core.c), which is the only writer of s_vst. */
void mcpx_apu_vp_report(void)
{
    int v, shown = 0;
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (!s_vst[v].frames)
            continue;
        if (shown++ < 16) {
            uint32_t f = s_vst[v].fmt;
            float hz = s_vst[v].rate > 0 ? 48000.0f / s_vst[v].rate : 0;
            float src_hz = s_vst[v].src * 1500.0f / (s_vst[v].frames ? s_vst[v].frames : 1);
            fprintf(stderr, "[VOICE] %3d: %5u frames, %3.0f%% short, %u starved; pitch %d (ps %d,"
                    " ef %.2f) = %.0f Hz, read %.0f Hz; %s%s%s %s, fmt 0x%08X; ea %.2f, peak %.2f;"
                    " lpf %d fc %d q %u clip %.0f; bins %u/%u vol %u/%u\n",
                    v, s_vst[v].frames,
                    s_vst[v].short_frames * 100.0f / s_vst[v].frames, s_vst[v].starved,
                    s_vst[v].p, s_vst[v].ps, s_vst[v].ef, hz, src_hz,
                    (f & NV_PAVS_VOICE_CFG_FMT_STEREO) ? "stereo" : "mono",
                    (f & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) ? " stream" : "",
                    (f & NV_PAVS_VOICE_CFG_FMT_LOOP) ? " loop" : "",
                    ((f >> 30) & 3) == 2 ? "adpcm" : ((f >> 28) & 3) == 1 ? "s16" :
                    ((f >> 28) & 3) == 0 ? "u8" : "s24/32",
                    f, s_vst[v].ea, s_vst[v].peak, s_vst[v].lpf, s_vst[v].fc, s_vst[v].q,
                    s_vst[v].clipped, s_vst[v].bin[0], s_vst[v].bin[1],
                    s_vst[v].vol[0], s_vst[v].vol[1]);
        }
        {
            /* v200: where each voice goes and how loud, all 8 bins -- volume
             * is attenuation in 1/64 dB steps (0 = full, 0xFFF = silent) --
             * and the range over the period, so a 3D sound DirectSound does
             * or does not fade with distance shows as a range or a constant */
            static int full = -1;
            if (full < 0) {
                const char *e = getenv("RECOMP_AUDIO_VOICES");
                full = e && e[0] == '1';
            }
            if (full && shown <= 48) {
                int b;
                fprintf(stderr, "[VOICE3D] %3d: %s, buffer %06X, %s, HRTF %s%u; bins/attenuation dB (min..max):",
                        v, v < MCPX_HW_MAX_3D_VOICES ? "3D" : "2D", s_vst[v].ba,
                        (s_vst[v].fmt & NV_PAVS_VOICE_CFG_FMT_LOOP) ? "loop" : "one-shot",
                        s_vst[v].hrtf == 0xFFFFu ? "-" : "", s_vst[v].hrtf == 0xFFFFu ? 0 : s_vst[v].hrtf);
                for (b = 0; b < 8; b++)
                    fprintf(stderr, " %u:%.1f..%.1f", s_vst[v].bin[b],
                            s_vst[v].vmin[b] / 64.0, s_vst[v].vmax[b] / 64.0);
                fputc('\n', stderr);
            }
        }
        memset(&s_vst[v], 0, sizeof s_vst[v]);
    }
    if (shown > 16)
        fprintf(stderr, "[VOICE] ... and %d more\n", shown - 16);
    fprintf(stderr, "[VOICE] level changes between frames: %u over 1 dB (largest %.1f dB),"
            " %u to or from silence -- now glided over each frame\n",
            s_vg_steps, s_vg_big, s_vg_mutes);
    s_vg_steps = s_vg_mutes = 0;
    s_vg_big = 0.0f;
}

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
