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
void xbox_QuitTitleAvailable(int ok);   /* v278b: the quit window's Quit to Title */
int xbox_QuitToTitleTake(void);
extern char g_xbox_note[96];

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

static int chs_on(void)
{
    init_once();
    if (s_armed) {
        s_armed = 0;
        g_xbox_note[0] = 0;
        s_c = s_ac; s_m = s_am;
        s_pick = s_ac * 100u + s_am;    /* v279: back to the title when it ends */
        s_pick_live = s_pick_obj;
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
    }
}

struct pk { uint32_t need[64]; int nn; uint32_t set[64]; int ns; int spawn; };
static void pk_need(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    (void)op; (void)cmd;
    if (w != 0xFFFFFFFFu && (w & 7u) == 1u && (w >> 19 & 1u) && p->nn < 64) p->need[p->nn++] = w;
}
static void pk_wrote(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    int i, j;
    (void)op; (void)cmd;
    for (i = 0; i < p->nn; i++)
        if (p->need[i] == w) {
            for (j = 0; j < p->ns && p->set[j] != w; j++) ;
            if (j == p->ns && p->ns < 64) p->set[p->ns++] = w;
        }
}
static void pk_clear(uint32_t w, uint32_t op, const uint8_t *cmd, void *ctx)
{
    struct pk *p = (struct pk *)ctx;
    (void)w;
    if (op == 0xE5u && p->spawn < 0) p->spawn = (int)le32(cmd + 0x14 + 4);
}

static void pick_setup(unsigned c, unsigned m)
{
    const unsigned id = c * 100u + m;
    unsigned par[16];
    int np = 0, entry = -1, i, li;
    struct pk pk;
    FILE *f;
    if (m == 0u || (m >= 94u && m <= 97u)) return;     /* Garages, openings: as they are */
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
    if (entry >= 0) {
        var_set(0x4Du, (uint32_t)entry);
        fprintf(stderr, "[MISSION-SELECT] mssn%04u: starts at spawn %d, the entry from mssn%04u\n", id, entry, par[0]);
        return;
    }
    memset(&pk, 0, sizeof pk);
    pk.spawn = -1;
    f = mm_open(id);
    if (!f) return;
    mm_walk(f, 0, 0, pk_need, &pk);
    for (li = 0; li < 4; li++) mm_walk(f, li, 0, pk_clear, &pk);
    fclose(f);
    for (i = 0; i < np && pk.nn; i++) {
        f = mm_open(par[i]);
        if (!f) continue;
        for (li = 0; li < 4; li++) mm_walk(f, li, 1, pk_wrote, &pk);
        fclose(f);
    }
    if (pk.spawn >= 0) var_set(0x4Du, (uint32_t)pk.spawn);
    for (i = 0; i < pk.ns; i++) save_call1(0x00039BE0u, sub_00039BE0, pk.set[i]);
    fprintf(stderr, "[MISSION-SELECT] mssn%04u (entered in place from mssn%04u): spawn %d%s", id, par[0],
            pk.spawn, pk.ns ? ", chapter flags set:" : "");
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
static void pick_exit_check(uint32_t o)
{
    uint32_t st, tgt;
    if (!s_pick_live || !o) return;
    st = MEM32(o + 0x5Cu);
    if (st != 0x5Du && st != 0x5Fu && st != 0x61u && st != 0x65u && st != 0x67u) return;
    if (st <= 0x61u) {
        tgt = MEM32(o + 0x54u);
        if (tgt == s_pick || (tgt < 100u && tgt == s_pick % 100u && (MEM32(o + 0x4Cu) & 0xFFFFu) == s_pick / 100u))
            return;                                 /* the same mission again */
    } else {
        tgt = 0;
    }
    fprintf(stderr, "[MISSION-SELECT] mssn%04u ended (state 0x%X, next %u): back to the title\n",
            s_pick, st, tgt);
    s_pick_live = 0;
    var_set(VAR_NEXT, SEQ_TITLE);
    MEM32(o + 0x5Cu) = GAME_END;
}

void sub_00051FC0(void)
{
    const uint32_t o = g_ecx;
    s_game_this = o;
    s_game_seen = s_seq_ticks;
    if (s_pick_live) {
        pick_exit_check(o);
        g_ecx = o;
    }
    sub_00051FC0_gen();
    if (s_pick_live) {
        const uint32_t a = g_eax, d = g_edx;
        pick_exit_check(o);
        g_eax = a; g_edx = d;
    }
}

/* v279: RECOMP_SHOW_MISSION=1 adds the current mission (mssnCCMM, from the
 * mission object: chapter +0x4C, number +0x50, an exit's three-digit target
 * being a whole id) and its objective to the window title */
extern void xbox_SetTitleMission(const char *t);
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
    if (id == last) return;
    last = id;
    if (id > 9999u) {
        xbox_SetTitleMission("");
    } else {
        char t[96];
        const char *ob = "";
        int i;
        if (!s_mm_state) mm_list();
        for (i = 0; i < s_mm_n; i++)
            if (s_mm_id[i] == id) { ob = s_mm_obj[i]; break; }
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

void jsrf_mission_select_tick(uint32_t state)
{
    int pick;
    quit_to_title_tick(state);
    show_mission_tick();
    if (state == 0x0Au || state == 0x0Cu) s_pick_live = 0;   /* v279: back at the title */
    if (state != 0x0Cu) {
        xbox_ListMenuSet(NULL, NULL, 0);
        if (s_armed && state > 0x1Cu) {             /* a game started without the new game's 1:1 */
            s_armed = 0;
            g_xbox_note[0] = 0;
        }
        return;
    }
    if (!s_mm_state) mm_list();
    if (s_mm_state != 1) return;
    xbox_ListMenuSet("Mission select", s_mm_ptr, s_mm_n);
    pick = xbox_ListMenuTake();
    if (pick >= 0 && pick < s_mm_n) {
        s_ac = s_mm_id[pick] / 100u;
        s_am = s_mm_id[pick] % 100u;
        s_armed = 1;
        s_pick_obj = s_mm_obj[pick][0] != 0;
        snprintf(g_xbox_note, sizeof g_xbox_note, "%s ready: choose NEW GAME", s_mm_text[pick]);
        fprintf(stderr, "[MISSION-SELECT] mssn%02u%02u armed for the next new game\n", s_ac, s_am);
    }
}
