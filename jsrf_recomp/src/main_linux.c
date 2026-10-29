/*
 * main_linux.c - Jet Set Radio Future, native Linux build (v244)
 *
 * The same start-up as main.c (the Windows build), without what only
 * Windows has: the dbghelp crash reports, the main-thread sampler
 * (RECOMP_PROFILE's [PROF] lines; on Linux `perf` does that job better) and
 * the copy of saves from the old %LOCALAPPDATA% place.
 *
 * Faults are handled as on Windows: win32_compat.c turns SIGSEGV into the
 * vectored exception the handler below takes, so the APU and the USB host
 * controller registers (pages that fault on purpose) are answered by the
 * same decoders.
 *
 * Layout: the executable sits in the game folder next to default.xbe and
 * jsrf.ini (what build_linux.sh sets up), or game/default.xbe under the
 * working folder.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>

#include <xbox/xboxrecomp.h>
#include "jsrf_version.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

#define YOUR_GAME_ENTRY_POINT 0x00148023u
extern void XAPILIB_mainCRTStartup(void);
extern int recomp_dispatch_init(void);

static char g_xbe_path[PATH_MAX] = "game/default.xbe";
static char g_game_dir[PATH_MAX] = "game";
static int  g_portable;
static int  g_ini_count;
static char g_ini_path[PATH_MAX];

/* ── jsrf.ini: NAME=value lines, as main.c reads them ──────── */

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
    static char own[128][64];
    static int n_own;
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
        {
            char *c = strstr(v, " ;");
            if (!c) c = strstr(v, "\t;");
            if (c) { *c = 0; v = ini_trim(v); }
        }
        if (!*k)
            continue;
        {
            int i, mine = 0;
            for (i = 0; i < n_own; i++)
                if (!strcasecmp(own[i], k)) { mine = 1; break; }
            if (getenv(k) && !mine)
                continue;                  /* the environment wins */
            if (!*v)
                continue;
            if (!mine && n_own < (int)(sizeof own / sizeof own[0]))
                snprintf(own[n_own++], sizeof own[0], "%s", k);
            if (!mine)
                g_ini_count++;
        }
        setenv(k, v, 1);
    }
    fclose(f);
}

static void portable_setup(void)
{
    char dir[PATH_MAX], xbe[PATH_MAX + 16];
    char *slash;
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    struct stat st;
    if (n <= 0)
        return;
    dir[n] = 0;
    slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = 0;
    snprintf(xbe, sizeof xbe, "%s/default.xbe", dir);
    if (stat(xbe, &st) == 0) {
        g_portable = 1;
        if (chdir(dir) != 0) { /* keep going from where we are */ }
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s", xbe);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    }
    snprintf(g_ini_path, sizeof g_ini_path, "%s/jsrf.ini", dir);
    ini_load(g_ini_path);
    {
        /* stderr to the file RECOMP_LOG names (relative: in the game folder).
         * Always, when it is set: from Steam stderr goes to a pipe nobody
         * reads, so "only when nothing captured it", as on Windows, would
         * lose the log just where it is wanted. */
        const char *log = getenv("RECOMP_LOG");
        if (log && *log) {
            if (!freopen(log, "w", stderr)) { /* keep the old stderr */ }
        }
    }
}

/* ── faults: the APU and USB registers, then a report ─────── */

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
extern void xbox_OhciReport(void);

static volatile int g_fatal_once;

static LONG veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t xbox_va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        int is_write = ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0;
        if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                && apu_hook_handle_mmio(ep->ContextRecord, fault_addr, xbox_va, is_write))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (xbox_OhciOwnsAddress(xbox_va) && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!__sync_lock_test_and_set(&g_fatal_once, 1)) {
        const CONTEXT *c = ep->ContextRecord;
        const uint8_t *code = (const uint8_t *)(uintptr_t)c->Rip;
        int i;
        fprintf(stderr, "[CRASH] exception 0x%08X at RIP=0x%llX, fault addr=0x%llX (%s)\n",
                (unsigned)ep->ExceptionRecord->ExceptionCode, (unsigned long long)c->Rip,
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n", g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
                (uint32_t)(ep->ExceptionRecord->ExceptionInformation[1]
                           - (uintptr_t)g_xbox_mem_offset));
        fprintf(stderr, "  host regs: rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n",
                (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
                (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
        fprintf(stderr, "  host regs: rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n",
                (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
                (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
        fprintf(stderr, "  code at RIP:");
        for (i = 0; i < 16; i++)
            fprintf(stderr, " %02X", code[i]);
        fprintf(stderr, "\n  (addr2line -e jsrf_recomp -f 0x... names the function)\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* main.c's thread profiler is Windows-only; the hook stays for the callers */
void recomp_prof_watch_this_thread(const char *name) { (void)name; }

static int load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || !(data = malloc((size_t)size))) {
        fclose(f);
        return 0;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return 1;
}

static int game_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    portable_setup();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Jet Set Radio Future - Static Recompilation (Linux) ===\n");
    fprintf(stderr, "[CONFIG] %s layout, game files in %s; %d setting(s) from %s\n",
            g_portable ? "portable" : "classic", g_game_dir, g_ini_count,
            g_ini_count ? g_ini_path : "no jsrf.ini");
    fprintf(stderr, "[CONFIG] build " JSRF_BUILD " (Linux), compiled " __DATE__ " " __TIME__ "\n");

    AddVectoredExceptionHandler(1, veh_handler);

    if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
        fprintf(stderr, "Failed to load default.xbe. Put this program in the game folder next"
                " to default.xbe, or the game files in game/ under the working folder.\n");
        return 1;
    }
    {
        const char *mb = getenv("RECOMP_RAM_MB");
        unsigned v = mb ? (unsigned)strtoul(mb, NULL, 0) : 128;
        if (v >= 64 && v <= 512) {
            extern void xbox_SetTotalRam(size_t bytes);
            xbox_SetTotalRam((size_t)v * 1024 * 1024);
        }
    }
    {
        const char *r = getenv("RECOMP_HEAP_RECLAIM");
        if (!r) setenv("RECOMP_HEAP_RECLAIM", "1", 1);
        else if (!strcmp(r, "0")) unsetenv("RECOMP_HEAP_RECLAIM");
        r = getenv("RECOMP_DEFER_APC");
        if (!r) setenv("RECOMP_DEFER_APC", "1", 1);
        else if (!strcmp(r, "0")) unsetenv("RECOMP_DEFER_APC");
    }
    {
        extern int apu_audio_debug(void);
        apu_audio_debug();
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Failed to initialize the Xbox memory layout"
                " (the address range it needs is taken).\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
    }
    xbox_OhciInit();
    xbox_kernel_init();
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        const char *sd = getenv("RECOMP_SAVE_DIR");
        char save[PATH_MAX + 32], gfull[PATH_MAX];
        if (!realpath(g_game_dir, gfull))
            snprintf(gfull, sizeof gfull, "%s", g_game_dir);
        if (sd && *sd && sd[0] == '/')
            snprintf(save, sizeof save, "%s", sd);
        else
            snprintf(save, sizeof save, "%s/%s", gfull, (sd && *sd) ? sd : "savegame");
        fprintf(stderr, "[CONFIG] saves in %s\n", save);
        xbox_path_init(gfull, save);
    }
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");
    xbox_WatchdogStart();
    {
        extern void xbox_mmcss_game_thread(const char *who);
        xbox_mmcss_game_thread("main");
    }
    printf("Entry point: 0x%08X\nStarting game...\n", YOUR_GAME_ENTRY_POINT);
    XAPILIB_mainCRTStartup();

    fprintf(stderr, "[EXIT] game entry point returned (eax=0x%08X)\n", g_eax);
    xbox_OhciReport();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}

/* The game runs on a thread with a 256 MB stack (the Windows build's
 * /STACK): the main thread's stack is the system's 8 MB. */
static int s_rc;
static void *game_thread(void *unused) { (void)unused; s_rc = game_main(); return NULL; }

int main(int argc, char **argv)
{
    pthread_t t;
    pthread_attr_t a;
    (void)argc; (void)argv;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)256 << 20);
    if (pthread_create(&t, &a, game_thread, NULL) != 0)
        return game_main();
    pthread_join(t, NULL);
    return s_rc;
}
