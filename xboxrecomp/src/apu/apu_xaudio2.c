/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      8      /* ring; at most this many queued */
/* v68: buffers of lead kept queued ahead of the one playing. The producer
 * runs at exactly the playback rate, so whatever lead it starts with is
 * what it keeps: it started with none, the queue sat between 0 and 1
 * buffers, and any late wake-up of the frame thread was a gap -- the music
 * stutter. Topped up with silence at the start and after each underrun.
 * RECOMP_APU_LEAD=<buffers> (0-6) overrides; default 3 (~64 ms). */
#define XA2_DEFAULT_LEAD  3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static int                     g_xa2_lead = -1;
static unsigned long           g_xa2_underruns, g_xa2_silence;
static const int16_t           g_xa2_zero[XA2_BUF_SAMPLES][2];
static int                     g_xa2_qmin = 99, g_xa2_qmax = -1;


/* ── v85: WASAPI output, bypassing XAudio2 ─────────────────────────────
 *
 * RECOMP_APU_BACKEND=wasapi       shared mode, through the Windows mixer
 * RECOMP_APU_BACKEND=wasapi_excl  exclusive mode: the device is ours, no
 *                                 Windows mixer or format conversion
 *
 * The APU frame thread pushes 1024-sample blocks into a ring; a render
 * thread woken by the device's event copies from the ring into the device
 * buffer. The lead (RECOMP_APU_LEAD, default 3 blocks, ~64 ms) is kept in
 * the ring, and RECOMP_APU_WASAPI_MS sets the device buffer (default 20).
 * A device period that finds the ring short is padded with silence and
 * counted as an underrun, so a timing problem shows in the log as a
 * number instead of a crackle nobody can attribute. */
#include <mmdeviceapi.h>
#include <audioclient.h>

static const GUID s_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID s_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID s_IID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID s_IID_IAudioRenderClient =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID s_KSDATAFORMAT_SUBTYPE_PCM =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#define WAS_RING (16384)                 /* stereo frames, power of two */
static int      g_was_mode;              /* 0 XAudio2, 1 shared, 2 exclusive */
static IAudioClient       *g_was_client;
static IAudioRenderClient *g_was_render;
static HANDLE   g_was_event, g_was_thread;
static volatile LONG g_was_stop;
static UINT32   g_was_buf_frames;
static int      g_was_bits;              /* device container bits: 16 or 32 */
static int16_t  g_was_ring[WAS_RING][2];
static volatile LONG g_was_w, g_was_r;   /* frame counters (wrap via mask) */
static volatile LONG g_was_playing;
static unsigned long g_was_underruns, g_was_dropped, g_was_periods;
static int      g_was_qmin = 1 << 30, g_was_qmax = -1;

static DWORD WINAPI was_thread_proc(LPVOID arg)
{
    HANDLE task = NULL;
    DWORD idx = 0;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* MMCSS "Pro Audio": what WASAPI clients are meant to run at, so a busy
     * game cannot starve the copy into the device buffer. */
    {
        typedef HANDLE (WINAPI *avset_t)(LPCSTR, LPDWORD);
        HMODULE av = LoadLibraryA("avrt.dll");
        avset_t f = av ? (avset_t)GetProcAddress(av, "AvSetMmThreadCharacteristicsA") : NULL;
        if (f) task = f("Pro Audio", &idx);
        if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    while (!InterlockedCompareExchange(&g_was_stop, 0, 0)) {
        UINT32 pad = 0, want, have, i;
        BYTE *dst = NULL;
        LONG r, w;
        if (WaitForSingleObject(g_was_event, 200) != WAIT_OBJECT_0)
            continue;
        if (g_was_mode == 1) {
            if (FAILED(IAudioClient_GetCurrentPadding(g_was_client, &pad)))
                continue;
            want = g_was_buf_frames - pad;
        } else {
            want = g_was_buf_frames;     /* exclusive event mode: whole buffer */
        }
        if (!want)
            continue;
        if (FAILED(IAudioRenderClient_GetBuffer(g_was_render, want, &dst)))
            continue;
        r = g_was_r; w = InterlockedCompareExchange(&g_was_w, 0, 0);
        have = (UINT32)(w - r);
        if ((int)have < g_was_qmin) g_was_qmin = (int)have;
        if ((int)have > g_was_qmax) g_was_qmax = (int)have;
        if (!g_was_playing) {
            int lead = g_xa2_lead < 0 ? XA2_DEFAULT_LEAD : g_xa2_lead;
            if (have >= (UINT32)(lead * XA2_BUF_SAMPLES) || lead == 0)
                g_was_playing = 1;
        }
        if (!g_was_playing)
            have = 0;
        else if (have < want) {
            g_was_underruns++;
            g_was_playing = 0;           /* rebuild the lead before resuming */
        }
        if (have > want) have = want;
        for (i = 0; i < want; i++) {
            int16_t l = 0, rr = 0;
            if (i < have) {
                l = g_was_ring[(r + (LONG)i) & (WAS_RING - 1)][0];
                rr = g_was_ring[(r + (LONG)i) & (WAS_RING - 1)][1];
            }
            if (g_was_bits == 16) {
                ((int16_t *)dst)[i * 2] = l;
                ((int16_t *)dst)[i * 2 + 1] = rr;
            } else {
                ((int32_t *)dst)[i * 2] = (int32_t)l << 16;
                ((int32_t *)dst)[i * 2 + 1] = (int32_t)rr << 16;
            }
        }
        InterlockedExchange(&g_was_r, r + (LONG)have);
        IAudioRenderClient_ReleaseBuffer(g_was_render, want, 0);
        g_was_periods++;
    }
    if (task) {
        typedef BOOL (WINAPI *avrev_t)(HANDLE);
        HMODULE av = GetModuleHandleA("avrt.dll");
        avrev_t f = av ? (avrev_t)GetProcAddress(av, "AvRevertMmThreadCharacteristics") : NULL;
        if (f) f(task);
    }
    CoUninitialize();
    return 0;
}

static void was_fmt(WAVEFORMATEXTENSIBLE *f, int bits, int valid)
{
    memset(f, 0, sizeof *f);
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = 2;
    f->Format.nSamplesPerSec = XA2_SAMPLE_RATE;
    f->Format.wBitsPerSample = (WORD)bits;
    f->Format.nBlockAlign = (WORD)(2 * bits / 8);
    f->Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * f->Format.nBlockAlign;
    f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f->Samples.wValidBitsPerSample = (WORD)valid;
    f->dwChannelMask = 3;   /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    f->SubFormat = s_KSDATAFORMAT_SUBTYPE_PCM;
}

static int was_init(int exclusive)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    HRESULT hr;
    WAVEFORMATEXTENSIBLE f;
    REFERENCE_TIME dur;
    int ms = 20, k;
    const char *e = getenv("RECOMP_APU_WASAPI_MS");
    if (e) ms = atoi(e);
    if (ms < 3) ms = 3;
    if (ms > 200) ms = 200;
    dur = (REFERENCE_TIME)ms * 10000;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { fprintf(stderr, "[WASAPI] no device enumerator: 0x%08lX\n", hr); return 0; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) { fprintf(stderr, "[WASAPI] no default output: 0x%08lX\n", hr); return 0; }
    hr = IMMDevice_Activate(dev, &s_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_was_client);
    if (FAILED(hr)) { IMMDevice_Release(dev); fprintf(stderr, "[WASAPI] activate failed: 0x%08lX\n", hr); return 0; }

    if (!exclusive) {
        was_fmt(&f, 16, 16);
        hr = IAudioClient_Initialize(g_was_client, AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /*AUTOCONVERTPCM*/
                | 0x08000000 /*SRC_DEFAULT_QUALITY*/,
                dur, 0, (WAVEFORMATEX *)&f, NULL);
        g_was_bits = 16;
    } else {
        /* the formats an S/PDIF or USB device usually takes, best first */
        static const int fm[3][2] = {{16, 16}, {32, 24}, {32, 32}};
        REFERENCE_TIME def = 0, mn = 0;
        IAudioClient_GetDevicePeriod(g_was_client, &def, &mn);
        if (dur < mn) dur = mn;
        hr = E_FAIL;
        for (k = 0; k < 3; k++) {
            was_fmt(&f, fm[k][0], fm[k][1]);
            hr = IAudioClient_Initialize(g_was_client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, (WAVEFORMATEX *)&f, NULL);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 n = 0;
                IAudioClient_GetBufferSize(g_was_client, &n);
                IAudioClient_Release(g_was_client);
                g_was_client = NULL;
                IMMDevice_Activate(dev, &s_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_was_client);
                dur = (REFERENCE_TIME)(10000.0 * 1000 * n / XA2_SAMPLE_RATE + 0.5);
                hr = IAudioClient_Initialize(g_was_client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, (WAVEFORMATEX *)&f, NULL);
            }
            if (SUCCEEDED(hr)) { g_was_bits = fm[k][0]; break; }
            /* a failed Initialize leaves the client unusable: start again */
            IAudioClient_Release(g_was_client);
            g_was_client = NULL;
            if (FAILED(IMMDevice_Activate(dev, &s_IID_IAudioClient, CLSCTX_ALL, NULL,
                                          (void **)&g_was_client)))
                break;
        }
    }
    IMMDevice_Release(dev);
    if (FAILED(hr) || !g_was_client) {
        fprintf(stderr, "[WASAPI] %s initialise failed: 0x%08lX\n",
                exclusive ? "exclusive" : "shared", hr);
        if (g_was_client) { IAudioClient_Release(g_was_client); g_was_client = NULL; }
        return 0;
    }
    g_was_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(g_was_client, g_was_event);
    IAudioClient_GetBufferSize(g_was_client, &g_was_buf_frames);
    hr = IAudioClient_GetService(g_was_client, &s_IID_IAudioRenderClient, (void **)&g_was_render);
    if (FAILED(hr)) { fprintf(stderr, "[WASAPI] no render client: 0x%08lX\n", hr); return 0; }
    {   /* start with the device buffer silent */
        BYTE *dst;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(g_was_render, g_was_buf_frames, &dst)))
            IAudioRenderClient_ReleaseBuffer(g_was_render, g_was_buf_frames,
                                             AUDCLNT_BUFFERFLAGS_SILENT);
    }
    g_was_stop = 0;
    g_was_thread = CreateThread(NULL, 0, was_thread_proc, NULL, 0, NULL);
    hr = IAudioClient_Start(g_was_client);
    if (FAILED(hr)) { fprintf(stderr, "[WASAPI] start failed: 0x%08lX\n", hr); return 0; }
    g_was_mode = exclusive ? 2 : 1;
    fprintf(stderr, "[WASAPI] %s mode, 48000 Hz stereo %d-bit, device buffer %u frames"
            " (%.1f ms)\n", exclusive ? "exclusive" : "shared", g_was_bits == 16 ? 16 : 24,
            g_was_buf_frames, g_was_buf_frames * 1000.0 / XA2_SAMPLE_RATE);
    return 1;
}

static int was_submit(const int16_t *samples, int n)
{
    LONG w = g_was_w, r = InterlockedCompareExchange(&g_was_r, 0, 0);
    int i;
    if (g_xa2_lead < 0) {
        const char *e = getenv("RECOMP_APU_LEAD");
        g_xa2_lead = e ? atoi(e) : XA2_DEFAULT_LEAD;
        if (g_xa2_lead < 0) g_xa2_lead = 0;
        if (g_xa2_lead > 8) g_xa2_lead = 8;
    }
    /* more than lead + 4 blocks queued: the producer is ahead of the device
     * clock; drop this block rather than let latency grow */
    if ((w - r) + n > (g_xa2_lead + 4) * XA2_BUF_SAMPLES || (w - r) + n > WAS_RING) {
        g_was_dropped++;
        return 0;
    }
    for (i = 0; i < n; i++) {
        g_was_ring[(w + i) & (WAS_RING - 1)][0] = samples[i * 2];
        g_was_ring[(w + i) & (WAS_RING - 1)][1] = samples[i * 2 + 1];
    }
    InterlockedExchange(&g_was_w, w + n);
    g_xa2_frames_written++;
    return 1;
}

static void was_shutdown(void)
{
    InterlockedExchange(&g_was_stop, 1);
    if (g_was_event) SetEvent(g_was_event);
    if (g_was_thread) { WaitForSingleObject(g_was_thread, 1000); CloseHandle(g_was_thread); }
    if (g_was_client) IAudioClient_Stop(g_was_client);
    if (g_was_render) IAudioRenderClient_Release(g_was_render);
    if (g_was_client) IAudioClient_Release(g_was_client);
    g_was_render = NULL; g_was_client = NULL; g_was_thread = NULL;
    g_was_mode = 0;
}

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    {
        const char *b = getenv("RECOMP_APU_BACKEND");
        if (b && !_strnicmp(b, "wasapi", 6)) {
            int excl = strstr(b, "excl") != NULL;
            if (was_init(excl)) {
                g_xa2_initialized = 1;
                g_xa2_frames_written = 0;
                return 1;
            }
            fprintf(stderr, "[WASAPI] falling back to XAudio2\n");
        }
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_was_mode) {
        was_shutdown();
        g_xa2_initialized = 0;
        return;
    }
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (g_was_mode && g_xa2_initialized) return was_submit(samples, num_samples);
    if (!g_xa2_initialized || !g_xa2_source) return 0;

    if (g_xa2_lead < 0) {
        const char *e = getenv("RECOMP_APU_LEAD");
        g_xa2_lead = e ? atoi(e) : XA2_DEFAULT_LEAD;
        if (g_xa2_lead < 0) g_xa2_lead = 0;
        if (g_xa2_lead > XA2_NUM_BUFS - 2) g_xa2_lead = XA2_NUM_BUFS - 2;
    }
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued < g_xa2_qmin) g_xa2_qmin = (int)state.BuffersQueued;
    if ((int)state.BuffersQueued > g_xa2_qmax) g_xa2_qmax = (int)state.BuffersQueued;
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;
    if ((int)state.BuffersQueued == 0) {
        /* Ran dry (or the very start): rebuild the lead with silence, which
         * is played once instead of a gap after every late frame. */
        int k;
        if (g_xa2_frames_written)
            g_xa2_underruns++;
        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)g_xa2_zero;
        for (k = 0; k < g_xa2_lead; k++)
            if (SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL)))
                g_xa2_silence++;
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

void xa2_get_stats(unsigned long *underruns, unsigned long *silence)
{
    *underruns = g_xa2_underruns + g_was_underruns;
    *silence = g_xa2_silence + g_was_dropped;
}

/* v84: what the audio engine itself saw. A glitch is a pass of XAudio2's
 * own processing that missed its deadline -- heard as a crackle, and not
 * visible in our queue (which never ran dry). The queue range says how much
 * lead we really kept. Min/max reset on each call. */
void xa2_get_health(unsigned long *glitches, int *qmin, int *qmax)
{
    XAUDIO2_PERFORMANCE_DATA perf;
    *glitches = 0;
    if (g_was_mode) {
        /* device periods short of data, and the ring depth in blocks */
        *glitches = g_was_underruns;
        *qmin = g_was_qmin > (1 << 29) ? 0 : g_was_qmin / XA2_BUF_SAMPLES;
        *qmax = g_was_qmax < 0 ? 0 : g_was_qmax / XA2_BUF_SAMPLES;
        g_was_qmin = 1 << 30;
        g_was_qmax = -1;
        return;
    }
    if (g_xa2) {
        memset(&perf, 0, sizeof perf);
        g_xa2->lpVtbl->GetPerformanceData(g_xa2, &perf);
        *glitches = (unsigned long)perf.GlitchesSinceEngineStarted;
    }
    *qmin = g_xa2_qmin > 8 ? 0 : g_xa2_qmin;
    *qmax = g_xa2_qmax < 0 ? 0 : g_xa2_qmax;
    g_xa2_qmin = 99;
    g_xa2_qmax = -1;
}

#else /* !_WIN32 -- v244: SDL2 audio on Linux, with XAudio2's queue rules */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same shape as the XAudio2 path above: 48 kHz stereo 16-bit, buffers of
 * 1024 samples, at most 8 queued (a full queue refuses the next one, which
 * is how the APU's frame thread keeps real time), and a lead of silence
 * rebuilt when it runs dry. SDL's queue is counted in bytes; "buffers
 * queued" is that over one buffer's size. */
#define SA_RATE     48000
#define SA_BUF      1024
#define SA_BUFS     8
#define SA_LEAD     3
static SDL_AudioDeviceID s_sa_dev;
static int s_sa_on, s_sa_lead = -1, s_sa_qmin = 99, s_sa_qmax = -1;
static unsigned long s_sa_under, s_sa_silence, s_sa_frames;

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    if (s_sa_on)
        return 1;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    memset(&want, 0, sizeof want);
    want.freq = SA_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = NULL;                     /* queued */
    s_sa_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_sa_dev) {
        fprintf(stderr, "[AUDIO] no audio device: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(s_sa_dev, 0);
    s_sa_on = 1;
    fprintf(stderr, "[AUDIO] SDL audio (%s) at %d Hz stereo 16-bit, %d x %d-sample buffers\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq,
            SA_BUFS, SA_BUF);
    return 1;
}

void xa2_shutdown(void)
{
    if (s_sa_dev) {
        SDL_CloseAudioDevice(s_sa_dev);
        s_sa_dev = 0;
    }
    s_sa_on = 0;
}

int xa2_is_active(void) { return s_sa_on; }

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    const Uint32 buf_bytes = SA_BUF * 2 * sizeof(int16_t);
    Uint32 queued;
    int q, n;
    if (!s_sa_on)
        return 0;
    if (s_sa_lead < 0) {
        const char *e = getenv("RECOMP_APU_LEAD");
        s_sa_lead = e ? atoi(e) : SA_LEAD;
        if (s_sa_lead < 0) s_sa_lead = 0;
        if (s_sa_lead > SA_BUFS - 2) s_sa_lead = SA_BUFS - 2;
    }
    queued = SDL_GetQueuedAudioSize(s_sa_dev);
    q = (int)((queued + buf_bytes - 1) / buf_bytes);
    if (q < s_sa_qmin) s_sa_qmin = q;
    if (q > s_sa_qmax) s_sa_qmax = q;
    if (q >= SA_BUFS)
        return 0;
    if (queued == 0) {
        static const int16_t zero[SA_BUF][2];
        int k;
        if (s_sa_frames)
            s_sa_under++;
        for (k = 0; k < s_sa_lead; k++)
            if (SDL_QueueAudio(s_sa_dev, zero, buf_bytes) == 0)
                s_sa_silence++;
    }
    n = num_samples > SA_BUF ? SA_BUF : num_samples;
    if (SDL_QueueAudio(s_sa_dev, samples, (Uint32)n * 2 * sizeof(int16_t)) != 0)
        return 0;
    s_sa_frames++;
    return 1;
}

int  xa2_get_buffer_size(void) { return SA_BUF; }
void xa2_get_stats(unsigned long *u, unsigned long *s) { *u = s_sa_under; *s = s_sa_silence; }
void xa2_get_health(unsigned long *g, int *a, int *b)
{
    *g = 0;
    *a = s_sa_qmin > SA_BUFS ? 0 : s_sa_qmin;
    *b = s_sa_qmax < 0 ? 0 : s_sa_qmax;
    s_sa_qmin = 99;
    s_sa_qmax = -1;
}

#endif /* _WIN32 */
