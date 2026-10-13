/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#ifndef XBOX_CONTIG_SIZE
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)
#endif
#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

/* Slot+1 of each (subchannel, method) in s_seen, 0 for none. note() runs
 * for every word of the pushbuffer, and its linear search over the methods
 * seen so far was the single biggest cost of the GPU thread: about half of
 * it in play (run 58: ~5 s of every 10 unaccounted for by drawing). */
static uint16_t s_seen_slot[8][0x2000 / 4];

/* RECOMP_QUIET=1 (play_jsrf.bat): no method survey at all. It only feeds
 * the periodic report, and it runs for every word of the pushbuffer. */
static int s_quiet = -1;

static void note(uint32_t subch, uint32_t method)
{
    uint16_t k;
    if (s_quiet) {
        if (s_quiet < 0)
            s_quiet = getenv("RECOMP_QUIET") != NULL;
        if (s_quiet)
            return;
    }
    k = s_seen_slot[subch & 7][(method & 0x1FFC) >> 2];
    if (k) {
        s_seen[k - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        s_seen_slot[subch & 7][(method & 0x1FFC) >> 2] = (uint16_t)s_seen_count;
    }
}

/* v169: n of the same method at once (the index runs: 73 million words in
 * 10 s on the Deck, each one a call to note() before) */
static void note_n(uint32_t subch, uint32_t method, uint32_t n)
{
    uint16_t k;
    if (!n)
        return;
    note(subch, method);
    if (s_quiet || n == 1)
        return;
    k = s_seen_slot[subch & 7][(method & 0x1FFC) >> 2];
    if (k)
        s_seen[k - 1].count += n - 1;
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
/* v146: index runs go to the executor in one call (nv2a_pb_exec.c) when
 * the executor is set up (the survey still counts each word);
 * RECOMP_PB_BULK=0 sends them word by word as before */
extern int nv2a_pb_exec_indices16(const uint32_t *p, uint32_t n);
static int bulk_ok(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_PB_BULK");
        v = !(e && e[0] == '0');
    }
    return v;
}

static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w;
        if ((va & 0x0FFFFFFFu) >= XBOX_CONTIG_SIZE - 4)
            break;                       /* v75: see nv2a_pb_run */
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            if (noninc && method == 0x1800u && subch == 0 && s_exec_enabled && bulk_ok()) {
                uint32_t n = count;
                if (end_va > va && (end_va - va) / 4u < n)
                    n = (end_va - va) / 4u;
                if (nv2a_pb_exec_indices16((const uint32_t *)(mem + va), n)) {
                    note_n(subch, method, n);   /* the survey still counts them */
                    va += n * 4u;
                    words += n;
                    continue;
                }
            }
            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* Run the ring from GET to PUT, following JUMPs.
 *
 * The scan used to be handed address ranges: [last PUT, PUT) going forward,
 * and on a wrap [last PUT, highest PUT ever seen) plus [lowest PUT seen,
 * PUT). But D3D's ring wraps with a JUMP written after its last commands,
 * and PUT never points at the JUMP itself, so everything between the last
 * kick-off and the JUMP lay beyond "highest PUT seen" and was never run --
 * every lap, a different slice of state changes and draws went missing. It
 * hits whatever happened to be there: any object, the HUD, at random, and
 * more often the faster frames go round the ring (JSRF's stretched and
 * flickering objects once the GPU renderer made it fast).
 *
 * Here the walk simply follows the stream: commands until PUT, a JUMP
 * anywhere moves to its target, CALLs run their subroutine. */
void nv2a_pb_run(uint32_t get_va, uint32_t put_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = get_va, words = 0, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    {
        static int scan = -1;
        if (scan < 0)
            scan = getenv("RECOMP_PB_SCAN") != NULL;
        if (!(scan || s_exec_enabled))
            return;
    }
    while (va != put_va && words < 0x400000u) {
        uint32_t w;
        /* v75: never read outside the contiguous window. A stream that leads
         * there is not one being read correctly (stale or overwritten ring
         * contents, a GET the title moved); stop the run rather than fault
         * (run 74: a JUMP to physical 0x0436106C, past the end of RAM). */
        if ((va & 0x0FFFFFFFu) >= XBOX_CONTIG_SIZE - 4) {
            static int said;
            if (said++ < 4)
                fprintf(stderr, "[PB] stream left memory at 0x%08X (run %08X..%08X);"
                        " stopping this run\n", va, get_va, put_va);
            break;
        }
        w = *(const uint32_t *)(mem + va);

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            /* A ring wraps once between kick-offs; a stream that keeps
             * jumping is not one we are reading correctly. */
            if (++jumps > 64) {
                static int said;
                if (!said++)
                    fprintf(stderr, "[PB] more than 64 jumps between GET %08X and"
                            " PUT %08X; stopping this run\n", get_va, put_va);
                break;
            }
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            words++;
            continue;
        }
        va += 4;
        words++;
        if ((w & 0xFFFF0003u) == 0x00020000u)    /* RETURN outside a call */
            continue;
        if ((w & 3u) == 2u) {                    /* CALL */
            uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
            s_tot_calls++;
            words += pb_walk(sub, sub + 0x400000u, 1, &jumps, &unknown);
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;
            uint32_t i;

            if (noninc && method == 0x1800u && subch == 0 && s_exec_enabled && bulk_ok()) {
                /* v146: index runs in one call */
                uint32_t n = count;
                if (put_va > va && (put_va - va) / 4u < n)
                    n = (put_va - va) / 4u;
                if (nv2a_pb_exec_indices16((const uint32_t *)(mem + va), n)) {
                    note_n(subch, method, n);   /* the survey still counts them */
                    va += n * 4u;
                    words += n;
                    continue;
                }
            }
            for (i = 0; i < count && va != put_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m, *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
