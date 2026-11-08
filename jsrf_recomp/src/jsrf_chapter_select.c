/* Chapter select (v276, RECOMP_CHAPTER_SELECT=<chapter>[:<mission>]): picking
 * the tutorial (Roboy in the Garage, or the title menu) starts that story
 * mission instead. The launcher's Settings -> Debug offers the chapters'
 * openings and Garages.
 *
 * From andeecollard/jsrf-recomp (https://github.com/andeecollard/jsrf-recomp,
 * MIT, by andeecollard): the chapter-select half of
 * diagnostics/jsrf_first_fault/chapter_select.c, a clean-room implementation
 * from its docs/jsrf/cleanroom/CHAPTER_SELECT_SPEC.md. Adapted: our register
 * names, the mission-file check on Windows too, the flag words written so the
 * address translation leaves them alone; its RECOMP_CHAPTER_JUMP (a test
 * harness's jump from free play) is not taken.
 *
 * 0x7CAE0 is sequence state 0x20, "prepare tutorial", reached only through the
 * sequence's state table at 0x20D2B8. The replacement prepares a story stage
 * for chapter C, mission M instead and leaves by state 0x1C, the state every
 * story entry of the game itself goes through. tools/wrap_dsound.py renames
 * the generated body sub_0007CAE0_gen; without the setting it runs as it is.
 *
 * Missions (mssnCCMM.bin): CC96 is the chapter's opening (what the game loads
 * after a chapter clear), CC00 its Garage (chapter 5's is 0510), 0101 the
 * new game's opening. Jumping into a chapter skips its earlier scripts, and the
 * game's next save point saves the new chapter over your progress. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#else
#include <sys/stat.h>
#include "win32_compat.h"
#endif
#include "gen/recomp_types.h"

#if defined(_WIN32)
BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);
#else
BOOL xbox_translate_path(const char *xbox_path, char *host_path_buf, DWORD buf_size);
#endif

extern void sub_000127C0(void);   /* the action manager's VAR setter */
extern void sub_0003AE20(void);   /* save: clear a flag scope */
extern void sub_0003AEA0(void);   /* save: clear a counter scope */
extern void sub_00039BE0(void);   /* save: write one flag word */
extern void sub_00039F40(void);   /* save: clear the sprayed-tag set */
extern void sub_0004EAF0(void);   /* the per-stage reset */
extern void sub_0007CAE0_gen(void);

/* ---- Guest facts (the spec's sections 0-4) ---- */
#define AM_PTR          0x0022FCE0u   /* the action manager pointer */
#define SAVE            0x001EFFB0u   /* the save object */
#define SEQ_STATE       0x48u         /* SEQ+0x48: sequence state */

#define VAR_RESUME      0x000u        /* VAR[0]: state to resume */
#define VAR_CHAPTER     0x002u        /* VAR[2] */
#define VAR_MISSION     0x003u        /* VAR[3]: first mission of the stage */
#define VAR_CHARACTER   0x04Du        /* VAR[0x4D] */
#define VAR_FADE        0x19Au

#define SEQ_ST_STORY_PREPARE 0x1Cu    /* 0x7C9E0 */

#define RET_MARK 0x0007BDD0u          /* return address our own guest calls push */

/* A save flag word: (scope << 16) | flag bits, as the flag writer takes it.
 * Built from its parts so tools/translate_release.py does not take the
 * constant for an address. */
#define FLAGW(hi, lo) (((uint32_t)(hi) << 16) | (uint32_t)(lo))

static inline uint32_t am(void) { return MEM32(AM_PTR); }

/* 0x127C0: thiscall this=AM, (index, value), ret 8. Writes go through the
 * game's own setter. */
static void var_set(uint32_t index, uint32_t value)
{
    g_ecx = am();
    PUSH32(g_esp, value);
    PUSH32(g_esp, index);
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(0x000127C0u, sub_000127C0);
}

/* A thiscall on SAVE with one stack argument (ret 4). */
static void save_call1(uint32_t va, recomp_func_t fn, uint32_t arg)
{
    g_ecx = SAVE;
    PUSH32(g_esp, arg);
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(va, fn);
}

/* ---- The setting ---- */
static int s_inited, s_on;
static unsigned s_c, s_m;

/* "<C>[:<M>]", both 0..99 (the mission file name holds two digits of each). */
static void init_once(void)
{
    const char *v;
    char *end;
    long c, m = 96;
    if (s_inited) return;
    s_inited = 1;
    v = getenv("RECOMP_CHAPTER_SELECT");
    if (!v || !*v || strcmp(v, "0") == 0) return;
    c = strtol(v, &end, 10);
    if (end == v) goto bad;
    if (*end == ':') {
        const char *p = end + 1;
        m = strtol(p, &end, 10);
        if (end == p) goto bad;
    }
    if (*end || c < 0 || c > 99 || m < 0 || m > 99) goto bad;
    s_on = 1; s_c = (unsigned)c; s_m = (unsigned)m;
    fprintf(stderr, "[CHAPTER-SELECT] on: picking the tutorial starts mssn%02u%02u (chapter %u, mission %u)\n",
            s_c, s_m, s_c, s_m);
    return;
bad:
    fprintf(stderr, "[CHAPTER-SELECT] ignored: RECOMP_CHAPTER_SELECT=\"%s\" is not <chapter>[:<mission>]"
            " with both in 0..99\n", v);
}

/* Does D:\Media\Mission\mssnCCMM.bin exist? 1 yes, 0 no, -1 cannot tell. */
static int mission_file_state(unsigned c, unsigned m)
{
    char xp[64];
    snprintf(xp, sizeof xp, "D:\\Media\\Mission\\mssn%02u%02u.bin", c, m);
#if defined(_WIN32)
    {
        WCHAR hp[1024];
        if (!xbox_translate_path(xp, hp, 1024)) return -1;
        return GetFileAttributesW(hp) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    }
#else
    {
        char hp[1024];
        struct stat st;
        if (!xbox_translate_path(xp, hp, (DWORD)sizeof hp)) return -1;
        return stat(hp, &st) == 0 ? 1 : 0;
    }
#endif
}

static int chs_on(void)
{
    init_once();
    if (!s_on) return 0;
    if (mission_file_state(s_c, s_m) == 0) {
        fprintf(stderr, "[CHAPTER-SELECT] mssn%02u%02u.bin is not on the disc; the tutorial runs unchanged\n",
                s_c, s_m);
        return 0;
    }
    return 1;
}

/* 0x7CAE0, thiscall this=SEQ, no arguments, plain ret. Replaces the tutorial
 * preparation (save backup 0x3B680, sandbox 0x3B420, stage, state 0x21) with
 * a story-stage entry for chapter C, mission M that leaves by state 0x1C. */
static void chs_tutorial_prepare(void)
{
    const uint32_t seq = g_ecx;
    const unsigned c = s_c, m = s_m;
    unsigned k;

    /* 1. VAR[0x19A] = 1, as 0x7CAF7 and 0x7C9F7 do. */
    var_set(VAR_FADE, 1u);

    /* 2. The chapter change of 0x53B40: the chapter word at SAVE+4 (set here
     *    rather than incremented), then 0x3AE20(1) clears the chapter flag
     *    set and 0x3AEA0(1) the chapter counter. */
    MEM32(SAVE + 4u) = c;
    save_call1(0x0003AE20u, sub_0003AE20, 1u);
    save_call1(0x0003AEA0u, sub_0003AEA0, 1u);

    /* 3. Chapter 1: the state the new-game opening mssn0101 leaves behind,
     *    which the chapter-1 hub mssn0100 tests, written with the flag writer
     *    0x39BE0. 0101 writes them itself. */
    if (c == 1u && m != 101u) {
        static const uint32_t words[] = {
            FLAGW(8, 0x0001),   /* +1:0   */
            FLAGW(0, 0x0009),   /* -1:1   */
            FLAGW(8, 0x0011),   /* +1:2   */
            FLAGW(8, 0x000A),   /* +2:1   */
            FLAGW(0, 0x00A2),   /* -2:20  */
            FLAGW(8, 0x0E13),   /* +3:450 */
            FLAGW(8, 0x0E3B),   /* +3:455 */
        };
        for (k = 0; k < sizeof words / sizeof words[0]; ++k)
            save_call1(0x00039BE0u, sub_00039BE0, words[k]);
    }
    /*    Chapter 7: mssn0700's command 12 (opcode 0x8A) clears the
     *    sprayed-tag set SAVE+0x118 through 0x39F40. 0796 -> 0794 -> 0700 and
     *    0795 -> 0700 on the discs' exit tables, so only a start that does not
     *    pass through 0700 reproduces the clear here. */
    if (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) {
        g_ecx = SAVE;
        PUSH32(g_esp, RET_MARK);
        RECOMP_ABI_CALL(0x00039F40u, sub_00039F40);
    }

    /* 4. The stage's first mission: stage state 0 (0x4EFA0) creates
     *    0x4E930(VAR[2], VAR[3], 0). */
    var_set(VAR_CHAPTER, c);
    var_set(VAR_MISSION, m);

    /* 5. The per-stage reset every story entry runs before state 0x1C. */
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(0x0004EAF0u, sub_0004EAF0);

    /* 6. Keep the saved character, as the continue path does at
     *    0x7C5CB..0x7C5DE (0x4EAF0 has just set VAR[0x4D] to 0). */
    var_set(VAR_CHARACTER, MEM32(SAVE + 0xCu));

    /* 7-8. Resume state and next sequence state: 0x1C, "prepare and start a
     *    story stage" (0x7C9E0). */
    var_set(VAR_RESUME, SEQ_ST_STORY_PREPARE);
    MEM32(seq + SEQ_STATE) = SEQ_ST_STORY_PREPARE;

    fprintf(stderr, "[CHAPTER-SELECT] the tutorial pick starts mssn%02u%02u (chapter %u, mission %u)%s\n",
            c, m, c, m, c == 1u && m != 101u ? ", chapter-1 opening flags set" :
            (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) ? ", chapter-7 tag clear applied" : "");
    fflush(stderr);

    g_esp += 4;                      /* ret */
}

void sub_0007CAE0(void)
{
    if (chs_on()) chs_tutorial_prepare();
    else sub_0007CAE0_gen();
}
