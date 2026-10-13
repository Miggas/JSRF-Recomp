/**
 * Jet Set Radio Future - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details:
 *   Title:       Jet Set Radio Future (retail, SE-010, region 0x7)
 *   Title ID:    0x5345000A
 *   XDK:         4134 (D3D8 qfe2, DSOUND/XAPI qfe1)
 *   Base addr:   0x00010000
 *   Entry point: 0x00148023 (XAPILIB mainCRTStartup)
 *   Code size:   ~1519 KB (.text)
 *   Sections:    11 (.text D3D DSOUND MMATRIX XGRPH XPP .rdata .data DOLBY $$XTIMAGE $$XSIMAGE)
 *   Kernel imports: 120
 */

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>   /* v140: SHFileOperationA */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00148023  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── v139: portable layout and jsrf.ini ─────────────────────
 *
 * The exe can sit in the game folder itself, next to default.xbe (what
 * build_portable.bat sets up): it then runs from there, whatever folder it
 * was started from (a desktop shortcut, Steam's "Start in"), and finds the
 * game beside it. Otherwise the old layout: game\default.xbe under the
 * working folder.
 *
 * Settings come from jsrf.ini next to the exe: NAME=value lines, the same
 * RECOMP_* names the .bat files set. A variable already in the environment
 * (a .bat, Steam launch options) wins over the file. RECOMP_LOG names a
 * file for stderr when nothing else captured it; without it a release run
 * keeps no log. */
static char g_xbe_path[MAX_PATH] = YOUR_GAME_XBE_PATH;
static char g_game_dir[MAX_PATH] = YOUR_GAME_DIR;
static int  g_portable;
static int  g_ini_count;
static char g_ini_path[MAX_PATH];

static char *ini_trim(char *p)
{
    char *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (e - p >= 2 && ((p[0] == '"' && e[-1] == '"') || (p[0] == '\'' && e[-1] == '\''))) {
        e[-1] = 0;
        p++;
    }
    return p;
}

static void ini_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *k = ini_trim(line), *eq, *v;
        if (!*k || *k == ';' || *k == '#' || *k == '[')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = ini_trim(eq + 1);
        k = ini_trim(k);
        {   /* a comment after the value */
            char *c = strstr(v, " ;");
            if (!c) c = strstr(v, "\t;");
            if (c) { *c = 0; v = ini_trim(v); }
        }
        if (!*k || getenv(k))
            continue;                      /* the environment wins */
        if (!*v)
            continue;                      /* empty: leave unset */
        _putenv_s(k, v);
        g_ini_count++;
    }
    fclose(f);
}

static void portable_setup(void)
{
    char dir[MAX_PATH], xbe[MAX_PATH];
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    slash = strrchr(dir, '\\');
    if (!slash)
        return;
    *slash = 0;
    snprintf(xbe, sizeof xbe, "%s\\default.xbe", dir);
    if (GetFileAttributesA(xbe) != INVALID_FILE_ATTRIBUTES) {
        g_portable = 1;
        SetCurrentDirectoryA(dir);
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s", xbe);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    }
    snprintf(g_ini_path, sizeof g_ini_path, "%s\\jsrf.ini", dir);
    ini_load(g_ini_path);
    {
        /* stderr to a file, unless the caller already redirected it */
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        int captured = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
        const char *log = getenv("RECOMP_LOG");
        if (!captured && log && *log) {
            freopen(log, "w", stderr);
            freopen("NUL", "w", stdout);
        }
    }
}

/* v140: the first run with the saves in the game folder copies the old
 * ones (%LOCALAPPDATA%\\xboxrecomp) there, if that folder is not there yet */
static void save_migrate(const char *save)
{
    char old[MAX_PATH], from[MAX_PATH + 2], to[MAX_PATH + 2];
    const char *la = getenv("LOCALAPPDATA");
    SHFILEOPSTRUCTA op;
    if (!la || GetFileAttributesA(save) != INVALID_FILE_ATTRIBUTES)
        return;
    snprintf(old, sizeof old, "%s\\xboxrecomp", la);
    if (GetFileAttributesA(old) == INVALID_FILE_ATTRIBUTES)
        return;
    memset(from, 0, sizeof from);
    memset(to, 0, sizeof to);
    snprintf(from, MAX_PATH, "%s", old);       /* double-NUL ended */
    snprintf(to, MAX_PATH, "%s", save);
    memset(&op, 0, sizeof op);
    op.wFunc = FO_COPY;
    op.pFrom = from;
    op.pTo = to;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationA(&op) == 0 && !op.fAnyOperationsAborted)
        fprintf(stderr, "[CONFIG] saves copied from %s to %s\n", old, save);
    else
        fprintf(stderr, "[CONFIG] could not copy the saves from %s\n", old);
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline).
 * Entry VA 0x00148023 was named from XbSymbolDatabase, so the generated
 * function is XAPILIB_mainCRTStartup rather than sub_00148023. */
extern void XAPILIB_mainCRTStartup(void);
#define xbe_entry_point XAPILIB_mainCRTStartup

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

/* The emulated USB host controllers (src/usb/ohci.c), wired the same way.
 *
 * Like the APU, the toolkit ships the model but nothing calls it: ohci.h says
 * "the title's VEH routes the faults back to xbox_OhciHandleMmio", and the
 * template's VEH did not. With RECOMP_USB set, xbox_OhciInit makes the two
 * 4 KB register blocks at 0xFED00000/0xFED08000 fault; each fault is decoded
 * and answered here. JSRF's input setup goes through XAPI's own USB driver,
 * so without this it finds no host controller and its input object's create
 * fails (and the game's failure path double-frees). */
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
extern void xbox_OhciReport(void);

static DWORD g_main_tid;
static volatile int g_game_returned;


/* ── Fatal exceptions that are not access violations ──────────
 *
 * Run 10 ended with no [CRASH], no [EXIT] and no "Game returned": every
 * kernel exit path prints [EXIT], so the process died of something the VEH
 * above ignored. The usual one is a host stack overflow. Guest calls are
 * host C calls, and the default 1 MB host stack is small next to big lifted
 * frames; then the default handler has no stack to report with and the
 * process vanishes. The fix is /STACK in CMakeLists.txt. This handler makes
 * it, or anything else fatal, visible: SetThreadStackGuarantee (in WinMain)
 * keeps room to print, and the host stack is scanned for the generated
 * functions that fill it. With recursion, the top entry is the recursing
 * guest function. */
static volatile LONG g_fatal_once;

static void print_sym(const char *pfx, DWORD64 a, unsigned count)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), a, &disp, sym))
        fprintf(stderr, "%s%-28s x%u\n", pfx, sym->Name, count);
    else
        fprintf(stderr, "%s0x%llX x%u\n", pfx, (unsigned long long)a, count);
}

/* Histogram of return addresses on a host stack, by function: names the
 * generated functions (= guest functions) that own its frames. Needs the
 * .pdb next to the exe (CMakeLists.txt now emits one for Release). */
static void scan_host_stack(uintptr_t rsp, uintptr_t hi)
{
    HMODULE self = GetModuleHandleA(NULL);
    uintptr_t img_lo = (uintptr_t)self, img_hi = img_lo + 0x10000000u;
    if (hi > rsp) {
        enum { N = 64 };
        static DWORD64 fn[N]; static unsigned cnt[N]; unsigned used = 0, i;
        const uintptr_t *w = (const uintptr_t *)rsp;
        size_t nw = (hi - rsp) / sizeof(uintptr_t);
        unsigned outer[12]; DWORD64 outer_fn[12]; unsigned no = 0;
        for (size_t k = 0; k < nw; k++) {
            DWORD64 base;
            uintptr_t v = w[k];
            if (v < img_lo || v >= img_hi) continue;
            {
                char b2[sizeof(SYMBOL_INFO) + 8];
                SYMBOL_INFO *s2 = (SYMBOL_INFO *)b2; DWORD64 d = 0;
                memset(b2, 0, sizeof(b2));
                s2->SizeOfStruct = sizeof(SYMBOL_INFO); s2->MaxNameLen = 1;
                if (!SymFromAddr(GetCurrentProcess(), v, &d, s2)) continue;
                base = s2->Address;
            }
            for (i = 0; i < used && fn[i] != base; i++) {}
            if (i == used) { if (used == N) continue; fn[used] = base; cnt[used++] = 0; }
            cnt[i]++;
            if (no && outer_fn[no - 1] == base) outer[no - 1]++;
            else if (no < 12) { outer_fn[no] = base; outer[no++] = 1; }
        }
        fprintf(stderr, "  host stack, innermost frames:\n");
        for (i = 0; i < no; i++) print_sym("    ", outer_fn[i], outer[i]);
        fprintf(stderr, "  host stack, functions by number of frames (recursion shows here):\n");
        for (unsigned r = 0; r < 12 && used; r++) {
            unsigned best = 0;
            for (i = 1; i < used; i++) if (cnt[i] > cnt[best]) best = i;
            if (!cnt[best]) break;
            print_sym("    ", fn[best], cnt[best]);
            cnt[best] = 0;
        }
    }
}

static void report_fatal(PEXCEPTION_POINTERS ep, const char *why)
{
    ULONG_PTR lo = 0, hi = 0;
    uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;

    if (InterlockedExchange(&g_fatal_once, 1)) return;
    GetCurrentThreadStackLimits(&lo, &hi);
    fprintf(stderr, "\n[CRASH] %s: code 0x%08lX at RIP=0x%llX (thread %lu%s)\n", why,
            ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)ep->ContextRecord->Rip, GetCurrentThreadId(),
            GetCurrentThreadId() == g_main_tid ? ", main game thread" : "");
    fprintf(stderr, "  host stack: rsp=0x%llX, %llu KB used of %llu KB\n",
            (unsigned long long)rsp, (unsigned long long)(hi - rsp) / 1024,
            (unsigned long long)(hi - lo) / 1024);
    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
    print_guest_context((void *)ep->ContextRecord->Rip);

    scan_host_stack(rsp, hi);
    fflush(stderr);
    fflush(stdout);
}


/* ── Hang sampler ─────────────────────────────────────────────
 *
 * The watchdog says *that* the game stopped progressing; this says *where*.
 * A few seconds before the watchdog fires, it suspends the main game thread
 * a few times, reads its host RIP and stack, and prints the generated
 * functions on it. A spin loop shows up as the same innermost function in
 * every sample. RECOMP_SAMPLE_SECS overrides when (0 disables); by default
 * it is RECOMP_WATCHDOG_SECS - 3. */
static HANDLE   g_main_thread;
static ULONG_PTR g_main_stack_hi;
static volatile uint32_t *g_main_esp;   /* the main thread's TLS g_esp */

static void print_rip(DWORD64 rip)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym))
        fprintf(stderr, "  RIP 0x%llX = %s+0x%llX\n", (unsigned long long)rip,
                sym->Name, (unsigned long long)disp);
    else {
        HMODULE m = NULL; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)rip, &m))
            GetModuleFileNameA(m, name, sizeof name);
        fprintf(stderr, "  RIP 0x%llX (no symbol; module %s)\n",
                (unsigned long long)rip, name);
    }
}

static void sample_main_thread(int k, const char *why)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(g_main_thread) == (DWORD)-1) return;
    if (GetThreadContext(g_main_thread, &ctx)) {
        uint32_t esp = g_main_esp ? *g_main_esp : 0;
        fprintf(stderr, "\n[SAMPLE %d] main game thread (%s, guest esp=0x%08X):\n",
                k + 1, why, esp);
        print_rip(ctx.Rip);
        scan_host_stack((uintptr_t)ctx.Rsp, g_main_stack_hi);
        if (g_xbox_mem_offset && esp) {
            const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp);
            int i, shown = 0;
            fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
            for (i = 0; i < 512 && shown < 24; i++)
                if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                    fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, sp[i]);
                    shown++;
                }
        }
        fflush(stderr);
    }
    ResumeThread(g_main_thread);
}

/* Samples the main game thread three times: once at RECOMP_SAMPLE_SECS (if
 * set), and whenever a file named sample.flag appears in the working folder
 * (debug\sample_hang.bat makes one) -- so a freeze that happens at a random point
 * of play can be caught when it happens. */
static DWORD WINAPI sampler_thread(LPVOID p)
{
    DWORD secs = (DWORD)(uintptr_t)p, start = GetTickCount();
    int timed_done = secs == 0;
    for (;;) {
        Sleep(500);
        if (!timed_done && GetTickCount() - start >= secs * 1000) {
            int k;
            timed_done = 1;
            for (k = 0; k < 3; k++) { sample_main_thread(k, "timed"); Sleep(700); }
        }
        if (GetFileAttributesA("sample.flag") != INVALID_FILE_ATTRIBUTES) {
            int k;
            DeleteFileA("sample.flag");
            fprintf(stderr, "\n[SAMPLE] sample.flag found: sampling the main game thread\n");
            for (k = 0; k < 3; k++) { sample_main_thread(k, "sample.flag"); Sleep(700); }
        }
    }
    return 0;
}

/* v73: RECOMP_PROFILE=1 -- a sampling profiler of the main game thread.
 *
 * Run 72: frames dip to ~50 for 2-3 s on entering an area. The executor's
 * own timers say how long the GPU emulation took; nothing said where the
 * game thread spent a frame that ran long. About 1000 times a second this
 * suspends the main thread and records its host instruction pointer, in two
 * tallies: all samples, and samples taken while the current frame has run
 * past 18 ms (from the last flip, g_last_flip_qpc). Every 10 s the top
 * functions of each go to stderr as [PROF] lines -- the recompiled guest
 * function (sub_XXXXXXXX) or host function the thread was in -- and the
 * tallies restart. Costs the game thread about 2%. */
extern volatile uint64_t g_last_flip_qpc;

#define PROF_SLOTS 32768
static struct { DWORD64 rip; uint32_t all, slow; } s_prof[PROF_SLOTS];
static uint32_t s_prof_total, s_prof_slow_total;

/* v161: a second thread sampled the same way (JSRF's CRI vsync thread, which
 * decodes the music once per vblank), registered from recomp_manual.c */
#define PROF_EXTRA 4
static struct { DWORD64 rip; uint32_t all, slow; } s_prof2[PROF_EXTRA][PROF_SLOTS];
static uint32_t s_prof2_total[PROF_EXTRA];
static volatile HANDLE g_prof_thread2[PROF_EXTRA];
static char g_prof_thread2_name[PROF_EXTRA][48];
static volatile LONG g_prof_extra_n;
extern void (*g_thread_watch_hook)(const char *name);
void recomp_prof_watch_this_thread(const char *name);
void recomp_prof_watch_this_thread(const char *name)
{
    HANDLE h;
    LONG k;
    if (!getenv("RECOMP_PROFILE")) return;
    if (g_prof_extra_n >= PROF_EXTRA) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &h, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    k = InterlockedIncrement(&g_prof_extra_n) - 1;
    if (k >= PROF_EXTRA) { CloseHandle(h); return; }
    _snprintf(g_prof_thread2_name[k], sizeof g_prof_thread2_name[k], "%s", name);
    g_prof_thread2[k] = h;
}
static void prof2_note(int k, DWORD64 rip)
{
    uint32_t h = (uint32_t)((rip >> 2) * 2654435761u) & (PROF_SLOTS - 1), n;
    for (n = 0; n < 64; n++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_prof2[k][h].rip == rip || !s_prof2[k][h].rip) {
            s_prof2[k][h].rip = rip;
            s_prof2[k][h].all++;
            return;
        }
    }
}

static void prof_note(DWORD64 rip, int slow)
{
    uint32_t h = (uint32_t)((rip >> 2) * 2654435761u) & (PROF_SLOTS - 1), n;
    for (n = 0; n < 64; n++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_prof[h].rip == rip || !s_prof[h].rip) {
            s_prof[h].rip = rip;
            s_prof[h].all++;
            if (slow) s_prof[h].slow++;
            return;
        }
    }
}

typedef struct { char name[96]; uint32_t all, slow; } ProfFn;

static int prof_cmp_all(const void *a, const void *b)
{ return (int)((const ProfFn *)b)->all - (int)((const ProfFn *)a)->all; }
static int prof_cmp_slow(const void *a, const void *b)
{ return (int)((const ProfFn *)b)->slow - (int)((const ProfFn *)a)->slow; }

static void prof_report(void)
{
    static ProfFn fn[4096];
    int nfn = 0, i, j;
    char buf[sizeof(SYMBOL_INFO) + 128];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    for (i = 0; i < PROF_SLOTS; i++) {
        char name[96];
        DWORD64 disp = 0;
        if (!s_prof[i].rip) continue;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 127;
        if (SymFromAddr(GetCurrentProcess(), s_prof[i].rip, &disp, sym)) {
            _snprintf(name, sizeof name, "%s", sym->Name);
        } else {
            HMODULE m = NULL; char path[MAX_PATH] = "?"; const char *base;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)s_prof[i].rip, &m))
                GetModuleFileNameA(m, path, sizeof path);
            base = strrchr(path, '\\');
            _snprintf(name, sizeof name, "[%s]", base ? base + 1 : path);
        }
        name[sizeof name - 1] = 0;
        for (j = 0; j < nfn; j++)
            if (!strcmp(fn[j].name, name)) break;
        if (j == nfn) {
            if (nfn == 4096) continue;
            memcpy(fn[nfn].name, name, sizeof name);
            fn[nfn].all = fn[nfn].slow = 0;
            nfn++;
        }
        fn[j].all += s_prof[i].all;
        fn[j].slow += s_prof[i].slow;
    }
    if (s_prof_total) {
        qsort(fn, nfn, sizeof fn[0], prof_cmp_all);
        fprintf(stderr, "[PROF] main thread, %u samples (%u in frames past 18 ms); top functions:\n",
                s_prof_total, s_prof_slow_total);
        for (i = 0; i < nfn && i < 20; i++)
            fprintf(stderr, "[PROF]   %5.1f%%  %s\n", fn[i].all * 100.0 / s_prof_total, fn[i].name);
        if (s_prof_slow_total) {
            qsort(fn, nfn, sizeof fn[0], prof_cmp_slow);
            fprintf(stderr, "[PROF] during frames past 18 ms:\n");
            for (i = 0; i < nfn && i < 20 && fn[i].slow; i++)
                fprintf(stderr, "[PROF]   %5.1f%%  %s\n",
                        fn[i].slow * 100.0 / s_prof_slow_total, fn[i].name);
        }
    }
    memset(s_prof, 0, sizeof s_prof);
    s_prof_total = s_prof_slow_total = 0;
    for (int k = 0; k < PROF_EXTRA; k++) if (s_prof2_total[k]) {
        /* the same, for the extra threads (no slow-frame split) */
        static ProfFn fn2[4096];
        int n2 = 0;
        for (i = 0; i < PROF_SLOTS; i++) {
            char name[96];
            DWORD64 disp = 0;
            if (!s_prof2[k][i].rip) continue;
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 127;
            if (SymFromAddr(GetCurrentProcess(), s_prof2[k][i].rip, &disp, sym))
                _snprintf(name, sizeof name, "%s", sym->Name);
            else {
                /* v169: module and offset, so a system DLL's hot spot can be
                 * told apart from another (the executor's 43% at one address
                 * in run 168) */
                HMODULE m = NULL; char path[MAX_PATH]; const char *base;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)s_prof2[k][i].rip, &m)
                        && GetModuleFileNameA(m, path, sizeof path)) {
                    base = strrchr(path, '\\');
                    _snprintf(name, sizeof name, "[%s+0x%llX]", base ? base + 1 : path,
                              (unsigned long long)(s_prof2[k][i].rip - (DWORD64)(uintptr_t)m));
                } else
                    _snprintf(name, sizeof name, "[0x%llX]", (unsigned long long)s_prof2[k][i].rip);
            }
            name[sizeof name - 1] = 0;
            for (j = 0; j < n2; j++)
                if (!strcmp(fn2[j].name, name)) break;
            if (j == n2) {
                if (n2 == 4096) continue;
                memcpy(fn2[n2].name, name, sizeof name);
                fn2[n2].all = fn2[n2].slow = 0;
                n2++;
            }
            fn2[j].all += s_prof2[k][i].all;
        }
        qsort(fn2, n2, sizeof fn2[0], prof_cmp_all);
        fprintf(stderr, "[PROF] %s thread, %u samples; top functions:\n",
                g_prof_thread2_name[k], s_prof2_total[k]);
        for (i = 0; i < n2 && i < 30; i++)
            fprintf(stderr, "[PROF]   %5.1f%%  %s\n", fn2[i].all * 100.0 / s_prof2_total[k], fn2[i].name);
        memset(s_prof2[k], 0, sizeof s_prof2[k]);
        s_prof2_total[k] = 0;
    }
}

static DWORD WINAPI prof_thread(LPVOID unused)
{
    LARGE_INTEGER freq, now;
    DWORD last = GetTickCount();
    (void)unused;
    QueryPerformanceFrequency(&freq);
    {
        typedef unsigned (__stdcall *tbp_t)(unsigned);
        HMODULE w = LoadLibraryA("winmm.dll");
        tbp_t tbp = w ? (tbp_t)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        CONTEXT ctx;
        Sleep(1);
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_main_thread) == (DWORD)-1)
            continue;
        if (GetThreadContext(g_main_thread, &ctx)) {
            uint64_t lf = g_last_flip_qpc;
            int slow;
            QueryPerformanceCounter(&now);
            slow = lf && (uint64_t)now.QuadPart > lf
                && ((uint64_t)now.QuadPart - lf) * 1000 > 18 * (uint64_t)freq.QuadPart;
            prof_note(ctx.Rip, slow);
            s_prof_total++;
            if (slow) s_prof_slow_total++;
        }
        ResumeThread(g_main_thread);
        for (int k = 0; k < PROF_EXTRA; k++) if (g_prof_thread2[k]) {
            HANDLE t2 = g_prof_thread2[k];
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(t2) != (DWORD)-1) {
                if (GetThreadContext(t2, &ctx)) {
                    prof2_note(k, ctx.Rip);
                    s_prof2_total[k]++;
                }
                ResumeThread(t2);
            }
        }
        if (GetTickCount() - last >= 10000) {
            last = GetTickCount();
            prof_report();
        }
    }
    return 0;
}

static void sampler_start(void)
{
    g_thread_watch_hook = recomp_prof_watch_this_thread;   /* v168 */
    const char *s = getenv("RECOMP_SAMPLE_SECS");
    const char *w = getenv("RECOMP_WATCHDOG_SECS");
    long secs = s ? strtol(s, NULL, 0) : (w ? strtol(w, NULL, 0) - 3 : 0);
    ULONG_PTR lo;
    if (secs < 0) secs = 0;
    DeleteFileA("sample.flag");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    GetCurrentThreadStackLimits(&lo, &g_main_stack_hi);
    g_main_esp = &g_esp;
    CloseHandle(CreateThread(NULL, 256 * 1024, sampler_thread,
                             (LPVOID)(uintptr_t)secs, 0, NULL));
    if (getenv("RECOMP_PROFILE") && atoi(getenv("RECOMP_PROFILE")) > 0) {
        fprintf(stderr, "[PROF] sampling the main game thread (RECOMP_PROFILE)\n");
        CloseHandle(CreateThread(NULL, 256 * 1024, prof_thread, NULL, 0, NULL));
    }
}

static LONG WINAPI unhandled_filter(PEXCEPTION_POINTERS ep)
{
    report_fatal(ep, "unhandled exception");
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void exit_hook(void)
{
    if (!g_game_returned)
        fprintf(stderr, "\n[EXIT] exit() called while the game was running "
                        "(guest esp=0x%08X)\n", g_esp), fflush(stderr);
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION
                || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION
                || code == EXCEPTION_INT_OVERFLOW || code == 0xC0000374u /* heap corruption */) {
            report_fatal(ep, code == EXCEPTION_STACK_OVERFLOW ? "HOST STACK OVERFLOW"
                                                              : "fatal exception");
            TerminateProcess(GetCurrentProcess(), code);
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        /* USB host controller registers -> the OHCI model. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    portable_setup();                      /* v139: before anything reads a setting */

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Jet Set Radio Future - Static Recompilation ===\n");
    printf("Loading XBE...\n");
    fprintf(stderr, "[CONFIG] %s layout, game files in %s; %d setting(s) from %s\n",
            g_portable ? "portable" : "classic", g_game_dir, g_ini_count,
            g_ini_count ? g_ini_path : "no jsrf.ini");
    fprintf(stderr, "[CONFIG] build v183, compiled " __DATE__ " " __TIME__ "\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
    atexit(exit_hook);
    g_main_tid = GetCurrentThreadId();
    {   /* room for the fatal handler to print after a stack overflow */
        ULONG guarantee = 256 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }

    /* Step 1: Load XBE */
    if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory, or put\n"
                    "this exe in the game folder next to default.xbe.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");

    /* Guest RAM. Run 21 ran the 48 MB guest heap dry ("xbox_HeapAlloc: out
     * of memory") while loading the first stage after the logos, and JSRF
     * reports a failed load as "There's a problem with the disc". On the
     * console the title gets nearly all of 64 MB, but this runtime spends
     * 8 MB of it on the main stack (JSRF asks for 64 KB) and keeps the
     * contiguous window apart, so the heap is smaller than the game expects.
     * A 128 MB devkit-sized RAM gives the heap room. RECOMP_RAM_MB overrides
     * (64 restores the retail size). */
    {
        const char *mb = getenv("RECOMP_RAM_MB");
        unsigned v = mb ? (unsigned)strtoul(mb, NULL, 0) : 128;
        if (v >= 64 && v <= 512) {
            extern void xbox_SetTotalRam(size_t bytes);
            xbox_SetTotalRam((size_t)v * 1024 * 1024);
            printf("Guest RAM: %u MB\n", v);
        }
    }
    /* Give freed memory back to the guest heap (run 49). Without it a
     * release went nowhere -- NtFreeVirtualMemory handed guest addresses to
     * the host VirtualFree, which refused -- and a freed block came back
     * whole to the next request however small. JSRF frees each level's data
     * before loading the next, so after the tutorial the 112 MB heap was
     * exhausted with about 45 MB live, and the next load ("e204.dat") failed
     * as "There's a problem with the disc". RECOMP_HEAP_RECLAIM=0 turns it
     * off. Set before anything allocates: the switch is read once. */
    {
        const char *r = getenv("RECOMP_HEAP_RECLAIM");
        if (!r)
            _putenv("RECOMP_HEAP_RECLAIM=1");
        else if (!strcmp(r, "0"))
            _putenv("RECOMP_HEAP_RECLAIM=");
        printf("Heap reclaim: %s\n", getenv("RECOMP_HEAP_RECLAIM") ? "on" : "off");
    }
    /* File I/O completions at the next alertable wait, as on the console
     * (run 55): CRI's file server only finishes a read whose completion it
     * delivers itself, in SleepEx(0, TRUE). Without this no ADX stream gets
     * past its first block -- no music, no DJ voice, and the event after the
     * tutorial waits on a black screen. RECOMP_DEFER_APC=0 turns it off. */
    {
        const char *r = getenv("RECOMP_DEFER_APC");
        if (!r)
            _putenv("RECOMP_DEFER_APC=1");
        else if (!strcmp(r, "0"))
            _putenv("RECOMP_DEFER_APC=");
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Bring up the USB host controllers (no-op unless RECOMP_USB is set).
     * After the memory layout, because it protects the register blocks inside
     * the MCPX aperture that MemoryLayoutInit maps. */
    xbox_OhciInit();

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* v140: saves go in a "savegame" folder in the game folder;
         * RECOMP_SAVE_DIR puts them elsewhere (a relative path is taken from
         * the game folder). The first time, saves from the old place,
         * %LOCALAPPDATA%\\xboxrecomp, are copied over (copied, not moved). */
        const char *sd = getenv("RECOMP_SAVE_DIR");
        char save[MAX_PATH], gfull[MAX_PATH];
        if (!GetFullPathNameA(g_game_dir, MAX_PATH, gfull, NULL))
            snprintf(gfull, sizeof gfull, "%s", g_game_dir);
        if (sd && *sd && (sd[0] == '\\' || sd[0] == '/' || (sd[0] && sd[1] == ':')))
            snprintf(save, sizeof save, "%s", sd);
        else
            snprintf(save, sizeof save, "%s\\%s", gfull, (sd && *sd) ? sd : "savegame");
        save_migrate(save);
        fprintf(stderr, "[CONFIG] saves in %s\n", save);
        xbox_path_init(g_game_dir, save);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    sampler_start();

    /* Step 7: Call the recompiled entry point */
    {
        extern void xbox_mmcss_game_thread(const char *who);
        xbox_mmcss_game_thread("main");          /* v108 */
    }
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();
    g_game_returned = 1;

    printf("\nGame returned. Cleaning up...\n");
    fprintf(stderr, "[EXIT] game entry point returned (eax=0x%08X)\n", g_eax);
    xbox_OhciReport();

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
