/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput, plus SDL2 for the pads XInput does not see (v189)
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit (v250) */
#endif

/* ======================================================================== */
#if defined(_WIN32) || defined(__linux__)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#if defined(_WIN32)
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#else
/* v244: Linux. The same sources as on Windows -- the keyboard (from the
 * game window) and the SDL controllers -- with no XInput: every pad,
 * the Steam Deck's own controls included, comes through SDL. */
#include "win32_compat.h"
#ifndef XBOX_INPUT_SDL
#define XBOX_INPUT_SDL 1
#endif
/* XINPUT_STATE and XInputGetState come from win32_compat (never connected) */
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad;
                 XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;
static DWORD XInputSetState(DWORD i, XINPUT_VIBRATION *v) { (void)i; (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
static DWORD XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES *c)
{ (void)i; (void)f; (void)c; return ERROR_DEVICE_NOT_CONNECTED; }
#endif

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));

    if (key_down(VK_UP))     b |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(VK_DOWN))   b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(VK_LEFT))   b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(VK_RIGHT))  b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(VK_RETURN)) b |= XBOX_GAMEPAD_START;
    if (key_down(VK_BACK))   b |= XBOX_GAMEPAD_BACK;
    if (key_down(VK_SHIFT))  b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(VK_CONTROL))b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = key_down('Z') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = key_down('X') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = key_down('A') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = key_down('S') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = key_down('Q') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = key_down('E') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('1') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('3') ? 255 : 0;

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    pState->Gamepad.sThumbLX = axis_from_keys(VK_NUMPAD4, VK_NUMPAD6);
    pState->Gamepad.sThumbLY = axis_from_keys(VK_NUMPAD2, VK_NUMPAD8);
    pState->Gamepad.sThumbRX = axis_from_keys('J', 'L');
    pState->Gamepad.sThumbRY = axis_from_keys('K', 'I');

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* ---- v189: controllers XInput does not see -------------------------------
 *
 * XInput only knows Xbox-style pads, so a DualSense, a DualShock 4 or a
 * Switch Pro controller was invisible on Windows. They are read through
 * SDL's game controller support (SDL2, already built for the renderer),
 * which knows them all, rumble included. Xbox pads are left to XInput, so
 * none is seen twice. They are sources 5..8 for the per-player assignment
 * (usb/usb_gamepad.c), and port n also merges SDL pad n, so with one player
 * any of them drives player 1. RECOMP_SDL_PADS=0 turns this off. */
#if defined(XBOX_INPUT_SDL)
#include <SDL.h>
#define SDL_PADS 4
static SDL_GameController *s_sdl[SDL_PADS];
static char  s_sdl_name[SDL_PADS][48];
static XBOX_GAMEPAD s_sdl_prev[SDL_PADS];
static DWORD s_sdl_packet[SDL_PADS];
static int   s_sdl_on = -1;
static DWORD s_sdl_scan, s_sdl_update;
static CRITICAL_SECTION s_sdl_lock;

static int sdl_is_xinput_type(SDL_GameControllerType t)
{
#if defined(_WIN32)
    return t == SDL_CONTROLLER_TYPE_XBOX360 || t == SDL_CONTROLLER_TYPE_XBOXONE;
#else
    (void)t;
    return 0;                     /* no XInput here: SDL has them all */
#endif
}

static void sdl_scan(void)
{
    int i, k, n;
    for (k = 0; k < SDL_PADS; k++)
        if (s_sdl[k] && !SDL_GameControllerGetAttached(s_sdl[k])) {
            fprintf(stderr, "  PAD: %s disconnected\n", s_sdl_name[k]);
            SDL_GameControllerClose(s_sdl[k]);
            s_sdl[k] = NULL;
        }
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        SDL_JoystickID id;
        int open = 0, slot = -1;
        if (!SDL_IsGameController(i) || sdl_is_xinput_type(SDL_GameControllerTypeForIndex(i)))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(i);
        for (k = 0; k < SDL_PADS; k++) {
            if (s_sdl[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_sdl[k])) == id)
                open = 1;
            if (!s_sdl[k] && slot < 0)
                slot = k;
        }
        if (open || slot < 0)
            continue;
        s_sdl[slot] = SDL_GameControllerOpen(i);
        if (!s_sdl[slot])
            continue;
        memset(&s_sdl_prev[slot], 0, sizeof s_sdl_prev[slot]);
        snprintf(s_sdl_name[slot], sizeof s_sdl_name[slot], "%s",
                 SDL_GameControllerName(s_sdl[slot]) ? SDL_GameControllerName(s_sdl[slot])
                                                     : "controller");
        fprintf(stderr, "  PAD: %s connected (through SDL)\n", s_sdl_name[slot]);
    }
    fflush(stderr);
}

/* Called before every read: brings SDL's state up to date at most every
 * 2 ms, and looks for controllers coming and going once a second. */
static int sdl_poll(void)
{
    DWORD now;
    if (s_sdl_on < 0) {
        const char *e = getenv("RECOMP_SDL_PADS");
        InitializeCriticalSection(&s_sdl_lock);
        s_sdl_on = 0;
        if (!(e && e[0] == '0')) {
            SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
            /* v189b: by position, not by the letters printed on them: SDL
             * 2.30 maps a Nintendo pad's labelled A (the right button) to
             * A, so on a Switch Pro the right button jumped instead of the
             * bottom one. RECOMP_PAD_LABELS=1 goes by the labels. */
            {
                const char *lb = getenv("RECOMP_PAD_LABELS");
                SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS,
                            lb && lb[0] == '1' ? "1" : "0");
            }
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
                SDL_GameControllerEventState(SDL_IGNORE);
                SDL_JoystickEventState(SDL_IGNORE);
                s_sdl_on = 1;
                fprintf(stderr, "  PAD: DualSense, DualShock and other non-Xbox controllers"
                        " through SDL (RECOMP_SDL_PADS=0 turns this off)\n");
            } else {
                fprintf(stderr, "  PAD: SDL controllers unavailable: %s\n", SDL_GetError());
            }
            fflush(stderr);
        }
    }
    if (!s_sdl_on)
        return 0;
    EnterCriticalSection(&s_sdl_lock);
    now = GetTickCount();
    if (now - s_sdl_update >= 2) {
        s_sdl_update = now;
        SDL_GameControllerUpdate();
    }
    if (now - s_sdl_scan >= 1000) {
        s_sdl_scan = now;
        sdl_scan();
    }
    LeaveCriticalSection(&s_sdl_lock);
    return 1;
}

static DWORD sdl_get(int k, XBOX_INPUT_STATE *pState)
{
    SDL_GameController *c;
    XBOX_GAMEPAD *g;
    WORD b = 0;
    if (k < 0 || k >= SDL_PADS || !sdl_poll())
        return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&s_sdl_lock);
    c = s_sdl[k];
    if (!c) {
        LeaveCriticalSection(&s_sdl_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    memset(pState, 0, sizeof *pState);
    g = &pState->Gamepad;
#define SB(x) SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_##x)
    if (SB(DPAD_UP))    b |= XBOX_GAMEPAD_DPAD_UP;
    if (SB(DPAD_DOWN))  b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SB(DPAD_LEFT))  b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SB(DPAD_RIGHT)) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SB(START))      b |= XBOX_GAMEPAD_START;       /* Options */
    if (SB(BACK))       b |= XBOX_GAMEPAD_BACK;        /* Create / Share */
    if (SB(LEFTSTICK))  b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SB(RIGHTSTICK)) b |= XBOX_GAMEPAD_RIGHT_THUMB;
    g->wButtons = b;
    /* positions, as XInput pads are mapped above: cross = A, circle = B,
     * square = X, triangle = Y; L1 = black, R1 = white */
    g->bAnalogButtons[XBOX_BUTTON_A]     = SB(A) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_B]     = SB(B) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_X]     = SB(X) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_Y]     = SB(Y) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_BLACK] = SB(LEFTSHOULDER) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_WHITE] = SB(RIGHTSHOULDER) ? 255 : 0;
#undef SB
    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    /* SDL's Y axes grow downwards, XInput's upwards */
    g->sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    g->sThumbLY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    g->sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    g->sThumbRY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    if (memcmp(g, &s_sdl_prev[k], sizeof *g)) {
        s_sdl_prev[k] = *g;
        s_sdl_packet[k]++;
    }
    pState->dwPacketNumber = s_sdl_packet[k];
    LeaveCriticalSection(&s_sdl_lock);
    return ERROR_SUCCESS;
}

static DWORD sdl_rumble(int k, const XBOX_VIBRATION *v)
{
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
    if (k < 0 || k >= SDL_PADS || !sdl_poll())
        return rc;
    EnterCriticalSection(&s_sdl_lock);
    if (s_sdl[k]) {
        /* the motors keep their speed until the next report, as on the
         * console; SDL wants a duration, so the longest it takes */
        SDL_GameControllerRumble(s_sdl[k], v->wLeftMotorSpeed, v->wRightMotorSpeed, 0xFFFF);
        rc = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&s_sdl_lock);
    return rc;
}

static const char *sdl_name(int k)
{
    return (k >= 0 && k < SDL_PADS && s_sdl_on > 0 && s_sdl[k]) ? s_sdl_name[k] : NULL;
}
#else
static DWORD sdl_get(int k, XBOX_INPUT_STATE *pState) { (void)k; (void)pState; return ERROR_DEVICE_NOT_CONNECTED; }
static DWORD sdl_rumble(int k, const XBOX_VIBRATION *v) { (void)k; (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
static const char *sdl_name(int k) { (void)k; return NULL; }
#endif

/* Another device's state on top of pState: buttons and analog buttons
 * combined, a stick from whichever is pushed further. */
static void merge_state(XBOX_INPUT_STATE *pState, const XBOX_INPUT_STATE *o)
{
    XBOX_GAMEPAD *g = &pState->Gamepad;
    const XBOX_GAMEPAD *h = &o->Gamepad;
    int i;
#define WS_FARTHER(a, b) (abs((int)(b)) > abs((int)(a)) ? (b) : (a))
    g->wButtons |= h->wButtons;
    for (i = 0; i < 8; i++)
        if (h->bAnalogButtons[i] > g->bAnalogButtons[i])
            g->bAnalogButtons[i] = h->bAnalogButtons[i];
    g->sThumbLX = WS_FARTHER(g->sThumbLX, h->sThumbLX);
    g->sThumbLY = WS_FARTHER(g->sThumbLY, h->sThumbLY);
    g->sThumbRX = WS_FARTHER(g->sThumbRX, h->sThumbRX);
    g->sThumbRY = WS_FARTHER(g->sThumbRY, h->sThumbRY);
#undef WS_FARTHER
    pState->dwPacketNumber += o->dwPacketNumber;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

static DWORD input_get(DWORD dwPort, XBOX_INPUT_STATE *pState, int merge_kbd)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        XBOX_INPUT_STATE sp;
        int have = merge_kbd && sdl_get((int)dwPort, &sp) == ERROR_SUCCESS;   /* v189 */
        g_controller_connected[dwPort] = have ? TRUE : FALSE;
        if (merge_kbd && dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            if (have)
                merge_state(pState, &sp);
            return ERROR_SUCCESS;
        }
        if (have) {
            *pState = sp;
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (merge_kbd && dwPort == 0 && keyboard_enabled()) {
        XBOX_INPUT_STATE kb;
        int i;
        keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }

    /* v189: and the n-th controller XInput does not see, for port n */
    if (merge_kbd) {
        XBOX_INPUT_STATE sp;
        if (sdl_get((int)dwPort, &sp) == ERROR_SUCCESS)
            merge_state(pState, &sp);
    }

    /* v122: RECOMP_GL_EFFECTLOG=1 also logs the left stick's vertical zone
     * changes (forward / centre / back, at half deflection) with the same
     * millisecond clock as the [FX] lines, to line a brake up with the
     * frames its sparks are drawn in. */
    if (dwPort == 0) {
        static int on = -1, zone;
        if (on < 0)
            on = getenv("RECOMP_GL_EFFECTLOG") != NULL;
        if (on) {
            int y = pState->Gamepad.sThumbLY, z = y > 16384 ? 1 : y < -16384 ? -1 : 0;
            if (z != zone) {
                fprintf(stderr, "[STICK] t=%llu ms LY %d (%s)\n",
                        (unsigned long long)GetTickCount64(), y,
                        z > 0 ? "forward" : z < 0 ? "back" : "centre");
                zone = z;
            }
        }
    }

    return ERROR_SUCCESS;
}

/* v250: the quit window. Clicking both sticks (L3 + R3) opens it (drawn by
 * kernel/nv2a_gl_quit.h); while it is open the game gets a controller at
 * rest. Left / right (d-pad or left stick) picks YES or NO (NO first), A
 * takes the pick, B closes it; nothing reaches the game until every button
 * is let go again. Any controller (and the keyboard: Shift + Ctrl, then the
 * arrows and Z / X) works it. RECOMP_QUIT_COMBO=0 turns it off. */
#define QD_IDX 64
static volatile LONG s_qd_open;           /* 0 closed, 1 open (v276f: no state 2; s_qd_block per device) */
static WORD s_qd_prev_btn[QD_IDX];
static BYTE s_qd_prev_ab[QD_IDX];
static signed char s_qd_prev_x[QD_IDX];   /* left stick: -1 left, 0, 1 right */
static BYTE s_qd_held[QD_IDX];            /* v276d: a button down on this device */
static volatile BYTE s_qd_block[QD_IDX];  /* v276f: closed with a button down: the game gets nothing from it yet */
static volatile LONG s_qd_sel = 2;        /* v278b: 0 Quit Game, 1 Quit to Title, 2 Return */
static volatile LONG s_qd_title_ok;      /* v278b: the game can go back to the title now (jsrf) */
static volatile LONG s_qd_title_req;     /* v278b: Quit to Title was chosen */
static int  s_qd_off = -1;

int xbox_QuitDialogOpen(void) { return s_qd_open == 1; }
int xbox_QuitDialogSel(void) { return (int)s_qd_sel; }

/* v278: a list menu over the game (s_qd_open == 3), offered by the game's
 * code (jsrf: mission select at the title) with xbox_ListMenuSet; opened by
 * pressing LB + RB together (White + Black; Q + E on the keyboard), it holds
 * the game like the quit window. Up / down picks (held: repeats), left /
 * right a page, A takes the pick (xbox_ListMenuTake), B closes it. Drawn by
 * kernel/nv2a_gl_quit.h. */
#define LM_MAX 512
static const char *s_lm_title;
static const char *const *s_lm_items;
static volatile LONG s_lm_n;              /* 0: not offered now */
static volatile LONG s_lm_sel, s_lm_pick = -1;
static BYTE s_qd_prev_wb[QD_IDX];         /* 1 White (LB), 2 Black (RB) */
static signed char s_qd_prev_y[QD_IDX];   /* left stick: -1 down, 0, 1 up */
static DWORD s_lm_rep_at[QD_IDX];         /* when a held up / down repeats next */
char g_xbox_note[96];                     /* a line drawn at the bottom of the picture while not empty */

void xbox_ListMenuSet(const char *title, const char *const *items, int n)
{
    if (n > LM_MAX) n = LM_MAX;
    if (n > 0) { s_lm_title = title; s_lm_items = items; }
    InterlockedExchange(&s_lm_n, n > 0 ? n : 0);
}
int xbox_ListMenuOpen(void) { return s_qd_open == 3; }
int xbox_ListMenuCount(void) { return (int)s_lm_n; }
int xbox_ListMenuSel(void) { return (int)s_lm_sel; }
const char *xbox_ListMenuTitle(void) { return s_lm_title ? s_lm_title : ""; }
const char *xbox_ListMenuItem(int i) { return s_lm_items && i >= 0 && i < s_lm_n ? s_lm_items[i] : ""; }
int xbox_ListMenuTake(void) { return (int)InterlockedExchange(&s_lm_pick, -1); }
/* v278b: the list menu doesn't hold the game (Miggas): only the quit window does */
int xbox_DialogOpen(void) { return s_qd_open == 1; }
/* v278b: Quit to Title, offered while the game says it can (jsrf: in a stage) */
void xbox_QuitTitleAvailable(int ok) { InterlockedExchange(&s_qd_title_ok, ok ? 1 : 0); }
int xbox_QuitTitleOK(void) { return (int)s_qd_title_ok; }
int xbox_QuitToTitleTake(void) { return (int)InterlockedExchange(&s_qd_title_req, 0); }
static DWORD s_lm_last_t;                 /* v278b: the last move, to take one device's only */
static int s_lm_last_dir;

/* v252: while the window is open the game is held (kernel/nv2a_gl.c holds
 * it at its next frame), its sound too (apu/apu_core.c) */
volatile LONG g_xbox_game_held;

static void quit_filter(int idx, XBOX_INPUT_STATE *st);
static DWORD source_get(int src, XBOX_INPUT_STATE *pState);
/* the game is held and reads no input: the window reads every device */
void xbox_QuitDialogPoll(void)
{
    XBOX_INPUT_STATE st;
    for (int src = 0; src < XBOX_INPUT_SOURCES; src++) {
        memset(&st, 0, sizeof st);
        if (source_get(src, &st) == ERROR_SUCCESS) quit_filter(8 + src, &st);
    }
}

static void quit_now(void)
{
    fprintf(stderr, "[EXIT] quit from the quit window (L3 + R3)\n");
    fflush(stderr);
    fflush(stdout);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
}

static void quit_filter(int idx, XBOX_INPUT_STATE *st)
{
    const WORD sticks = XBOX_GAMEPAD_LEFT_THUMB | XBOX_GAMEPAD_RIGHT_THUMB;
    XBOX_GAMEPAD *g = &st->Gamepad;
    if (s_qd_off < 0) {
        const char *e = getenv("RECOMP_QUIT_COMBO");
        s_qd_off = e && e[0] == '0';
    }
    if (idx < 0 || idx >= QD_IDX) return;
    if (s_qd_off && s_lm_n <= 0 && s_qd_open == 0 && !s_qd_block[idx]) return;   /* v278: the menu works without the quit combo */
    WORD b = g->wButtons;
    BYTE ab = (BYTE)((g->bAnalogButtons[XBOX_BUTTON_A] > 30 ? 1 : 0) |
                     (g->bAnalogButtons[XBOX_BUTTON_B] > 30 ? 2 : 0));
    WORD pb = s_qd_prev_btn[idx];
    BYTE pab = s_qd_prev_ab[idx];
    BYTE wb = (BYTE)((g->bAnalogButtons[XBOX_BUTTON_WHITE] > 30 ? 1 : 0) |
                     (g->bAnalogButtons[XBOX_BUTTON_BLACK] > 30 ? 2 : 0));
    BYTE pwb = s_qd_prev_wb[idx];
    int ly = g->sThumbLY, sy = ly > 16000 ? 1 : ly < -16000 ? -1 : (ly > -8000 && ly < 8000) ? 0 : s_qd_prev_y[idx];
    int psy = s_qd_prev_y[idx];
    s_qd_prev_wb[idx] = wb;
    s_qd_prev_y[idx] = (signed char)sy;
    int lx = g->sThumbLX, sx = lx > 16000 ? 1 : lx < -16000 ? -1 : (lx > -8000 && lx < 8000) ? 0 : s_qd_prev_x[idx];
    int psx = s_qd_prev_x[idx];
    s_qd_prev_btn[idx] = b;
    s_qd_prev_ab[idx] = ab;
    s_qd_prev_x[idx] = (signed char)sx;
    int held = b != 0 || ab != 0 || wb != 0 || g->bAnalogButtons[XBOX_BUTTON_X] > 30 || g->bAnalogButtons[XBOX_BUTTON_Y] > 30;
    if (s_qd_open == 1 || s_qd_open == 3) s_qd_held[idx] = (BYTE)held;   /* v276d: every device, while open */

    if (s_qd_open == 0 && s_qd_block[idx]) {
        if (held) {                         /* v276f: still the button that closed it */
            memset(g, 0, sizeof *g);
            st->dwPacketNumber ^= 0x80000000u;
            return;
        }
        s_qd_block[idx] = 0;                /* let go: this device is the game's again */
    }
    if (s_qd_open == 0) {
        if (!s_qd_off && (b & sticks) == sticks && (pb & sticks) != sticks) {
            InterlockedExchange(&s_qd_sel, 2);           /* v278b: Return first */
            InterlockedExchange(&s_qd_open, 1);
            fprintf(stderr, "[QUIT] quit window open (left / right, A = choose, B = back)\n");
        } else if (s_lm_n > 0 && wb == 3 && pwb != 3) {
            if (s_lm_sel >= s_lm_n) InterlockedExchange(&s_lm_sel, 0);
            s_lm_rep_at[idx] = 0;
            InterlockedExchange(&s_qd_open, 3);
            fprintf(stderr, "[MENU] %s open (%ld entries)\n", xbox_ListMenuTitle(), (long)s_lm_n);
        } else {
            return;                         /* the game's input, untouched */
        }
    } else if (s_qd_open == 3) {
        /* v278: the list menu */
        LONG n = s_lm_n, sel = s_lm_sel;
        int dir = 0;
        int up = (b & XBOX_GAMEPAD_DPAD_UP) || sy > 0, dn = (b & XBOX_GAMEPAD_DPAD_DOWN) || sy < 0;
        int pup = (pb & XBOX_GAMEPAD_DPAD_UP) || psy > 0, pdn = (pb & XBOX_GAMEPAD_DPAD_DOWN) || psy < 0;
        DWORD now = GetTickCount();
        if ((up && !pup) || (dn && !pdn)) {
            dir = up ? -1 : 1;
            s_lm_rep_at[idx] = now + 400;           /* held: repeats after 0.4 s */
        } else if ((up || dn) && s_lm_rep_at[idx] && (LONG)(now - s_lm_rep_at[idx]) >= 0) {
            dir = up ? -1 : 1;
            s_lm_rep_at[idx] = now + 70;
        } else if (!up && !dn) {
            s_lm_rep_at[idx] = 0;
        }
        if (((b & XBOX_GAMEPAD_DPAD_LEFT) && !(pb & XBOX_GAMEPAD_DPAD_LEFT)) || (sx < 0 && psx >= 0)) dir = -10;
        if (((b & XBOX_GAMEPAD_DPAD_RIGHT) && !(pb & XBOX_GAMEPAD_DPAD_RIGHT)) || (sx > 0 && psx <= 0)) dir = 10;
        /* v278b: the same press read through two devices (the pad seen as
         * XInput and SDL, or a port and its source) moved two entries:
         * one move of a direction per 50 ms */
        if (dir && dir == s_lm_last_dir && now - s_lm_last_t < 50u) dir = 0;
        if (dir) { s_lm_last_dir = dir; s_lm_last_t = now; }
        if (dir && n > 0) {
            sel += dir;
            if (dir == -1 && sel < 0) sel = n - 1;          /* one step wraps around */
            else if (dir == 1 && sel >= n) sel = 0;
            if (sel < 0) sel = 0;
            if (sel >= n) sel = n - 1;
            InterlockedExchange(&s_lm_sel, sel);
        }
        if (n <= 0 || ((ab & 2) && !(pab & 2)) || ((ab & 1) && !(pab & 1))) {
            int any = 0;
            if (n > 0 && (ab & 1) && !(pab & 1)) {
                InterlockedExchange(&s_lm_pick, sel);
                fprintf(stderr, "[MENU] picked %ld: %s\n", (long)sel, xbox_ListMenuItem((int)sel));
            } else {
                fprintf(stderr, "[MENU] closed\n");
            }
            s_qd_held[idx] = (BYTE)held;
            for (int k = 8; k < QD_IDX; k++) { s_qd_block[k] = s_qd_held[k]; any |= s_qd_held[k]; }
            for (int k = 0; k < 8; k++) s_qd_block[k] = (BYTE)(s_qd_held[k] | any);
            memset(s_qd_held, 0, sizeof s_qd_held);
            InterlockedExchange(&s_qd_open, 0);
        }
    } else if (s_qd_open == 1) {
        WORD lr = XBOX_GAMEPAD_DPAD_LEFT | XBOX_GAMEPAD_DPAD_RIGHT;
        if (((b & lr) && (b & lr) != (pb & lr)) || (sx && sx != psx)) {
            /* v278b: three buttons; one step a press (50 ms between two
             * devices' copies of it); Quit to Title skipped when not offered */
            int d = (b & XBOX_GAMEPAD_DPAD_LEFT) || sx < 0 ? -1 : (b & XBOX_GAMEPAD_DPAD_RIGHT) || sx > 0 ? 1 : 0;
            DWORD now = GetTickCount();
            if (d && !(d == s_lm_last_dir && now - s_lm_last_t < 50u)) {
                LONG to = s_qd_sel + d;
                s_lm_last_dir = d; s_lm_last_t = now;
                if (to == 1 && !s_qd_title_ok) to += d;
                if (to < 0) to = 0;
                if (to > 2) to = 2;
                if (to == 1 && !s_qd_title_ok) to = s_qd_sel;
                InterlockedExchange(&s_qd_sel, to);
            }
        }
        if ((ab & 1) && !(pab & 1) && s_qd_sel == 0) quit_now();
        if ((ab & 1) && !(pab & 1) && s_qd_sel == 1 && s_qd_title_ok) {
            InterlockedExchange(&s_qd_title_req, 1);
            fprintf(stderr, "[QUIT] quit to title\n");
        }
        if (((ab & 2) && !(pab & 2)) || ((ab & 1) && !(pab & 1))) {
            /* v276f: each device that still has a button down gives the
             * game nothing until that device lets go (v276d waited for
             * every device, up to 2 seconds: a device the game no longer
             * reads -- the same pad seen twice, as XInput and SDL -- kept
             * the game deaf that long, Miggas). The ports the game reads
             * directly (0-3, merged sources) wait too if anything was down. */
            int any = 0;
            s_qd_held[idx] = (BYTE)held;
            for (int k = 8; k < QD_IDX; k++) { s_qd_block[k] = s_qd_held[k]; any |= s_qd_held[k]; }
            for (int k = 0; k < 8; k++) s_qd_block[k] = (BYTE)(s_qd_held[k] | any);
            memset(s_qd_held, 0, sizeof s_qd_held);
            InterlockedExchange(&s_qd_open, 0);
            fprintf(stderr, "[QUIT] back to the game\n");
        }
    }
    /* a controller at rest for the game; a new packet number so it reads it */
    memset(g, 0, sizeof *g);
    st->dwPacketNumber ^= 0x80000000u;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD r = input_get(dwPort, pState, 1);
    if (r == ERROR_SUCCESS) quit_filter((int)dwPort, pState);
    return r;
}

/* v186b: one input device on its own -- PC pad 0..3 without the keyboard
 * merged in, or the keyboard alone -- for the per-player assignment in
 * usb/usb_gamepad.c. */
static DWORD source_get(int src, XBOX_INPUT_STATE *pState);
DWORD xbox_InputGetSource(int src, XBOX_INPUT_STATE *pState)
{
    DWORD r = source_get(src, pState);
    if (r == ERROR_SUCCESS) quit_filter(8 + src, pState);   /* v250 */
    return r;
}

static DWORD source_get(int src, XBOX_INPUT_STATE *pState)
{
    if (!pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (src == XBOX_INPUT_SOURCE_KEYBOARD) {
        if (!keyboard_enabled())
            return ERROR_DEVICE_NOT_CONNECTED;
        keyboard_state(pState);
        return ERROR_SUCCESS;
    }
    if (src >= XBOX_INPUT_SOURCE_SDL0 && src < XBOX_INPUT_SOURCES)
        return sdl_get(src - XBOX_INPUT_SOURCE_SDL0, pState);     /* v189 */
    if (src < 0 || src >= XBOX_MAX_CONTROLLERS)
        return ERROR_DEVICE_NOT_CONNECTED;
    return input_get((DWORD)src, pState, 0);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    rc = XInputSetState(dwPort, &xi_vib);
    if (sdl_rumble((int)dwPort, pVibration) == ERROR_SUCCESS)   /* v189 */
        rc = ERROR_SUCCESS;
    return rc;
}

DWORD xbox_InputSetSource(int src, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    if (!pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (src >= XBOX_INPUT_SOURCE_SDL0 && src < XBOX_INPUT_SOURCES)
        return sdl_rumble(src - XBOX_INPUT_SOURCE_SDL0, pVibration);
    if (src < 0 || src >= XBOX_MAX_CONTROLLERS)
        return ERROR_DEVICE_NOT_CONNECTED;              /* the keyboard */
    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState((DWORD)src, &xi_vib);
}

const char *xbox_InputSourceName(int src)
{
    static char buf[4][24];
    const char *n;
    if (src == XBOX_INPUT_SOURCE_KEYBOARD)
        return "the keyboard";
    if (src >= XBOX_INPUT_SOURCE_SDL0 && src < XBOX_INPUT_SOURCES) {
        n = sdl_name(src - XBOX_INPUT_SOURCE_SDL0);
        return n ? n : "a controller";
    }
    if (src < 0 || src >= XBOX_MAX_CONTROLLERS)
        return "nothing";
    snprintf(buf[src], sizeof buf[src], "PC controller %d", src + 1);
    return buf[src];
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* neither: no input backend */
#error "xinput_device.c: no input backend for this platform"
#endif /* _WIN32 */
