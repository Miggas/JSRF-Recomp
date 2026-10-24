/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* v200i: text the game side adds to the window title (the area picker) */
static char s_title_extra[96];
void xbox_SetTitleExtra(const char *t)
{
    if (!t) t = "";
    strncpy(s_title_extra, t, sizeof s_title_extra - 1);
    s_title_extra[sizeof s_title_extra - 1] = 0;
}

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */
/* v87: the GL backend can render above the guest's resolution
 * (RECOMP_GL_SCALE) and hands over the full-size frame. Buffers are sized
 * for that; each published frame carries its own size. */
static uint32_t      s_scale = 1;
static uint32_t      s_pres_w[2], s_pres_h[2];
static uint32_t      s_rgb_w = 640, s_rgb_h = 480;
/* v88: the GL backend draws frames into this window itself; once it does,
 * the window thread stops painting over them */
static volatile HWND s_hwnd;
static volatile LONG s_gl_owned;

void *xbox_FramebufferWindowHwnd(void)
{
    return s_fb_running ? (void *)s_hwnd : NULL;
}

void xbox_FramebufferWindowGLOwned(void)
{
    InterlockedExchange(&s_gl_owned, 1);
}

static uint32_t fb_scale(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_SCALE");
        const char *r = getenv("RECOMP_RENDERER");
        v = e ? atoi(e) : 1;
        if (v < 1) v = 1;
        if (v > 8) v = 8;
        if (r && (!strcmp(r, "sw") || !strcmp(r, "software")))
            v = 1;
    }
    return (uint32_t)v;
}

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* v187: the shape the picture is shown at. 4:3 unless
 *   RECOMP_WIDESCREEN=1      16:9
 *   RECOMP_WIDESCREEN=auto   the shape of the main monitor (16:10 on a Steam
 *                            Deck, 21:9 on an ultrawide)
 *   RECOMP_ASPECT=w:h        any shape (implies widescreen), e.g. 16:10
 * The 3D is then drawn wider to fill it (nv2a_pb_exec.c, ws_k_for). */
double xbox_PresentAspect(void)
{
    static double a = 0.0;
    if (a == 0.0) {
        const char *e = getenv("RECOMP_ASPECT");
        const char *w = getenv("RECOMP_WIDESCREEN");
        double x = 0, y = 0;
        a = 4.0 / 3.0;
        if (e && sscanf(e, "%lf%*[:x/]%lf", &x, &y) == 2 && x > 0 && y > 0)
            a = x / y;
        else if (w && !_stricmp(w, "auto")) {
            int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            if (sw > 0 && sh > 0)
                a = (double)sw / sh;
        } else if (w && w[0] == '1')
            a = 16.0 / 9.0;
        if (a < 1.0 || a > 4.0)
            a = 4.0 / 3.0;
        if (a != 4.0 / 3.0)
            fprintf(stderr, "[WS] widescreen: shown at %.3f:1, 3D squeezed by %.4f "
                    "(RECOMP_WIDESCREEN / RECOMP_ASPECT)\n", a, (4.0 / 3.0) / a);
    }
    return a;
}

/* v187n: screens built for 4:3 only (the garage menus, the map, character
 * select, VS setup...; jsrf_widescreen.c) call xbox_WidescreenHold43() every
 * frame they are shown, and the picture is 4:3 while they do (after Piaka's
 * widescreen patch). v187p: switched at once, no 0.2 s slide; v187o's
 * "only gameplay is wide" is gone. s_ws_n: 1 while held at 4:3, else 0,
 * set once per presented frame so a frame is drawn and shown alike. */
static volatile ULONGLONG s_ws_hold_ms;
static volatile double s_ws_n;

void xbox_WidescreenHold43(void)
{
    s_ws_hold_ms = GetTickCount64();
}

static void ws_tick(void)
{
    static int said = -1;
    int narrow = GetTickCount64() - s_ws_hold_ms <= 50;
    if (narrow != said && said >= 0 && xbox_PresentAspect() > 4.0 / 3.0 + 1e-6)
        fprintf(stderr, "[WS] %s\n", narrow ? "4:3 screen: shown 4:3" : "widescreen again");
    said = narrow;
    s_ws_n = narrow ? 1.0 : 0.0;
}

/* (4/3) / shown aspect: 1 for 4:3, 0.75 for 16:9 (moving towards 1 while a
 * 4:3-only screen is shown) */
double xbox_WidescreenK(void)
{
    double k0 = (4.0 / 3.0) / xbox_PresentAspect();
    return k0 + (1.0 - k0) * s_ws_n;
}

/* the largest rectangle of the shown shape inside cw x ch */
void xbox_PresentFit(int cw, int ch, int *dw, int *dh)
{
    double a;
    ws_tick();                                  /* once per presented frame */
    a = (4.0 / 3.0) / xbox_WidescreenK();
    int w = cw, h = (int)(cw / a + 0.5);
    if (h > ch) { h = ch; w = (int)(ch * a + 0.5); }
    if (w > cw) w = cw;
    *dw = w; *dh = h;
}

/* v68: the window thread drew on its own 16 ms sleep, which beats against
 * the game's 60 Hz flips: every so often a frame was shown twice and the
 * next one never -- a regular hitch at a steady 60 fps. It now waits for
 * the flip's signal and draws each frame once, when it arrives.
 * RECOMP_FB_POLL=1 restores the timer. */
static HANDLE        s_frame_ev;
static volatile LONG s_frame_seq;

static void frame_published(void)
{
    InterlockedIncrement(&s_frame_seq);
    if (s_frame_ev)
        SetEvent(s_frame_ev);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height
                                             * s_scale * s_scale, 4);
        if (!s_present[next])
            return;
    }
    s_pres_w[next] = s_fb_width; s_pres_h[next] = s_fb_height;
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
    frame_published();
}

/* v67: a finished frame straight from the GL backend's read of its render
 * target (BGRA, rows bottom-up), so it no longer goes through guest memory
 * and two pixel conversions. */
int xbox_FramebufferWindowAccepts(uint32_t w, uint32_t h)
{
    static int pinned = -1;     /* v151: read once */
    if (pinned < 0) pinned = getenv("RECOMP_FB_VA") != NULL;
    return s_fb_running && !pinned
        && ((w == s_fb_width && h == s_fb_height)
            || (w == s_fb_width * s_scale && h == s_fb_height * s_scale));
}

int xbox_FramebufferWindowPresentPixels(const uint32_t *px, uint32_t w, uint32_t h)
{
    LONG next;
    uint32_t y;

    if (!px || !xbox_FramebufferWindowAccepts(w, h))
        return 0;
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height
                                             * s_scale * s_scale, 4);
        if (!s_present[next])
            return 0;
    }
    for (y = 0; y < h; y++)
        memcpy(s_present[next] + (size_t)y * w, px + (size_t)(h - 1 - y) * w,
               (size_t)w * 4);
    s_pres_w[next] = w; s_pres_h[next] = h;
    InterlockedExchange(&s_present_idx, next);
    frame_published();
    return 1;
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* v201b: while the free camera flies, the game sees no keys (WASD, Shift
 * and Ctrl would also move the skater) and the mouse is held in the window */
static volatile LONG s_key_mask, s_mouse_cap, s_wheel;

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255 || s_key_mask)
        return 0;
    return s_key_down[vk] != 0;
}

int xbox_FramebufferKeyRaw(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Free camera input, called from the game thread once a frame. capture=1
 * hides the cursor over the window, keeps it centred and reports how far it
 * moved since the last call; wheel is in notches (120 = 1). Only while the
 * window has the focus, so alt-tabbing away gives the mouse back. */
void xbox_FreecamInput(int capture, int *dx, int *dy, int *wheel)
{
    static int had;
    HWND h = s_hwnd;
    *dx = *dy = 0;
    *wheel = 0;
    InterlockedExchange(&s_key_mask, capture);
    if (!capture || !h || GetForegroundWindow() != h) {
        InterlockedExchange(&s_mouse_cap, 0);
        InterlockedExchange(&s_wheel, 0);
        had = 0;
        return;
    }
    InterlockedExchange(&s_mouse_cap, 1);
    {
        RECT r;
        POINT c, p;
        GetClientRect(h, &r);
        c.x = (r.right - r.left) / 2;
        c.y = (r.bottom - r.top) / 2;
        ClientToScreen(h, &c);
        if (had && GetCursorPos(&p)) {
            *dx = p.x - c.x;
            *dy = p.y - c.y;
        }
        SetCursorPos(c.x, c.y);
        had = 1;
    }
    *wheel = (int)InterlockedExchange(&s_wheel, 0);
}

/* v139: borderless full screen on the window's monitor (RECOMP_FULLSCREEN=1
 * at start, F11 / Alt+Enter to switch either way); the picture is fitted to
 * it at 4:3 with black bars */
static int s_fullscreen;
static RECT s_windowed;
static void fb_fullscreen(HWND h, int on)
{
    if (on == s_fullscreen)
        return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetWindowRect(h, &s_windowed);
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongPtrA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, HWND_NOTOPMOST, s_windowed.left, s_windowed.top,
                     s_windowed.right - s_windowed.left, s_windowed.bottom - s_windowed.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    s_fullscreen = on;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        /* Closing the game's window ends the game. It used to only stop
         * this thread, and the next flip opened a fresh window, so the only
         * way out was Task Manager. RECOMP_FB_CLOSE_KEEPS_RUNNING=1 restores
         * the old behaviour. */
        if (!getenv("RECOMP_FB_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "[EXIT] framebuffer window closed by the user\n");
            fflush(stderr);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        /* fall through */
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* v139: F11 or Alt+Enter switches between a window and full screen */
        if ((m == WM_KEYDOWN && w == VK_F11) || (m == WM_SYSKEYDOWN && w == VK_RETURN)) {
            if (!(l & 0x40000000))           /* not auto-repeat */
                fb_fullscreen(h, !s_fullscreen);
            return 0;
        }
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    case WM_MOUSEWHEEL:
        InterlockedExchangeAdd(&s_wheel, (short)HIWORD(w));
        return 0;

    case WM_SETCURSOR:
        if (s_mouse_cap && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;
    s_rgb_w = s_fb_width; s_rgb_h = s_fb_height;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t dw = s_rgb_w, dh = s_rgb_h;
    uint32_t row = ((dw * 3u) + 3u) & ~3u;
    uint32_t img = row * dh, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &dw, 4);
    memcpy(hdr + 22, &dh, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < dh; y++) {
        const uint32_t *src = s_rgb + (size_t)(dh - 1 - y) * dw;
        for (x = 0; x < dw; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, dw, dh, s_fb_va);
    return 0;
}

/* s_rgb (s_rgb_w x s_rgb_h) into the window's client area, 4:3 and
 * centred, the bars black. 1:1 when the sizes match; otherwise stretched,
 * with HALFTONE filtering when shrinking (COLORONCOLOR drops whole rows). */
static void fb_blit(HWND hwnd, HDC hdc, BITMAPINFO *bi)
{
    RECT cr;
    int cw, ch, dw, dh, dx, dy;
    GetClientRect(hwnd, &cr);
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0)
        return;
    xbox_PresentFit(cw, ch, &dw, &dh);      /* v187 */
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    bi->bmiHeader.biWidth = (LONG)s_rgb_w;
    bi->bmiHeader.biHeight = -(LONG)s_rgb_h;
    if (dw < (int)s_rgb_w) {
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
    } else {
        SetStretchBltMode(hdc, COLORONCOLOR);
    }
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, (int)s_rgb_w, (int)s_rgb_h,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
    if (dx > 0) {
        PatBlt(hdc, 0, 0, dx, ch, BLACKNESS);
        PatBlt(hdc, dx + dw, 0, cw - dx - dw, ch, BLACKNESS);
    }
    if (dy > 0) {
        PatBlt(hdc, 0, 0, cw, dy, BLACKNESS);
        PatBlt(hdc, 0, dy + dh, cw, ch - dy - dh, BLACKNESS);
    }
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.style         = CS_OWNDC;         /* a DC of its own, for OpenGL */
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* the scaled frame 1:1 if it fits the desktop, else the largest 4:3
         * that does; the window can be resized and the frame follows */
        RECT wa;
        LONG cw = (LONG)(s_fb_width * s_scale), ch = (LONG)(s_fb_height * s_scale);
        /* v187: as wide as the shown shape needs */
        cw = (LONG)(ch * xbox_PresentAspect() + 0.5);
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            LONG aw = wa.right - wa.left - 16, ah = wa.bottom - wa.top - 48;
            if (cw > aw) { ch = ch * aw / cw; cw = aw; }
            if (ch > ah) { cw = cw * ah / ch; ch = ah; }
        }
        r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && fs[0] == '1')
            fb_fullscreen(hwnd, 1);
    }
    hdc = GetDC(hwnd);
    s_hwnd = hwnd;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height * s_scale * s_scale, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, frames up to %ux%u)\n",
            s_fb_width, s_fb_height, s_fb_width * s_scale, s_fb_height * s_scale);

    {
        static LONG drawn_seq = -1;
        int poll = getenv("RECOMP_FB_POLL") != NULL;
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int fresh;
        DWORD rv = WAIT_TIMEOUT;
        if (poll) {
            Sleep(16);
        } else {
            /* Messages (input, resizing) wake it too; a timeout keeps the
             * title bar and a frozen picture going when no flips come. */
            rv = MsgWaitForMultipleObjects(1, &s_frame_ev, FALSE, 50, QS_ALLINPUT);
        }
        fresh = s_frame_seq != drawn_seq || rv == WAIT_TIMEOUT;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_gl_owned) {
            /* the GPU draws this window now: just keep the messages going */
        } else if (s_present_idx >= 0 && s_rgb && !fresh && !poll) {
            /* Woken by a message, not a frame: nothing new to draw. */
        } else if (s_present_idx >= 0 && s_rgb) {
            drawn_seq = s_frame_seq;
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx] && s_pres_w[idx]) {
                s_rgb_w = s_pres_w[idx]; s_rgb_h = s_pres_h[idx];
                memcpy(s_rgb, s_present[idx], (size_t)s_rgb_w * s_rgb_h * 4);
            }
            fb_blit(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            fb_blit(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= (s_title_extra[0] ? 250u : 1000u)) {
                LONG f = s_flips;
                wchar_t tb[256];
                _snwprintf(tb, 255, L"%ls | FPS: %.1f | draws: %ld%hs%hs", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws, s_title_extra[0] ? " | " : "",
                           s_title_extra);
                tb[255] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
    }
    }

    s_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    s_scale = fb_scale();
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    if (!s_frame_ev)
        s_frame_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
int xbox_FramebufferWindowAccepts(uint32_t w, uint32_t h) { (void)w; (void)h; return 0; }
int xbox_FramebufferWindowPresentPixels(const uint32_t *px, uint32_t w, uint32_t h)
{ (void)px; (void)w; (void)h; return 0; }
void *xbox_FramebufferWindowHwnd(void) { return 0; }
void xbox_FramebufferWindowGLOwned(void) {}
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
#endif
