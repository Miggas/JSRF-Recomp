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

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

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
    return t == SDL_CONTROLLER_TYPE_XBOX360 || t == SDL_CONTROLLER_TYPE_XBOXONE;
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

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get(dwPort, pState, 1);
}

/* v186b: one input device on its own -- PC pad 0..3 without the keyboard
 * merged in, or the keyboard alone -- for the per-player assignment in
 * usb/usb_gamepad.c. */
DWORD xbox_InputGetSource(int src, XBOX_INPUT_STATE *pState)
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
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

DWORD xbox_InputGetSource(int src, XBOX_INPUT_STATE *pState)
{
    if (src < 0 || src >= XBOX_MAX_CONTROLLERS)
        return ERROR_DEVICE_NOT_CONNECTED;   /* no keyboard source here */
    return xbox_InputGetState((DWORD)src, pState);
}

DWORD xbox_InputSetSource(int src, const XBOX_VIBRATION *pVibration)
{
    if (src < 0 || src >= XBOX_MAX_CONTROLLERS)
        return ERROR_DEVICE_NOT_CONNECTED;
    return xbox_InputSetState((DWORD)src, pVibration);
}

const char *xbox_InputSourceName(int src)
{
    static char buf[4][24];
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
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
