/* Built instead of nv2a_gl.c when XBOXRECOMP_GL is OFF: the GPU backend is
 * absent and the executor keeps the software rasteriser. */
#include "nv2a_gl.h"

int  nv2a_gl_active(void) { return 0; }
void nv2a_gl_draw(const NvGlState *st, const Nv2aVshOutput *v,
                  const float *fogf, const uint32_t *idx, uint32_t n_idx)
{ (void)st; (void)v; (void)fogf; (void)idx; (void)n_idx; }
void nv2a_gl_clear(const NvGlState *st, uint32_t param, uint32_t argb,
                   float zclear, uint32_t sclear)
{ (void)st; (void)param; (void)argb; (void)zclear; (void)sclear; }
void nv2a_gl_sync_to_guest(uint32_t surf_va) { (void)surf_va; }
void nv2a_gl_clear_rect(const NvGlState *st, uint32_t param, uint32_t argb,
                        float zclear, uint32_t sclear, int x0, int x1, int y0, int y1)
{ (void)st; (void)param; (void)argb; (void)zclear; (void)sclear; (void)x0; (void)x1; (void)y0; (void)y1; }
void nv2a_gl_end_frame(void) {}
void nv2a_gl_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e)
{ *a = *b = *c = *d = *e = 0; }
void nv2a_gl_stats2(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void nv2a_gl_stats3(uint64_t *a) { *a = 0; }
void nv2a_gl_probe_top(uint32_t o[5]) { int i; for (i = 0; i < 5; i++) o[i] = 0; }
void nv2a_gl_probe_at(const int *x, const int *y, int n, uint32_t *o) { int i; (void)x; (void)y; for (i = 0; i < n; i++) o[i] = 0; }
void nv2a_gl_window_stats(uint64_t *f, double *a, uint64_t *m, uint64_t *o, double *i,
                          uint64_t *b, uint64_t *c)
{ *f = *m = *o = *b = *c = 0; *a = *i = 0; }
NvGlVertex *nv2a_gl_map_vertices(uint32_t n) { (void)n; return 0; }
int nv2a_gl_gpu_vs_ok(void) { return 0; }
void nv2a_gl_frame_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e,
                         uint64_t *f) { *a = *b = *c = *d = *e = 0; *f = 1; }
int nv2a_gl_prepare_vsh(const uint32_t (*prog)[4], int n) { (void)prog; (void)n; return 0; }
float *nv2a_gl_map_raw(uint32_t n, uint32_t k) { (void)n; (void)k; return 0; }
void nv2a_gl_draw_raw(const NvGlState *st, const NvGlVsState *vs, const uint8_t *slots,
                      uint32_t k, const uint32_t *idx, uint32_t n_idx)
{ (void)st; (void)vs; (void)slots; (void)k; (void)idx; (void)n_idx; }
void nv2a_gl_unmap_raw(void) {}
int nv2a_gl_map_native(uint32_t k, const NvGlNatAttr *a, const float (*cval)[4], uint32_t ns,
                       const uint8_t *const *src, const uint32_t *len, const uint32_t *stride)
{ (void)k; (void)a; (void)cval; (void)ns; (void)src; (void)len; (void)stride; return 0; }
void nv2a_gl_draw_mapped(const NvGlState *st, const uint32_t *idx, uint32_t n_idx)
{ (void)st; (void)idx; (void)n_idx; }
void nv2a_gl_unmap_vertices(void) {}
void nv2a_gl_times(uint64_t t[4], uint64_t *freq) { t[0] = t[1] = t[2] = t[3] = 0; *freq = 0; }
uint32_t nv2a_gl_present(uint32_t surf_va) { return surf_va; }
float nv2a_gl_probe_z(int i) { (void)i; return -1.0f; }
int nv2a_gl_gpu_frame_stats(char *b, size_t n) { (void)b; (void)n; return 0; }
int nv2a_gl_submit_stats(char *b, size_t n) { (void)b; (void)n; return 0; }
int nv2a_gl_call_stats(char *b, size_t n) { (void)b; (void)n; return 0; }
void nv2a_gl_vis_enable(int on) { (void)on; }
void nv2a_gl_vis_clear(void) {}
uint32_t nv2a_gl_vis_count(void) { return 0; }
void nv2a_gl_trace_frame(int on) { (void)on; }
void nv2a_gl_trace_draw(int draw_no) { (void)draw_no; }
void nv2a_gl_trace_report(void) {}
void nv2a_gl_idle(uint32_t draws) { (void)draws; }
