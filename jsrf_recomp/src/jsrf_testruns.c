/*
 * jsrf_testruns.c -- v311: Early Test Runs (Idea by Miggas; the idea from
 * mstan's JSRFRecomp, no code taken).
 *
 * Roboy's Test Run menu is built by sub_000756E0 (thiscall on the menu,
 * no arguments): it does nothing unless the story's chapter is 9
 * (sub_000149D0 on the save object, 0x1EFFB0), then fills four lists of test
 * runs from the menu's data ([this+0xF8], rows of 16 bytes at +0x000, +0x0D4,
 * +0x1A4 and +0x204: 13, 13, 6 and 7 of them) into [this+0xD4..0xE0], each
 * row's availability (+8) 1 when the graffiti souls of its area (row +4, an
 * index into the 14 area IDs at 0x21C098) are all collected
 * (sub_0003A0A0 / sub_0003A130, total and collected, on the save object),
 * and marks each list and the menu itself available ([this+0xB4] +8, +0x18,
 * +0x28, +0x38 and +0x58).
 *
 * Now, before chapter 9, the menu is built as if it were chapter 9 (the
 * chapter getter says 9 to it alone), then each test run stays available
 * only if its area's rule is met as well as its souls: a chapter finished
 * (Miggas's list: 99th Street after chapter 2, ...) and, for some, a battle
 * won (a save flag). Areas with no rule wait for the end of the story as
 * before. The lists' and the menu's marks follow.
 *
 * A test run's results give its character rewards only in chapter 9 too
 * (sub_0011A8B0): told 9 the same way, the rewards' own checks kept.
 *
 * RECOMP_EARLY_TESTRUNS=0 turns it off. RECOMP_TESTRUNS_LOG=1 logs the
 * menu's rows and every area's souls; RECOMP_TESTRUNS_ALL=1 makes every row
 * available (to read their names in the menu); RECOMP_TESTRUNS_RULES=
 * "area:chapter[:flagword],..." replaces the built-in rules (area: 0-13,
 * chapter: the one that must be finished, flagword: a save flag word, hex,
 * that must hold).
 */

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

#define SAVE_OBJ    0x001EFFB0u       /* the save object: chapter, souls, flags */
#define AREA_IDS    0x0021C098u       /* the 14 graffiti-soul areas */
#define RET_MARK    0x0007BDD0u       /* return address our own guest calls push */

extern void sub_000756E0_gen(void);
extern void sub_000149D0_gen(void);
extern void sub_0011A8B0_gen(void);
extern void sub_0003A0A0(void);
extern void sub_0003A130(void);

typedef void (*cs_word_fn)(uint32_t w, uint32_t op, int list, void *ctx);
int cs_mission_writes(unsigned id, cs_word_fn fn, void *ctx);
int cs_flag_holds(uint32_t w);

static int env_on(const char *name, int def)
{
    const char *e = getenv(name);
    return e && e[0] ? e[0] != '0' : def;
}

/* ---- the rules: an area's test runs need its chapter finished and, for
 * some, a flag (a battle won) ---- */
typedef struct { int area; int chapter; uint32_t flag; } Rule;
#define MAX_RULES 16
static Rule s_rules[MAX_RULES];
static int s_nrules = -1;

/* a save flag word of the global set, its value 1 (scope 2 in bits 0-2, the
 * index in 3-18, the value in 19), built from its parts so the release
 * translator doesn't take it for an address */
#define GLOBAL_SET(i) ((1u << 19) | ((uint32_t)(i) << 3) | 2u)

/* v311c: Miggas's rules, by the areas' order (v311's log and the menu):
 * 0 Shibuya Terminal, 1 Chuo Street, 2 Dogenzaka Hill, 3 Hikage Street,
 * 4 Rokkaku-dai Heights, 5 Tokyo Underground Sewage Facility, 6 Kibogaoka
 * Hill, 7 The Bottom Point of the Sewage Facility, 8 The Fortified
 * Residential Zone, 9 99th Street, 10 Sky Dinosaurian Square, 11 (no test
 * runs), 12 Highway Zero, 13 The Skyscraper District and Pharaoh Park.
 * The chapter 7 battles' clears (opcode 0xE5) set global[15] (0721,
 * Dogenzaka Hill), [16] (0726, Hikage Street) and [17] (0756, the
 * Skyscraper District), flags the save keeps. */
static const Rule BUILTIN[] = {
    { 9, 2, 0 },                    /* 99th Street: Rapid 99, chapter 2 */
    { 5, 3, 0 },                    /* the sewage facility: chapter 3 */
    { 7, 3, 0 },                    /* its bottom point: chapter 3 */
    { 10, 6, 0 },                   /* Sky Dinosaurian Square: chapter 6 */
    { 2, 7, GLOBAL_SET(15) },       /* Dogenzaka Hill: chapter 7, 0721 won */
    { 13, 7, GLOBAL_SET(17) },      /* the Skyscraper District: chapter 7, 0756 won */
    { 3, 7, GLOBAL_SET(16) },       /* Hikage Street: chapter 7, 0726 won */
};

static void rules_load(void)
{
    const char *e = getenv("RECOMP_TESTRUNS_RULES");
    s_nrules = 0;
    if (!e || !*e) {
        s_nrules = (int)(sizeof BUILTIN / sizeof BUILTIN[0]);
        memcpy(s_rules, BUILTIN, sizeof BUILTIN);
        return;
    }
    while (e && *e && s_nrules < MAX_RULES) {
        Rule r = { -1, 9, 0 };
        char *end;
        r.area = (int)strtol(e, &end, 10);
        if (end == e) break;
        e = end;
        if (*e == ':') { r.chapter = (int)strtol(e + 1, &end, 10); e = end; }
        if (*e == ':') { r.flag = (uint32_t)strtoul(e + 1, &end, 16); e = end; }
        if (r.area >= 0 && r.area < 14) s_rules[s_nrules++] = r;
        while (*e && *e != ',') e++;
        if (*e == ',') e++;
    }
    if (s_nrules)
        fprintf(stderr, "[TESTRUNS] %d rule(s) from RECOMP_TESTRUNS_RULES\n", s_nrules);
}

/* may area's test runs be played in chapter ch (souls aside) */
static int area_open(int area, uint32_t ch)
{
    int i;
    for (i = 0; i < s_nrules; i++)
        if (s_rules[i].area == area)
            return ch > (uint32_t)s_rules[i].chapter && (!s_rules[i].flag || cs_flag_holds(s_rules[i].flag));
    return 0;                                  /* no rule: the end of the story, as before */
}

/* ---- guest calls ---- */
static uint32_t save_call(uint32_t va, recomp_func_t fn, int nargs, uint32_t arg)
{
    uint32_t sv = g_ecx, r;
    g_ecx = SAVE_OBJ;
    if (nargs) PUSH32(g_esp, arg);
    PUSH32(g_esp, RET_MARK);
    RECOMP_ABI_CALL(va, fn);
    r = g_eax;
    g_ecx = sv;
    return r;
}

/* the chapter getter, the menu's build told 9 while it runs */
static int s_say9;

void sub_000149D0(void)
{
    if (s_say9 && g_ecx == SAVE_OBJ) {
        /* a thiscall with no arguments: return as it would */
        g_esp += 4;
        g_eax = 9;
        return;
    }
    sub_000149D0_gen();
}

static uint32_t real_chapter(void)
{
    return save_call(0x000149D0u, sub_000149D0_gen, 0, 0);
}

/* ---- the log (RECOMP_TESTRUNS_LOG) ---- */
static void log_flagword(uint32_t w, uint32_t op, int list, void *ctx)
{
    static const char *scope[8] = { "mission", "chapter", "global", "special", "?4", "?5", "?6", "?7" };
    (void)ctx;
    if (w >= 0xFFFFFFFEu || (w & 7u) == 0u) return;         /* command ends; mission-local flags */
    fprintf(stderr, "[TESTRUNS]   list %d op 0x%02X writes %s[%u] = %u (word %05X), now %s\n", list, op,
            scope[w & 7u], (w >> 3) & 0xFFFFu, (w >> 19) & 1u, w,
            cs_flag_holds(w) ? "so" : "not so");
}

static void log_once(uint32_t ch)
{
    static int done;
    static const unsigned battles[3] = { 721, 726, 756 };   /* Miggas: the chapter 7 rival battles */
    int i;
    if (done) return;
    done = 1;
    fprintf(stderr, "[TESTRUNS] chapter %u; the 14 areas' graffiti souls (collected / total):\n", ch);
    for (i = 0; i < 14; i++) {
        uint32_t id = MEM32(AREA_IDS + 4u * (uint32_t)i);
        uint32_t tot = save_call(0x0003A0A0u, sub_0003A0A0, 1, id);
        uint32_t got = save_call(0x0003A130u, sub_0003A130, 1, id);
        fprintf(stderr, "[TESTRUNS]   area %2d (0x%05X): %u / %u%s\n", i, id, got, tot,
                tot == got ? " (all)" : "");
    }
    for (i = 0; i < 3; i++) {
        fprintf(stderr, "[TESTRUNS] mssn%04u's flags:\n", battles[i]);
        if (!cs_mission_writes(battles[i], log_flagword, NULL))
            fprintf(stderr, "[TESTRUNS]   (its file wasn't opened)\n");
    }
}

/* ---- the menu ---- */
static const uint32_t LIST_SRC[4] = { 0x000u, 0x0D4u, 0x1A4u, 0x204u };
static const int LIST_N[4] = { 13, 13, 6, 7 };

void sub_000756E0(void)
{
    const uint32_t menu = g_ecx;
    uint32_t ch, data, marks;
    int l, r, any = 0, logit = env_on("RECOMP_TESTRUNS_LOG", 0), all = env_on("RECOMP_TESTRUNS_ALL", 0);
    if (!env_on("RECOMP_EARLY_TESTRUNS", 1) || !menu || !MEM32(menu + 0xCCu)) {
        sub_000756E0_gen();
        return;
    }
    if (s_nrules < 0) rules_load();
    ch = real_chapter();
    if (logit) log_once(ch);
    if (ch >= 9u && !all) {                     /* the story finished: as the game does */
        sub_000756E0_gen();
        return;
    }
    s_say9 = 1;
    sub_000756E0_gen();
    s_say9 = 0;
    data = MEM32(menu + 0xF8u);
    marks = MEM32(menu + 0xB4u);
    if (!data || !marks) return;
    for (l = 0; l < 4; l++) {
        const uint32_t rows = MEM32(menu + 0xD4u + 4u * (uint32_t)l);
        int open = 0;
        if (!rows) continue;
        for (r = 0; r < LIST_N[l]; r++) {
            const uint32_t src = data + LIST_SRC[l] + 0x10u * (uint32_t)r;
            /* v311c: the first list keeps the area at +4, the others at +0
             * (as the game's own soul test reads them) */
            const int area = (int)MEM32(src + (l ? 0u : 4u));
            uint32_t *av = (uint32_t *)XBOX_PTR(rows + 0x10u * (uint32_t)r + 8u);
            const int souls = *av != 0;
            if (all) *av = 1;
            else if (*av && (area < 0 || area >= 14 || (ch < 9u && !area_open(area, ch)))) *av = 0;
            if (logit)
                fprintf(stderr, "[TESTRUNS] list %d row %2d: area %2d, data %08X %08X %08X %08X, souls %s -> %s\n",
                        l, r, area, MEM32(src), MEM32(src + 4u), MEM32(src + 8u), MEM32(src + 12u),
                        souls ? "all" : "not all", *av ? "open" : "locked");
            open |= *av != 0;
        }
        MEM32(marks + 8u + 0x10u * (uint32_t)l) = (uint32_t)open;
        any |= open;
    }
    MEM32(marks + 0x58u) = (uint32_t)any;
    if (logit)
        fprintf(stderr, "[TESTRUNS] chapter %u: the Test Run menu %s\n", ch, any ? "open" : "locked");
}

/* v311c: a test run's results (sub_0011A8B0, made only by the Test Run
 * menu's code, sub_00075057) give its character rewards (sub_0011A6F0:
 * rank, area, not yet had) only in chapter 9: before it, it is told 9 too,
 * the rewards' own checks kept */
void sub_0011A8B0(void)
{
    if (env_on("RECOMP_EARLY_TESTRUNS", 1) && real_chapter() < 9u) {
        s_say9 = 1;
        sub_0011A8B0_gen();
        s_say9 = 0;
        return;
    }
    sub_0011A8B0_gen();
}
