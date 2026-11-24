/*
 * jsrf_photo.c -- v313: photo mode (Idea by Miggas; the game's holds and the
 * camera's draw as JSRF-PC's photo mode found them, spriteice/JSRF-PC: its
 * findings, our own code).
 *
 * R3, let go on its own (input/xinput_device.c), opens it while the player
 * has control: in play, none of the game manager's four holds on (a pause
 * menu, a fade, a scene) and the camera following the player's skater. Then:
 *
 *   - The world is held by the game itself: the game manager's fourth hold
 *     (+0x4C), the one it takes for its fades, asked for through the game's
 *     own call (sub_00012730, thiscall on the game manager, on or off). While
 *     a hold is on, the game's update walks the tasks with that hold's own
 *     do-nothing slots, so nothing moves, and the frame's draw (sub_000131F0)
 *     goes on, each object drawn by that hold's draw (v313c: as the game
 *     draws a held frame; v313 lowered the hold for the frame's draw, every
 *     object drawn as in play, and the skates' trail, built from the camera
 *     of the moment it was last updated, followed the flying camera -- Miggas:
 *     drawn the game's way only the trail goes; RECOMP_PHOTO_HELD_DRAW=0 for
 *     v313's way).
 *   - The camera flies: the camera's draw (sub_000A2960) builds the view
 *     from the eye (+0x44) and the point looked at (+0x5C), which are placed
 *     before it from photo mode's own eye and direction (the point 64 ahead,
 *     so the game's view never turns over), and the skater's fade distance
 *     (+0x230) is set far, so the skater is drawn whole wherever the camera
 *     is. The map's areas drawn are made from that eye by the same draw.
 *   - The HUD is hidden (jsrf_widescreen.c's HUD scale reads 0 meanwhile),
 *     and the boost's effect is left out (jsrf_boost.c).
 *
 * Closing (R3 again, or B) puts the camera's eye, point and distance back
 * as the game had them and lets the hold go. The controls are the pads'
 * (xbox_PhotoPad): the left stick moves along the view, the right stick
 * turns it (RECOMP_CAM_INVERT_X / _Y as for the right stick's camera), RT and
 * LT go straight up and down, RB four times as fast, LB a quarter, Y back to
 * where photo mode started. A takes a photo: the renderer saves the next
 * frame (kernel/nv2a_render.c), without the legend.
 *
 * RECOMP_PHOTO=0 turns photo mode off; [PHOTO] lines say what it does.
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#else
#include "win32_compat.h"
#endif
#include "gen/recomp_types.h"

/* a pad, as input/xinput_xbox.h has it (XBOX_GAMEPAD) */
typedef struct {
    WORD  wButtons;
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig */
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XBOX_GAMEPAD;
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

extern size_t xbox_GetMappedSize(void);
extern void xbox_PhotoAllow(int ok);            /* input/xinput_device.c */
extern int  xbox_PhotoOpen(void);
extern void xbox_PhotoClose(void);
extern void xbox_PhotoPad(XBOX_GAMEPAD *out);
extern int  xbox_PhotoResetTake(void);
extern void sub_000131F0_gen(void);             /* the frame's draw */
extern void sub_00012730(void);                 /* the game manager's fourth hold, on or off */

#define GM_HOLDS        0x40u   /* the game manager's four holds, a dword each */
#define GM_HOLD_OURS    0x4Cu   /* the fourth: the fades', which keeps the camera drawing */
#define CAM_EYE         0x44u
#define CAM_LOOK        0x5Cu
#define CAM_DISTANCE    0x230u  /* the skater's fade reads it */
#define LOOK_AHEAD      64.0
#define SPEED           2.0     /* units a frame at full tilt */

enum { PH_IDLE, PH_HOLDING };
static int ph_state, ph_off = -1, ph_inv_x, ph_inv_y;
static uint32_t ph_frame, ph_follow_frame, ph_follow_cam, ph_cam, ph_asked_frame;
static float ph_game_eye[3], ph_game_look[3], ph_game_dist;
static double ph_pos[3], ph_yaw, ph_pitch;          /* the flying eye */
static double ph_start_pos[3], ph_start_yaw, ph_start_pitch;

static int ph_ok(uint32_t va, uint32_t n)
{
    uint64_t end = (uint64_t)va + n;
    return va >= 0x10000u && end <= xbox_GetMappedSize();
}

static int ph_enabled(void)
{
    if (ph_off < 0) {
        const char *e = getenv("RECOMP_PHOTO");
        ph_off = e && e[0] == '0';
        e = getenv("RECOMP_CAM_INVERT_X");
        ph_inv_x = e && e[0] == '1';
        e = getenv("RECOMP_CAM_INVERT_Y");
        ph_inv_y = e && e[0] == '1';
        fprintf(stderr, "[PHOTO] photo mode: %s\n",
                ph_off ? "off (RECOMP_PHOTO=0)" : "R3 (click the right stick and let go) in play");
    }
    return !ph_off;
}

/* The camera's publication saw it following the player (recomp_manual.c,
 * sub_000A28B0, in the follow modes). */
void jsrf_photo_follow_seen(uint32_t cam)
{
    ph_follow_cam = cam;
    ph_follow_frame = ph_frame;
}

int jsrf_photo_holding(void)
{
    return ph_state == PH_HOLDING;
}

/* the game's own call, thiscall on the game manager, with the caller's
 * registers kept */
static void ph_hold(uint32_t gm, int on)
{
    uint32_t r[10] = { g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp,
                       (uint32_t)g_df };
    double fp[8];
    int top = g_fp_top, cmp = g_fp_cmp;
    uint16_t cc = g_fp_cc, cw = g_fp_control_word;
    memcpy(fp, g_fp_stack, sizeof fp);
    g_esp -= 4;
    MEM32(g_esp) = on ? 1u : 0u;
    g_esp -= 4;
    MEM32(g_esp) = 0;                   /* a return address for its ret */
    g_ecx = gm;
    sub_00012730();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_esp = r[3]; g_ebx = r[4];
    g_esi = r[5]; g_edi = r[6]; g_ebp = r[7]; g_seh_ebp = r[8]; g_df = (int)r[9];
    memcpy(g_fp_stack, fp, sizeof fp);
    g_fp_top = top; g_fp_cmp = cmp; g_fp_cc = cc; g_fp_control_word = cw;
    ph_asked_frame = ph_frame;
}

static void ph_dir(double yaw, double pitch, double f[3])
{
    f[0] = cos(pitch) * sin(yaw);
    f[1] = sin(pitch);
    f[2] = cos(pitch) * cos(yaw);
}

static double ph_axis(int v)
{
    double x = v / 32767.0, a = fabs(x);
    if (a < 0.2) return 0;
    if (a > 1) a = 1;
    a = (a - 0.2) / 0.8;
    return (x < 0 ? -a : a) * a;        /* a gentle curve: fine moves near the middle */
}

static void ph_start(uint32_t gm)
{
    const float *eye = (const float *)XBOX_PTR(ph_cam + CAM_EYE);
    const float *look = (const float *)XBOX_PTR(ph_cam + CAM_LOOK);
    double d[3], h;
    int k;
    memcpy(ph_game_eye, eye, sizeof ph_game_eye);
    memcpy(ph_game_look, look, sizeof ph_game_look);
    ph_game_dist = MEMF(ph_cam + CAM_DISTANCE);
    for (k = 0; k < 3; k++) {
        ph_pos[k] = eye[k];
        d[k] = (double)look[k] - eye[k];
    }
    h = sqrt(d[0] * d[0] + d[2] * d[2]);
    ph_yaw = atan2(d[0], d[2]);
    ph_pitch = atan2(d[1], h > 1e-6 ? h : 1e-6);
    memcpy(ph_start_pos, ph_pos, sizeof ph_pos);
    ph_start_yaw = ph_yaw;
    ph_start_pitch = ph_pitch;
    xbox_PhotoResetTake();
    ph_hold(gm, 1);
    ph_state = PH_HOLDING;
    fprintf(stderr, "[PHOTO] the world held; camera %08X from (%.1f %.1f %.1f)\n",
            ph_cam, ph_pos[0], ph_pos[1], ph_pos[2]);
}

static void ph_stop(uint32_t gm)
{
    if (ph_ok(ph_cam, CAM_DISTANCE + 4)) {
        memcpy((void *)XBOX_PTR(ph_cam + CAM_EYE), ph_game_eye, sizeof ph_game_eye);
        memcpy((void *)XBOX_PTR(ph_cam + CAM_LOOK), ph_game_look, sizeof ph_game_look);
        MEMF(ph_cam + CAM_DISTANCE) = ph_game_dist;
    }
    ph_hold(gm, 0);
    ph_state = PH_IDLE;
    fprintf(stderr, "[PHOTO] the world goes on; the camera is the game's again\n");
}

static void ph_fly(void)
{
    XBOX_GAMEPAD p;
    double lx, ly, rx, ry, up, speed = SPEED, f[3], r[3];
    int k;
    xbox_PhotoPad(&p);
    if (xbox_PhotoResetTake()) {
        memcpy(ph_pos, ph_start_pos, sizeof ph_pos);
        ph_yaw = ph_start_yaw;
        ph_pitch = ph_start_pitch;
        return;
    }
    lx = ph_axis(p.sThumbLX);
    ly = ph_axis(p.sThumbLY);
    rx = ph_axis(p.sThumbRX) * (ph_inv_x ? -1 : 1);
    ry = ph_axis(p.sThumbRY) * (ph_inv_y ? -1 : 1);
    up = (p.bAnalogButtons[XBOX_BUTTON_RTRIGGER] - p.bAnalogButtons[XBOX_BUTTON_LTRIGGER]) / 255.0;
    if (p.bAnalogButtons[XBOX_BUTTON_BLACK] > 30) speed *= 4.0;   /* RB */
    if (p.bAnalogButtons[XBOX_BUTTON_WHITE] > 30) speed *= 0.25;  /* LB */
    ph_yaw += rx * 0.045;
    ph_pitch += ry * 0.035;
    if (ph_pitch > 1.48) ph_pitch = 1.48;
    if (ph_pitch < -1.48) ph_pitch = -1.48;
    ph_dir(ph_yaw, ph_pitch, f);
    r[0] = cos(ph_yaw); r[1] = 0; r[2] = -sin(ph_yaw);
    for (k = 0; k < 3; k++)
        ph_pos[k] += (f[k] * ly + r[k] * lx) * speed;
    ph_pos[1] += up * speed;
}

/* The camera's draw (jsrf_boost.c's sub_000A2960): photo mode's eye and
 * point placed first, while it holds the world and this is its camera. */
void jsrf_photo_place(uint32_t cam)
{
    double f[3];
    float eye[3], look[3];
    int k;
    if (ph_state != PH_HOLDING || cam != ph_cam || !ph_ok(cam, CAM_DISTANCE + 4))
        return;
    ph_dir(ph_yaw, ph_pitch, f);
    for (k = 0; k < 3; k++) {
        eye[k] = (float)ph_pos[k];
        look[k] = (float)(ph_pos[k] + f[k] * LOOK_AHEAD);
    }
    memcpy((void *)XBOX_PTR(cam + CAM_EYE), eye, sizeof eye);
    memcpy((void *)XBOX_PTR(cam + CAM_LOOK), look, sizeof look);
    MEMF(cam + CAM_DISTANCE) = 100.0f;  /* the skater drawn whole */
}

/* The frame's draw (thiscall on the game manager): photo mode's step, then
 * the draw, with our hold down for its length while photo mode has it. */
void jsrf_nt3000_voice(void);          /* recomp_manual.c (v313d) */
void sub_000131F0(void)
{
    uint32_t gm = g_ecx, holds = 0, i;
    int ours = 0;
    if (!ph_frame) jsrf_nt3000_voice();   /* v313d: once, at the first frame */
    ph_frame++;
    if (ph_enabled() && ph_ok(gm, GM_HOLDS + 16)) {
        for (i = 0; i < 4; i++) holds |= MEM32(gm + GM_HOLDS + 4 * i);
        if (ph_state == PH_IDLE) {
            int allowed = !holds && ph_frame - ph_follow_frame <= 3u && ph_ok(ph_follow_cam, CAM_DISTANCE + 4);
            xbox_PhotoAllow(allowed);
            if (xbox_PhotoOpen()) {
                if (allowed) {
                    ph_cam = ph_follow_cam;
                    ph_start(gm);
                } else {
                    xbox_PhotoClose();
                }
            }
        } else {
            xbox_PhotoAllow(1);
            if (!xbox_PhotoOpen()) {
                ph_stop(gm);
            } else {
                /* the game let our hold go (a fade of its own ending): ask again */
                if (!MEM32(gm + GM_HOLD_OURS) && ph_frame - ph_asked_frame > 2u)
                    ph_hold(gm, 1);
                ph_fly();
            }
        }
        ours = ph_state == PH_HOLDING && MEM32(gm + GM_HOLD_OURS);
    }
    {   /* v313c: the frame drawn as the game's hold draws it, unless
         * RECOMP_PHOTO_HELD_DRAW=0 */
        static int lower = -1;
        if (lower < 0) {
            const char *e = getenv("RECOMP_PHOTO_HELD_DRAW");
            lower = e && e[0] == '0';
            if (lower)
                fprintf(stderr, "[PHOTO] RECOMP_PHOTO_HELD_DRAW=0: the frame drawn as in play (the trail shows)\n");
        }
        if (!lower) ours = 0;
    }
    if (ours) {
        uint32_t was = MEM32(gm + GM_HOLD_OURS);
        MEM32(gm + GM_HOLD_OURS) = 0;
        sub_000131F0_gen();
        MEM32(gm + GM_HOLD_OURS) = was;
    } else {
        sub_000131F0_gen();
    }
}
