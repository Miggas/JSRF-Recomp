/* Minimal Linux runtime so the lifted JSRF C can run functions in isolation.
 * Guest memory: one flat mapping, guest VA v lives at host g_mem + v.
 * Kernel thunks: slot i of the thunk table is patched to VA 0xFE000000 + 4*i
 * (same scheme as xboxrecomp) and dispatched here. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef void (*recomp_func_t)(void);
typedef union RecompXmm { float f[4]; double d[2]; uint32_t u[4]; int32_t i[4]; uint64_t q[2]; } RecompXmm;
typedef union RecompMmx { uint64_t q; uint32_t u[2]; uint16_t w[4]; uint8_t b[8]; } RecompMmx;

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp, g_fs_base;
__thread double g_fp_stack[8];
__thread int g_fp_top, g_fp_cmp, g_df;
__thread uint16_t g_fp_control_word = 0x027F, g_fp_cc;
__thread RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
__thread RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile uint32_t g_icall_trace[16];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
ptrdiff_t g_xbox_mem_offset;
int g_force_return;

#define MEM32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))

/* ---- kernel model, mirrored exactly in the Python/Unicorn side ---- */
static int g_slot_ordinal[512];          /* thunk slot -> ordinal */
uint32_t g_reserve_next = 0x00F81000u;
int g_kernel_unknown;                    /* first unknown ordinal hit, or 0 */
static uint32_t arg(int i) { return MEM32(g_esp + 4 + 4 * i); }
static void ret_std(uint32_t eax, int nargs) { g_eax = eax; g_esp += 4 + 4 * nargs; }

static void k_dispatch(int ord)
{
    switch (ord) {
    case 291: case 277: case 294:          /* RtlInitialize/Enter/LeaveCriticalSection */
        ret_std(0, 1); return;
    case 184: {                            /* NtAllocateVirtualMemory(*Base, ZeroBits, *Size, Type, Prot) */
        uint32_t pbase = arg(0), psize = arg(2), type = arg(3);
        uint32_t base = MEM32(pbase), size = (MEM32(psize) + 0xFFF) & ~0xFFFu;
        if (base == 0 && (type & 0x2000)) { base = g_reserve_next; g_reserve_next += size; }
        MEM32(pbase) = base & ~0xFFFu; MEM32(psize) = size;
        ret_std(0, 5); return;
    }
    case 199:                              /* NtFreeVirtualMemory(*Base, *Size, Type) */
        ret_std(0, 3); return;
    case 8:                                /* DbgPrint (cdecl) */
        g_eax = 0; g_esp += 4; return;
    default:
        if (!g_kernel_unknown) { g_kernel_unknown = ord; fprintf(stderr, "[rt] unknown ordinal %d, return addr %08X, esp %08X, icall trace:", ord, MEM32(g_esp), g_esp);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & 15]); fprintf(stderr, "\n"); }
        ret_std(0xC0000001u, 0); return;
    }
}
static int g_cur_slot;
static void k_generic(void) { k_dispatch(g_slot_ordinal[g_cur_slot]); }
recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    if (va >= 0xFE000000u && va < 0xFE000000u + 4 * 120) { g_cur_slot = (int)((va - 0xFE000000u) / 4); return k_generic; }
    return NULL;
}
extern recomp_func_t recomp_lookup(uint32_t va);
#ifndef RT_HAVE_MANUAL
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

#endif
uint32_t g_icall_failed;
uint32_t g_unimpl_va;
#ifndef RT_HAVE_MANUAL
void recomp_icall_fail_log(uint32_t va) { if (!g_icall_failed) g_icall_failed = va; }
void recomp_icall_not_code_log(uint32_t va) { if (!g_icall_failed) g_icall_failed = va; }
void recomp_unimpl(const char *t, uint32_t va) { (void)t; if (!g_unimpl_va) g_unimpl_va = va; }
#endif
void recomp_debug_service(uint32_t s, uint32_t a) { (void)s; (void)a; }
uint64_t xbox_ReadTimeStampCounter(void) { return 0; }

/* ---- API for the Python driver ---- */
uint8_t *rt_init(uint32_t span)
{
    void *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    g_xbox_mem_offset = (ptrdiff_t)p;
    g_fs_base = 0x4000;
    return (uint8_t *)p;
}
void rt_set_slot(int slot, int ordinal) { g_slot_ordinal[slot] = ordinal; }

/* Call guest function `va` (stdcall/cdecl) with args; returns eax. */
uint32_t rt_call(uint32_t va, uint32_t esp_top, int nargs, const uint32_t *args, uint32_t *esp_out)
{
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) return 0xDEADBEEF;
    g_esp = esp_top;
    for (int i = nargs - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = args[i]; }
    g_esp -= 4; MEM32(g_esp) = 0xCAFEF00Du;    /* return address */
    g_ebp = 0; g_seh_ebp = 0;
    fn();
    if (esp_out) *esp_out = g_esp;
    return g_eax;
}
