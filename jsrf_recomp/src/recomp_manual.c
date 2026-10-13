/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

/* Register model, MEM32 and recomp_func_t. The template declared g_eax as a
 * plain extern here, but the runtime defines it thread-local (RECOMP_TLS), so
 * take the declarations from the generated header instead. Note it defines
 * macros named after the registers (eax, esp, ...): don't use those as
 * identifiers in this file. */
#include <windows.h>            /* before recomp_types.h: it #defines eax, esp, ... */
#include "gen/recomp_types.h"

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ══ JSRF heap checker ═══════════════════════════════════════════════════
 *
 * Run 4/5 hung in RtlAllocateHeap on a free list that loops back on itself:
 * the game freed 0x0105EE68 while that block was already free. The XAPI heap
 * itself lifts correctly (tools/difftest: 50k+ ops, zero divergence against
 * the original x86), so the bad free comes from a caller.
 *
 * These wrap the three heap entry points. Every block the heap hands out is
 * recorded; a free or realloc of a pointer that is not live is reported with
 * the guest call stack and recent indirect-call targets, then SKIPPED so the
 * heap stays consistent and the run continues. One run therefore lists every
 * bad free instead of hanging on the first.
 *
 *   0x001497DC RtlAllocateHeap(heap, flags, size)        stdcall, ret 0xC
 *   0x00149F5E RtlFreeHeap(heap, flags, ptr)             stdcall, ret 0xC
 *   0x0014A12E RtlReAllocateHeap(heap, flags, ptr, size) stdcall, ret 0x10
 *
 * Set RECOMP_HEAPCHK=0 to turn the checking off (the wrappers then just call
 * through). The generated bodies are the sub_X_gen functions; the recompiler
 * emits them under that name because of the extern declarations below
 * (--exclude-manual src/recomp_manual.c).
 */
extern void sub_001497DC_gen(void);
extern void sub_00149F5E_gen(void);
extern void sub_0014A12E_gen(void);

#define HC_BITS 21                          /* 2M slots: plenty for live blocks */
#define HC_SIZE (1u << HC_BITS)
#define HC_EMPTY 0u
#define HC_TOMB  1u                         /* heap pointers are 8-aligned, never 1 */
static uint32_t *hc_tab;
static uint32_t *hc_size, *hc_site;          /* parallel to hc_tab */
static uint32_t hc_live, hc_allocs, hc_frees, hc_bad;
#define HC_RING 8192u                        /* recently freed blocks */
static struct { uint32_t p, size, site; } hc_ring[HC_RING];
static uint32_t hc_ring_n;

/* First guest return address above the heap wrappers: who asked for this. */
static uint32_t hc_caller(uint32_t sp)
{
    for (uint32_t k = 0; k < 48; k++) {
        uint32_t w = MEM32(sp + 4 * k);
        if (w >= 0x00011000u && w < 0x001C3F60u
                && !(w >= 0x00149000u && w < 0x0014AA00u)   /* XAPI heap + wrappers */
                && !(w >= 0x0017C900u && w < 0x0017C9C0u))  /* CRT malloc/new/delete */
            return w;
    }
    return 0;
}
static CRITICAL_SECTION hc_lock;
static int hc_state = -1;                   /* -1 unknown, 0 off, 1 on */

static int hc_on(void)
{
    if (hc_state < 0) {
        const char *e = getenv("RECOMP_HEAPCHK");
        hc_state = !(e && *e == '0');
        if (hc_state) {
            hc_tab = (uint32_t *)calloc(HC_SIZE, sizeof(uint32_t));
            hc_size = (uint32_t *)calloc(HC_SIZE, sizeof(uint32_t));
            hc_site = (uint32_t *)calloc(HC_SIZE, sizeof(uint32_t));
            InitializeCriticalSection(&hc_lock);
            if (!hc_tab || !hc_size || !hc_site) hc_state = 0;
            fprintf(stderr, "[HEAPCHK] %s\n", hc_state ? "tracking guest heap blocks (RECOMP_HEAPCHK=0 disables)"
                                                       : "allocation failed, disabled");
        }
    }
    return hc_state;
}

static uint32_t hc_hash(uint32_t p) { return (p * 0x9E3779B1u) >> (32 - HC_BITS); }

static void hc_add_info(uint32_t p, uint32_t size, uint32_t site)
{
    uint32_t i = hc_hash(p), tomb = 0xFFFFFFFFu;
    for (;; i = (i + 1) & (HC_SIZE - 1)) {
        if (hc_tab[i] == p) { if (size) { hc_size[i] = size; hc_site[i] = site; } return; }
        if (hc_tab[i] == HC_TOMB && tomb == 0xFFFFFFFFu) tomb = i;
        if (hc_tab[i] == HC_EMPTY) break;
    }
    i = tomb != 0xFFFFFFFFu ? tomb : i;
    hc_tab[i] = p; hc_size[i] = size; hc_site[i] = site;
    hc_live++;
}
static void hc_add(uint32_t p) { hc_add_info(p, 0, 0); }

/* Removes p; returns 1 if it was live. Records it in the recent-free ring. */
static int hc_del_site(uint32_t p, uint32_t free_site)
{
    uint32_t i = hc_hash(p);
    for (;; i = (i + 1) & (HC_SIZE - 1)) {
        if (hc_tab[i] == p) {
            uint32_t r = hc_ring_n++ % HC_RING;
            hc_ring[r].p = p; hc_ring[r].size = hc_size[i]; hc_ring[r].site = free_site;
            hc_tab[i] = HC_TOMB; hc_live--; return 1;
        }
        if (hc_tab[i] == HC_EMPTY) return 0;
    }
}
static int hc_del(uint32_t p) { return hc_del_site(p, 0); }
static int hc_has(uint32_t p, uint32_t *size)
{
    uint32_t i = hc_hash(p);
    for (;; i = (i + 1) & (HC_SIZE - 1)) {
        if (hc_tab[i] == p) { if (size) *size = hc_size[i]; return 1; }
        if (hc_tab[i] == HC_EMPTY) return 0;
    }
}

/* What is p, if not a live block start? */
static void hc_explain(uint32_t p)
{
    uint32_t n = hc_ring_n < HC_RING ? hc_ring_n : HC_RING, k, i;
    for (k = 0; k < n; k++) {                  /* newest first */
        uint32_t r = (hc_ring_n - 1 - k) % HC_RING;
        if (hc_ring[r].p == p) {
            fprintf(stderr, "  => DOUBLE FREE: freed before (%u frees ago) by code at 0x%08X\n", k, hc_ring[r].site);
            return;
        }
    }
    for (i = 0; i < HC_SIZE; i++) {            /* interior of a live block? */
        uint32_t q = hc_tab[i];
        if (q > HC_TOMB && hc_size[i] && q < p && p < q + hc_size[i]) {
            fprintf(stderr, "  => INTERIOR pointer: +0x%X into live block 0x%08X (size 0x%X) allocated by code at 0x%08X\n",
                    p - q, q, hc_size[i], hc_site[i]);
            return;
        }
    }
    for (k = 0; k < n; k++) {                  /* inside something already freed? */
        uint32_t r = (hc_ring_n - 1 - k) % HC_RING;
        if (hc_ring[r].size && hc_ring[r].p < p && p < hc_ring[r].p + hc_ring[r].size) {
            fprintf(stderr, "  => USE AFTER FREE: +0x%X into block 0x%08X freed by code at 0x%08X\n",
                    p - hc_ring[r].p, hc_ring[r].p, hc_ring[r].site);
            return;
        }
    }
    fprintf(stderr, "  => never allocated by the XAPI heap (not live, not recently freed)\n");
}

static void hc_report(const char *what, uint32_t p, uint32_t sp)
{
    hc_bad++;
    if (hc_bad > 40) {
        if ((hc_bad % 1000) == 0)
            fprintf(stderr, "[HEAPCHK] %u bad frees/reallocs so far (reports suppressed)\n", hc_bad);
        return;
    }
    fprintf(stderr, "[HEAPCHK] BAD %s(0x%08X): not a live block  [#%u, live=%u allocs=%u frees=%u]\n",
            what, p, hc_bad, hc_live, hc_allocs, hc_frees);
    hc_explain(p);
    if (p >= 0x00010008u && p < 0x04000000u)   /* only peek inside guest RAM */
        fprintf(stderr, "  header @%08X: %08X %08X\n", p - 8, MEM32(p - 8), MEM32(p - 4));
    fprintf(stderr, "  guest stack (code-like words from esp=%08X):", sp);
    {
        int shown = 0;
        for (uint32_t k = 0; k < 160 && shown < 20; k++) {
            uint32_t w = MEM32(sp + 4 * k);
            if (w >= 0x00011000u && w < 0x001C3F60u) {
                fprintf(stderr, "%s +%-3u %08X", (shown % 5) ? "" : "\n   ", 4 * k, w);
                shown++;
            }
        }
    }
    fprintf(stderr, "\n  recent ICALL targets:");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & 15]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void sub_001497DC(void)                     /* RtlAllocateHeap */
{
    uint32_t size, site;
    if (!hc_on()) { sub_001497DC_gen(); return; }
    size = MEM32(g_esp + 12);
    site = hc_caller(g_esp);
    sub_001497DC_gen();
    if (g_eax) {
        EnterCriticalSection(&hc_lock);
        hc_add_info(g_eax, size, site); hc_allocs++;
        LeaveCriticalSection(&hc_lock);
    }
}

void sub_00149F5E(void)                     /* RtlFreeHeap */
{
    uint32_t p;
    int ok;
    if (!hc_on()) { sub_00149F5E_gen(); return; }
    p = MEM32(g_esp + 12);
    if (!p) { sub_00149F5E_gen(); return; }
    EnterCriticalSection(&hc_lock);
    ok = hc_del_site(p, hc_caller(g_esp));
    if (ok) hc_frees++;
    else hc_report("free", p, g_esp);
    LeaveCriticalSection(&hc_lock);
    if (!ok) {                              /* skip it: report success, pop args */
        g_eax = 1;
        g_esp += 16;
        return;
    }
    sub_00149F5E_gen();
}

void sub_0014A12E(void)                     /* RtlReAllocateHeap */
{
    uint32_t p, r;
    int ok;
    if (!hc_on()) { sub_0014A12E_gen(); return; }
    p = MEM32(g_esp + 12);
    if (p) {
        EnterCriticalSection(&hc_lock);
        /* Check membership without removing: the lifted body may call the
         * alloc/free wrappers above itself when it moves the block. */
        ok = hc_has(p, 0);
        if (!ok) hc_report("realloc", p, g_esp);
        LeaveCriticalSection(&hc_lock);
        if (!ok) {                          /* skip: fail the realloc */
            g_eax = 0;
            g_esp += 20;
            return;
        }
    }
    {
        uint32_t newsize = MEM32(g_esp + 16), site = hc_caller(g_esp);
        sub_0014A12E_gen();
        r = g_eax;
        if (r) {
            EnterCriticalSection(&hc_lock);
            if (p && r != p) hc_del_site(p, site);   /* the body may already have freed it */
            hc_add_info(r, newsize, site);           /* in place or moved: record new size */
            LeaveCriticalSection(&hc_lock);
        }
    }
}

/* ── Unresolved-stub hits ────────────────────────────────────────────────
 * tools/log_stubs.py patches recomp_stubs_unresolved.c so each stub calls this
 * on its first call. A stub reached at run time is a real function the
 * disassembler missed: its body is skipped and eax is left as garbage. Seed
 * its address (analysis/seeds.json) and re-lift. */
void recomp_stub_hit(uint32_t stub_va, uint32_t ret_va, uint32_t eax_val)
{
    static int n;
    if (n++ < 200) {
        fprintf(stderr, "[STUB] 0x%08X reached (not lifted -- body skipped), called from 0x%08X, eax=0x%08X\n",
                stub_va, ret_va, eax_val);
        fflush(stderr);
    }
}

/* ── DirectSound GP DSP doorbell ──────────────────────────────
 *
 * Run 12: the main thread spun forever inside DirectSound's effects-image
 * download (CDirectSound_DownloadEffectsImage 0x19E6D7 -> 0x1A1769):
 *
 *     mov [ebx], 3          ; ebx = scratch + 0x810, "load the new image"
 *   L: cmp [ebx], 0 / jne L ; wait for the GP DSP to clear it
 *
 * The toolkit's GP DSP is a passthrough stub that never runs DSP code, so
 * nothing ever clears the word. 0x1A1905 waits on the same word before it
 * posts a command. The word's address is *(*(*(this+8)+0x10)) + 0x810, and
 * there is no global anchor for it, so both functions are wrapped to learn
 * it from `this`. A host thread then completes every posted command at once.
 * That is what the toolkit's RECOMP_DSP_ACK probe does for a fixed address.
 * It is a handshake acknowledgement, not a DSP: the effects (reverb etc.)
 * won't be applied, but init no longer blocks the game. */
/* ── DirectSound I3DL2 listener and effect data (v115) ───────
 *
 * IDirectSound_SetI3DL2Listener (0x1A095E, stdcall: this, pds3dl, dwApply)
 * hands the reverb parameters to the native reverb in apu_dsp.c, the GP DSP
 * that would run dsstdfx.bin's reverb not being emulated; and
 * IDirectSound_SetEffectData (0x19F0AA, stdcall: this, index, offset, data,
 * size, apply) is logged, to see what else the title tunes. */
/* v115c: the generated bodies are renamed <name>_gen by
 * tools/wrap_dsound.py (no re-lift needed); callers keep the plain names. */
extern void DSOUND_IDirectSound_SetI3DL2Listener_gen(void);
extern void DSOUND_IDirectSound_SetEffectData_gen(void);
extern void mcpx_apu_set_i3dl2(const float p[12]);

void DSOUND_IDirectSound_SetI3DL2Listener(void)
{
    uint32_t pl = MEM32(g_esp + 8), apply = MEM32(g_esp + 12);
    if (pl) {
        float p[12];
        int32_t li[12];
        int k;
        for (k = 0; k < 12; k++) li[k] = (int32_t)MEM32(pl + 4 * k);
        for (k = 0; k < 12; k++) {
            /* LONG lRoom, lRoomHF; FLOAT rolloff, decay, decayHF; LONG lReflections;
             * FLOAT reflDelay; LONG lReverb; FLOAT reverbDelay, diffusion, density, hfRef */
            if (k == 0 || k == 1 || k == 5 || k == 7) p[k] = (float)li[k];
            else memcpy(&p[k], &li[k], 4);
        }
        {
            static unsigned n;
            if (n++ < 60)
                fprintf(stderr, "[DSOUND] SetI3DL2Listener(apply %u)\n", apply);
        }
        mcpx_apu_set_i3dl2(p);
    }
    DSOUND_IDirectSound_SetI3DL2Listener_gen();
}

void DSOUND_IDirectSound_SetEffectData(void)
{
    uint32_t idx = MEM32(g_esp + 8), off = MEM32(g_esp + 12), data = MEM32(g_esp + 16),
             size = MEM32(g_esp + 20), apply = MEM32(g_esp + 24);
    static unsigned n;
    if (n++ < 200) {
        uint32_t k;
        fprintf(stderr, "[DSOUND] SetEffectData(effect %u, offset 0x%X, %u bytes, apply %u):",
                idx, off, size, apply);
        for (k = 0; k < size / 4 && k < 16 && data; k++)
            fprintf(stderr, " %06X", MEM32(data + 4 * k));
        fputc('\n', stderr);
    }
    DSOUND_IDirectSound_SetEffectData_gen();
}

extern void sub_001A1769_gen(void);
extern void sub_001A1905_gen(void);

static volatile uint32_t dsp_bell[4];
static volatile LONG dsp_nbell;

/* v111: what DirectSound puts in the GP scratch memory -- where the
 * effects image and the effect parameters live (the GP would DMA them) --
 * and every command it posts, with the block after the doorbell. Once a
 * second the first 64 KB of scratch is compared with the last look and the
 * changed words listed (160 at most per look; more is an upload), with the
 * time since start. RECOMP_APU_GP_LOG=0 turns it off; RECOMP_APU_GP_DUMP=1
 * also saves the scratch as gp_scratch_N.bin (N = 0..7) after each change. */
/* v112: the region compared is the GP's own memory (X/Y/P at 0xFE830000,
 * plain RAM here, where DirectSound writes effect state), not the scratch
 * page: past its first 0x800 bytes that was DirectSound's heap (run 111). */
#define SCR_BYTES 0x10000u
#define GP_MEM_VA 0xFE830000u
static uint32_t scr_prev[SCR_BYTES / 4];
static ULONGLONG scr_t0;

static void scr_look(uint32_t base, int first)
{
    static int dumps = -1, dumping;
    uint32_t i, changed = 0, listed = 0;
    double t = (GetTickCount64() - scr_t0) / 1000.0;
    if (dumps < 0) {
        const char *e = getenv("RECOMP_APU_GP_DUMP");
        dumping = e && e[0] == '1';
        dumps = 0;
    }
    for (i = 0; i < SCR_BYTES / 4; i++)
        if (MEM32(base + i * 4) != scr_prev[i])
            changed++;
    if (!changed)
        return;
    if (first || changed > 160) {
        fprintf(stderr, "[APU-GP] %.1f s: GP memory %08X: %u words changed%s\n", t, base, changed,
                first ? " (first look)" : " (an upload)");
    } else {
        fprintf(stderr, "[APU-GP] %.1f s: GP memory %08X: %u words changed\n", t, base, changed);
        for (i = 0; i < SCR_BYTES / 4 && listed < 160; i++) {
            uint32_t v = MEM32(base + i * 4);
            if (v != scr_prev[i]) {
                fprintf(stderr, "[APU-GP]   +%04X = %08X (was %08X)\n", i * 4, v, scr_prev[i]);
                listed++;
            }
        }
    }
    for (i = 0; i < SCR_BYTES / 4; i++)
        scr_prev[i] = MEM32(base + i * 4);
    if (dumping && dumps < 16) {
        char name[32];
        FILE *f;
        snprintf(name, sizeof name, "gp_mem_%d.bin", dumps++);
        f = fopen(name, "wb");
        if (f) {
            fwrite(scr_prev, 1, sizeof scr_prev, f);
            fclose(f);
            fprintf(stderr, "[APU-GP]   GP memory written to %s\n", name);
        }
    }
    fflush(stderr);
}

/* v113: the GP scratch (see mcpx_apu_gp_scratch_snapshot) taken each time
 * DirectSound posts a command, compared with the previous take: which words
 * of the effects' state change between areas with and without echo. */
#define GPS_WORDS (0x80000u / 4)
extern uint32_t mcpx_apu_gp_scratch_snapshot(uint32_t *dst, uint32_t max_words,
                                             uint32_t *sge_out, uint32_t *pages_out);
static uint32_t gps_prev[GPS_WORDS], gps_cur[GPS_WORDS];
static int gps_have;

static void gps_look(unsigned cmd)
{
    static int dumps = -1, dumping;
    uint32_t sge = 0, pages = 0, n, i, changed = 0, listed = 0;
    double t = (GetTickCount64() - scr_t0) / 1000.0;
    if (dumps < 0) {
        const char *e = getenv("RECOMP_APU_GP_DUMP");
        dumping = e && e[0] == '1';
        dumps = 0;
    }
    n = mcpx_apu_gp_scratch_snapshot(gps_cur, GPS_WORDS, &sge, &pages);
    if (!n) {
        static int said;
        if (!said++)
            fprintf(stderr, "[APU-GP] %.1f s: no GP scratch (GPSADDR %08X, %u pages)\n", t, sge, pages);
        return;
    }
    if (!gps_have) {
        fprintf(stderr, "[APU-GP] %.1f s: GP scratch: table at %08X, %u pages, %u KB taken\n",
                t, sge, pages, n / 256);
    } else {
        for (i = 0; i < n; i++)
            if (gps_cur[i] != gps_prev[i]) changed++;
        fprintf(stderr, "[APU-GP] %.1f s: after command %u, scratch: %u words changed\n", t, cmd, changed);
        for (i = 0; i < n && listed < 200; i++)
            if (gps_cur[i] != gps_prev[i]) {
                fprintf(stderr, "[APU-GP]   scratch+%05X = %08X (was %08X)\n", i * 4, gps_cur[i], gps_prev[i]);
                listed++;
            }
    }
    if (dumping && dumps < 16 && (!gps_have || changed)) {
        char name[32];
        FILE *f;
        snprintf(name, sizeof name, "gp_scr_%d.bin", dumps++);
        f = fopen(name, "wb");
        if (f) {
            fwrite(gps_cur, 4, n, f);
            fclose(f);
            fprintf(stderr, "[APU-GP]   scratch written to %s\n", name);
        }
    }
    memcpy(gps_prev, gps_cur, n * 4);
    gps_have = 1;
    fflush(stderr);
}

/* v114: the reverb's state block itself. The GP scratch stays empty here
 * (run 113: all zero -- the DSP that would fill it by DMA never runs), and
 * so does GP memory, so DirectSound must keep the effects' state in its
 * copy of the image and have the DSP fetch it (command 2). dsstdfx.bin's
 * I3DL2 reverb state starts with its own header -- scratch offset 0x70C60,
 * size 0x5CC60, 0x628, 0, 2, 0x1560, 0x1560, 0xC00 -- so all copies of that
 * header in guest RAM are found once and their 0x8A8-byte state blocks
 * compared at every command. */
#define RV_WORDS (0x8A8u / 4)
static const uint32_t rv_sig[8] = {0x070C60, 0x05CC60, 0x628, 0, 2, 0x1560, 0x1560, 0xC00};
static uint32_t rv_at[4], rv_prev[4][RV_WORDS];
static int rv_n = -1;

static void rv_find(void)
{
    uint32_t va;
    uint32_t region_end = 0;
    rv_n = 0;
    for (va = 0x80000000u; va < 0x84000000u - 0x1000u && rv_n < 4; va += 4) {
        if (va >= region_end) {
            /* only committed, readable memory */
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t hp = (uintptr_t)XBOX_PTR(va);
            if (!VirtualQuery((LPCVOID)hp, &mbi, sizeof mbi))
                break;
            {
                uint64_t e = (uint64_t)va + ((uintptr_t)mbi.BaseAddress + mbi.RegionSize - hp);
                region_end = e > 0x84000000ull ? 0x84000000u : (uint32_t)e;
            }
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                va = region_end - 4;
                continue;
            }
        }
        if (MEM32(va) != rv_sig[0]) continue;
        {
            int k, ok = va + 64 <= region_end;
            for (k = 1; k < 8 && ok; k++)
                ok = MEM32(va + 4 * k) == rv_sig[k];
            if (ok) {
                uint32_t i;
                rv_at[rv_n] = va;
                for (i = 0; i < RV_WORDS; i++) rv_prev[rv_n][i] = MEM32(va + 4 * i);
                fprintf(stderr, "[APU-RV] reverb state block found at %08X\n", va);
                rv_n++;
            }
        }
    }
    if (!rv_n)
        fprintf(stderr, "[APU-RV] no reverb state block in guest RAM\n");
}

static void rv_look(unsigned cmd)
{
    int b;
    double t = (GetTickCount64() - scr_t0) / 1000.0;
    if (rv_n < 0) {
        rv_find();
        if (rv_n) {
            int k;
            uint32_t i;
            for (k = 0; k < rv_n; k++) {
                fprintf(stderr, "[APU-RV] %.1f s: block %08X as found:", t, rv_at[k]);
                for (i = 0; i < RV_WORDS; i++)
                    fprintf(stderr, "%s%06X", (i % 16) ? " " : "\n[APU-RV]   ", rv_prev[k][i]);
                fputc('\n', stderr);
            }
        }
        return;
    }
    for (b = 0; b < rv_n; b++) {
        uint32_t i, changed = 0;
        for (i = 0; i < RV_WORDS; i++)
            if (MEM32(rv_at[b] + 4 * i) != rv_prev[b][i]) changed++;
        if (!changed) continue;
        fprintf(stderr, "[APU-RV] %.1f s: command %u, block %08X: %u words changed\n",
                t, cmd, rv_at[b], changed);
        for (i = 0; i < RV_WORDS; i++) {
            uint32_t v = MEM32(rv_at[b] + 4 * i);
            if (v != rv_prev[b][i]) {
                fprintf(stderr, "[APU-RV]   +%03X = %08X (was %08X)\n", i * 4, v, rv_prev[b][i]);
                rv_prev[b][i] = v;
            }
        }
    }
    fflush(stderr);
}

static DWORD WINAPI dsp_bell_thread(LPVOID unused)
{
    unsigned acks = 0, cmds = 0, ticks = 0;
    int log = -1, first = 1;
    (void)unused;
    {
        const char *e = getenv("RECOMP_APU_GP_LOG");
        log = e && e[0] == '1';                    /* v116: off unless asked */
    }
    for (;;) {
        LONG n = dsp_nbell;
        for (LONG i = 0; i < n; i++) {
            uint32_t a = dsp_bell[i];
            uint32_t v = MEM32(a);
            if (v) {
                if (log && cmds++ < 400) {
                    int k;
                    fprintf(stderr, "[APU-GP] %.1f s: command %u at %08X, block:",
                            (GetTickCount64() - scr_t0) / 1000.0, v, a);
                    for (k = 1; k <= 12; k++)
                        fprintf(stderr, " %08X", MEM32(a + 4 * k));
                    fputc('\n', stderr);
                }
                MEM32(a) = 0;
                if (log)
                    rv_look(v);                     /* v114 */
                if (acks++ < 8)
                    fprintf(stderr, "[DSP] doorbell 0x%08X: command %u acknowledged\n", a, v);
            }
        }
        if (log && n && ++ticks >= 1000 && first) {  /* v113: one look only */
            ticks = 0;
            scr_look(GP_MEM_VA, first);
            first = 0;
        }
        Sleep(1);
    }
    return 0;
}

static void dsp_bell_learn(uint32_t self)
{
    uint32_t p, a;
    if (!self || self >= 0x84000000u) return;
    p = MEM32(self + 8);
    if (!p || p >= 0x84000000u) return;
    p = MEM32(p + 0x10);
    if (!p || p >= 0x84000000u) return;
    a = MEM32(p) + 0x810;
    if (a < 0x1000u) return;
    for (LONG i = 0; i < dsp_nbell; i++)
        if (dsp_bell[i] == a) return;
    if (dsp_nbell >= 4) return;
    dsp_bell[dsp_nbell] = a;
    InterlockedIncrement(&dsp_nbell);
    fprintf(stderr, "[DSP] GP command doorbell at 0x%08X: completing commands from the host\n", a);
    if (dsp_nbell == 1) {
        scr_t0 = GetTickCount64();
        CloseHandle(CreateThread(NULL, 64 * 1024, dsp_bell_thread, NULL, 0, NULL));
    }
}

void sub_001A1769(void) { dsp_bell_learn(g_ecx); sub_001A1769_gen(); }
void sub_001A1905(void) { dsp_bell_learn(g_ecx); sub_001A1905_gen(); }

/* ── D3D fences: tell the GPU executor where D3D polls ────────
 *
 * Run 14: the main thread spun in D3D_BlockOnTime (0x191440) after its
 * first Swap, waiting for the GPU to write the fence time. D3D pushes
 * SEMAPHORE_RELEASE (0x1D70) for that, and the toolkit's executor now
 * handles it (patches/xboxrecomp-nv2a-semaphore.patch). The target address
 * lives in a DMA object the runtime doesn't model, so it comes from D3D
 * itself: g_pDevice = [0x19DCE0], and the CPU polls [g_pDevice+0x34]. */
extern void sub_00191390_gen(void);          /* D3D_SetFence */
extern void sub_00191440_gen(void);          /* D3D_BlockOnTime */
extern void xbox_Nv2aSetSemaphoreVA(uint32_t va);

/* What D3D's GPU interrupt handler does for a NO_OPERATION notify: wake
 * BlockOnTime, which sleeps on the device's busy event at g_pDevice+0x2440
 * (0x191501: KeWaitForSingleObject(dev+0x2440, ...)). */
extern void xbox_Nv2aSetNotifyHook(void (*fn)(uint32_t));
extern int xbox_SignalGuestEvent(uint32_t guest_va);

static void d3d_notify(uint32_t param)
{
    uint32_t dev = MEM32(0x0019DCE0u);
    (void)param;
    if (dev && dev < 0x84000000u)
        xbox_SignalGuestEvent(dev + 0x2440);
}

static void d3d_sem_learn(void)
{
    uint32_t dev = MEM32(0x0019DCE0u);
    static int hooked;
    if (!hooked) {
        hooked = 1;
        xbox_Nv2aSetNotifyHook(d3d_notify);
    }
    if (dev && dev < 0x84000000u) {
        uint32_t va = MEM32(dev + 0x34);
        if (va) xbox_Nv2aSetSemaphoreVA(va);
    }
}

void sub_00191390(void) { d3d_sem_learn(); sub_00191390_gen(); }
void sub_00191440(void) { d3d_sem_learn(); sub_00191440_gen(); }

/* ── Loader trace ─────────────────────────────────────────────
 *
 * Run 17: stuck on the SEGA logo at 60 fps, 3 draws a frame, after the
 * game had loaded the title, attract-demo and stage data. JSRF's loading
 * runs as "jobs": 0x25390 starts job N (start table 0x1EC068), and
 * 0x25310 runs one step per frame (step table 0x1EC0F0) until the step
 * returns non-zero. Job 22 is the logo. This logs each job's start, its
 * completion and, every ~5 s, a job that is still running, so a stuck one
 * is named. Lines start with [LOAD]. */
extern void sub_00025390_gen(void);
extern void sub_00025310_gen(void);

static uint32_t ld_steps, ld_job = 0xFFFFFFFFu;
static DWORD    ld_t0, ld_last_report;

void sub_00025390(void)                     /* start load job (thiscall, ret 8) */
{
    uint32_t self = g_ecx, job = MEM32(g_esp + 4), arg = MEM32(g_esp + 8);
    uint32_t busy = MEM32(self + 0x44);
    sub_00025390_gen();
    if (!busy) {
        int started = MEM32(self + 0x44) == 1;
        fprintf(stderr, "[LOAD] job %u arg 0x%08X %s (start fn 0x%08X, step fn 0x%08X)\n",
                job, arg, started ? "started" : "finished immediately",
                job < 0x21 ? MEM32(0x1EC068u + 4 * job) : 0,
                job < 0x21 ? MEM32(0x1EC0F0u + 4 * job) : 0);
        if (started) { ld_job = job; ld_steps = 0; ld_t0 = ld_last_report = GetTickCount(); }
    } else {
        fprintf(stderr, "[LOAD] job %u requested while job %u still busy (ignored by the game)\n",
                job, MEM32(self + 0x48));
    }
}

void sub_00025310(void)                     /* one step of the active job */
{
    uint32_t self = g_ecx;
    uint32_t was = MEM32(self + 0x44), job = MEM32(self + 0x48);
    sub_00025310_gen();
    if (was != 1) return;
    ld_steps++;
    if (MEM32(self + 0x44) != 1) {
        fprintf(stderr, "[LOAD] job %u done after %u steps, %lu ms\n",
                job, ld_steps, GetTickCount() - ld_t0);
        ld_job = 0xFFFFFFFFu;
    } else if (GetTickCount() - ld_last_report > 5000) {
        ld_last_report = GetTickCount();
        fprintf(stderr, "[LOAD] job %u still running: %u steps, %lu ms (obj 0x%08X)\n",
                job, ld_steps, ld_last_report - ld_t0, self);
    }
}

/* ── Boot logo trace ──────────────────────────────────────────
 *
 * Run 18: every load job finishes, yet the game stays on the SEGA logo.
 * The boot sequence (state [this+0x48]) is waiting in state 8 for the logo
 * task (id 0x1DDA, update 0x7E360) to delete itself. That task walks
 * states [this+0x98] 0..15: for each logo, fade in (wait for the fade task,
 * id 6, update 0x24700, done flag +0xC0), show 120 frames, fade out. This
 * logs each logo-state change and, every 2 s, the frame counter and the
 * fade's current/target values, so the wait that never ends is named.
 * Lines start with [LOGO]. */
extern void sub_0007E360_gen(void);
extern void sub_00024700_gen(void);

static uint32_t logo_last_state = 0xFFFFFFFFu, logo_calls, logo_dumped;
static void task_report(void);
static DWORD    logo_last_report;
static uint32_t fade_obj;

void sub_0007E360(void)                     /* logo task update (thiscall) */
{
    uint32_t self = g_ecx;
    sub_0007E360_gen();
    logo_calls++;
    {
        uint32_t st = MEM32(self + 0x98);
        if (st != logo_last_state) {
            fprintf(stderr, "[LOGO] state %u -> %u after %u updates (counter %u)\n",
                    logo_last_state, st, logo_calls, MEM32(self + 0x9C));
            logo_last_state = st;
        } else if (GetTickCount() - logo_last_report > 2000) {
            logo_last_report = GetTickCount();
            fprintf(stderr, "[LOGO] still state %u, counter %u, skip flag %u, updates %u",
                    st, MEM32(self + 0x9C), MEM32(self + 0xA0), logo_calls);
            if (fade_obj) {
                float cur[4], tgt[4], spd;
                for (int i = 0; i < 4; i++) {
                    uint32_t c = MEM32(fade_obj + 0x98 + 4 * i), t = MEM32(fade_obj + 0xA8 + 4 * i);
                    memcpy(&cur[i], &c, 4); memcpy(&tgt[i], &t, 4);
                }
                { uint32_t s = MEM32(fade_obj + 0xB8); memcpy(&spd, &s, 4); }
                fprintf(stderr, " | fade done=%u cur %.3f %.3f %.3f %.3f tgt %.3f %.3f %.3f %.3f step %.5f",
                        MEM32(fade_obj + 0xC0), cur[0], cur[1], cur[2], cur[3],
                        tgt[0], tgt[1], tgt[2], tgt[3], spd);
            } else {
                fprintf(stderr, " | fade task update never ran");
            }
            fprintf(stderr, "\n");
            if (!logo_dumped++ || logo_dumped == 5) task_report();
        }
    }
}

static uint32_t fade_calls;

void sub_00024700(void)                     /* fade task update (thiscall) */
{
    static int logged;
    uint32_t o = g_ecx, esp0 = g_esp;
    uint32_t before = MEM32(o + 0x98), done0 = MEM32(o + 0xC0);
    fade_obj = o;
    fade_calls++;
    sub_00024700_gen();
    /* First calls after a fade starts: what one update did to it. */
    if (!done0 && MEM32(o + 0xA8) && logged < 6) {
        extern RECOMP_TLS uint16_t g_fp_control_word;
        extern RECOMP_TLS int g_fp_top;
        float b, a; uint32_t after = MEM32(o + 0x98);
        memcpy(&b, &before, 4); memcpy(&a, &after, 4);
        logged++;
        fprintf(stderr, "[FADE] update obj 0x%08X: alpha %.5f -> %.5f (raw %08X -> %08X) done %u, "
                        "step raw %08X, esp %08X -> %08X, fpcw %04X top %d\n",
                o, b, a, before, after, MEM32(o + 0xC0), MEM32(o + 0xB8),
                esp0, g_esp, g_fp_control_word, g_fp_top);
    }
}

/* The task tree [[0x22FCE0]+0x87DC]: node +4 flags (negative = skipped
 * along with its children), +8 id, +0x24 parent, +0x28 first child, +0x30
 * next sibling; vtable slot 1 is the per-frame update. Printed once when the
 * logo stalls, to show whether the fade task (id 6) is in it and enabled. */
static void task_dump(uint32_t node, int depth, int *budget)
{
    while (node && node < 0x84000000u && (*budget)-- > 0) {
        uint32_t vt = MEM32(node);
        fprintf(stderr, "[TASK] %*s0x%08X id %-5d flags 0x%08X vtbl 0x%08X update 0x%08X\n",
                depth * 2, "", node, (int)MEM32(node + 8), MEM32(node + 4), vt,
                vt && vt < 0x84000000u ? MEM32(vt + 4) : 0);
        if (MEM32(node + 0x28)) task_dump(MEM32(node + 0x28), depth + 1, budget);
        node = MEM32(node + 0x30);
    }
}

static void task_report(void)
{
    uint32_t mgr = MEM32(0x0022FCE0u);
    int budget = 120;
    if (!mgr) return;
    fprintf(stderr, "[TASK] tree (root 0x%08X, %u nodes counted by the game); fade task id 6 = 0x%08X, updates since start %u\n",
            MEM32(mgr + 0x87DC), MEM32(mgr + 0x87E8), MEM32(mgr + 0x98 + 6 * 4), fade_calls);
    fprintf(stderr, "[TASK] manager pause flags +0x40..+0x4C: %u %u %u %u\n",
            MEM32(mgr + 0x40), MEM32(mgr + 0x44), MEM32(mgr + 0x48), MEM32(mgr + 0x4C));
    task_dump(MEM32(mgr + 0x87DC), 0, &budget);
}

/* ── CRI ADX file-layer trace ─────────────────────────────────
 *
 * Run 22: after loading, the title music (title.adx) is read 240 times in
 * a row, always 25 sectors at offset 0, and then the game writes
 * Media\Cache\JSRF_FATAL.ERR. CRI's file layer (wxCi*, Ver.7.49) reports
 * problems through a callback at [0x2615C4], installed by 0x140230
 * (setter, cdecl: func, ctx), and called as func(ctx, message, arg).
 *
 * The setter is wrapped so that it installs *itself* as the callback: a call
 * whose second argument is a message string is CRI reporting an error, which
 * is printed and then passed on to the game's own callback. Read requests
 * (0x1403B0) and completions (0x1401B0) are logged for the first few.
 * Lines start with [CRI]. */
extern void sub_00140230_gen(void);
extern void sub_001403B0_gen(void);
extern void sub_001401B0_gen(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static uint32_t cri_real_cb, cri_msgs;

/* A report is func(ctx, message, arg); the setter is set(func, ctx). CRI
 * formats most messages into a buffer in .data first (0x273640), so a report
 * is recognised by its second argument being readable text, and a setter by
 * its first being code (or 0) while the second is not text. */
static int cri_is_text(uint32_t p)
{
    int i;
    if (p < 0x00011000u || p >= 0x08000000u) return 0;
    for (i = 0; i < 4; i++) {
        uint8_t c = MEM8(p + i);
        if (c < 0x20 || c > 0x7E) return 0;
    }
    return 1;
}

static int cri_is_message(uint32_t a0, uint32_t a1)
{
    int a0_code = a0 == 0 || (a0 >= 0x00011000u && a0 < 0x001C3F60u);
    return cri_is_text(a1) || !a0_code;
}

static void cri_copy(char *out, uint32_t p)
{
    int i = 0;
    if (cri_is_text(p))
        for (; i < 159 && MEM8(p + i); i++) out[i] = (char)MEM8(p + i);
    out[i] = 0;
}

void sub_00140230(void)
{
    uint32_t a0 = MEM32(g_esp + 4), a1 = MEM32(g_esp + 8), a2 = MEM32(g_esp + 12);
    if (a0 != 0x00140230u && cri_is_message(a0, a1)) {   /* CRI reporting */
        if (cri_msgs++ < 60) {
            char m0[160], m1[160];
            cri_copy(m0, a0); cri_copy(m1, a1);
            fprintf(stderr, "[CRI] report: \"%s\" (args 0x%08X 0x%08X 0x%08X%s%s%s)\n",
                    m1, a0, a1, a2, m0[0] ? ", first arg \"" : "", m0, m0[0] ? "\"" : "");
        }
        if (cri_real_cb) {
            recomp_func_t f = recomp_lookup(cri_real_cb);
            if (f) { f(); return; }              /* same stack, cdecl: caller cleans */
        }
        g_esp += 4;                              /* plain ret */
        return;
    }
    if (a0 == 0x00140230u) {                     /* our own redirect being re-set */
        sub_00140230_gen();
        return;
    }
    fprintf(stderr, "[CRI] error callback set to 0x%08X (ctx 0x%08X)\n", a0, a1);
    cri_real_cb = a0;
    sub_00140230_gen();                          /* stores [0x2615C4]/[0x2615C8] */
    MEM32(0x002615C4u) = 0x00140230u;            /* route reports through here */
}

static uint32_t cri_reqs, cri_dones;

/* Run 53: after the tutorial the event (e204) loads and the screen stays
 * black. CRI's file layer refuses every read of the DJ voice file because
 * another file is still "reading": a file leaves state 2 only in the file
 * server (0x140BA0), and the server seems not to run again. Every 5 s while
 * the loading-screen task sits still: the CRI thread counters (vsync loop
 * 0x25EFAC, file thread 0x25EFB0), the per-level server run counters
 * (0x2728C0), the server's busy flag (0x2615D8) and lock hook (0x2615E8),
 * and every open file: state, read-issue and pending flags, sector. */
/* Run 54: the file server's level-5 slots are empty, so it must run from the
 * ADX server (0x141F10 -> 0x13A9E0, registered at level 2 and called by the
 * vsync thread through [0x261638]). Count both, and how often 0x141F10 found
 * its re-entry guard [0x26173c] already set and did nothing. */
static volatile long cri_adx_runs, cri_adx_skips, cri_fs_runs;
/* the generated bodies these wrap (declared here so a fresh lift's
 * --exclude-manual scan sees them as wrapped, not replaced) */
extern void sub_00141F10_gen(void);
extern void sub_00140C30_gen(void);
static void bgm_buffer_watch(void);
void sub_00141F10(void)
{
    if (MEM32(0x0026173Cu))
        cri_adx_skips++;
    else
        cri_adx_runs++;
    sub_00141F10_gen();
    bgm_buffer_watch();
}

/* v71: the music's decoded-sample buffer, sampled after every ADX server
 * run (the vsync thread). Run 70: the title music plays right for a few
 * seconds and then crackles, while the voice playing it reads its loop
 * buffer at exactly 44.1 kHz and never runs dry. So either the decoder falls
 * behind (this buffer empties) or the decoded samples reach the loop buffer
 * wrongly (it stays full). Every 2 s: min/avg/max samples buffered, the
 * ADXT status, and server runs. Off with RECOMP_QUIET. */
extern void sub_0013A9A0(void);
static void bgm_buffer_watch(void)
{
    static int quiet = -1;
    static DWORD last;
    static long n, sum, mn = 0x7FFFFFFF, mx = -1, zero;
    static uint32_t last_adxt;
    uint32_t mgr, snd, cur, adxt, save_eax, save_esp;
    int samples;
    DWORD now;
    if (quiet < 0) {
        /* v173b: RECOMP_AUDIO_REPORT=1 keeps this on under RECOMP_QUIET, so
         * the music can be watched with the GPU survey off (closer to how
         * the game runs when played) */
        const char *ar = getenv("RECOMP_AUDIO_REPORT");
        quiet = getenv("RECOMP_QUIET") != NULL && !(ar && ar[0] == '1');
    }
    if (quiet)
        return;
    {
        extern void mcpx_apu_music_watch(void);   /* v174: where the new music went */
        mcpx_apu_music_watch();
    }
    mgr = MEM32(0x0022FCE0u);
    snd = mgr ? MEM32(mgr + 0x98 + 5 * 4) : 0;
    if (!snd || snd >= 0x08000000u)
        return;
    cur = MEM32(snd + 0x78);
    adxt = cur < 4 ? MEM32(snd + 0x68 + 4 * cur) : 0;
    if (!adxt || adxt >= 0x08000000u || !MEM32(adxt + 0x18))
        return;
    save_eax = g_eax; save_esp = g_esp;
    g_esp -= 4; MEM32(g_esp) = 0;                  /* channel 0 */
    g_esp -= 4; MEM32(g_esp) = adxt;
    g_esp -= 4; MEM32(g_esp) = 0;                  /* return address */
    sub_0013A9A0();
    g_esp = save_esp;
    samples = (int)g_eax;
    g_eax = save_eax;
    if (adxt != last_adxt) { n = sum = zero = 0; mn = 0x7FFFFFFF; mx = -1; last_adxt = adxt; }
    n++; sum += samples;
    if (samples < mn) mn = samples;
    if (samples > mx) mx = samples;
    if (samples <= 0) zero++;
    now = GetTickCount();
    if (!last) last = now;
    if (now - last >= 2000 && n) {
        fprintf(stderr, "[BGM] music buffer (ADXT 0x%08X, status %d, slot %u): %ld server runs,"
                " samples min %ld avg %ld max %ld, empty %ld times\n",
                adxt, (int)(int8_t)MEM8(adxt + 1), MEM32(snd + 0x78), n, mn, sum / n, mx, zero);
        last = now; n = sum = zero = 0; mn = 0x7FFFFFFF; mx = -1;
    }
}

void sub_00140C30(void)
{
    cri_fs_runs++;
    sub_00140C30_gen();
}

static void cri_report(void)
{
    uint32_t f, k;
    fprintf(stderr, "[CRI] ADX server: %ld runs, %ld skipped (guard 0x26173C = %u); wxCi file"
                    " server %ld runs; level-2 slots 0x%08X/0x%08X 0x%08X/0x%08X\n",
            cri_adx_runs, cri_adx_skips, MEM32(0x0026173Cu), cri_fs_runs,
            MEM32(0x00261638u), MEM32(0x0026163Cu), MEM32(0x00261640u), MEM32(0x00261644u));
    fprintf(stderr, "[CRI] threads: vsync loop %u, file loop %u; server runs",
            MEM32(0x0025EFACu), MEM32(0x0025EFB0u));
    for (k = 0; k < 7; k++)
        fprintf(stderr, " %u", MEM32(0x002728C0u + 4 * k));
    fprintf(stderr, "; file-server busy %u, lock 0x%08X/0x%08X, level-5 servers",
            MEM32(0x002615D8u), MEM32(0x002615E8u), MEM32(0x002615F0u));
    for (k = 0; k < 4; k++)
        fprintf(stderr, " 0x%08X", MEM32(0x002615F8u + 5 * 32 + 8 * k));
    fputc('\n', stderr);
    for (f = 0x00273780u; f < 0x00276C00u; f += 0x150)
        if (MEM8(f))
            fprintf(stderr, "[CRI]   file 0x%08X: state %u issue %u pending %u sector %u/%u"
                            " last read %u sectors\n", f, MEM8(f + 1), MEM32(f + 0x148),
                    MEM32(f + 0x14C), MEM32(f + 0x18), MEM32(f + 0x14), MEM32(f + 0x20));
}

void sub_001403B0(void)                          /* wxCiReqRd(file, nsct, buf), cdecl */
{
    uint32_t f = MEM32(g_esp + 4), n = MEM32(g_esp + 8), buf = MEM32(g_esp + 12);
    uint32_t pos = f ? MEM32(f + 0x18) : 0, ssz = f ? MEM32(f + 0x0C) : 0;
    sub_001403B0_gen();
    if (cri_reqs++ < 30)
        fprintf(stderr, "[CRI] read request file 0x%08X: %d sectors at sector %u (size %u, file %u sectors) buf 0x%08X -> %u, state %u\n",
                f, (int)n, pos, ssz, f ? MEM32(f + 0x14) : 0, buf, g_eax, f ? MEM8(f + 1) : 0);
}

void sub_001401B0(void)                          /* ReadFileEx completion, stdcall 3 */
{
    uint32_t err = MEM32(g_esp + 4), bytes = MEM32(g_esp + 8), ovl = MEM32(g_esp + 12);
    uint32_t f = ovl ? MEM32(ovl + 0x10) : 0;
    uint32_t in_server = MEM32(0x002615D8u);
    sub_001401B0_gen();
    if (cri_dones++ < 30)
        fprintf(stderr, "[CRI] read complete file 0x%08X: err %u, %u bytes; now sector %u, state %u%s\n",
                f, err, bytes, f ? MEM32(f + 0x18) : 0, f ? MEM8(f + 1) : 0,
                in_server ? " (inside the file server)" : " (outside the file server)");
    /* Run 56: even with completions deferred to the next alertable wait, the
     * file stayed in state 2 with nothing pending. The server only finishes a
     * read it saw pending and then completed inside its own SleepEx; if the
     * completion lands anywhere else nothing ever finishes it. When that
     * happens, finish it the way the server would (0x140C0F): the bytes
     * read, the sector advanced, state back to 1. Inside the server it is
     * left to the server. RECOMP_CRI_FINISH=0 disables this. */
    {
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("RECOMP_CRI_FINISH");
            on = !(e && !strcmp(e, "0"));
        }
        if (on && f && !err && !in_server && MEM8(f + 1) == 2 && MEM32(f + 0x14C) == 0) {
            static int said;
            MEM32(f + 0x1C) = MEM32(f + 0x20) * MEM32(f + 0x0C);
            MEM32(f + 0x18) = MEM32(f + 0x18) + MEM32(f + 0x20);
            MEM8(f + 1) = 1;
            if (said++ < 8)
                fprintf(stderr, "[CRI] finished read of file 0x%08X outside the server: now sector %u\n",
                        f, MEM32(f + 0x18));
        }
    }
}

/* ── The game's idle thread ───────────────────────────────────
 *
 * 0x13B180 is a thread whose whole job is `while (!stop) idle_count++;`.
 * On the console it runs at the lowest priority and soaks up spare CPU
 * time; here it is a real host thread spinning flat out on its own core.
 * This version counts and sleeps instead. The count only feeds a CPU-load
 * meter, which will now read as "busy". RECOMP_IDLE_SPIN=1 restores the
 * original spin. */
extern void sub_0013B180_gen(void);
extern void XAPILIB_ExitThread(void);    /* 0x147E4E */

void sub_0013B180(void)
{
    if (getenv("RECOMP_IDLE_SPIN")) { sub_0013B180_gen(); return; }
    while (!MEM32(0x0025EFC0u)) {
        MEM32(0x0025EFA8u) = MEM32(0x0025EFA8u) + 1;
        Sleep(1);
    }
    /* Tail of the original: push 0xF0000001; [0x25EFC4] = 1; call 0x147E4E
     * (thread exit, does not return). */
    g_esp -= 4; MEM32(g_esp) = 0xF0000001u;
    MEM32(0x0025EFC4u) = 1;
    g_esp -= 4; MEM32(g_esp) = 0x0013B1B8u;
    XAPILIB_ExitThread();
}

/* ── Boot/title sequence trace ────────────────────────────────
 *
 * Run 24: no disc error any more, but the game stays on the loading screen
 * (still rendering). The boot task (id 0, update 0x7BDD0) runs one handler
 * per frame from the table at 0x20D2B8, indexed by its state [this+0x48]:
 * state 6 starts the logo load, 8 waits for the logo task, and so on. This
 * logs every state change, the handler that runs in the new state, and
 * every 5 s the state it is sitting in. Lines start with [BOOT]. */
extern void sub_0007BDD0_gen(void);

static uint32_t boot_state = 0xFFFFFFFFu;
static DWORD    boot_since, boot_report;

void sub_0007BDD0(void)
{
    uint32_t self = g_ecx;
    sub_0007BDD0_gen();
    {
        uint32_t st = MEM32(self + 0x48);
        DWORD now = GetTickCount();
        if (st != boot_state) {
            fprintf(stderr, "[BOOT] state %d -> %u (handler 0x%08X) after %lu ms\n",
                    (int)boot_state, st, st < 0x40 ? MEM32(0x0020D2B8u + 4 * st) : 0,
                    boot_state == 0xFFFFFFFFu ? 0 : now - boot_since);
            boot_state = st; boot_since = boot_report = now;
        } else if (now - boot_report > 5000) {
            boot_report = now;
            fprintf(stderr, "[BOOT] still in state %u (handler 0x%08X) for %lu ms\n",
                    st, st < 0x40 ? MEM32(0x0020D2B8u + 4 * st) : 0, now - boot_since);
        }
    }
}

/* ── Loading-screen task trace ────────────────────────────────
 *
 * Run 25: the boot sequence waits in state 12 for task id 8 to end. Task 8
 * is the loading screen (vtable 0x1CAAF8, update 0x4EF90), itself a state
 * machine on [this+0x44] (table 0x1FA008, 21 states) that waits on tasks 9
 * and 10 and on game settings ([mgr+0x7868+4*n], read by 0x127E0). This logs
 * its state changes and, when it sits still, which of those it is looking
 * at, plus one dump of the task tree. Lines start with [LOADSCR]. */
extern void sub_0004EF90_gen(void);

static uint32_t ls_state = 0xFFFFFFFFu, ls_dumped;
static DWORD    ls_since, ls_report;

void sub_0004EF90(void)
{
    uint32_t self = g_ecx;
    sub_0004EF90_gen();
    {
        uint32_t st = MEM32(self + 0x44), mgr = MEM32(0x0022FCE0u);
        DWORD now = GetTickCount();
        if (st != ls_state) {
            fprintf(stderr, "[LOADSCR] state %d -> %u (handler 0x%08X) after %lu ms\n",
                    (int)ls_state, st, st < 0x15 ? MEM32(0x001FA008u + 4 * st) : 0,
                    ls_state == 0xFFFFFFFFu ? 0 : now - ls_since);
            ls_state = st; ls_since = ls_report = now;
        } else if (now - ls_report > 5000 && mgr) {
            uint32_t t9 = MEM32(mgr + 0x98 + 9 * 4), t10 = MEM32(mgr + 0x98 + 10 * 4);
            ls_report = now;
            fprintf(stderr, "[LOADSCR] still state %u (handler 0x%08X) %lu ms; task9 0x%08X task10 0x%08X (state +0x48 %u); "
                            "settings 2=%d 3=%d 4=%d 10=%d 0x1cb=%d\n",
                    st, st < 0x15 ? MEM32(0x001FA008u + 4 * st) : 0, now - ls_since,
                    t9, t10, t10 ? MEM32(t10 + 0x48) : 0,
                    (int)MEM32(mgr + 0x7868 + 2 * 4), (int)MEM32(mgr + 0x7868 + 3 * 4),
                    (int)MEM32(mgr + 0x7868 + 4 * 4), (int)MEM32(mgr + 0x7868 + 10 * 4),
                    (int)MEM32(mgr + 0x7868 + 0x1cb * 4));
            if (!ls_dumped++) task_report();
            cri_report();
        }
    }
}

/* ── Title BGM gate ───────────────────────────────────────────
 *
 * Run 26: the loading screen (task 8) waits in its last state for task 2,
 * the title/attract sequence (update 0x7B8D0). Task 2 starts by waiting for
 * the music: 0x1167F0 returns the BGM state of the sound task (id 5,
 * +0xBC), and task 2 waits for it to read 2 ("ready"). The sound task sets
 * 2 once the current ADXT stream has 0x2000 samples buffered (0x13A9A0).
 * The music never buffers -- CRI's file layer reads the first 25 sectors
 * of title.adx and the decoder does not consume them, most likely because
 * the emulated APU never advances a voice (no APU interrupt since run 11).
 *
 * This logs the ADXT state while the gate is closed and, after
 * RECOMP_BGM_WAIT_MS (default 4000 ms; 0 = wait forever, the original
 * behaviour), reports the music as ready so the game carries on without
 * it. The game then runs silent, which is what it would do on a console
 * with no sound either. Lines start with [BGM]. */
extern void sub_001167F0_gen(void);
extern void sub_0013A9A0(void);

static DWORD bgm_wait_since, bgm_report;
static int   bgm_bypassed;

static int bgm_wait_ms(void)
{
    static int v = -2;
    if (v == -2) {
        const char *e = getenv("RECOMP_BGM_WAIT_MS");
        v = e ? atoi(e) : 4000;
    }
    return v;
}

void sub_001167F0(void)
{
    sub_001167F0_gen();
    {
        uint32_t st = g_eax, mgr = MEM32(0x0022FCE0u);
        uint32_t snd = mgr ? MEM32(mgr + 0x98 + 5 * 4) : 0;
        DWORD now = GetTickCount();
        if (st == 2 || !snd) { bgm_wait_since = 0; return; }
        if (!bgm_wait_since) bgm_wait_since = bgm_report = now;
        if (now - bgm_report > 2000 || now == bgm_wait_since) {
            uint32_t cur = MEM32(snd + 0x78);
            uint32_t adxt = cur < 4 ? MEM32(snd + 0x68 + 4 * cur) : 0;
            int samples = -1;
            uint32_t save_eax = g_eax;
            if (adxt && adxt < 0x08000000u) {
                g_esp -= 4; MEM32(g_esp) = 0;          /* channel 0 */
                g_esp -= 4; MEM32(g_esp) = adxt;
                g_esp -= 4; MEM32(g_esp) = 0;          /* return address */
                sub_0013A9A0();
                g_esp += 8;
                samples = (int)g_eax;
            }
            g_eax = save_eax;
            bgm_report = now;
            fprintf(stderr, "[BGM] waiting for the music: BGM state %u, ADXT 0x%08X status %d, %d samples buffered (needs 8192), %lu ms\n",
                    st, adxt, adxt && adxt < 0x08000000u ? (int)(int8_t)MEM8(adxt + 1) : -1,
                    samples, now - bgm_wait_since);
        }
        if (st == 1 && bgm_wait_ms() > 0 && (int)(now - bgm_wait_since) >= bgm_wait_ms()) {
            if (!bgm_bypassed++)
                fprintf(stderr, "[BGM] music never became ready after %d ms; reporting it ready so the game "
                                "continues without it (RECOMP_BGM_WAIT_MS=0 disables this)\n", bgm_wait_ms());
            g_eax = 2;
        }
    }
}

/* The title sequence's other gate (when its +0x54 flag is set): task 9 must
 * reach state 2 (0x4EA30). Logged so a closed gate here is visible too. */
extern void sub_0004EA30_gen(void);
static DWORD t9_report;

void sub_0004EA30(void)
{
    sub_0004EA30_gen();
    if (!g_eax && GetTickCount() - t9_report > 2000) {
        uint32_t mgr = MEM32(0x0022FCE0u), t9 = mgr ? MEM32(mgr + 0x98 + 9 * 4) : 0;
        t9_report = GetTickCount();
        fprintf(stderr, "[BGM] title waits for task 9 (0x%08X, update 0x%08X) to reach state 2: state %u\n",
                t9, t9 ? MEM32(MEM32(t9) + 4) : 0, t9 ? MEM32(t9 + 0x48) : 0);
    }
}

/* ── Controller trace ─────────────────────────────────────────
 *
 * Run 28: the PRESS START screen. The keyboard reaches the emulated pad
 * ([INPUT] ... buttons=0x0010 while Enter is held), yet the game does not
 * react. These wrap XAPI's device enumeration and XInputGetState so the log
 * shows whether the game ever finds the pad, opens it, and what it reads.
 * Lines start with [XINPUT]. */
extern void sub_001BD5FF_gen(void);   /* XGetDevices(type)                       */
extern void sub_001BD621_gen(void);   /* XGetDeviceChanges(type, *ins, *rem)     */
extern void sub_001C3BA1_gen(void);   /* XInputOpen(type, port, slot, poll)      */
extern void sub_001C3E14_gen(void);   /* XInputGetState(h, *state)               */

static unsigned xi_gd, xi_gdc, xi_open, xi_get;
static uint32_t xi_last_buttons = 0xFFFFFFFFu;

void sub_001BD5FF(void)
{
    uint32_t type = MEM32(g_esp + 4);
    sub_001BD5FF_gen();
    if (xi_gd++ < 10)
        fprintf(stderr, "[XINPUT] XGetDevices(type 0x%08X) -> 0x%08X\n", type, g_eax);
}

void sub_001BD621(void)
{
    uint32_t type = MEM32(g_esp + 4), pi = MEM32(g_esp + 8), pr = MEM32(g_esp + 12);
    sub_001BD621_gen();
    if (pi && pr && (MEM32(pi) || MEM32(pr) || xi_gdc < 5)) {
        if (xi_gdc++ < 20)
            fprintf(stderr, "[XINPUT] XGetDeviceChanges(type 0x%08X) -> %u, inserted 0x%08X removed 0x%08X\n",
                    type, g_eax, MEM32(pi), MEM32(pr));
    }
}

void sub_001C3BA1(void)
{
    uint32_t type = MEM32(g_esp + 4), port = MEM32(g_esp + 8), slot = MEM32(g_esp + 12);
    uint32_t pp = MEM32(g_esp + 16);
    sub_001C3BA1_gen();
    if (xi_open++ < 20)
        fprintf(stderr, "[XINPUT] XInputOpen(type 0x%08X, port %u, slot %u, poll %s: bytes %02X %02X %02X %02X) -> handle 0x%08X\n",
                type, port, slot, pp ? "given" : "NULL (auto-poll)",
                pp ? MEM8(pp) : 0, pp ? MEM8(pp + 1) : 0, pp ? MEM8(pp + 2) : 0, pp ? MEM8(pp + 3) : 0,
                g_eax);
}

/* XInputPoll(h): with auto-poll off the title must call this to queue the
 * next interrupt transfer; counted here. */
extern void sub_001C3EBD_gen(void);
static unsigned xi_poll;
void sub_001C3EBD(void) { xi_poll++; sub_001C3EBD_gen(); }
extern unsigned xbox_OhciReportCount(void);
static DWORD xi_report;

/* XID report completion (thiscall, ecx = device block). Copies the report
 * from the URB buffer at dev+0x34 (past its 2-byte header) to dev+0x14,
 * which XInputGetState reads. Run 29 had packets advancing and buttons
 * stuck at zero; this shows what actually arrived in the buffer the URB
 * names. Logs the first few completions and every one whose report has a
 * button or trigger set. */
extern void sub_001BDE21_gen(void);
static unsigned xi_xid_n, xi_xid_said;
void sub_001BDE21(void)
{
    uint32_t dev = g_ecx;
    uint32_t len = MEM32(dev + 0x66), buf = MEM32(dev + 0x6a);
    int any = 0, i;
    for (i = 2; i < 12; i++) any |= MEM8(dev + 0x32 + i);
    xi_xid_n++;
    if ((xi_xid_n <= 3 || any) && xi_xid_said < 40) {
        xi_xid_said++;
        fprintf(stderr, "[XINPUT] report #%u dev 0x%08X urb buf 0x%08X len %u: "
                        "%02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X\n",
                xi_xid_n, dev, buf, len,
                MEM8(dev + 0x32), MEM8(dev + 0x33), MEM8(dev + 0x34), MEM8(dev + 0x35),
                MEM8(dev + 0x36), MEM8(dev + 0x37), MEM8(dev + 0x38), MEM8(dev + 0x39),
                MEM8(dev + 0x3a), MEM8(dev + 0x3b));
    }
    sub_001BDE21_gen();
}

void sub_001C3E14(void)
{
    uint32_t h = MEM32(g_esp + 4), ps = MEM32(g_esp + 8);
    sub_001C3E14_gen();
    xi_get++;
    if (ps && GetTickCount() - xi_report > 3000) {
        xi_report = GetTickCount();
        fprintf(stderr, "[XINPUT] %u XInputGetState calls, %u XInputPoll calls, %u USB reports delivered; "
                        "last: rc %u packet %u buttons 0x%04X\n",
                xi_get, xi_poll, xbox_OhciReportCount(), g_eax, MEM32(ps), MEM16(ps + 4));
    }
    if (ps) {
        uint32_t b = MEM16(ps + 4) | ((uint32_t)MEM8(ps + 6) << 16);   /* wButtons, A */
        if (b != xi_last_buttons || xi_get < 4) {
            xi_last_buttons = b;
            fprintf(stderr, "[XINPUT] XInputGetState(0x%08X) #%u -> %u, packet %u buttons 0x%04X A=%u\n",
                    h, xi_get, g_eax, MEM32(ps), MEM16(ps + 4), MEM8(ps + 6));
        }
    }
}

/* ── v118b/c: the in-air crash (sub_00039D40 reading 0x78000000) ─────────
 *
 * sub_00039D40(list, count) asks the flag object at 0x1EFFB0 whether every
 * flag in a list has its wanted value (sub_00039B50 per element; the loop
 * stops at the first that does not). Callers pass table entries that are not
 * always real lists -- count 0x017BE784 with a NULL list is normal, and the
 * first element decides -- so the answer must stay exactly the original's
 * (v118b answered "no" to those and the game skipped loading its assets:
 * everything 3D black). Only what the console could never survive is changed:
 * an element outside guest memory, which on the console is a page fault and
 * here was a walk from 0x013F3F07 to 0x78000000 mid-jump. The loop is run
 * here, element by element, as the original runs it; reaching an unmapped
 * element, or 65536 elements all passing, answers "no" and is logged.
 * A list wholly inside memory goes straight to the generated body.
 * RECOMP_INPUT_GUARD=0 turns the check off. */
extern void sub_00039D40_gen(void);
extern void sub_00039B50(void);
extern size_t xbox_GetMappedSize(void);

static int guest_range_ok(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    if (end <= xbox_GetMappedSize())
        return 1;
    return va >= 0x80000000u && end <= 0x84000000ull;
}

void sub_00039D40(void)
{
    static int on = -1;
    /* sub_00039D40(list, count): `mov ebx,[esp+0xC]` after one push is the
     * second argument (the count), `mov ebp,[esp+0x14]` after four the first
     * (the list). v118b/d read them the other way round. */
    uint32_t list = MEM32(g_esp + 4), count = MEM32(g_esp + 8);
    uint32_t self = g_ecx, i;
    if (on < 0) {
        const char *e = getenv("RECOMP_INPUT_GUARD");
        on = !(e && e[0] == '0');
    }
    if (!on || (count < 0x40000000u && guest_range_ok(list, count * 4u))) {
        sub_00039D40_gen();
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t a = list + i * 4u;
        if (!guest_range_ok(a, 4) || i >= 65536u) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "[INPUT] flag list ran %s after %u of %u elements: list"
                        " 0x%08X (caller ret %08X) -- answered no\n",
                        i >= 65536u ? "on" : "out of memory", i, count, list, MEM32(g_esp));
                fflush(stderr);
            }
            g_eax = 0;
            g_esp += 12;                       /* ret 8 */
            return;
        }
        g_esp -= 4; MEM32(g_esp) = MEM32(a);   /* push the element */
        g_esp -= 4; MEM32(g_esp) = 0x00039D60u;
        g_ecx = self;
        sub_00039B50();                        /* ret 4: pops both */
        if (!g_eax) {
            g_esp += 12;
            return;                            /* eax 0 */
        }
    }
    g_eax = 1;
    g_esp += 12;
}

/* ── v124: who sets a given texture (the brake sparks) ───────────────────
 *
 * RECOMP_TEX_WHO=<physical address> (default 0x0106F000, the 64x64 spark
 * sprite of run 121): each D3DDevice_SetTexture(stage, tex) whose texture
 * data is there logs the time (same clock as [STICK]/[FX]) and the guest
 * call chain -- return addresses on the stack that follow a call -- so the
 * code that draws, and then spawns, the sparks can be found. The first 24
 * calls in full, then one line a second with the count. RECOMP_TEX_WHO=0
 * turns it off. */
extern void D3D8_D3DDevice_SetTexture_gen(void);

void D3D8_D3DDevice_SetTexture(void)
{
    static int init;
    static uint32_t want;
    uint32_t stage = MEM32(g_esp + 4), tex = MEM32(g_esp + 8);
    if (!init) {
        const char *e = getenv("RECOMP_TEX_WHO");
        init = 1;
        want = e ? (uint32_t)strtoul(e, NULL, 0) : 0x0106F000u;
    }
    if (want && tex && guest_range_ok(tex, 8) && (MEM32(tex + 4) & 0x7FFFFFFFu) == want) {
        static unsigned n, per_sec;
        static unsigned long long last_ms;
        unsigned long long now = (unsigned long long)GetTickCount64();
        per_sec++;
        if (n < 24) {
            uint32_t k, shown = 0;
            n++;
            fprintf(stderr, "[TEXWHO] t=%llu ms stage %u tex %08X data %08X; calls:", now,
                    stage, tex, MEM32(tex + 4));
            for (k = 0; k < 96 && shown < 12; k++) {
                uint32_t v = MEM32(g_esp + 4 * k);
                if (v >= 0x00011005u && v < 0x0018C000u && MEM8(v - 5) == 0xE8) {
                    fprintf(stderr, " %08X", v);
                    shown++;
                }
            }
            fputc('\n', stderr);
            fflush(stderr);
        } else if (now - last_ms >= 1000) {
            fprintf(stderr, "[TEXWHO] t=%llu ms: %u sets in the last second\n", now, per_sec);
            per_sec = 0;
            last_ms = now;
        }
    }
    D3D8_D3DDevice_SetTexture_gen();
}

/* ── v130: the DirectSound idle-voice crash (sub_001A2E2E, list Blink 0) ──
 *
 * The APU interrupt handles a trapped SE2FE_IDLE_VOICE (method 0x8000) inline:
 * sub_001A200D(voice) looks up the voice's buffer object and, if the object's
 * list at +0x4C is not empty, calls sub_001A2E2E on it, which follows the
 * list's Blink (+0x50) and Flink (+0x4C) to the entries 0x4C below them.
 * Twice now (after "load game", and run 129 at a state change) Flink was set
 * and Blink was 0: an object being built or torn down while the interrupt
 * ran. Here such an object is skipped (the idle notification for it is
 * dropped) instead of reading 0xFFFFFFBE. RECOMP_DSOUND_GUARD=0 turns it off. */
extern void sub_001A2E2E_gen(void);
static int dsound_link_ok(uint32_t p)
{
    return p >= 0x1000u && guest_range_ok(p - 0x4Cu, 0x60u);
}
void sub_001A2E2E(void)
{
    static int on = -1;
    uint32_t obj = g_ecx;
    if (on < 0) {
        const char *e = getenv("RECOMP_DSOUND_GUARD");
        on = !(e && e[0] == '0');
    }
    if (on && (!guest_range_ok(obj, 0x70u) || !dsound_link_ok(MEM32(obj + 0x4Cu))
               || !dsound_link_ok(MEM32(obj + 0x50u)))) {
        static unsigned n;
        if (n++ < 8) {
            fprintf(stderr, "[DSOUND] idle voice: buffer object 0x%08X has a broken list"
                    " (Flink %08X Blink %08X) -- skipped\n", obj,
                    guest_range_ok(obj, 0x70u) ? MEM32(obj + 0x4Cu) : 0,
                    guest_range_ok(obj, 0x70u) ? MEM32(obj + 0x50u) : 0);
            fflush(stderr);
        }
        g_esp += 4;                            /* plain ret */
        return;
    }
    sub_001A2E2E_gen();
}

/* ── v125/v126: rcpss / rsqrtss with the console's precision ─────────────
 *
 * The brake sparks and smoke are drawn at a fixed spot in the map instead of
 * following the skates, here and in xemu, but not on the console or in
 * Cxbx-Reloaded. The lifter (like QEMU) computed rcpss/rsqrtss exactly; the
 * console's CPU returns a 12-bit approximation. v125 used the host's own
 * instructions: that fixed the sparks but broke geometry on an AMD host, whose
 * approximation tables are not Intel's. v126 reproduces the Pentium III's
 * results bit for bit (sse_p3.h) on any host.
 *
 * wrap_dsound.py rewrites the 27 sites to recomp_rcpss_at / recomp_rsqrtss_at
 * with a site number. Settings:
 *   default: Pentium III values at site 4 (sub_0014C410) only, exact elsewhere
 *   RECOMP_SSE=p3 | host | exact  every site    (RECOMP_SSE_EXACT=1 = exact)
 *   RECOMP_SSE_EXACT_SITES=3,7,20   these sites exact, the rest per RECOMP_SSE
 *   RECOMP_SSE_ONLY_SITES=3,7,20    only these sites approximate, the rest exact
 */
#include <xmmintrin.h>
#include <string.h>
#include "sse_p3.h"
#define SSE_SITES 64
enum { SSE_P3, SSE_HOST, SSE_EXACT };
static signed char s_sse_mode[SSE_SITES];
static int s_sse_default = -1;
static volatile long s_sse_init;

static void sse_list(const char *e, int mode)
{
    while (e && *e) {
        char *end;
        long id = strtol(e, &end, 10);
        if (end == e) { e++; continue; }
        if (id >= 0 && id < SSE_SITES) s_sse_mode[id] = (signed char)mode;
        e = end;
    }
}

static void sse_setup(void)
{
    const char *m = getenv("RECOMP_SSE"), *x = getenv("RECOMP_SSE_EXACT");
    const char *ex = getenv("RECOMP_SSE_EXACT_SITES"), *on = getenv("RECOMP_SSE_ONLY_SITES");
    int d = SSE_P3, i;
    static const char *nm[] = { "console (Pentium III tables)", "host instructions", "exact" };
    if (m && !strcmp(m, "host")) d = SSE_HOST;
    else if (m && !strcmp(m, "exact")) d = SSE_EXACT;
    if (x && x[0] == '1') d = SSE_EXACT;
    /* v127 default (no RECOMP_SSE*): console values at site 4 only
     * (sub_0014C410, the one the brake sparks need), exact everywhere else.
     * Approximating all 27 broke geometry and shading. RECOMP_SSE=p3 gives
     * console values at every site. */
    if (!m && !x && !on && !ex) { d = SSE_EXACT; on = "4"; }
    for (i = 0; i < SSE_SITES; i++) s_sse_mode[i] = (signed char)(on ? SSE_EXACT : d);
    if (on) sse_list(on, d == SSE_EXACT ? SSE_P3 : d);
    if (ex) sse_list(ex, SSE_EXACT);
    fprintf(stderr, "[SSE] rcpss/rsqrtss: %s%s%s%s%s\n", nm[d],
            on ? ", approximate only at sites " : "", on ? on : "",
            ex ? ", exact at sites " : "", ex ? ex : "");
    s_sse_default = d;
}

static inline int sse_mode(int id)
{
    if (s_sse_default < 0) {
        /* first call; one thread wins, the others wait for it */
        if (InterlockedCompareExchange(&s_sse_init, 1, 0) == 0) { sse_setup(); s_sse_init = 2; }
        while (s_sse_init != 2) { }
    }
    return (unsigned)id < SSE_SITES ? s_sse_mode[id] : s_sse_default;
}

static inline float sse_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t sse_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

float recomp_rcpss_at(float x, int id)
{
    switch (sse_mode(id)) {
    case SSE_EXACT: return 1.0f / x;
    case SSE_HOST:  return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
    default:        return sse_f(p3_rcp_bits(sse_u(x)));
    }
}
float recomp_rsqrtss_at(float x, int id)
{
    switch (sse_mode(id)) {
    case SSE_EXACT: return 1.0f / sqrtf(x);
    case SSE_HOST:  return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
    default:        return sse_f(p3_rsq_bits(sse_u(x)));
    }
}
/* v125 names, for a gen tree that has not been through wrap_dsound.py again */
float recomp_rcpss(float x) { return recomp_rcpss_at(x, -1); }
float recomp_rsqrtss(float x) { return recomp_rsqrtss_at(x, -1); }
