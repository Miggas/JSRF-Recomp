/* Chapter select (v276, RECOMP_CHAPTER_SELECT=<chapter>[:<mission>]): a new
 * game starts that story mission instead of the opening, mssn0101. The
 * launcher's Settings -> Debug offers the chapters' openings and Garages.
 *
 * From andeecollard/jsrf-recomp (https://github.com/andeecollard/jsrf-recomp,
 * MIT, by andeecollard): the chapter-select half of
 * diagnostics/jsrf_first_fault/chapter_select.c, a clean-room implementation
 * from its docs/jsrf/cleanroom/CHAPTER_SELECT_SPEC.md. Adapted: our register
 * names, the mission-file check on Windows too, the flag words written so the
 * address translation leaves them alone, and hooked on the new game rather
 * than the tutorial; its RECOMP_CHAPTER_JUMP (a test harness's jump from free
 * play) is not taken.
 *
 * A new game (sequence state 0x13 with VAR[0x1CC] = 0, at 0x7C5EC) has set
 * VAR[2] = 1 and VAR[3] = 1 (0x7C3B2, after the save reset 0x3AED0 and the
 * chapter step 0x39C70) and run the stage reset 0x4EAF0, then goes to state
 * 0x1C, 0x7C9E0, which builds the stage object; the stage creates mission
 * VAR[2]:VAR[3]. 0x7C9E0 is reached only through the sequence's state table
 * at 0x20D2B8. The wrapper, when the setting is on and VAR[2]:VAR[3] is 1:1
 * (only the new game's opening; continuing sets mission 97), prepares
 * chapter C, mission M first, as the spec's tutorial redirect does, then runs
 * the original. tools/wrap_dsound.py renames the generated body
 * sub_0007C9E0_gen; without the setting it runs as it is.
 *
 * Missions (mssnCCMM.bin): CC96 is the chapter's opening (what the game loads
 * after a chapter clear), CC00 its Garage (chapter 5's is 0510), 0101 the
 * new game's opening. Jumping into a chapter skips its earlier scripts, and the
 * game's next save point saves the new chapter over your progress. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#else
#include <sys/stat.h>
#include "win32_compat.h"
#endif
#include "gen/recomp_types.h"
#if !defined(_WIN32)
#include <dirent.h>
#endif

/* v278: mission select (input/xinput_device.c, kernel/nv2a_gl_quit.h) */
void xbox_ListMenuSet(const char *title, const char *const *items, int n);
int xbox_ListMenuTake(void);
int xbox_ListMenuOpen(void);
void xbox_ListMenuShow(int sel);
void xbox_ListMenuSelect(int sel);
void xbox_QuitTitleAvailable(int ok);   /* v278b: the quit window's Quit to Title */
int xbox_QuitToTitleTake(void);
extern char g_xbox_note[96];
extern char g_xbox_timer[24];             /* v300: the mission timer, top-left */

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
extern void sub_0007C9E0_gen(void);

/* ---- Guest facts (the spec's sections 0-4) ---- */
#define AM_PTR          0x0022FCE0u   /* the action manager pointer */
#define AM_VARS         0x7868u       /* VAR[i] = AM + 0x7868 + 4*i */
#define SAVE            0x001EFFB0u   /* the save object */

#define VAR_CHAPTER     0x002u        /* VAR[2] */
#define VAR_MISSION     0x003u        /* VAR[3]: first mission of the stage */

#define RET_MARK 0x0007BDD0u          /* return address our own guest calls push */

/* A save flag word: (scope << 16) | flag bits, as the flag writer takes it.
 * Built from its parts so tools/translate_release.py does not take the
 * constant for an address. */
#define FLAGW(hi, lo) (((uint32_t)(hi) << 16) | (uint32_t)(lo))

static inline uint32_t am(void) { return MEM32(AM_PTR); }
static inline uint32_t var_get(uint32_t i) { uint32_t a = am(); return a ? MEM32(a + AM_VARS + 4u * i) : 0u; }

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
    fprintf(stderr, "[CHAPTER-SELECT] on: a new game starts mssn%02u%02u (chapter %u, mission %u)\n",
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

/* v278: a mission picked in mission select (the title's LB + RB menu), for
 * the next new game; wins over RECOMP_CHAPTER_SELECT, used once */
static volatile int s_armed;
static unsigned s_ac, s_am;
static unsigned s_pick;                /* v279: the picked mission, CC * 100 + MM */
static int s_pick_obj, s_pick_live;    /* it has an objective; its end goes to the title */
static unsigned s_sel_target;          /* v281: the pick, after the character select (0 none) */
static int s_pick_seen;                /* v285b: the picked mission has run */
static int s_pick_cleared;             /* v289: it has been cleared */
static uint32_t s_tm_obj;              /* v300: the timer's mission object */

static int chs_on(void)
{
    init_once();
    if (s_armed) {
        s_armed = 0;
        g_xbox_note[0] = 0;
        s_c = s_ac; s_m = s_am;
        s_pick = s_ac * 100u + s_am;    /* v279: back to the title when it ends */
        s_pick_live = s_pick_obj;
        s_pick_seen = 0;
        s_pick_cleared = 0;
        s_tm_obj = 0;                   /* v300: the timer from 0 */
        g_xbox_timer[0] = 0;
        /* v281: the chapter's character select (mssnCC95, Roboy's menu in
         * the Garage) first, for a pick in chapters 1-9 that isn't an
         * opening or Garage */
        s_sel_target = 0;
        if (s_ac >= 1u && s_ac <= 9u && s_am != 0u && !(s_am >= 94u && s_am <= 97u)
            && mission_file_state(s_ac, 95u) == 1) {
            s_sel_target = s_pick;
            s_m = 95u;
        }
        return 1;                       /* listed from the disc: there */
    }
    if (!s_on) return 0;
    if (mission_file_state(s_c, s_m) == 0) {
        fprintf(stderr, "[CHAPTER-SELECT] mssn%02u%02u.bin is not on the disc; the new game starts as usual\n",
                s_c, s_m);
        return 0;
    }
    return 1;
}


static void pick_setup(unsigned c, unsigned m);

/* Before 0x7C9E0 builds the stage: chapter C, mission M instead of the new
 * game's 1:1 (the spec's tutorial redirect, steps 2-4; the fade, the stage
 * reset 0x4EAF0 and the character are already the new game's). */
static void chs_new_game(void)
{
    const unsigned c = s_c, m = s_m;
    unsigned k;

    /* The chapter change of 0x53B40: the chapter word at SAVE+4 (set here
     * rather than incremented), then 0x3AE20(1) clears the chapter flag set
     * and 0x3AEA0(1) the chapter counter. */
    MEM32(SAVE + 4u) = c;
    save_call1(0x0003AE20u, sub_0003AE20, 1u);
    save_call1(0x0003AEA0u, sub_0003AEA0, 1u);

    /* Chapter 1: the state the new-game opening mssn0101 leaves behind, which
     * the chapter-1 hub mssn0100 tests, written with the flag writer 0x39BE0.
     * 0101 writes them itself. */
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
    /* Chapter 7: mssn0700's command 12 (opcode 0x8A) clears the sprayed-tag
     * set SAVE+0x118 through 0x39F40. 0796 -> 0794 -> 0700 and 0795 -> 0700
     * on the discs' exit tables, so only a start that does not pass through
     * 0700 reproduces the clear here. */
    if (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) {
        g_ecx = SAVE;
        PUSH32(g_esp, RET_MARK);
        RECOMP_ABI_CALL(0x00039F40u, sub_00039F40);
    }

    /* The stage's first mission: stage state 0 (0x4EFA0) creates
     * 0x4E930(VAR[2], VAR[3], 0). */
    var_set(VAR_CHAPTER, c);
    var_set(VAR_MISSION, m);
    pick_setup(c, m);                   /* v280: where the player starts, what the parent set */

    /* v281: a mission select pick has every character unlocked, as the
     * game's UnlockCharacter (0x39CB0) does it: bit <id> of the unlocked
     * set at SAVE+0x14 and global 0x151 + id = -1, for the 24 ids */
    if (s_pick) {
        MEM32(SAVE + 0x14u) |= 0x00FFFFFFu;
        for (k = 0; k < 24u; ++k) var_set(0x151u + k, 0xFFFFFFFFu);
        fprintf(stderr, "[MISSION-SELECT] all characters unlocked%s\n",
                s_sel_target ? "; the character select first" : "");
    }

    fprintf(stderr, "[CHAPTER-SELECT] the new game starts mssn%02u%02u (chapter %u, mission %u)%s\n",
            c, m, c, m, c == 1u && m != 101u ? ", chapter-1 opening flags set" :
            (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) ? ", chapter-7 tag clear applied" : "");
    fflush(stderr);
}

/* 0x7C9E0, sequence state 0x1C, thiscall this=SEQ, no arguments, plain ret. */
void sub_0007C9E0(void)
{
    if (var_get(VAR_CHAPTER) == 1u && var_get(VAR_MISSION) == 1u && chs_on()) {
        const uint32_t seq = g_ecx;
        chs_new_game();
        g_ecx = seq;
    }
    sub_0007C9E0_gen();
}

/* ---- v278: mission select (Miggas's idea, for a player who asked to
 * refight bosses and redo races without save files) ----
 * At the title (sequence state 0xC: the title stage running), LB + RB opens
 * a list of every mission file on the disc (D:\Media\Mission\mssnCCMM.bin).
 * The pick is armed for the next new game: "choose NEW GAME" shows at the
 * bottom, and the new game's stage starts that mission (the same path as
 * RECOMP_CHAPTER_SELECT). Choosing it again, or B, changes or keeps it. */
#define MSN_MAX 512
static int s_mm_state;                 /* 0 not listed, 1 listed, -1 nothing found */
static int s_mm_n;
static uint16_t s_mm_id[MSN_MAX];       /* CC * 100 + MM */
static char s_mm_text[MSN_MAX][96];
static const char *s_mm_ptr[MSN_MAX];
static char s_mm_obj[MSN_MAX][80];      /* v279: the objective line, "" when none */

/* v279: a mission's objective, the English text of the first message
 * command (opcode 0xEF, args 0-6 the file offsets of the text in Japanese,
 * English, French, German, Spanish, Italian, -) with text in its immediate
 * list (header +0x174: offset, count; commands 0x54 bytes, the opcode at
 * +0x10, the arguments after it). The game shows it as the mission starts.
 * Hubs, free roam, cutscenes and openings have none. Read from the disc's
 * file; the $ markup (line and page breaks, colours) is dropped. */
static FILE *mm_open(unsigned id)
{
    char xp[64];
    FILE *f = NULL;
    snprintf(xp, sizeof xp, "D:\\Media\\Mission\\mssn%02u%02u.bin", id / 100u, id % 100u);
#if defined(_WIN32)
    {
        WCHAR hp[1024];
        if (xbox_translate_path(xp, hp, 1024)) f = _wfopen(hp, L"rb");
    }
#else
    {
        char hp[1024];
        if (xbox_translate_path(xp, hp, (DWORD)sizeof hp)) f = fopen(hp, "rb");
    }
#endif
    return f;
}

static void mm_objective(unsigned id, char *out, size_t cap)
{
    FILE *f;
    uint8_t h[8], c[0x54];
    uint32_t off, cnt, i;
    out[0] = 0;
    f = mm_open(id);
    if (!f) return;
    if (fseek(f, 0x174, SEEK_SET) || fread(h, 1, 8, f) != 8) goto done;
    off = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    cnt = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
    if (cnt > 1024u) cnt = 1024u;
    for (i = 0; i < cnt && !out[0]; i++) {
        uint32_t op, en;
        char t[160];
        size_t k, n, o = 0;
        if (fseek(f, (long)(off + 0x54u * i), SEEK_SET) || fread(c, 1, sizeof c, f) != sizeof c) break;
        op = (uint32_t)c[0x10] | (uint32_t)c[0x11] << 8 | (uint32_t)c[0x12] << 16 | (uint32_t)c[0x13] << 24;
        en = (uint32_t)c[0x18] | (uint32_t)c[0x19] << 8 | (uint32_t)c[0x1A] << 16 | (uint32_t)c[0x1B] << 24;
        if (op != 0xEFu || !en || fseek(f, (long)en, SEEK_SET)) continue;
        n = fread(t, 1, sizeof t - 1, f);
        t[n] = 0;
        for (k = 0; k < n && t[k] && o + 1 < cap; k++) {
            unsigned char ch = (unsigned char)t[k];
            if (ch == '$' && k + 1 < n) {          /* $n $r $a $e $p ...; $cN: a colour */
                if (t[k + 1] == 'c' && k + 2 < n) k++;
                k++;
                if (t[k] == 'n' && o && out[o - 1] != ' ') out[o++] = ' ';
                continue;
            }
            if (ch == 0x81 && k + 1 < n && (t[k + 1] == 'g' || t[k + 1] == 'h')) { out[o++] = '"'; k++; continue; }
            if (ch < 0x20 || ch >= 0x7F) continue;
            if (ch == ' ' && (!o || out[o - 1] == ' ')) continue;
            out[o++] = (char)ch;
        }
        while (o && out[o - 1] == ' ') o--;
        out[o] = 0;
    }
done:
    fclose(f);
}

static int mm_cmp(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static int mm_name(const char *f)      /* "mssnCCMM.bin" -> CCMM, else -1 */
{
    int k, v = 0;
    if ((f[0] | 32) != 'm' || (f[1] | 32) != 's' || (f[2] | 32) != 's' || (f[3] | 32) != 'n') return -1;
    for (k = 4; k < 8; k++) {
        if (f[k] < '0' || f[k] > '9') return -1;
        v = v * 10 + (f[k] - '0');
    }
    if (f[8] != '.' || (f[9] | 32) != 'b' || (f[10] | 32) != 'i' || (f[11] | 32) != 'n' || f[12]) return -1;
    return v;
}

static void mm_list(void)
{
    uint16_t ids[MSN_MAX];
    int n = 0, i;
#if defined(_WIN32)
    WCHAR hp[1024];
    if (xbox_translate_path("D:\\Media\\Mission\\mssn*.bin", hp, 1024)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(hp, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char a[64];
                int v, k;
                for (k = 0; k < 63 && fd.cFileName[k]; k++) a[k] = fd.cFileName[k] < 128 ? (char)fd.cFileName[k] : '?';
                a[k] = 0;
                v = mm_name(a);
                if (v >= 0 && n < MSN_MAX) ids[n++] = (uint16_t)v;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
#else
    char hp[1024];
    if (xbox_translate_path("D:\\Media\\Mission", hp, (DWORD)sizeof hp)) {
        DIR *d = opendir(hp);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                int v = mm_name(de->d_name);
                if (v >= 0 && n < MSN_MAX) ids[n++] = (uint16_t)v;
            }
            closedir(d);
        }
    }
#endif
    qsort(ids, (size_t)n, sizeof ids[0], mm_cmp);
    for (i = 0; i < n; i++) {
        unsigned c = ids[i] / 100u, m = ids[i] % 100u;
        s_mm_id[i] = ids[i];
        mm_objective(ids[i], s_mm_obj[i], sizeof s_mm_obj[i]);
        if (s_mm_obj[i][0])
            snprintf(s_mm_text[i], sizeof s_mm_text[i], "%02u%02u  %s", c, m, s_mm_obj[i]);
        else if (m == 96u)
            snprintf(s_mm_text[i], sizeof s_mm_text[i], "%02u%02u  Chapter %u: opening", c, m, c);
        else if (m == 0u || (c == 5u && m == 10u))
            snprintf(s_mm_text[i], sizeof s_mm_text[i], "%02u%02u  Chapter %u: Garage", c, m, c);
        else
            snprintf(s_mm_text[i], sizeof s_mm_text[i], "%02u%02u  Chapter %u", c, m, c);
        s_mm_ptr[i] = s_mm_text[i];
    }
    s_mm_n = n;
    s_mm_state = n ? 1 : -1;
    {
        int k = 0;
        for (i = 0; i < n; i++) k += s_mm_obj[i][0] != 0;
        fprintf(stderr, "[MISSION-SELECT] %d missions on the disc, %d with an objective%s\n", n, k,
                n ? "; LB + RB at the title opens the list" : "");
    }
}

/* every sequence step (recomp_manual.c, sub_0007BDD0), with its state */
/* v278b: Quit to Title (the quit window), as the game's own game-over
 * choice does it (0x4AF0C, in sub_0004AE90): the next sequence state after
 * the stage, VAR[0x199], is 0xA (build the title), and the stage's game
 * object goes to its end state 0x63; the stage closes, sequence state 0x1F
 * reads VAR[0x199]. Offered while a stage runs (sequence state 0x1E).
 * v278c: the game object is the one whose update, sub_00051FC0 (its state
 * +0x5C through the 119-handler table at 0x1F9888, 0x4AE90 among them), ran
 * in the last half second: v278b looked in the action manager's slot 0xB,
 * which isn't it, so Quit to Title was always greyed (Miggas). */
#define VAR_NEXT   0x199u
#define SEQ_TITLE  0x0Au
#define GAME_END   0x63u

extern void sub_00051FC0_gen(void);
static volatile uint32_t s_game_this;
static volatile uint32_t s_game_seen;    /* v278d: the sequence step it last updated at */
static uint32_t s_seq_ticks;             /* sequence steps (game frames) */

/* ---- v280: start a picked mission the way its parent would (Miggas: 0451
 * started outside the arena's barrier, 0461's helicopters never came) ----
 * In the story a challenge is entered from its area (the parent: a mission of
 * the same chapter with an exit to it, header +0x194, 0x44-byte commands:
 * opcode 0xFB/0xFC/0xFD, target CC*100+MM, entry). The entry is global 0x4D,
 * which opcode 0x48 uses to place the player (the mission's spawn table
 * +0x3C, record 0x4D + slot); -1 keeps the player where they are, inside the
 * challenge, which a new game can't. And the parent writes the chapter flags
 * the challenge's start tests (0460 sets chapter[150..153], the four
 * helicopters 0461 spawns). So, for a pick with a parent:
 *  - an entry from the parent: global 0x4D = it;
 *  - entry -1 (a challenge started in place): global 0x4D = the spawn its
 *    clear puts the player at (opcode 0xE5, args[1]; 0451: 3, in the arena),
 *    if it has one, and the chapter flags its immediate list tests for 1
 *    that a parent writes as 1, written with 0x39BE0.
 * Flag words: bits 0-2 the set (1 = chapter), 3-18 the index, 19 the value. */
static int rd(FILE *f, uint32_t off, void *buf, size_t n)
{
    return !fseek(f, (long)off, SEEK_SET) && fread(buf, 1, n, f) == n;
}
static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* calls fn(word, ctx) for each condition (writes = 0) or write (writes = 1)
 * word of list li's commands; with op_only, only opcode op_only's */
typedef void (*word_fn)(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx);
static void mm_walk(FILE *f, int li, int writes, word_fn fn, void *ctx)
{
    uint8_t h[8], c[0x54], wb[4 * 32];
    uint32_t off, cnt, i, k;
    if (!rd(f, 0x174u + 8u * (uint32_t)li, h, 8)) return;
    off = le32(h); cnt = le32(h + 4);
    if (cnt > 1024u) cnt = 1024u;
    for (i = 0; i < cnt; i++) {
        uint32_t wo, wc, op;
        if (!rd(f, off + 0x54u * i, c, sizeof c)) return;
        op = le32(c + 0x10);
        wo = le32(c + (writes ? 8 : 0)); wc = le32(c + (writes ? 12 : 4));
        if (!wc) { fn(0xFFFFFFFFu, op, c, ctx); continue; }
        if (wc > 32u) wc = 32u;
        if (!rd(f, wo, wb, 4u * wc)) continue;
        for (k = 0; k < wc; k++) fn(le32(wb + 4u * k), op, c, ctx);
        fn(0xFFFFFFFEu, op, c, ctx);              /* the command's end */
    }
}

struct pk { uint32_t need[64]; int nn; uint32_t set[64]; int ns; int spawn; uint32_t own[128]; int no;
            uint32_t cur[64]; int nc; uint32_t clr[16]; int ncl; int has_clear; };
static void pk_need(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    int i;
    (void)op; (void)cmd;
    if (w >= 0xFFFFFFFEu || (w & 7u) != 1u || !(w >> 19 & 1u)) return;
    for (i = 0; i < p->no; i++) if (p->own[i] == (w & ~(1u << 19))) return;   /* its own state */
    for (i = 0; i < p->nn; i++) if (p->need[i] == w) return;
    if (p->nn < 64) p->need[p->nn++] = w;
}
static void pk_own(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    (void)op; (void)cmd;
    if (w < 0xFFFFFFFEu && (w & 7u) == 1u && p->no < 128) p->own[p->no++] = w & ~(1u << 19);
}
/* v282: the parent command that writes the most of the needed flags, as
 * one entry into the challenge does (0460 sets all four helicopters in one;
 * 0450 sets one police squad per trigger, so one squad) */
static void pk_wrote(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    int i;
    (void)op; (void)cmd;
    if (w == 0xFFFFFFFEu) {
        if (p->nc > p->ns) { memcpy(p->set, p->cur, sizeof p->cur[0] * (size_t)p->nc); p->ns = p->nc; }
        p->nc = 0;
        return;
    }
    for (i = 0; i < p->nn; i++)
        if (p->need[i] == w && p->nc < 64) { p->cur[p->nc++] = w; break; }
}
/* v285: the chapter flags a mission's clear (opcode 0xE5) waits for, set
 * whoever writes them in the story (0511 / 0512: chapter[1] / [2], the
 * order of the chapter 5 races, from the race before; Miggas: they never
 * finished) */
static void pk_clearflags(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    int i;
    (void)cmd;
    if (op != 0xE5u) return;
    p->has_clear = 1;
    if (w >= 0xFFFFFFFEu || (w & 7u) != 1u || !(w >> 19 & 1u)) return;
    for (i = 0; i < p->ncl; i++) if (p->clr[i] == w) return;
    if (p->ncl < 16) p->clr[p->ncl++] = w;
}

/* v294: and the chapter flags of what leads to the clear (Miggas: 0812
 * never cleared after both Zero Beats: its clear waits for mission[150],
 * which a listener writes only with chapter[60] on, set in the story by
 * what comes before). From the clear's conditions back through the
 * commands that write them (three steps): their chapter conditions for 1. */
struct mcmd { uint32_t op; uint8_t nc, nw; uint32_t c[16], w[16]; };
static struct mcmd s_mcmds[1536];
static int mm_load_cmds(FILE *f)
{
    int n = 0, li;
    for (li = 0; li < 4; li++) {
        uint8_t h[8], c[0x54], wb[64];
        uint32_t off, cnt, i, k;
        if (!rd(f, 0x174u + 8u * (uint32_t)li, h, 8)) continue;
        off = le32(h); cnt = le32(h + 4);
        if (cnt > 512u) cnt = 512u;
        for (i = 0; i < cnt && n < 1536; i++) {
            struct mcmd *m = &s_mcmds[n];
            uint32_t co, cc, wo, wc;
            if (!rd(f, off + 0x54u * i, c, sizeof c)) break;
            co = le32(c); cc = le32(c + 4); wo = le32(c + 8); wc = le32(c + 12);
            m->op = le32(c + 0x10);
            m->nc = (uint8_t)(cc > 16u ? 16u : cc);
            m->nw = (uint8_t)(wc > 16u ? 16u : wc);
            if (m->nc && rd(f, co, wb, 4u * m->nc)) for (k = 0; k < m->nc; k++) m->c[k] = le32(wb + 4u * k);
            else m->nc = 0;
            if (m->nw && rd(f, wo, wb, 4u * m->nw)) for (k = 0; k < m->nw; k++) m->w[k] = le32(wb + 4u * k);
            else m->nw = 0;
            n++;
        }
    }
    return n;
}
static int wset_has(const uint32_t *s, int n, uint32_t w)
{
    int i;
    for (i = 0; i < n; i++) if (s[i] == w) return 1;
    return 0;
}
static void pk_clear_chain(FILE *f, struct pk *p)
{
    uint32_t need[64];
    int nn = 0, n = mm_load_cmds(f), i, k, step;
    for (i = 0; i < n; i++)                                       /* the clear's own conditions */
        if (s_mcmds[i].op == 0xE5u)
            for (k = 0; k < s_mcmds[i].nc; k++) {
                const uint32_t w = s_mcmds[i].c[k];
                if (!(w >> 19 & 1u)) continue;
                if ((w & 7u) == 0u && nn < 64 && !wset_has(need, nn, w)) need[nn++] = w;
            }
    for (step = 0; step < 3; step++) {
        const int had = nn;
        for (i = 0; i < n; i++) {
            const struct mcmd *m = &s_mcmds[i];
            int writes = 0;
            for (k = 0; k < m->nw && !writes; k++) writes = wset_has(need, had, m->w[k]);
            if (!writes) continue;
            for (k = 0; k < m->nc; k++) {
                const uint32_t w = m->c[k];
                if (!(w >> 19 & 1u)) continue;
                if ((w & 7u) == 1u) {
                    if (p->ncl < 16 && !wset_has(p->clr, p->ncl, w)) p->clr[p->ncl++] = w;
                } else if ((w & 7u) == 0u && nn < 64 && !wset_has(need, nn, w)) {
                    need[nn++] = w;
                }
            }
        }
        if (nn == had) break;
    }
}

static void pk_clear(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    (void)w;
    if (w != 0xFFFFFFFEu && op == 0xE5u && p->spawn < 0) p->spawn = (int)le32(cmd + 0x14 + 4);
}

/* v284: start spots picked by hand (Miggas: skate there, press F9 with
 * RECOMP_SHOW_MISSION=1, the log has the position), for challenges whose
 * file has no spot inside them; RECOMP_MISSION_SPAWN=CCMM:x,y,z tries one */
struct place { unsigned id; float x, y, z, yaw; };   /* yaw in degrees, < 0: the record's */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static const struct place s_places[] = {
    /* v296: Miggas' spots, from F9 */
    { 451u, 1488.3f, 60.0f, 1358.6f, 360.0f },   /* Hikage Street: inside the drones' arena */
    { 751u, -43.5f, 1358.0f, 298.8f, 90.0f },    /* Fortified Zone: the Golden Rhinos' jets */
};
static volatile int s_conv_on;          /* v290: a pick whose ending is changed (s_convs) */
static unsigned s_conv_id;
static volatile int s_place_on;
static unsigned s_place_id;
static float s_place_p[3], s_place_yaw;

static void place_arm(unsigned id)
{
    const char *e = getenv("RECOMP_MISSION_SPAWN");
    size_t i;
    s_place_on = 0;
    /* v284d: several missions, separated by ';' */
    while (e && *e) {
        unsigned eid; float x, y, z, yaw = -1.0f;
        if (sscanf(e, " %u:%f,%f,%f,%f", &eid, &x, &y, &z, &yaw) >= 4 && eid == id) {
            s_place_id = id; s_place_p[0] = x; s_place_p[1] = y; s_place_p[2] = z;
            s_place_yaw = yaw >= 360.0f ? yaw - 360.0f : yaw;
            s_place_on = 1;
            return;
        }
        e = strchr(e, ';');
        if (e) e++;
    }
    for (i = 0; i < sizeof s_places / sizeof s_places[0]; i++)
        if (s_places[i].id && s_places[i].id == id) {
            s_place_id = id;
            s_place_p[0] = s_places[i].x; s_place_p[1] = s_places[i].y; s_place_p[2] = s_places[i].z;
            s_place_yaw = s_places[i].yaw;
            if (s_place_yaw >= 360.0f) s_place_yaw -= 360.0f;   /* 360 is 0, a whole turn */
            s_place_on = 1;
        }
}

/* Once the picked mission's object has its script (+0x1040), before its
 * immediate list places the player (opcode 0x48: spawn record global 0x4D
 * + slot, 0x2C bytes, the position at +8): that record's position becomes
 * the chosen spot. The script's spawn table is at +0x3C (relocated: a
 * pointer), its count at +0x40. */
static void place_tick(uint32_t o)
{
    uint32_t sc, tab, n, idx, c, m, r;
    if (!s_place_on || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    if ((m >= 100u ? m : c * 100u + m) != s_place_id) return;
    sc = MEM32(o + 0x1040u);
    if (!sc) return;
    tab = MEM32(sc + 0x3Cu); n = MEM32(sc + 0x40u);
    if (!n || n > 512u || tab < sc || tab > sc + 0x400000u) return;   /* v285c: loaded yet */
    s_place_on = 0;
    idx = var_get(0x4Du);
    if (idx >= n) idx = 0;
    if (!tab || !n) {
        fprintf(stderr, "[MISSION-SELECT] mssn%04u: no spawn table to place the player with\n", s_place_id);
        return;
    }
    r = tab + 0x2Cu * idx;
    MEM32(r + 0x08u) = f2u(s_place_p[0]);
    MEM32(r + 0x0Cu) = f2u(s_place_p[1]);
    MEM32(r + 0x10u) = f2u(s_place_p[2]);
    if (s_place_yaw >= 0.0f) {
        /* the facing: angles at +0x14 (x, y, z; 0x10000 a turn, y the yaw:
         * 0 looks along +Z, 0x4000 along +X); a pose-and-target record
         * (kind 2) also gets its target 100 units ahead */
        const float a = s_place_yaw * 3.14159265f / 180.0f;
        MEM32(r + 0x14u) = 0u;
        MEM32(r + 0x18u) = (uint32_t)(int32_t)(s_place_yaw * 65536.0f / 360.0f);
        MEM32(r + 0x1Cu) = 0u;
        if (MEM32(r) == 2u) {
            MEM32(r + 0x20u) = f2u(s_place_p[0] + 100.0f * sinf(a));
            MEM32(r + 0x24u) = f2u(s_place_p[1]);
            MEM32(r + 0x28u) = f2u(s_place_p[2] + 100.0f * cosf(a));
        }
    }
    var_set(0x4Du, idx);
    fprintf(stderr, "[MISSION-SELECT] mssn%04u: player placed at %.1f, %.1f, %.1f facing %.0f (spawn %u)\n",
            s_place_id, s_place_p[0], s_place_p[1], s_place_p[2], s_place_yaw, idx);
}

/* v290: the endings of the menu's missions (Miggas: skip the cutscenes,
 * and a mission with no cleared animation gets one). Once a pick's object
 * has its script, these commands of its blocking list become a
 * mission_cleared (0xE5: player 0, a spawn, the managed action, the exit,
 * music 0xA, the cleared jingle); each keeps its own conditions and flags.
 * Read from the mission files:
 *  - a win cutscene (play_event 0xE3) before the mission's own clear:
 *    0451 (event 0x33), 0514 (0x45), 0751 (0x56), 0816 (0x58), 0872 (0x5D),
 *    0880 (0x60) take that clear's spawn, action and exit;
 *  - a win that ends in a cutscene and an exit, with no clear: 0122 (event
 *    0x1A, then on to chapter 2), 0513 (0x43, then 0514), 0890 (0x66), and
 *    the fade_and_exit (0xEE) of 0231 and 0461, get one, at the current
 *    entry's spawn;
 *  - 0360's last switch cutscene (0x2D: the gate opens) ends it, with the
 *    exit it leads to (to 0370);
 *  - v299c: 0750 has two phases (Miggas): the first's cutscene (0x53) is
 *    left as it is and leads to the second (0750 again), whose own clear
 *    ends the pick, now right after its device is sprayed (s_wpatches).
 * The opcode found must be the one listed, else nothing changes. The lists
 * in the loaded script are pointers (header +0x174 + 8 * list: commands,
 * count), commands 0x54 bytes, the opcode at +0x10, the arguments after. */
struct conv { uint16_t id; uint8_t idx, op, exit, managed; int8_t spawn; };
static const struct conv s_convs[] = {
    { 122u, 2, 0xE3, 0, 0xB, -1 }, { 122u, 1, 0xE6, 0, 0xB, -1 },
    { 231u, 2, 0xEE, 0, 0xB, -1 },
    { 360u, 11, 0xE3, 2, 0xB, -1 },
    { 451u, 2, 0xE3, 0, 0xB, 3 },
    { 461u, 1, 0xEE, 0, 0xB, -1 }, { 461u, 2, 0xEE, 0, 0xB, -1 },
    { 461u, 3, 0xEE, 0, 0xB, -1 }, { 461u, 4, 0xEE, 0, 0xB, -1 },
    { 513u, 2, 0xE3, 0, 0xF, -1 }, { 513u, 3, 0xE6, 0, 0xF, -1 },
    { 514u, 1, 0xE3, 0, 0xB, 1 },
    { 751u, 1, 0xE3, 0, 0xB, 6 },
    { 816u, 1, 0xE3, 0, 0xB, 14 },
    { 872u, 1, 0xE3, 0, 0xB, 12 },
    { 880u, 1, 0xE3, 0, 0xB, 1 },
    { 890u, 1, 0xE3, 0, 0xB, -1 }, { 890u, 2, 0xE6, 0, 0xB, -1 },
};
static int conv_has(unsigned id)
{
    size_t i;
    for (i = 0; i < sizeof s_convs / sizeof s_convs[0]; i++)
        if (s_convs[i].id == id) return 1;
    return 0;
}
static void conv_tick(uint32_t o)
{
    uint32_t sc, c, m, cmds, n, k = 0, cur;
    size_t i;
    if (!s_conv_on || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    if ((m >= 100u ? m : c * 100u + m) != s_conv_id) return;
    sc = MEM32(o + 0x1040u);
    if (!sc) return;
    /* v285c: the file is read after the object has its buffer, and its
     * offsets become pointers after: wait for a list that points into it */
    cmds = MEM32(sc + 0x174u + 8u * 3u); n = MEM32(sc + 0x178u + 8u * 3u);
    if (!n || n > 512u || cmds < sc || cmds > sc + 0x400000u) return;
    s_conv_on = 0;
    cur = var_get(0x4Du);
    if (cur >= MEM32(sc + 0x40u)) cur = 0;
    for (i = 0; i < sizeof s_convs / sizeof s_convs[0]; i++) {
        const struct conv *v = &s_convs[i];
        uint32_t cm, op;
        if (v->id != s_conv_id) continue;
        if (v->idx >= n) continue;
        cm = cmds + 0x54u * v->idx;
        op = MEM32(cm + 0x10u);
        if (op != v->op) {
            fprintf(stderr, "[MISSION-SELECT] mssn%04u: command %u is 0x%X, not 0x%X: left as it is\n",
                    s_conv_id, v->idx, op, v->op);
            continue;
        }
        MEM32(cm + 0x28u) = 0u;
        MEM32(cm + 0x24u) = 0xAu;                                   /* music */
        MEM32(cm + 0x20u) = v->exit;                                /* the exit */
        MEM32(cm + 0x1Cu) = v->managed;                             /* managed action */
        MEM32(cm + 0x18u) = v->spawn >= 0 ? (uint32_t)v->spawn : cur;
        MEM32(cm + 0x14u) = 0u;                                     /* player */
        MEM32(cm + 0x10u) = 0xE5u;
        k++;
    }
    fprintf(stderr, "[MISSION-SELECT] mssn%04u: %u ending command(s) now the cleared animation\n", s_conv_id, k);
}

/* v295: cutscenes at the start of a pick, skipped (Miggas: 0812's three).
 * Its immediate list plays them (play_event 0xE3) as the mission starts,
 * right after its file is in, maybe in the same update as its offsets
 * become pointers; so each one's condition (an array of its own) is
 * pointed at a mission flag nothing sets (mission[511] = 1) as soon as the
 * file's bytes for it are in, whether its offsets are pointers yet or not:
 * the command and the condition word must be the ones listed. */
struct skip { uint16_t id; uint8_t list, idx, op; uint32_t cond; };
static const struct skip s_skips[] = {
    { 812u, 0, 5, 0xE3, FLAGW(0, 0x02D1) },   /* event 0x61, IF chapter[90] = 0 */
    { 812u, 0, 6, 0xE3, FLAGW(0, 0x02D1) },   /* event 0x77 */
    { 812u, 0, 7, 0xE3, FLAGW(0, 0x02D1) },   /* event 0x78 */
};
static volatile int s_skip_on;
static unsigned s_skip_id;
static uint32_t s_skip_done;              /* a bit per s_skips entry */
static int skip_has(unsigned id)
{
    size_t i;
    for (i = 0; i < sizeof s_skips / sizeof s_skips[0]; i++)
        if (s_skips[i].id == id) return 1;
    return 0;
}
static void skip_tick(uint32_t o)
{
    uint32_t sc, c, m;
    size_t i;
    int left = 0;
    if (!s_skip_on || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    if ((m >= 100u ? m : c * 100u + m) != s_skip_id) return;
    sc = MEM32(o + 0x1040u);
    if (!sc) return;
    for (i = 0; i < sizeof s_skips / sizeof s_skips[0]; i++) {
        const struct skip *k = &s_skips[i];
        uint32_t lp, n, cm, co;
        if (k->id != s_skip_id || (s_skip_done >> i & 1u)) continue;
        left++;
        lp = MEM32(sc + 0x174u + 8u * k->list); n = MEM32(sc + 0x178u + 8u * k->list);
        if (!n || n > 512u || k->idx >= n) continue;                 /* not in yet */
        if (lp < sc) lp += sc;                                       /* an offset still */
        cm = lp + 0x54u * k->idx;
        if (MEM32(cm + 0x10u) != k->op || MEM32(cm + 4u) != 1u) continue;
        co = MEM32(cm);
        if (co < sc) co += sc;
        if (MEM32(co) != k->cond) continue;                          /* not in yet */
        MEM32(co) = FLAGW(8, 0x0FF8);                                /* mission[511] = 1 */
        s_skip_done |= 1u << i;
        left--;
        fprintf(stderr, "[MISSION-SELECT] mssn%04u: start cutscene (event 0x%X) skipped\n", s_skip_id,
                MEM32(cm + 0x14u));
    }
    if (!left) s_skip_on = 0;
}

/* v299c: a flag a command writes, changed in every script of the pick's
 * mission as it loads (a mission that reloads itself for its next phase
 * gets it again). 0750's second phase (Miggas: clear before Professor K
 * speaks): its device sprayed (listener 4, actor_list_done 0xA) wrote
 * mission[115], which starts K's three lines, the last setting mission[116]
 * for the clear (blocking 9); it now writes mission[116], so the clear
 * comes at once and the lines never start. */
struct wpatch { uint16_t id; uint8_t list, idx, op; uint32_t from, to; };
static const struct wpatch s_wpatches[] = {
    { 750u, 1, 4, 0x0A, FLAGW(8, 0x0398), FLAGW(8, 0x03A0) },   /* mission[115] -> [116] */
};
static void wpatch_tick(uint32_t o)
{
    uint32_t sc, c, m, id;
    size_t i;
    if (!s_pick_live || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    id = m >= 100u ? m : c * 100u + m;
    if (id != s_pick) return;
    sc = MEM32(o + 0x1040u);
    if (!sc) return;                        /* checked every update: a reload may reuse the buffer */
    for (i = 0; i < sizeof s_wpatches / sizeof s_wpatches[0]; i++) {
        const struct wpatch *w = &s_wpatches[i];
        uint32_t lp, n, cm, wo, wc, k;
        if (w->id != id) continue;
        lp = MEM32(sc + 0x174u + 8u * w->list); n = MEM32(sc + 0x178u + 8u * w->list);
        if (!n || n > 512u || w->idx >= n) continue;                 /* not in yet */
        if (lp < sc) lp += sc;                                       /* an offset still */
        if (lp > sc + 0x400000u) continue;
        cm = lp + 0x54u * w->idx;
        if (MEM32(cm + 0x10u) != w->op) continue;
        wo = MEM32(cm + 8u); wc = MEM32(cm + 0xCu);
        if (wc > 32u) continue;
        if (wo < sc) wo += sc;
        if (wo > sc + 0x400000u) continue;
        for (k = 0; k < wc; k++)
            if (MEM32(wo + 4u * k) == w->from) {
                MEM32(wo + 4u * k) = w->to;
                fprintf(stderr, "[MISSION-SELECT] mssn%04u: command %u writes %08X now\n", id, w->idx, w->to);
            }
    }
}

/* v285: how many other missions a mission's exits lead to */
static int mm_outs(unsigned id)
{
    uint8_t h[8], e[8];
    uint32_t off, cnt, k, outs[8];
    int no = 0, j;
    FILE *f = mm_open(id);
    if (!f) return -1;
    if (rd(f, 0x194u, h, 8)) {
        off = le32(h); cnt = le32(h + 4);
        if (cnt > 64u) cnt = 64u;
        for (k = 0; k < cnt; k++) {
            uint32_t op, t;
            if (!rd(f, off + 0x44u * k, e, sizeof e)) break;
            op = le32(e); t = le32(e + 4);
            if (!(op == 0xFBu || op == 0xFCu || op == 0xFDu) || t == id || (t < 100u && t == id % 100u)) continue;
            for (j = 0; j < no && outs[j] != t; j++) ;
            if (j == no && no < 8) outs[no++] = t;
        }
    }
    fclose(f);
    return no;
}

static void pick_setup(unsigned c, unsigned m)
{
    const unsigned id = c * 100u + m;
    unsigned par[16];
    int np = 0, entry = -1, i, li;
    struct pk pk;
    FILE *f;
    if (m == 0u || (m >= 94u && m <= 97u)) return;     /* Garages, openings: as they are */
    s_conv_on = 0;
    if (conv_has(id)) {                                  /* v290: its ending changed (s_convs) */
        s_conv_id = id;
        s_conv_on = 1;
    }
    s_skip_on = 0;
    if (skip_has(id)) {                                  /* v295: start cutscenes skipped (s_skips) */
        s_skip_id = id;
        s_skip_done = 0;
        s_skip_on = 1;
    }
    if (!s_mm_state) mm_list();
    for (i = 0; i < s_mm_n && np < 16; i++) {
        uint8_t h[8], e[12];
        uint32_t off, cnt, k;
        const unsigned p = s_mm_id[i];
        int is_parent = 0;
        if (p / 100u != c || p == id) continue;
        f = mm_open(p);
        if (!f) continue;
        if (rd(f, 0x194u, h, 8)) {
            off = le32(h); cnt = le32(h + 4);
            if (cnt > 64u) cnt = 64u;
            for (k = 0; k < cnt; k++) {
                uint32_t op, t;
                if (!rd(f, off + 0x44u * k, e, sizeof e)) break;
                op = le32(e); t = le32(e + 4);
                if ((op == 0xFBu || op == 0xFCu || op == 0xFDu) && (t == id || (t < 100u && t == m))) {
                    is_parent = 1;
                    if ((int32_t)le32(e + 8) >= 0 && entry < 0) entry = (int32_t)le32(e + 8);
                }
            }
        }
        fclose(f);
        if (is_parent) par[np++] = p;
    }
    if (!np) return;
    memset(&pk, 0, sizeof pk);
    pk.spawn = -1;
    f = mm_open(id);
    if (!f) return;
    {
        /* v282: a challenge that only goes back to one place (0481 -> 0480)
         * also gets the chapter flags its other lists test that the parent
         * writes (0481's clear waits for chapter[50], from 0480); its own
         * state flags (ones it writes itself) are left alone */
        uint8_t h[8], e[8];
        uint32_t off, cnt, k, outs[8];
        int no = 0, j;
        if (rd(f, 0x194u, h, 8)) {
            off = le32(h); cnt = le32(h + 4);
            if (cnt > 64u) cnt = 64u;
            for (k = 0; k < cnt; k++) {
                uint32_t op, t;
                if (!rd(f, off + 0x44u * k, e, sizeof e)) break;
                op = le32(e); t = le32(e + 4);
                if (!(op == 0xFBu || op == 0xFCu || op == 0xFDu) || t == id || (t < 100u && t == m)) continue;
                for (j = 0; j < no && outs[j] != t; j++) ;
                if (j == no && no < 8) outs[no++] = t;
            }
        }
        for (li = 0; li < 4; li++) mm_walk(f, li, 1, pk_own, &pk);
        if (entry < 0) mm_walk(f, 0, 0, pk_need, &pk);
        if (no == 1) for (li = 0; li < 4; li++) mm_walk(f, li, 0, pk_need, &pk);
    }
    if (entry < 0)
        for (li = 0; li < 4; li++) mm_walk(f, li, 0, pk_clear, &pk);
    for (li = 0; li < 4; li++) mm_walk(f, li, 0, pk_clearflags, &pk);
    pk_clear_chain(f, &pk);
    fclose(f);
    for (i = 0; i < np && pk.nn; i++) {
        f = mm_open(par[i]);
        if (!f) continue;
        for (li = 0; li < 4; li++) mm_walk(f, li, 1, pk_wrote, &pk);
        fclose(f);
    }
    if (entry >= 0) var_set(0x4Du, (uint32_t)entry);
    else if (pk.spawn >= 0) var_set(0x4Du, (uint32_t)pk.spawn);
    place_arm(id);
    /* v283: areas whose objective comes after story steps of their own
     * (Miggas). 0360 (Sewage Facility, "Find the switches and spray 'em!")
     * starts with the opening and Rapid 99's trap into Garam's half-pipe
     * (0361); its switches step follows Garam joining: chapter[1] = 1
     * (opening seen) and chapter[5] = 1 (the state 0361's talk leaves),
     * which brings the gates (bucket list 1), the switches (actor list 0)
     * and the trigger for the switches cutscene. */
    if (id == 360u) {
        save_call1(0x00039BE0u, sub_00039BE0, FLAGW(8, 0x0009));   /* +1:1 */
        save_call1(0x00039BE0u, sub_00039BE0, FLAGW(8, 0x0029));   /* +1:5 */
        fprintf(stderr, "[MISSION-SELECT] mssn0360: starts after Garam joins (chapter flags 1 and 5)\n");
    }
    for (i = 0; i < pk.ns; i++) save_call1(0x00039BE0u, sub_00039BE0, pk.set[i]);
    for (i = 0; i < pk.ncl; i++) save_call1(0x00039BE0u, sub_00039BE0, pk.clr[i]);
    if (pk.ncl) {
        fprintf(stderr, "[MISSION-SELECT] mssn%04u: its clear's chapter flags set:", id);
        for (i = 0; i < pk.ncl; i++) fprintf(stderr, " %u", (pk.clr[i] >> 3) & 0xFFFFu);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[MISSION-SELECT] mssn%04u (from mssn%04u): spawn %d%s%s", id, par[0],
            entry >= 0 ? entry : pk.spawn, entry >= 0 ? " (its entry)" : " (entered in place)",
            pk.ns ? ", chapter flags set:" : "");
    for (i = 0; i < pk.ns; i++) fprintf(stderr, " %u", (pk.set[i] >> 3) & 0xFFFFu);
    fprintf(stderr, "\n");
}

/* v279: a mission picked in mission select goes back to the title when it
 * ends (Miggas). A mission ends through one of its exit commands, 0x53FB0
 * (handleMssnExit), which sets the mission object's state: 0x5D, 0x5F, 0x61
 * go to mission +0x54 (cut, fade, paused), 0x63 quits to the title, 0x65
 * is the next chapter, 0x67 the stored mission. Any of them but the mission
 * itself again (a retry) becomes the quit to the title, the same way as the
 * quit window's Quit to Title. Only for a pick with an objective: openings,
 * Garages and free roam play on as usual. Checked before and after the
 * object's update, as the exit is set from inside it. */
/* v289: whether the picked mission has been cleared (Miggas: 0666, 0671 and
 * 0746 clear with an exit to themselves, and the mission, started again,
 * plays the talk where the rival joins, then goes back to its area; a
 * retry of the same mission had been let through). Cleared: a
 * mission_cleared (0xE5) of its blocking list has its conditions met; a
 * retry after a fail doesn't. Flags: SAVE + 0x6A14 (mission), +0x18
 * (chapter), +0x58 (global), +0x98 (special), a bit array each. */
static int flag_holds(uint32_t w)
{
    static const uint32_t base[4] = { 0x6A14u, 0x18u, 0x58u, 0x98u };
    const uint32_t l = w & 7u, i = (w >> 3) & 0xFFFFu, v = (w >> 19) & 1u;
    if (l > 3u || i >= 0x200u) return 0;
    return ((MEM32(SAVE + base[l] + (i >> 5) * 4u) >> (i & 31u)) & 1u) == v;
}
static void cleared_tick(uint32_t o)
{
    uint32_t sc, c, m, cmds, n, i, k;
    if (s_pick_cleared || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    if ((m >= 100u ? m : c * 100u + m) != s_pick) return;
    sc = MEM32(o + 0x1040u);
    if (!sc) return;
    cmds = MEM32(sc + 0x174u + 8u * 3u); n = MEM32(sc + 0x178u + 8u * 3u);
    if (!n || n > 512u || cmds < sc || cmds > sc + 0x400000u) return;   /* not loaded yet */
    for (i = 0; i < n; i++) {
        const uint32_t cm = cmds + 0x54u * i;
        uint32_t co = MEM32(cm), cc = MEM32(cm + 4u), ok = 1;
        if (MEM32(cm + 0x10u) != 0xE5u) continue;
        if (cc && co < sc) co += sc;                 /* an offset, if not made a pointer */
        for (k = 0; k < cc && k < 32u && ok; k++) ok = (uint32_t)flag_holds(MEM32(co + 4u * k));
        if (ok) {
            s_pick_cleared = 1;
            fprintf(stderr, "[MISSION-SELECT] mssn%04u cleared\n", s_pick);
            return;
        }
    }
}

static void pick_exit_check(uint32_t o)
{
    uint32_t st, tgt;
    if (!o) return;
    st = MEM32(o + 0x5Cu);
    if (s_pick_live && !s_sel_target) {
        /* v285b: another mission running in its place without an exit we
         * saw (a cutscene that loads the next one, 0513 -> 0514) ends it */
        const uint32_t c = MEM32(o + 0x4Cu) & 0xFFFFu, m = MEM32(o + 0x50u) & 0xFFFFu;
        const uint32_t mid = m >= 100u ? m : c * 100u + m;
        if (mid == s_pick) s_pick_seen = 1;
        else if (s_pick_seen && st < 0x5Du && m != 95u) {
            fprintf(stderr, "[MISSION-SELECT] mssn%04u was followed by mssn%04u: back to the title\n", s_pick, mid);
            s_pick_live = 0; s_pick_seen = 0;
            var_set(VAR_NEXT, SEQ_TITLE);
            MEM32(o + 0x5Cu) = GAME_END;
            return;
        }
    }
    /* v281: the character select's exit (to the Garage, CC00) goes to the
     * pick instead, started as v280 does */
    if (s_sel_target && (st == 0x5Du || st == 0x5Fu || st == 0x61u)
        && ((MEM32(o + 0x50u) & 0xFFFFu) == 95u || MEM32(o + 0x54u) == s_sel_target / 100u * 100u)) {
        const unsigned id = s_sel_target;
        s_sel_target = 0;
        MEM32(o + 0x54u) = id;
        fprintf(stderr, "[MISSION-SELECT] character chosen: on to mssn%04u\n", id);
        pick_setup(id / 100u, id % 100u);
        return;
    }
    if (!s_pick_live) return;
    if (st < 0x5Du) cleared_tick(o);
    if (st != 0x5Du && st != 0x5Fu && st != 0x61u && st != 0x65u && st != 0x67u) return;
    if (st <= 0x61u) {
        tgt = MEM32(o + 0x54u);
        if (!s_pick_cleared
            && (tgt == s_pick || (tgt < 100u && tgt == s_pick % 100u && (MEM32(o + 0x4Cu) & 0xFFFFu) == s_pick / 100u)))
            return;                                 /* the same mission again: a retry */
    } else {
        tgt = 0;
    }
    fprintf(stderr, "[MISSION-SELECT] mssn%04u ended (state 0x%X, next %u): back to the title\n",
            s_pick, st, tgt);
    s_pick_live = 0;
    var_set(VAR_NEXT, SEQ_TITLE);
    MEM32(o + 0x5Cu) = GAME_END;
}

/* v291: a picked mission starts with 20 spray cans (Miggas). A second
 * after the picked mission's object first runs (its immediate list, which
 * resets the player's counters, has run by then), player 0 gets them through
 * the game's CPlayer::setQuantities (0x7FE70; thiscall: cans, stamina %,
 * interaction, -1 leaves one alone): up to the most the character carries
 * (+0xFA4), the HUD's counter too. Again after a retry (a new object).
 * Player 0: the player state in slot 0xC, its player's slot at +0x8D4. */
#define PICK_CANS 20u
static int s_pick_cans;                 /* v292: 0 no cans, 1 twenty, 2 as many as the character carries */
extern void sub_0007FE70(void);
static uint32_t s_cans_obj;
static int s_cans_wait, s_cans_look;
/* player 0's CPlayer, 0 while there is none */
static uint32_t player0(void)
{
    const uint32_t a = am();
    const uint32_t st = a ? MEM32(a + 0x98u + 4u * 0x0Cu) : 0;
    const uint32_t pid = st ? MEM32(st + 0x8D4u) : 0;
    return st && pid && pid < 0x1DF0u ? MEM32(a + 0x98u + 4u * pid) : 0;
}
static void cans_tick(uint32_t o)
{
    uint32_t c, m, pl;
    if (!s_pick || !s_pick_cans || s_sel_target || !o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    if ((m >= 100u ? m : c * 100u + m) != s_pick) return;
    /* v291b: the object runs while the stage still loads, before the
     * player is made (Miggas' log: "no player" a second in): look for the
     * player each update (for up to a minute), then wait a second more, so
     * the immediate list's counter reset has run */
    if (o != s_cans_obj) { s_cans_obj = o; s_cans_look = 3600; s_cans_wait = 0; return; }
    if (s_cans_look > 0) {
        if (!player0()) {
            if (--s_cans_look == 0) {
                const uint32_t a = am(), st = a ? MEM32(a + 0x98u + 4u * 0x0Cu) : 0;
                fprintf(stderr, "[MISSION-SELECT] mssn%04u: no player to give cans to (slot 0xC 0x%08X, +8D4 %08X)\n",
                        s_pick, st, st ? MEM32(st + 0x8D4u) : 0u);
            }
            return;
        }
        s_cans_look = 0;
        s_cans_wait = 60;
        return;
    }
    if (s_cans_wait <= 0 || --s_cans_wait) return;
    pl = player0();
    {
        /* v292: "Max cans" asks for more than anyone carries: the game
         * keeps it to the character's most (+0xFA4) */
        const uint32_t want = s_pick_cans == 2 ? 999u : PICK_CANS;
        if (!pl || MEM32(pl + 0xFA0u) >= (s_pick_cans == 2 ? MEM32(pl + 0xFA4u) : want)) return;
        PUSH32(g_esp, 0xFFFFFFFFu);          /* interaction: as it is */
        PUSH32(g_esp, 0xFFFFFFFFu);          /* stamina: as it is */
        PUSH32(g_esp, want);
    }
    PUSH32(g_esp, RET_MARK);
    g_ecx = pl;
    RECOMP_ABI_CALL(0x0007FE70u, sub_0007FE70);
    fprintf(stderr, "[MISSION-SELECT] mssn%04u: %u cans (carries up to %u)\n", s_pick,
            MEM32(pl + 0xFA0u), MEM32(pl + 0xFA4u));
}

/* v300: a timer for a picked mission (Idea by Miggas), drawn top-left. It
 * counts the mission's updates (60 a second, the game's own time: nothing
 * while paused) while its script runs freely: the mission object's step
 * (+0x5C) is RunCmds (0x0F) and no countdown holds the players (global
 * 0x1B4). A cutscene (steps 0x12-0x13, from the start or in the middle) or
 * a talk (0x1E-0x1F) stops it until it ends. It stops for good at the
 * cleared screen (steps 0x18-0x1D) or a race's results (0x32-0x37) and
 * shows that time until the pick ends. A failed run (steps 0x14-0x17) or a
 * quit from the pause menu (global 0x19B) starts it again from 0 with the
 * retry's new mission object; a mission that reloads itself for its next
 * phase (0750) keeps counting. */
static uint32_t s_tm_frames;
static int s_tm_frozen, s_tm_retry;
static void timer_show(void)
{
    uint32_t cs = s_tm_frames * 100u / 60u;
    snprintf(g_xbox_timer, sizeof g_xbox_timer, "%u:%02u.%02u", cs / 6000u, cs / 100u % 60u, cs % 100u);
}
static void timer_tick(uint32_t o)
{
    uint32_t c, m, id, st;
    if (!o) return;
    c = MEM32(o + 0x4Cu) & 0xFFFFu; m = MEM32(o + 0x50u) & 0xFFFFu;
    id = m >= 100u ? m : c * 100u + m;
    if (id != s_pick) return;
    st = MEM32(o + 0x5Cu);
    if ((st >= 0x14u && st <= 0x17u) || var_get(0x19Bu)) s_tm_retry = 1;
    if (o != s_tm_obj) {
        if (!s_tm_obj || s_tm_retry) {
            s_tm_frames = 0;
            s_tm_frozen = 0;
            s_tm_retry = 0;
        }
        s_tm_obj = o;
    }
    if (!s_tm_frozen) {
        if ((st >= 0x18u && st <= 0x1Du) || (st >= 0x32u && st <= 0x37u) || s_pick_cleared) {
            s_tm_frozen = 1;
            fprintf(stderr, "[MISSION-SELECT] mssn%04u: time %u:%02u.%02u\n", id,
                    s_tm_frames * 100u / 60u / 6000u, s_tm_frames * 100u / 60u / 100u % 60u,
                    s_tm_frames * 100u / 60u % 100u);
        } else if (st == 0x0Fu && !var_get(0x1B4u))
            s_tm_frames++;
    }
    timer_show();
}

/* v300b: the game's own race timer hidden in picked races (Miggas: 0112,
 * 0232, 0666, 0671, 0721, 0746), where the mission timer (v300) counts.
 * The HUD's time (TimerDisplay, vtable 0x1D23F0) in mode 3 (+0x160) shows
 * the mission's command timer counting up; its draw (DrawListDefault,
 * thiscall, ret 4) is skipped for it. The result screen's time (mode 6)
 * is left as it is. */
static int race_timer_hidden(void)
{
    static const uint16_t ids[] = { 112u, 232u, 666u, 671u, 721u, 746u };
    size_t i;
    if (!s_pick_live) return 0;
    for (i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (ids[i] == s_pick) return 1;
    return 0;
}
extern void sub_0010EC10_gen(void);
void sub_0010EC10(void)
{
    if (g_ecx && MEM32(g_ecx + 0x160u) == 3u && race_timer_hidden()) {
        g_esp += 8;                         /* ret 4 */
        return;
    }
    sub_0010EC10_gen();
}

void sub_00051FC0(void)
{
    const uint32_t o = g_ecx;
    s_game_this = o;
    s_game_seen = s_seq_ticks;
    if (s_place_on || s_conv_on || s_skip_on) {
        place_tick(o);
        conv_tick(o);
        skip_tick(o);
        g_ecx = o;
    }
    if (s_pick_live) {
        wpatch_tick(o);                     /* v299c */
        g_ecx = o;
    }
    if (s_pick) {
        cans_tick(o);
        g_ecx = o;
    }
    if (s_pick_live) timer_tick(o);         /* v300 */
    if (s_pick_live || s_sel_target) {
        pick_exit_check(o);
        g_ecx = o;
    }
    sub_00051FC0_gen();
    if (s_pick_live || s_sel_target) {
        const uint32_t a = g_eax, d = g_edx;
        pick_exit_check(o);
        g_eax = a; g_edx = d;
    }
}

/* v279: RECOMP_SHOW_MISSION=1 adds the current mission (mssnCCMM, from the
 * mission object: chapter +0x4C, number +0x50, an exit's three-digit target
 * being a whole id) and its objective to the window title */
extern void xbox_SetTitleMission(const char *t);
static char s_title_ob[80];               /* the current mission's objective */
static float s_face;                      /* v284c: the way the player last moved, degrees */
static void show_mission_tick(void)
{
    static int on = -1;
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t o, id = 0xFFFFFFFEu;
    if (on < 0) {
        const char *e = getenv("RECOMP_SHOW_MISSION");
        on = e && e[0] == '1';
    }
    if (!on) return;
    o = s_game_this && s_seq_ticks - s_game_seen <= 30u ? s_game_this : 0;
    if (o) {
        uint32_t c = MEM32(o + 0x4Cu) & 0xFFFFu, m = MEM32(o + 0x50u) & 0xFFFFu;
        id = m >= 100u ? m : c * 100u + m;
    }
    {
        /* v284: the player's position (the tracker DAT_00257CE4, +0x7C) in
         * the title, refreshed every 15 frames; F9 writes it to the log */
        static int was9;
        static uint32_t tk;
        const int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const uint32_t tr = MEM32(0x00257CE4u);
        float p[3] = { 0, 0, 0 };
        if (tr) {
            p[0] = u2f(MEM32(tr + 0x7Cu));
            p[1] = u2f(MEM32(tr + 0x80u));
            p[2] = u2f(MEM32(tr + 0x84u));
        }
        {
            /* the facing, from the way the player last moved (more than half
             * a unit a frame on the ground plane): degrees, 0 along +Z, 90
             * along +X, as the spawn records' yaw */
            static float lp[3];
            static int have;
            const float dx = p[0] - lp[0], dz = p[2] - lp[2];
            if (have && tr && dx * dx + dz * dz > 0.25f && dx * dx + dz * dz < 400.0f) {
                s_face = atan2f(dx, dz) * 180.0f / 3.14159265f;
                if (s_face < 0.0f) s_face += 360.0f;
            }
            lp[0] = p[0]; lp[1] = p[1]; lp[2] = p[2]; have = tr != 0;
            if (down && !was9 && id <= 9999u)
                fprintf(stderr, "[MISSION] mssn%04u position: %.1f, %.1f, %.1f facing %.0f"
                        "  (RECOMP_MISSION_SPAWN=%04u:%.1f,%.1f,%.1f,%.0f)\n",
                        id, p[0], p[1], p[2], s_face, id, p[0], p[1], p[2], s_face);
        }
        was9 = down;
        if (id <= 9999u && tr && ++tk >= 15u) {
            char t[96];
            tk = 0;
            snprintf(t, sizeof t, "mission %04u @ %.0f, %.0f, %.0f facing %.0f%s%.50s", id, p[0], p[1], p[2],
                     s_face, s_title_ob[0] ? ": " : "", s_title_ob);
            xbox_SetTitleMission(t);
        }
    }
    if (id == last) return;
    last = id;
    if (id > 9999u) {
        s_title_ob[0] = 0;
        xbox_SetTitleMission("");
    } else {
        char t[96];
        const char *ob = "";
        int i;
        if (!s_mm_state) mm_list();
        for (i = 0; i < s_mm_n; i++)
            if (s_mm_id[i] == id) { ob = s_mm_obj[i]; break; }
        snprintf(s_title_ob, sizeof s_title_ob, "%s", ob);
        snprintf(t, sizeof t, "mission %04u%s%.80s", id, ob[0] ? ": " : "", ob);
        xbox_SetTitleMission(t);
        fprintf(stderr, "[MISSION] mssn%04u%s%s\n", id, ob[0] ? ": " : "", ob);
    }
}

/* v278d: counted in game frames, not real time: the quit window holds the
 * game for as long as it is open, and v278c's half second had always run
 * out by the time the game went on ("no stage running", Miggas) */
static uint32_t game_obj(void)
{
    uint32_t o = s_game_this;
    return o && s_seq_ticks - s_game_seen <= 30u ? o : 0;
}

static void quit_to_title_tick(uint32_t state)
{
    uint32_t o;
    s_seq_ticks++;
    o = state == 0x1Eu ? game_obj() : 0;
    xbox_QuitTitleAvailable(o != 0);
    if (!xbox_QuitToTitleTake()) return;
    if (!o) {
        fprintf(stderr, "[QUIT] quit to title: no stage running (sequence state 0x%X)\n", state);
        return;
    }
    fprintf(stderr, "[QUIT] quit to title: game object 0x%08X state 0x%X -> 0x63, next 0xA\n",
            o, MEM32(o + 0x5Cu));
    var_set(VAR_NEXT, SEQ_TITLE);
    MEM32(o + 0x5Cu) = GAME_END;
}

/* v288: the missions the menu offers, in this order, under their chapters
 * (Miggas' list): a heading ("\x01" + text) and a gap ("") between
 * chapters, which the menu doesn't let the selection land on; a mission
 * not on the disc is left out. Labels are "Location - Objective". */
static const struct { uint16_t id; const char *label; } s_menu_def[] = {
    { 0u, "\x01" "Chapter 1" },
    { 112u, "Dogenzaka Hill - Race against Beat!" },
    { 113u, "Dogenzaka Hill - Defeat the Rokkaku police!" },
    { 122u, "Shibuya Terminal - Defeat the Rokkaku police!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 2" },
    { 231u, "Chuo Street - Defeat the Rokkaku police!" },
    { 232u, "Chuo Street - Race against Poison Jam!" },
    { 252u, "99th Street - Capture the flag against Rapid 99!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 3" },
    { 360u, "Sewers - Find the switches and spray 'em!" },
    { 371u, "Bottom Point - Spray the backs of Poison Jam!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 4" },
    { 451u, "Hikage Street - Eliminate the terror drone!" },
    { 461u, "Kibogaoka Hill - Defeat the Rokkaku police!" },
    { 481u, "Highway Zero - Spray the backs of the Immortals!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 5" },
    { 511u, "Future Site - Death Ball against Doom Riders!" },
    { 512u, "Future Site - Death Ball against The Immortals!" },
    { 513u, "Future Site - Death Ball against Love Shockers!" },
    { 514u, "Future Site - Destroy the Rokkaku piece of junk!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 6" },
    { 661u, "Sky Dinosaurian - Spray the backs of the Noise Tanks!" },
    { 666u, "Future Site - Race against Jazz!" },
    { 671u, "Highway Zero - Race against Soda!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 7" },
    { 721u, "Dogenzaka Hill - Race against Doom Riders!" },
    { 726u, "Hikage Street - Capture the flag against Love Shockers!" },
    { 746u, "Bottom Point - Race against Cube!" },
    { 750u, "Fortified Zone - Spray the devices to stop them!" },
    { 751u, "Fortified Zone - Defeat all the Golden Rhino's Jets!" },
    { 756u, "99th Street - Spray the backs of the Immortals!" },
    { 0u, "" },
    { 0u, "\x01" "Chapter 8" },
    { 816u, "Chuo Street - Defeat the Golden Rhinos!" },
    { 872u, "Highway Zero - Defeat the Golden Rhinos!" },
    { 880u, "Site of Tokyo Line - Destroy the Golden Rhino's train machine!" },
    { 812u, "Shibuya Terminal - Spray the backs of Zero Beat!" },
    { 890u, "?" "?" "?" "?" "? - Defeat A. KU. MU.!" },
};
#define MENU_MAX (sizeof s_menu_def / sizeof s_menu_def[0])
static const char *s_menu_ptr[MENU_MAX];
static uint16_t s_menu_id[MENU_MAX];
static int s_menu_n = -1;

static void menu_build(void)
{
    size_t i;
    s_menu_n = 0;
    for (i = 0; i < MENU_MAX; i++) {
        const unsigned id = s_menu_def[i].id;
        if (id && mission_file_state(id / 100u, id % 100u) != 1) {
            fprintf(stderr, "[MISSION-SELECT] mssn%04u is not on the disc: left out\n", id);
            continue;
        }
        s_menu_id[s_menu_n] = (uint16_t)id;
        s_menu_ptr[s_menu_n] = s_menu_def[i].label;
        s_menu_n++;
    }
    while (s_menu_n > 0 && !s_menu_ptr[s_menu_n - 1][0]) s_menu_n--;   /* no gap at the end */
    {
        int k = 0;
        for (i = 0; i < (size_t)s_menu_n; i++) k += s_menu_id[i] != 0;
        fprintf(stderr, "[MISSION-SELECT] the menu offers %d missions\n", k);
    }
}

static int s_ms_step, s_ms_row;          /* v292: which menu; the mission's row */
static const char *const s_cans_items[3] = { "No cans", "20 cans", "Max cans" };

void jsrf_mission_select_tick(uint32_t state)
{
    int pick;
    quit_to_title_tick(state);
    show_mission_tick();
    if (state == 0x0Au || state == 0x0Cu) s_pick_live = 0, s_sel_target = 0, s_pick = 0, s_cans_obj = 0;   /* v279: back at the title */
    if (!s_pick_live && g_xbox_timer[0]) g_xbox_timer[0] = 0, s_tm_obj = 0;   /* v300 */
    /* v292b: a countdown holds the players with global 0x1B4 from its start
     * until its GO lets them go (the HUD's announcement, mode 0); quitting to
     * the title during one (Miggas) left it set, and the character stayed
     * stuck in the character select and the next missions, until another
     * countdown reached GO. At the title nothing can be counting down. */
    if ((state == 0x0Au || state == 0x0Cu) && var_get(0x1B4u)) {
        fprintf(stderr, "[QUIT] the countdown's hold on the players (global 0x1B4 = %u) let go at the title\n",
                var_get(0x1B4u));
        var_set(0x1B4u, 0);
    }
    if (state != 0x0Cu) {
        xbox_ListMenuSet(NULL, NULL, 0);
        s_ms_step = 0;
        if (s_armed && state > 0x1Cu) {             /* a game started without the new game's 1:1 */
            s_armed = 0;
            g_xbox_note[0] = 0;
        }
        return;
    }
    if (!s_mm_state) mm_list();
    if (s_mm_state != 1) return;
    if (s_menu_n < 0) menu_build();
    if (s_menu_n <= 0) return;
    /* v292: two menus (Miggas): the mission, then "Start with" the cans;
     * B on the second goes back to the first, on the same mission */
    if (s_ms_step == 0) {
        xbox_ListMenuSet("Mission select", s_menu_ptr, s_menu_n);
        pick = xbox_ListMenuTake();
        if (pick >= 0 && pick < s_menu_n && s_menu_id[pick]) {
            s_ms_row = pick;
            s_ms_step = 1;
            xbox_ListMenuSet("Start with", s_cans_items, 3);
            xbox_ListMenuShow(0);
        }
        return;
    }
    xbox_ListMenuSet("Start with", s_cans_items, 3);
    pick = xbox_ListMenuTake();
    if (pick < 0 || pick > 2) {
        if (!xbox_ListMenuOpen()) {                 /* B: back to the missions */
            s_ms_step = 0;
            xbox_ListMenuSet("Mission select", s_menu_ptr, s_menu_n);
            xbox_ListMenuShow(s_ms_row);
        }
        return;
    }
    s_ms_step = 0;
    xbox_ListMenuSet("Mission select", s_menu_ptr, s_menu_n);
    xbox_ListMenuSelect(s_ms_row);                  /* it opens on this mission next time */
    {
        const unsigned id = s_menu_id[s_ms_row];
        int j, obj = 0;
        for (j = 0; j < s_mm_n; j++)
            if (s_mm_id[j] == id) { obj = s_mm_obj[j][0] != 0; break; }
        s_ac = id / 100u;
        s_am = id % 100u;
        s_armed = 1;
        s_pick_cans = pick;
        /* back to the title at its end: a mission with an objective, or a
         * challenge leading back to one place (v285: the chapter 5 races) */
        s_pick_obj = obj || mm_outs(id) == 1;
        snprintf(g_xbox_note, sizeof g_xbox_note, "%s (%s) ready: choose NEW GAME",
                 s_menu_ptr[s_ms_row], s_cans_items[pick]);
        fprintf(stderr, "[MISSION-SELECT] mssn%02u%02u armed for the next new game, %s\n", s_ac, s_am,
                s_cans_items[pick]);
    }
}
