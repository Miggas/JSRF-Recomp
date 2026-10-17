/**
 * NV2A rasterisation through OpenGL 3.3 core. See nv2a_gl.h.
 *
 * Conventions:
 *  - Screen space is the NV2A's: x right, y down, pixel centres at +0.5.
 *    The vertex shader maps y so the top of the surface is the top of the GL
 *    framebuffer; GL row 0 is therefore guest row height-1, and every upload
 *    and readback flips rows. Because the picture keeps its orientation, a
 *    triangle that is clockwise on screen is clockwise to GL as well, and the
 *    title's FRONT_FACE / CULL_FACE values (which are GL enums on the NV2A)
 *    apply unchanged.
 *  - The NV2A takes GL enums for depth/stencil functions, stencil ops, blend
 *    factors and equations, so those pass straight through.
 *  - Positions arrive as the executor's screen coordinates (x, y in pixels,
 *    z in depth units) with the clip-space w kept. x*w etc. is linear in
 *    clip space, so GL clips and interpolates perspective-correctly from it.
 *    Depth clamp stands in for the NV2A's lack of near/far clipping.
 */
#include "nv2a_gl.h"
#include "xbox_memory_layout.h"   /* v117: xbox_gw_* write-watch */
#include <math.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <glad/gl.h>
#include "nv2a_gl_thread_rt.h"   /* v172: the GL thread (and the GL call counts of v169) */
#include "nv2a_gl_thread.h"      /* generated: tools/gen_glthread.py */
#include "nv2a_gl_thread_rt2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>
#else
#include <sys/stat.h>
#endif

/* ── context ──────────────────────────────────────────────────────────── */

static int s_state = -1;              /* -1 untried, 0 failed/off, 1 active */
static int s_scale = 1;               /* v87: internal resolution, see SC() */
static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static GLuint s_vao, s_vbo, s_ebo, s_white2d, s_whitecube;
static uint64_t s_n_draws, s_n_tris, s_n_uploads, s_n_readbacks, s_n_texup;
static uint64_t s_n_gpucopy, s_n_present_reads, s_n_cpu_taken;
static uint32_t s_frame = 1;
static uint64_t s_tm[4];               /* see nv2a_gl_times */
#if defined(_WIN32)
/* v169: rdtsc-based, in the same units (SDL's counter is QueryPerformanceCounter
 * here, ~140 ns a read under Proton); see recomp_fastclock.h */
#include "recomp_fastclock.h"
#define TM_NOW() ((uint64_t)fast_qpc())
#else
#define TM_NOW() SDL_GetPerformanceCounter()
#endif

/* v169: GL_ARRAY_BUFFER is context state (not the VAO's) and the GPU-vertex
 * draw bound it again on every draw: one driver call each. */
static GLuint s_bound_ab = (GLuint)-1;
/* v169: the generic inputs' current values as last sent (bit set = known) */
static float s_cur_attr[16][4];
static uint32_t s_cur_attr_ok;
static void bind_ab(GLuint b)
{
    if (b != s_bound_ab) {
        glBindBuffer(GL_ARRAY_BUFFER, b);
        s_bound_ab = b;
    }
}

/* Uniform locations, per program (v78: one program per vertex-shader kind).
 * The u_* names below are the current program's. */
/* v187: the presented shape (video/fb_present.c) */
void xbox_PresentFit(int cw, int ch, int *dw, int *dh);

typedef struct GlProg {
    GLuint id;
    GLint L_bmat, L_bls, L_osi;
    GLint L_scr, L_wsk, L_wsc, L_ws2d, L_vp, L_zs, L_zs2, L_zdirect, L_poff, L_rc_on, L_cicw, L_aicw, L_cocw, L_aocw, L_f0, L_f1, L_final0, L_final1, L_fc0, L_fc1, L_control, L_sp, L_clipmode, L_fogc, L_atest, L_afunc, L_aref, L_tscale, L_iscube, L_tvalid, L_vsmode, L_present, L_c, L_prog, L_nins, L_comp, L_mv, L_imv0, L_texm, L_skin, L_light, L_texm_on, L_texgen, L_texpl, L_samb, L_emis, L_malpha, L_cmat, L_lmask, L_spec, L_lamb, L_ldif, L_lspc, L_lhalf, L_ldir, L_lpos, L_latt, L_lrange, L_fog_en, L_fog_mode, L_fogp;
    GLint L_s2d[4], L_scube[4];
    /* What this program's uniforms were last set to (apply_state). */
    int ap_valid, ap_w, ap_h, ap_zdirect, ap_scr, ap_ws2d;
    float ap_wsk, ap_wsc;
    NvGlState ap;
    float ap_poff[3];
    GLint ap_isc[4], ap_tv[4];
    float ap_tsc[4][2];
    /* Vertex-shader uniforms (upload_vs). */
    uint32_t vs_const_gen, vs_present;
    int c_n;              /* v117: active size of u_c[] with consecutive locations; <= 0 unknown */
    int vs_ff_valid, vs_mode;
    int32_t vs_fog[2];
    float vs_fogp[2];
    struct NvGlFF vs_ff;
    /* For generated vertex programs: the program it was built from. */
    uint32_t key_hash, key_n;
    uint32_t (*key)[4];
    uint32_t used;
    /* v103: the vertex source, and the twin linked with the fragment shader
     * that leaves depth to GL (early depth test), built when first needed */
    const char *vsrc; int vsrc_owned;
    struct GlProg *nz;
} GlProg;
static GlProg *P;                    /* the bound program */
#define u_vp (P->L_vp)
#define u_scr (P->L_scr)
#define u_wsk (P->L_wsk)
#define u_wsc (P->L_wsc)
#define u_ws2d (P->L_ws2d)
#define u_bmat (P->L_bmat)
#define u_bls (P->L_bls)
#define u_osi (P->L_osi)
#define u_zs (P->L_zs)
#define u_zs2 (P->L_zs2)
#define u_zdirect (P->L_zdirect)
#define u_poff (P->L_poff)
#define u_rc_on (P->L_rc_on)
#define u_cicw (P->L_cicw)
#define u_aicw (P->L_aicw)
#define u_cocw (P->L_cocw)
#define u_aocw (P->L_aocw)
#define u_f0 (P->L_f0)
#define u_f1 (P->L_f1)
#define u_final0 (P->L_final0)
#define u_final1 (P->L_final1)
#define u_fc0 (P->L_fc0)
#define u_fc1 (P->L_fc1)
#define u_control (P->L_control)
#define u_sp (P->L_sp)
#define u_clipmode (P->L_clipmode)
#define u_fogc (P->L_fogc)
#define u_atest (P->L_atest)
#define u_afunc (P->L_afunc)
#define u_aref (P->L_aref)
#define u_tscale (P->L_tscale)
#define u_iscube (P->L_iscube)
#define u_tvalid (P->L_tvalid)
#define u_vsmode (P->L_vsmode)
#define u_present (P->L_present)
#define u_c (P->L_c)
#define u_prog (P->L_prog)
#define u_nins (P->L_nins)
#define u_comp (P->L_comp)
#define u_mv (P->L_mv)
#define u_imv0 (P->L_imv0)
#define u_texm (P->L_texm)
#define u_skin (P->L_skin)
#define u_light (P->L_light)
#define u_texm_on (P->L_texm_on)
#define u_texgen (P->L_texgen)
#define u_texpl (P->L_texpl)
#define u_samb (P->L_samb)
#define u_emis (P->L_emis)
#define u_malpha (P->L_malpha)
#define u_cmat (P->L_cmat)
#define u_lmask (P->L_lmask)
#define u_spec (P->L_spec)
#define u_lamb (P->L_lamb)
#define u_ldif (P->L_ldif)
#define u_lspc (P->L_lspc)
#define u_lhalf (P->L_lhalf)
#define u_ldir (P->L_ldir)
#define u_lpos (P->L_lpos)
#define u_latt (P->L_latt)
#define u_lrange (P->L_lrange)
#define u_fog_en (P->L_fog_en)
#define u_fog_mode (P->L_fog_mode)
#define u_fogp (P->L_fogp)
#define u_s2d (P->L_s2d)
#define u_scube (P->L_scube)
static GlProg s_p_old, s_p_ff;     /* passthrough, fixed function */
#define NV_GL_VSH_PROGS 96
static GlProg s_p_vsh[NV_GL_VSH_PROGS];
static uint32_t s_prog_clock;
static int s_gpu_vs;               /* the GPU vertex shaders built */
static GLuint s_vao2, s_vbo2;      /* raw vertex inputs (nv2a_gl_map_raw) */
static int s_cur_vao = 1;

static void use_prog(GlProg *p)
{
    if (P != p) {
        glUseProgram(p->id);
        P = p;
    }
}


static const char *VS_FF =
"#version 330 core\n"
"layout(location=0) in vec4 i0;   layout(location=1) in vec4 i1;\n"
"layout(location=2) in vec4 i2;   layout(location=3) in vec4 i3;\n"
"layout(location=4) in vec4 i4;   layout(location=5) in vec4 i5;\n"
"layout(location=6) in vec4 i6;   layout(location=7) in vec4 i7;\n"
"layout(location=8) in vec4 i8;   layout(location=9) in vec4 i9;\n"
"layout(location=10) in vec4 i10; layout(location=11) in vec4 i11;\n"
"layout(location=12) in vec4 i12; layout(location=13) in vec4 i13;\n"
"layout(location=14) in vec4 i14; layout(location=15) in vec4 i15;\n"
"uniform vec2 u_vp; uniform int u_scr; uniform float u_wsk; uniform float u_wsc; uniform int u_ws2d;\n"
"uniform float u_zs;\n"
"uniform uint u_present;\n"
"uniform vec4 u_c[192];\n"
"uniform mat4 u_comp; uniform mat4 u_mv[4]; uniform mat4 u_imv0; uniform mat4 u_texm[4];\n"
"uniform int u_skin; uniform int u_light; uniform int u_texm_on[4]; uniform int u_texgen[16];\n"
"uniform vec4 u_texpl[16];\n"
"uniform vec3 u_samb; uniform vec3 u_emis; uniform float u_malpha;\n"
"uniform uint u_cmat; uniform uint u_lmask; uniform int u_spec;\n"
"uniform vec3 u_lamb[8]; uniform vec3 u_ldif[8]; uniform vec3 u_lspc[8]; uniform vec3 u_lhalf[8];\n"
"uniform vec3 u_ldir[8]; uniform vec3 u_lpos[8]; uniform vec3 u_latt[8]; uniform float u_lrange[8];\n"
"uniform int u_fog_en; uniform uint u_fog_mode; uniform vec2 u_fogp;\n"
"#ifdef OLD_CLIP\n"
"out vec4 v_d0; out vec4 v_d1; out vec4 v_t0; out vec4 v_t1; out vec4 v_t2;\n"
"out vec4 v_t3; out float v_fog;\n"
"out vec2 v_zw;\n"
"#else\n"
"noperspective out vec4 q_d0; noperspective out vec4 q_d1; noperspective out vec4 q_t0;\n"
"noperspective out vec4 q_t1; noperspective out vec4 q_t2; noperspective out vec4 q_t3;\n"
"noperspective out float q_fog; noperspective out float v_iw;\n"
"#endif\n"
"noperspective out float v_z;\n"
"noperspective out float v_behind;\n"
"vec4 V[16];\n"
"bool has(int a) { return ((u_present >> uint(a)) & 1u) != 0u; }\n"
"float fogf(float d) {\n"
"  if (u_fog_en == 0) return 1.0;\n"
"  float px = u_fogp.x, py = u_fogp.y, f;\n"
"  if (u_fog_mode == 0x800u || u_fog_mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (u_fog_mode == 0x801u || u_fog_mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (u_fog_mode == 0x802u || u_fog_mode == 0x803u || u_fog_mode == 0x804u) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return clamp(f, 0.0, 1.0);\n"
"}\n"
"void light(vec4 eye, inout vec4 d0, inout vec4 d1) {\n"
"  vec4 vd = d0, vs = d1;\n"
"  uint cm = u_cmat;\n"
"  uint em_src = cm & 3u, am_src = (cm >> 2) & 3u, df_src = (cm >> 4) & 3u, sp_src = (cm >> 6) & 3u;\n"
"  vec3 nrm = has(2) ? V[2].xyz : vec3(0.0, 0.0, 1.0);\n"
"  vec3 n = (u_imv0 * vec4(nrm, 0.0)).xyz;\n"
"  float l = length(n);\n"
"  if (l > 0.0) n /= l;\n"
"  vec3 c, sp = vec3(0.0);\n"
"  for (int k = 0; k < 3; k++) {\n"
"    float e = u_emis[k], am = u_samb[k];\n"
"    if (em_src == 1u) e = vd[k]; else if (em_src == 2u) e = vs[k];\n"
"    if (am_src == 1u) am *= vd[k]; else if (am_src == 2u) am *= vs[k];\n"
"    c[k] = e + am;\n"
"  }\n"
"  for (int i = 0; i < 8; i++) {\n"
"    uint mode = (u_lmask >> uint(2 * i)) & 3u;\n"
"    if (mode == 0u) continue;\n"
"    vec3 vp, hv;\n"
"    float att = 1.0;\n"
"    if (mode == 1u) { vp = u_ldir[i]; hv = u_lhalf[i]; }\n"
"    else {\n"
"      vp = u_lpos[i] - eye.xyz;\n"
"      float d = length(vp);\n"
"      if (d > 0.0) vp /= d;\n"
"      float den = u_latt[i].x + u_latt[i].y * d + u_latt[i].z * d * d;\n"
"      att = den > 0.0 ? 1.0 / den : 1.0;\n"
"      if (u_lrange[i] > 0.0 && d > u_lrange[i]) att = 0.0;\n"
"      hv = vec3(vp.xy, vp.z + 1.0);\n"
"      float hl = length(hv);\n"
"      if (hl > 0.0) hv /= hl;\n"
"    }\n"
"    float ndl = max(dot(n, vp), 0.0), ndh = max(dot(n, hv), 0.0);\n"
"    for (int k = 0; k < 3; k++) {\n"
"      float am = u_lamb[i][k], df = u_ldif[i][k];\n"
"      if (am_src == 1u) am *= vd[k]; else if (am_src == 2u) am *= vs[k];\n"
"      if (df_src == 1u) df *= vd[k]; else if (df_src == 2u) df *= vs[k];\n"
"      c[k] += att * (am + df * ndl);\n"
"      if (u_spec != 0 && ndl > 0.0) {\n"
"        float spc = u_lspc[i][k];\n"
"        if (sp_src == 1u) spc *= vd[k]; else if (sp_src == 2u) spc *= vs[k];\n"
"        sp[k] += att * spc * pow(ndh, 10.0);\n"
"      }\n"
"    }\n"
"  }\n"
"  d0.rgb = clamp(c, 0.0, 1.0);\n"
"  d1.rgb = clamp(sp, 0.0, 1.0);\n"
"  d0.a = (df_src == 1u) ? vd.a : (df_src == 2u ? vs.a : u_malpha);\n"
"}\n"
"void run_ff(out vec4 pos, out vec4 d0, out vec4 d1, out float fz, out vec4 t[4]) {\n"
"  vec4 P = V[0];\n"
"  vec4 eye = vec4(0.0), clip;\n"
"  int nmat = 1;\n"
"  if (u_skin == 1 || u_skin == 2) nmat = 2;\n"
"  else if (u_skin == 3 || u_skin == 4) nmat = 3;\n"
"  else if (u_skin == 5 || u_skin == 6) nmat = 4;\n"
"  if (nmat > 1) {\n"
"    vec4 wt = V[1], w = vec4(1.0, 0.0, 0.0, 0.0);\n"
"    float sum = 0.0;\n"
"    for (int i = 0; i < nmat - 1; i++) { w[i] = wt[i]; sum += wt[i]; }\n"
"    w[nmat - 1] = ((u_skin & 1) != 0) ? 1.0 - sum : wt[nmat - 1];\n"
"    for (int i = 0; i < nmat; i++) eye += w[i] * (u_mv[i] * P);\n"
"    clip = u_comp * eye;\n"
"  } else {\n"
"    eye = u_mv[0] * P;\n"
"    clip = u_comp * P;\n"
"  }\n"
"  pos = vec4(0.0, 0.0, 0.0, clip.w);\n"
"  if (clip.w != 0.0) pos.xyz = clip.xyz / clip.w + u_c[59].xyz;\n"
"  d0 = has(3) ? V[3] : vec4(1.0);\n"
"  d1 = has(4) ? V[4] : vec4(0.0);\n"
"#if FF_LIGHT\n"
"  light(eye, d0, d1);\n"
"#endif\n"
"#ifdef FOG_PER_VERTEX\n"
"  fz = abs(eye.z);\n"
"#else\n"
"  /* v130: signed, so it interpolates through the eye plane; the abs is\n"
"   * taken per pixel (u_fog_en 2) */\n"
"  fz = eye.z;\n"
"#endif\n"
"#if FF_TEXGEN\n"
"  bool need_n = false;\n"
"  for (int i = 0; i < 16; i++)\n"
"    if (u_texgen[i] == 0x2402 || u_texgen[i] == 0x8511 || u_texgen[i] == 0x8512) need_n = true;\n"
"  vec3 en = vec3(0.0), refl = vec3(0.0);\n"
"  if (need_n) {\n"
"    en = (u_imv0 * vec4(V[2].xyz, 0.0)).xyz;\n"
"    float l = length(en);\n"
"    if (l > 0.0) en /= l;\n"
"    l = length(eye.xyz);\n"
"    vec3 ev = l > 0.0 ? eye.xyz / l : vec3(0.0);\n"
"    refl = ev - 2.0 * dot(en, ev) * en;\n"
"  }\n"
"#endif\n"
"  for (int i = 0; i < 4; i++) {\n"
"    vec4 tc = V[9 + i];\n"
"#if FF_TEXGEN\n"
"    for (int c = 0; c < 4; c++) {\n"
"      int g = u_texgen[i * 4 + c];\n"
"      vec4 pl = u_texpl[i * 4 + c];\n"
"      if (g == 0x2400) tc[c] = dot(pl, eye);\n"
"      else if (g == 0x2401) tc[c] = dot(pl, P);\n"
"      else if (g == 0x2402) {\n"
"        float m = 2.0 * sqrt(refl.x * refl.x + refl.y * refl.y + (refl.z + 1.0) * (refl.z + 1.0));\n"
"        if (c < 2) tc[c] = m > 0.0 ? refl[c] / m + 0.5 : 0.5;\n"
"      }\n"
"      else if (g == 0x8511) { if (c < 3) tc[c] = en[c]; }\n"
"      else if (g == 0x8512) { if (c < 3) tc[c] = refl[c]; }\n"
"    }\n"
"#endif\n"
"    if (u_texm_on[i] != 0) tc = u_texm[i] * tc;\n"
"    t[i] = tc;\n"
"  }\n"
"}\n"
"void main() {\n"
"  vec4 pos, d0, d1, t[4];\n"
"  float fog, fz;\n"
"  V[0] = i0; V[1] = i1; V[2] = i2; V[3] = i3; V[4] = i4; V[5] = i5; V[6] = i6; V[7] = i7;\n"
"  V[8] = i8; V[9] = i9; V[10] = i10; V[11] = i11; V[12] = i12; V[13] = i13; V[14] = i14;\n"
"  V[15] = i15;\n"
"  run_ff(pos, d0, d1, fz, t);\n"
"#ifdef FOG_PER_VERTEX\n"
"  fog = fogf(fz);\n"
"#else\n"
"  /* v130: the fog coordinate itself, turned into the factor per pixel */\n"
"  fog = (u_fog_en != 0) ? (isnan(fz) ? 1.0e30 : fz) : 1.0;\n"
"#endif\n"
"  v_behind = (pos.w > 0.0) ? 0.0 : 1.0e6;\n"
"#ifdef OLD_CLIP\n"
"  float w = pos.w;\n"
"  gl_Position = vec4((pos.x * (2.0 / u_vp.x) - 1.0) * w,\n"
"                     (1.0 - pos.y * (2.0 / u_vp.y)) * w,\n"
"                     (pos.z * u_zs * 2.0 - 1.0) * w, w);\n"
"  v_zw = vec2(pos.z * u_zs * w, w);\n"
"  if (u_scr != 0) {\n"
"    /* screen space as given: flat, no w clip; depth from z alone */\n"
"    gl_Position = vec4(pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - pos.y * (2.0 / u_vp.y),\n"
"                       pos.z * u_zs * 2.0 - 1.0, 1.0);\n"
"    v_zw = vec2(pos.z * u_zs, 1.0);\n"
"  }\n"
"#ifndef NO_ZCULL\n"
"  /* v103: the depth-range cull (v96) and the near-w cull as clip\n"
"   * distances -- linear in clip space, so exactly the true-depth test the\n"
"   * fragment shader did, without the discard that cost the early depth\n"
"   * test */\n"
"  gl_ClipDistance[0] = v_zw.x + 1.0e-4 * v_zw.y;\n"
"  gl_ClipDistance[1] = 1.0001 * v_zw.y - v_zw.x;\n"
"  gl_ClipDistance[2] = v_zw.y - 1.0e-3;\n"
"#endif\n"
"  v_d0 = d0; v_d1 = d1; v_t0 = t[0]; v_t1 = t[1]; v_t2 = t[2];\n"
"  v_t3 = t[3]; v_fog = fog;\n"
"#else\n"
"  float w = pos.w;\n"
"  vec3 ndc = vec3(pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - pos.y * (2.0 / u_vp.y),\n"
"                  pos.z * u_zs * 2.0 - 1.0);\n"
"  float iw = (w > 0.0 || w < 0.0) ? 1.0 / w : 1.0;\n"
"  gl_Position = (w < 0.0) ? vec4(ndc * w, w) : vec4(ndc, 1.0);\n"
"  v_iw = iw;\n"
"  q_d0 = d0 * iw; q_d1 = d1 * iw; q_t0 = t[0] * iw; q_t1 = t[1] * iw;\n"
"  q_t2 = t[2] * iw; q_t3 = t[3] * iw; q_fog = fog * iw;\n"
"#endif\n"
"  /* v187: widescreen: x squeezed by u_wsk (1 = none) about u_wsc, the\n"
"   * drawing area's centre in NDC (v187l); which batches, the executor\n"
"   * decides (ws_k_for) */\n"
"  gl_Position.x = gl_Position.x * u_wsk + u_wsc * (1.0 - u_wsk) * gl_Position.w;\n"
"  v_z = pos.z;\n"
"}\n"
"\n";
static const char *VSH_HEAD =
"#version 330 core\n"
"layout(location=0) in vec4 i0;   layout(location=1) in vec4 i1;\n"
"layout(location=2) in vec4 i2;   layout(location=3) in vec4 i3;\n"
"layout(location=4) in vec4 i4;   layout(location=5) in vec4 i5;\n"
"layout(location=6) in vec4 i6;   layout(location=7) in vec4 i7;\n"
"layout(location=8) in vec4 i8;   layout(location=9) in vec4 i9;\n"
"layout(location=10) in vec4 i10; layout(location=11) in vec4 i11;\n"
"layout(location=12) in vec4 i12; layout(location=13) in vec4 i13;\n"
"layout(location=14) in vec4 i14; layout(location=15) in vec4 i15;\n"
"uniform vec2 u_vp; uniform int u_scr; uniform float u_wsk; uniform float u_wsc; uniform int u_ws2d;\n"
"uniform float u_zs;\n"
"uniform vec4 u_c[192];\n"
"uniform int u_fog_en; uniform uint u_fog_mode; uniform vec2 u_fogp;\n"
"#ifdef OLD_CLIP\n"
"out vec4 v_d0; out vec4 v_d1; out vec4 v_t0; out vec4 v_t1; out vec4 v_t2;\n"
"out vec4 v_t3; out float v_fog;\n"
"out vec2 v_zw;\n"
"#else\n"
"noperspective out vec4 q_d0; noperspective out vec4 q_d1; noperspective out vec4 q_t0;\n"
"noperspective out vec4 q_t1; noperspective out vec4 q_t2; noperspective out vec4 q_t3;\n"
"noperspective out float q_fog; noperspective out float v_iw;\n"
"#endif\n"
"noperspective out float v_z;\n"
"noperspective out float v_behind;\n"
"float fogf(float d) {\n"
"  if (u_fog_en == 0) return 1.0;\n"
"  float px = u_fogp.x, py = u_fogp.y, f;\n"
"  if (u_fog_mode == 0x800u || u_fog_mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (u_fog_mode == 0x801u || u_fog_mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (u_fog_mode == 0x802u || u_fog_mode == 0x803u || u_fog_mode == 0x804u) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return clamp(f, 0.0, 1.0);\n"
"}\n"
"float finf() { return uintBitsToFloat(0x7F800000u); }\n"
"vec4 ucon(int i) { return (i < 0 || i >= 192) ? u_c[0] : u_c[i]; }\n"
"float rcc_(float x) {\n"
"  float f = 1.0 / x, af = abs(f);\n"
"  if (af < 5.42101e-20) f = (f < 0.0) ? -5.42101e-20 : 5.42101e-20;\n"
"  if (af > 1.884467e19) f = (f < 0.0) ? -1.884467e19 : 1.884467e19;\n"
"  return f;\n"
"}\n"
"vec4 exp_(float x) { float fl = floor(x); return vec4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"vec4 log_(float x) {\n"
"  float ax = abs(x);\n"
"  if (ax == 0.0) return vec4(-finf(), 1.0, -finf(), 1.0);\n"
"  float e = floor(log2(ax));\n"
"  return vec4(e, ax / exp2(e), log2(ax), 1.0);\n"
"}\n"
"vec4 lit_(vec4 c) {\n"
"  float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
"  float p = clamp(c.w, -127.9961, 127.9961);\n"
"  return vec4(1.0, nl, c.x > 0.0 ? pow(nh, p) : 0.0, 1.0);\n"
"}\n"
"vec4 min_(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
"vec4 max_(vec4 a, vec4 b) { return vec4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n"
"vec4 slt_(vec4 a, vec4 b) { return vec4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n"
"vec4 sge_(vec4 a, vec4 b) { return vec4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n"
"void main() {\n"
"  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0), R4 = vec4(0.0);\n"
"  vec4 R5 = vec4(0.0), R6 = vec4(0.0), R7 = vec4(0.0), R8 = vec4(0.0), R9 = vec4(0.0);\n"
"  vec4 R10 = vec4(0.0), R11 = vec4(0.0), R12 = vec4(0.0);\n"
"  vec4 O3 = vec4(0.0, 0.0, 0.0, 1.0), O4 = vec4(0.0, 0.0, 0.0, 1.0), O5 = vec4(0.0);\n"
"  vec4 O1 = vec4(0.0), O2 = vec4(0.0), O6 = vec4(0.0), O7 = vec4(0.0), O8 = vec4(0.0);\n"
"  vec4 O9 = vec4(0.0, 0.0, 0.0, 1.0), O10 = vec4(0.0, 0.0, 0.0, 1.0);\n"
"  vec4 O11 = vec4(0.0, 0.0, 0.0, 1.0), O12 = vec4(0.0, 0.0, 0.0, 1.0);\n"
"  int a0 = 0;\n"
"  vec4 pos, d0, d1, t[4];\n"
"  float fog, fz;\n"
"\n";
static const char *VSH_TAIL =
"  pos = R12; d0 = O3; d1 = O4; fz = O5.x;\n"
"  t[0] = O9; t[1] = O10; t[2] = O11; t[3] = O12;\n"
"#ifdef FOG_PER_VERTEX\n"
"  fog = fogf(fz);\n"
"#else\n"
"  /* v130: the fog coordinate itself, turned into the factor per pixel */\n"
"  fog = (u_fog_en != 0) ? (isnan(fz) ? 1.0e30 : fz) : 1.0;\n"
"#endif\n"
"  v_behind = (pos.w > 0.0) ? 0.0 : 1.0e6;\n"
"#ifdef OLD_CLIP\n"
"  float w = pos.w;\n"
"  gl_Position = vec4((pos.x * (2.0 / u_vp.x) - 1.0) * w,\n"
"                     (1.0 - pos.y * (2.0 / u_vp.y)) * w,\n"
"                     (pos.z * u_zs * 2.0 - 1.0) * w, w);\n"
"  v_zw = vec2(pos.z * u_zs * w, w);\n"
"  if (u_scr != 0) {\n"
"    /* screen space as given: flat, no w clip; depth from z alone */\n"
"    gl_Position = vec4(pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - pos.y * (2.0 / u_vp.y),\n"
"                       pos.z * u_zs * 2.0 - 1.0, 1.0);\n"
"    v_zw = vec2(pos.z * u_zs, 1.0);\n"
"  }\n"
"#ifndef NO_ZCULL\n"
"  /* v103: the depth-range cull (v96) and the near-w cull as clip\n"
"   * distances -- linear in clip space, so exactly the true-depth test the\n"
"   * fragment shader did, without the discard that cost the early depth\n"
"   * test */\n"
"  gl_ClipDistance[0] = v_zw.x + 1.0e-4 * v_zw.y;\n"
"  gl_ClipDistance[1] = 1.0001 * v_zw.y - v_zw.x;\n"
"  gl_ClipDistance[2] = v_zw.y - 1.0e-3;\n"
"#endif\n"
"  v_d0 = d0; v_d1 = d1; v_t0 = t[0]; v_t1 = t[1]; v_t2 = t[2];\n"
"  v_t3 = t[3]; v_fog = fog;\n"
"#else\n"
"  float w = pos.w;\n"
"  vec3 ndc = vec3(pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - pos.y * (2.0 / u_vp.y),\n"
"                  pos.z * u_zs * 2.0 - 1.0);\n"
"  float iw = (w > 0.0 || w < 0.0) ? 1.0 / w : 1.0;\n"
"  gl_Position = (w < 0.0) ? vec4(ndc * w, w) : vec4(ndc, 1.0);\n"
"  v_iw = iw;\n"
"  q_d0 = d0 * iw; q_d1 = d1 * iw; q_t0 = t[0] * iw; q_t1 = t[1] * iw;\n"
"  q_t2 = t[2] * iw; q_t3 = t[3] * iw; q_fog = fog * iw;\n"
"#endif\n"
"  /* v187: widescreen: x squeezed by u_wsk (1 = none) about u_wsc, the\n"
"   * drawing area's centre in NDC (v187l); which batches, the executor\n"
"   * decides (ws_k_for) */\n"
"  gl_Position.x = gl_Position.x * u_wsk + u_wsc * (1.0 - u_wsk) * gl_Position.w;\n"
"  v_z = pos.z;\n"
"}\n"
"\n";

/* v77: the vertex shader before GPU vertex processing, kept as the fallback
 * when the new one does not build on a driver. */
static const char *VS_OLD =
"#version 330 core\n"
"layout(location=0) in vec4 a_pos;\n"
"layout(location=1) in vec4 a_d0;\n"
"layout(location=2) in vec4 a_d1;\n"
"layout(location=3) in vec4 a_t0;\n"
"layout(location=4) in vec4 a_t1;\n"
"layout(location=5) in vec4 a_t2;\n"
"layout(location=6) in vec4 a_t3;\n"
"layout(location=7) in float a_fog;\n"
"uniform vec2 u_vp; uniform int u_scr; uniform float u_wsk; uniform float u_wsc; uniform int u_ws2d;\n"
"uniform float u_zs;\n"
"#ifdef OLD_CLIP\n"
"out vec4 v_d0; out vec4 v_d1; out vec4 v_t0; out vec4 v_t1; out vec4 v_t2;\n"
"out vec4 v_t3; out float v_fog;\n"
"out vec2 v_zw;\n"
"#else\n"
"noperspective out vec4 q_d0; noperspective out vec4 q_d1; noperspective out vec4 q_t0;\n"
"noperspective out vec4 q_t1; noperspective out vec4 q_t2; noperspective out vec4 q_t3;\n"
"noperspective out float q_fog; noperspective out float v_iw;\n"
"#endif\n"
"noperspective out float v_z;\n"
"noperspective out float v_behind;\n"
"void main() {\n"
"#ifdef OLD_CLIP\n"
"  float w = a_pos.w;\n"
"  gl_Position = vec4((a_pos.x * (2.0 / u_vp.x) - 1.0) * w,\n"
"                     (1.0 - a_pos.y * (2.0 / u_vp.y)) * w,\n"
"                     (a_pos.z * u_zs * 2.0 - 1.0) * w, w);\n"
"  v_zw = vec2(a_pos.z * u_zs * w, w);\n"
"  if (u_scr != 0) {\n"
"    /* screen space as given: flat, no w clip; depth from z alone */\n"
"    gl_Position = vec4(a_pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - a_pos.y * (2.0 / u_vp.y),\n"
"                       a_pos.z * u_zs * 2.0 - 1.0, 1.0);\n"
"    v_zw = vec2(a_pos.z * u_zs, 1.0);\n"
"  }\n"
"#ifndef NO_ZCULL\n"
"  /* v103: the depth-range cull (v96) and the near-w cull as clip\n"
"   * distances -- linear in clip space, so exactly the true-depth test the\n"
"   * fragment shader did, without the discard that cost the early depth\n"
"   * test */\n"
"  gl_ClipDistance[0] = v_zw.x + 1.0e-4 * v_zw.y;\n"
"  gl_ClipDistance[1] = 1.0001 * v_zw.y - v_zw.x;\n"
"  gl_ClipDistance[2] = v_zw.y - 1.0e-3;\n"
"#endif\n"
"  v_d0 = a_d0; v_d1 = a_d1; v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2;\n"
"  v_t3 = a_t3; v_fog = a_fog;\n"
"#else\n"
"  float w = a_pos.w;\n"
"  vec3 ndc = vec3(a_pos.x * (2.0 / u_vp.x) - 1.0, 1.0 - a_pos.y * (2.0 / u_vp.y),\n"
"                  a_pos.z * u_zs * 2.0 - 1.0);\n"
"  float iw = (w > 0.0 || w < 0.0) ? 1.0 / w : 1.0;\n"
"  gl_Position = (w < 0.0) ? vec4(ndc * w, w) : vec4(ndc, 1.0);\n"
"  v_iw = iw;\n"
"  q_d0 = a_d0 * iw; q_d1 = a_d1 * iw; q_t0 = a_t0 * iw; q_t1 = a_t1 * iw;\n"
"  q_t2 = a_t2 * iw; q_t3 = a_t3 * iw; q_fog = a_fog * iw;\n"
"#endif\n"
"  /* v187: widescreen: x squeezed by u_wsk (1 = none) about u_wsc, the\n"
"   * drawing area's centre in NDC (v187l); which batches, the executor\n"
"   * decides (ws_k_for) */\n"
"  gl_Position.x = gl_Position.x * u_wsk + u_wsc * (1.0 - u_wsk) * gl_Position.w;\n"
"  v_z = a_pos.z;\n"
"  v_behind = 0.0;\n"
"}\n";

/* The register combiners, a line-for-line port of nv2a_rc_eval and
 * rc_stage_fetch in the executor, so the two renderers agree. */
static const char *FS =
"#version 330 core\n"
"#ifdef OLD_CLIP\n"
"in vec4 v_d0; in vec4 v_d1; in vec4 v_t0; in vec4 v_t1; in vec4 v_t2;\n"
"in vec4 v_t3; in float v_fog;\n"
"in vec2 v_zw;\n"
"#else\n"
"noperspective in vec4 q_d0; noperspective in vec4 q_d1; noperspective in vec4 q_t0;\n"
"noperspective in vec4 q_t1; noperspective in vec4 q_t2; noperspective in vec4 q_t3;\n"
"noperspective in float q_fog; noperspective in float v_iw;\n"
"vec4 v_d0, v_d1, v_t0, v_t1, v_t2, v_t3; float v_fog;\n"
"#endif\n"
"noperspective in float v_z;\n"
"noperspective in float v_behind;\n"
"uniform float u_zs2; uniform int u_zdirect; uniform vec3 u_poff;\n"
"#ifndef ZTOL\n#define ZTOL 1.0e-3\n#endif\n"
"const float u_ztol = ZTOL;\n"
"out vec4 o_col;\n"
"uniform int u_rc_on;\n"
"uniform uint u_cicw[8]; uniform uint u_aicw[8];\n"
"uniform uint u_cocw[8]; uniform uint u_aocw[8];\n"
"uniform vec4 u_f0[8]; uniform vec4 u_f1[8];\n"
"uniform uint u_final0; uniform uint u_final1;\n"
"uniform vec4 u_fc0; uniform vec4 u_fc1;\n"
"uniform uint u_control; uniform uint u_sp; uniform uint u_clipmode;\n"
"uniform vec4 u_fogc;\n"
"uniform int u_fog_en; uniform uint u_fog_mode; uniform vec2 u_fogp;\n"
"/* v130: the NV2A applies its fog function per pixel to the interpolated\n"
" * fog coordinate. Done per vertex and clamped there, a big surface whose\n"
" * corners are all far away (JSRF's river, one quad) came out fogged\n"
" * everywhere, near parts included: a flat haze-coloured river. */\n"
"float fogf_px(float d) {\n"
"  float px = u_fogp.x, py = u_fogp.y, f;\n"
"  if (u_fog_mode == 0x800u || u_fog_mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (u_fog_mode == 0x801u || u_fog_mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (u_fog_mode == 0x802u || u_fog_mode == 0x803u || u_fog_mode == 0x804u) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return clamp(f, 0.0, 1.0);\n"
"}\n"
"uniform int u_atest; uniform uint u_afunc; uniform int u_aref;\n"
"uniform vec2 u_tscale[4]; uniform int u_iscube[4]; uniform int u_tvalid[4];\n"
"uniform vec4 u_bmat[4]; uniform vec2 u_bls[4]; uniform uint u_osi;\n"
"uniform sampler2D u_s0; uniform sampler2D u_s1;\n"
"uniform sampler2D u_s2; uniform sampler2D u_s3;\n"
"uniform samplerCube u_c0; uniform samplerCube u_c1;\n"
"uniform samplerCube u_c2; uniform samplerCube u_c3;\n"
"vec4 R[16];\n"
"bool g_kill;\n"
"vec4 fetch(int st, vec4 c, sampler2D s2, samplerCube sc) {\n"
"  uint mode = (u_sp >> uint(st * 5)) & 31u;\n"
"  if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);\n"
"  if (mode == 1u || mode == 2u) {\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    return texture(s2, c.xy / q * u_tscale[st]);\n"
"  }\n"
"  if (mode == 3u) {\n"
"    if (u_iscube[st] != 0) return texture(sc, c.xyz);\n"
"    return texture(s2, c.xy * u_tscale[st]);\n"
"  }\n"
"  if (mode == 4u) return clamp(c, 0.0, 1.0);\n"
"  if (mode == 5u) {\n"
"    for (int j = 0; j < 4; j++) {\n"
"      bool ge = ((u_clipmode >> uint(st * 4 + j)) & 1u) != 0u;\n"
"      if (ge ? c[j] >= 0.0 : c[j] < 0.0) g_kill = true;\n"
"    }\n"
"    return vec4(0.0);\n"
"  }\n"
"  return vec4(0.0);\n"
"}\n"
"/* v104: bump environment mapping (stage modes 6 and 7), as xemu's psh.c:\n"
" * the previous stage's texel read as a signed (du, dv) from blue and\n"
" * green, turned by the stage's 2x2 matrix, offsets this stage's\n"
" * coordinates; mode 7 also scales by luminance */\n"
"float sgn3(float x) { x *= 255.0; return (x >= 128.0 ? x - 256.0 : x) / 127.0; }\n"
"vec4 bump(int st, vec4 src, vec4 c, sampler2D s2) {\n"
"  vec2 d = vec2(sgn3(src.b), sgn3(src.g));\n"
"  vec4 m = u_bmat[st];\n"
"  d = vec2(m.x * d.x + m.z * d.y, m.y * d.x + m.w * d.y);\n"
"#ifdef BUMP_OLD_LOD\n"
"  vec4 r = texture(s2, (c.xy + d) * u_tscale[st]);\n"
"#else\n"
"  /* v129: the mip level from the stage's own coordinates, not the\n"
"   * perturbed ones: the noise in (du, dv) changes from pixel to pixel, and\n"
"   * counted into the derivatives it pushed the lookup to the smallest mips,\n"
"   * one flat colour (JSRF's river at low camera angles) */\n"
"  vec2 uv0 = c.xy * u_tscale[st];\n"
"  vec4 r = textureGrad(s2, (c.xy + d) * u_tscale[st], dFdx(uv0), dFdy(uv0));\n"
"#endif\n"
"  if (((u_sp >> uint(st * 5)) & 31u) == 7u)\n"
"    r.rgb *= clamp(src.r * u_bls[st].x + u_bls[st].y, 0.0, 1.0);\n"
"  return r;\n"
"}\n"
"bool is_bump(int st) { uint m = (u_sp >> uint(st * 5)) & 31u; return m == 6u || m == 7u; }\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0);\n"
"  uint m = (b >> 5) & 7u;\n"
"  if (m == 0u) return p;\n"
"  if (m == 1u) return 1.0 - clamp(x, 0.0, 1.0);\n"
"  if (m == 2u) return 2.0 * p - 1.0;\n"
"  if (m == 3u) return -2.0 * p + 1.0;\n"
"  if (m == 4u) return p - 0.5;\n"
"  if (m == 5u) return -p + 0.5;\n"
"  if (m == 6u) return x;\n"
"  return -x;\n"
"}\n"
"float map_out(float x, uint m) {\n"
"  if (m == 0x08u) return x - 0.5;\n"
"  if (m == 0x10u) return x * 2.0;\n"
"  if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20u) return x * 4.0;\n"
"  if (m == 0x30u) return x * 0.5;\n"
"  return x;\n"
"}\n"
"vec3 rd_rgb(uint b) {\n"
"  vec4 r = R[int(b & 15u)];\n"
"  vec3 v = ((b & 0x10u) != 0u) ? vec3(r.a) : r.rgb;\n"
"  return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float rd_a(uint b) {\n"
"  vec4 r = R[int(b & 15u)];\n"
"  return map_in(((b & 0x10u) != 0u) ? r.a : r.b, b);\n"
"}\n"
"void main() {\n"
"#ifndef OLD_CLIP\n"
"  {\n"
"    float rw = 1.0 / v_iw;\n"
"    v_d0 = q_d0 * rw; v_d1 = q_d1 * rw; v_t0 = q_t0 * rw; v_t1 = q_t1 * rw;\n"
"    v_t2 = q_t2 * rw; v_t3 = q_t3 * rw; v_fog = q_fog * rw;\n"
"  }\n"
"#endif\n"
"  g_kill = false;\n"
"  vec4 t[4];\n"
"  t[0] = u_tvalid[0] != 0 || ((u_sp & 31u) >= 4u) ? fetch(0, v_t0, u_s0, u_c0) : vec4(1.0);\n"
"  t[1] = u_tvalid[1] != 0 || (((u_sp >> 5) & 31u) >= 4u) ? fetch(1, v_t1, u_s1, u_c1) : vec4(1.0);\n"
"  t[2] = u_tvalid[2] != 0 || (((u_sp >> 10) & 31u) >= 4u) ? fetch(2, v_t2, u_s2, u_c2) : vec4(1.0);\n"
"  t[3] = u_tvalid[3] != 0 || (((u_sp >> 15) & 31u) >= 4u) ? fetch(3, v_t3, u_s3, u_c3) : vec4(1.0);\n"
"  for (int i = 0; i < 4; i++) {\n"
"    uint mode = (u_sp >> uint(i * 5)) & 31u;\n"
"    if (mode == 0u) t[i] = vec4(0.0, 0.0, 0.0, 1.0);\n"
"  }\n"
"  if (is_bump(1) && u_tvalid[1] != 0) t[1] = bump(1, t[0], v_t1, u_s1);\n"
"  if (is_bump(2) && u_tvalid[2] != 0) {\n"
"    uint k = (u_osi >> 16) & 3u;\n"
"    t[2] = bump(2, k == 1u ? t[1] : t[0], v_t2, u_s2);\n"
"  }\n"
"  if (is_bump(3) && u_tvalid[3] != 0) {\n"
"    uint k = (u_osi >> 20) & 3u;\n"
"    t[3] = bump(3, k == 2u ? t[2] : (k == 1u ? t[1] : t[0]), v_t3, u_s3);\n"
"  }\n"
"  if (g_kill) discard;\n"
"  vec4 outc;\n"
"#ifdef SIMPLE_FS\n"
"  outc = v_d0;\n"
"  if (u_tvalid[0] != 0) outc *= t[0];\n"
"#else\n"
"  if (u_rc_on == 0) {\n"
"    outc = v_d0;\n"
"    if (u_tvalid[0] != 0) outc *= texture(u_s0, v_t0.xy * u_tscale[0]);\n"
"  } else {\n"
"    for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
"#ifdef FOG_PER_VERTEX\n"
"    R[3] = vec4(u_fogc.rgb, v_fog);\n"
"#else\n"
"    R[3] = vec4(u_fogc.rgb, u_fog_en != 0 ? fogf_px(u_fog_en == 2 ? abs(v_fog) : v_fog) : v_fog);\n"
"#endif\n"
"    R[4] = v_d0; R[5] = v_d1;\n"
"    R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"    R[12].a = ((u_sp & 31u) != 0u) ? t[0].a : 1.0;\n"
"    uint n = min(u_control & 0xFFu, 8u);\n"
"    uint flags = u_control >> 8;\n"
"    for (uint s = 0u; s < n; s++) {\n"
"      uint icw = u_cicw[s], ocw = u_cocw[s], aicw = u_aicw[s], aocw = u_aocw[s];\n"
"      uint fl = ocw >> 12, afl = aocw >> 12;\n"
"      R[1] = u_f0[((flags & 0x010u) != 0u) ? s : 0u];\n"
"      R[2] = u_f1[((flags & 0x100u) != 0u) ? s : 0u];\n"
"      bool mux_cd = ((flags & 1u) != 0u) ? (R[12].a >= 0.5)\n"
"                  : ((int(R[12].a * 255.0) & 1) != 0);\n"
"      vec3 A = rd_rgb(icw >> 24), B = rd_rgb(icw >> 16);\n"
"      vec3 C = rd_rgb(icw >> 8),  D = rd_rgb(icw);\n"
"      float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16);\n"
"      float aC = rd_a(aicw >> 8),  aD = rd_a(aicw);\n"
"      float dab = dot(A, B), dcd = dot(C, D);\n"
"      vec3 ab, cd, ms;\n"
"      for (int i = 0; i < 3; i++) {\n"
"        float pab = ((fl & 2u) != 0u) ? dab : A[i] * B[i];\n"
"        float pcd = ((fl & 1u) != 0u) ? dcd : C[i] * D[i];\n"
"        float sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;\n"
"        ab[i] = clamp(map_out(pab, fl & 0x38u), -1.0, 1.0);\n"
"        cd[i] = clamp(map_out(pcd, fl & 0x38u), -1.0, 1.0);\n"
"        ms[i] = clamp(map_out(sum, fl & 0x38u), -1.0, 1.0);\n"
"      }\n"
"      float apab = aA * aB, apcd = aC * aD;\n"
"      float asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;\n"
"      float aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);\n"
"      float acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);\n"
"      float ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);\n"
"      uint d;\n"
"      d = (ocw >> 4) & 15u;\n"
"      if (d != 0u) { R[int(d)].rgb = ab; if ((fl & 0x80u) != 0u) R[int(d)].a = ab.b; }\n"
"      d = ocw & 15u;\n"
"      if (d != 0u) { R[int(d)].rgb = cd; if ((fl & 0x40u) != 0u) R[int(d)].a = cd.b; }\n"
"      d = (ocw >> 8) & 15u;\n"
"      if (d != 0u) R[int(d)].rgb = ms;\n"
"      d = (aocw >> 4) & 15u; if (d != 0u) R[int(d)].a = aab;\n"
"      d = aocw & 15u;        if (d != 0u) R[int(d)].a = acd;\n"
"      d = (aocw >> 8) & 15u; if (d != 0u) R[int(d)].a = ams;\n"
"      R[0] = vec4(0.0);\n"
"    }\n"
"    if (u_final0 != 0u || u_final1 != 0u) {\n"
"      uint ff = u_final1 & 0xFFu;\n"
"      R[1] = u_fc0; R[2] = u_fc1;\n"
"      vec3 a = ((ff & 0x40u) != 0u) ? vec3(1.0) - R[5].rgb : R[5].rgb;\n"
"      vec3 b = ((ff & 0x20u) != 0u) ? vec3(1.0) - R[12].rgb : R[12].rgb;\n"
"      R[14] = vec4(((ff & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);\n"
"      vec3 E = rd_rgb(u_final1 >> 24), F = rd_rgb(u_final1 >> 16);\n"
"      R[15] = vec4(E * F, 0.0);\n"
"      vec3 A = rd_rgb(u_final0 >> 24), B = rd_rgb(u_final0 >> 16);\n"
"      vec3 C = rd_rgb(u_final0 >> 8),  D = rd_rgb(u_final0);\n"
"      outc.rgb = D + C * (vec3(1.0) - A) + B * A;\n"
"      outc.a = rd_a(u_final1 >> 8);\n"
"    } else {\n"
"      outc = R[12];\n"
"    }\n"
"  }\n"
"#endif\n"
"  outc = clamp(outc, 0.0, 1.0);\n"
"  if (u_atest != 0) {\n"
"    int v = int(outc.a * 255.0 + 0.5);\n"
"    bool pass = true;\n"
"    if (u_afunc == 0x200u) pass = false;\n"
"    else if (u_afunc == 0x201u) pass = v <  u_aref;\n"
"    else if (u_afunc == 0x202u) pass = v == u_aref;\n"
"    else if (u_afunc == 0x203u) pass = v <= u_aref;\n"
"    else if (u_afunc == 0x204u) pass = v >  u_aref;\n"
"    else if (u_afunc == 0x205u) pass = v != u_aref;\n"
"    else if (u_afunc == 0x206u) pass = v >= u_aref;\n"
"    if (!pass) discard;\n"
"  }\n"
"  o_col = outc;\n"
"#ifdef NZ_MARK\n  o_col = vec4(1.0, 0.0, 1.0, 1.0);\n#endif\n"
"#if !defined(NO_ZCULL) && !defined(OLD_CLIP)\n"
"  /* v96: NV097_SET_ZMIN_MAX_CONTROL CULL_NEAR_FAR (JSRF sets 1, the D3D\n"
"   * default): the chip drops a pixel whose depth falls outside 0..zmax, with\n"
"   * z interpolated linearly on screen -- as the software rasteriser does,\n"
"   * whose result for the traced beam (14 pixels) is the reference. */\n"
"  {\n"
"#ifdef OLD_CLIP\n"
"    /* the true depth here, z/w from perspective-interpolated clip-space z\n"
"     * and w -- right even on a triangle GL clipped, where screen z\n"
"     * interpolated on screen is not (the traced beam's near-plane slivers) */\n"
"    float zn = v_zw.x / v_zw.y;\n"
"    /* and the software rasteriser's near plane, w > 0.001: nothing real is\n"
"     * that close to the eye, and what lies along w = 0 projects to lines\n"
"     * across the whole screen (the beam's last slivers, run 96) */\n"
"    if (v_zw.y < 1.0e-3) discard;\n"
"#else\n"
"    float zn = v_z * u_zs2;\n"
"#endif\n"
"    if (zn < -1.0e-4 || zn > 1.0001) discard;\n"
"  }\n"
"#endif\n"
"  if (u_zdirect != 0 && v_behind <= 0.0) {\n"
"    float z = v_z;\n"
"    if (u_poff.x != 0.0)\n"
"      z += u_poff.y * max(abs(dFdx(v_z)), abs(dFdy(v_z))) + u_poff.z;\n"
"    /* v100: only while it agrees with the true depth here. On a triangle\n"
"     * GL clipped, the flags and screen z interpolated on screen can come\n"
"     * out as anything (run 99: the sky dome writing depth 0 over the\n"
"     * sewer floor, which then lost the depth test in rows). */\n"
"#ifdef OLD_CLIP\n"
"    float zt = v_zw.x / v_zw.y;\n"
"#else\n"
"    float zt = gl_FragCoord.z;\n"
"#endif\n"
"    gl_FragDepth = (abs(v_z * u_zs2 - zt) < u_ztol) ? clamp(z * u_zs2, 0.0, 1.0)\n"
"                                                     : gl_FragCoord.z;\n"
"  } else {\n"
"    gl_FragDepth = gl_FragCoord.z;\n"
"  }\n"
"}\n";

/* v94: RECOMP_GL_OLD_CLIP=1 builds every shader with OLD_CLIP defined: the
 * pre-v94 way of handing positions to GL (see the vertex shader tails). */
static int old_clip(void)
{
    /* v95: v94's screen-space positions made the spotlight band worse
     * (run 94), so the old way is the default again; RECOMP_GL_XEMU_CLIP=1
     * keeps the v94 path for comparison. */
    static int v = -1;
    if (v < 0)
        v = getenv("RECOMP_GL_XEMU_CLIP") == NULL;
    return v;
}

/* v96: RECOMP_GL_NO_ZCULL=1 keeps pixels outside the depth range (pre-v96) */
static int no_zcull(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("RECOMP_GL_NO_ZCULL") != NULL;
    return v;
}

/* v100: RECOMP_GL_ZDIRECT_CHECK=0 writes the title's depth even where it
 * disagrees with the true depth (pre-v100) */
static int no_zcheck(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_ZDIRECT_CHECK");
        v = e && !strcmp(e, "0");
    }
    return v;
}

/* v104, diagnostic: RECOMP_GL_SIMPLE_FS=1 replaces the register-combiner
 * part of the fragment shader with diffuse * texture 0 (wrong colours), to
 * measure what that part costs the GPU */
static int simple_fs(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("RECOMP_GL_SIMPLE_FS") != NULL;
    return v;
}

/* v129: RECOMP_GL_BUMP_LOD=0 picks a bump-mapped stage's mip level from the
 * perturbed coordinates (pre-v129) */
static int bump_old(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_BUMP_LOD");
        v = e && !strcmp(e, "0");
        fprintf(stderr, "[GL] bump-mapped stages: mip level from %s (RECOMP_GL_BUMP_LOD)\n",
                v ? "the perturbed coordinates" : "the unperturbed coordinates");
    }
    return v;
}

/* v130: RECOMP_GL_FOG_VERTEX=1 works the fog factor out per vertex (pre-v130) */
static int fog_vertex(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_FOG_VERTEX");
        v = e && e[0] == '1';
        fprintf(stderr, "[GL] fog factor per %s (RECOMP_GL_FOG_VERTEX)\n", v ? "vertex" : "pixel");
    }
    return v;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    const char *srcs[3];
    const char *nl = strchr(src, '\n');
    if ((old_clip() || no_zcull() || no_zcheck() || simple_fs() || bump_old() || fog_vertex()) && nl && !strncmp(src, "#version", 8)) {
        /* after the #version line, which has to come first */
        static char ver[64];
        size_t n = (size_t)(nl - src) + 1;
        if (n >= sizeof ver) n = sizeof ver - 1;
        memcpy(ver, src, n); ver[n] = 0;
        srcs[0] = ver;
        {
            static char defs[160];
            snprintf(defs, sizeof defs, "%s%s%s%s%s%s", old_clip() ? "#define OLD_CLIP 1\n" : "",
                     no_zcull() ? "#define NO_ZCULL 1\n" : "",
                     no_zcheck() ? "#define ZTOL 1.0e30\n" : "",
                     simple_fs() ? "#define SIMPLE_FS 1\n" : "",
                     bump_old() ? "#define BUMP_OLD_LOD 1\n" : "",
                     fog_vertex() ? "#define FOG_PER_VERTEX 1\n" : "");
            srcs[1] = defs;
        }
        srcs[2] = nl + 1;
        glShaderSource(sh, 3, srcs, NULL);
    } else {
        glShaderSource(sh, 1, &src, NULL);
    }
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[GL] %s shader failed to compile:\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* v79: linked programs saved to disk (shader_cache\, next to the game) and
 * loaded back with glProgramBinary. Compiling a shader takes the AMD driver
 * tens to hundreds of milliseconds, part of it at the first draw, and every
 * new vertex program the game uses meant one such hitch (run 78: the big
 * one-off stutters). With the cache, a program compiled once is loaded in
 * about a millisecond on every later run. Keyed by the GL renderer and
 * version and both shader sources, so a driver update just rebuilds.
 * RECOMP_GL_NO_SHADER_CACHE=1 turns it off. */
static unsigned s_cache_hits, s_cache_misses;
static uint64_t s_tm_compile;          /* ticks spent compiling and linking */

static int shader_cache_on(void)
{
    static int on = -1;
    if (on < 0) {
        GLint nf = 0;
        on = 0;
        if (!getenv("RECOMP_GL_NO_SHADER_CACHE") && glad_glProgramBinary && glad_glGetProgramBinary
                && glad_glProgramParameteri) {
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nf);
            on = nf > 0;
        }
#if defined(_WIN32)
        if (on) CreateDirectoryA("shader_cache", NULL);
#else
        if (on) mkdir("shader_cache", 0755);
#endif
        fprintf(stderr, "[GL] shader cache %s\n", on ? "on (shader_cache)" :
                "off (no program binaries, or RECOMP_GL_NO_SHADER_CACHE)");
    }
    return on;
}

static uint64_t fnv64(uint64_t h, const char *p)
{
    while (*p)
        h = (h ^ (uint8_t)*p++) * 0x100000001B3ull;
    return h;
}

static void cache_path(char *b, size_t n, const char *vsrc, const char *fsrc)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv64(h, (const char *)glGetString(GL_RENDERER));
    h = fnv64(h, (const char *)glGetString(GL_VERSION));
    h = fnv64(h, vsrc);
    h = fnv64(h, fsrc);
    if (old_clip())
        h = fnv64(h, "OLD_CLIP");
    if (no_zcull())
        h = fnv64(h, "NO_ZCULL");
    if (no_zcheck())
        h = fnv64(h, "NO_ZCHECK");
    if (simple_fs())
        h = fnv64(h, "SIMPLE_FS");
    if (bump_old())
        h = fnv64(h, "BUMP_OLD_LOD");
    if (fog_vertex())
        h = fnv64(h, "FOG_PER_VERTEX");
#if defined(_WIN32)
    snprintf(b, n, "shader_cache\\%016llx.bin", (unsigned long long)h);
#else
    snprintf(b, n, "shader_cache/%016llx.bin", (unsigned long long)h);
#endif
}

static GLuint cache_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t hdr[2];
    void *data;
    GLuint pr;
    GLint ok = 0;
    if (!f)
        return 0;
    if (fread(hdr, 4, 2, f) != 2 || !hdr[1] || hdr[1] > (64u << 20)
            || !(data = malloc(hdr[1]))) {
        fclose(f);
        return 0;
    }
    if (fread(data, 1, hdr[1], f) != hdr[1]) {
        free(data);
        fclose(f);
        return 0;
    }
    fclose(f);
    pr = glCreateProgram();
    glProgramBinary(pr, (GLenum)hdr[0], data, (GLsizei)hdr[1]);
    free(data);
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(pr);
        return 0;                     /* stale (driver changed): rebuild */
    }
    return pr;
}

static void cache_save(const char *path, GLuint pr)
{
    GLint len = 0;
    GLenum fmt = 0;
    void *data;
    FILE *f;
    glGetProgramiv(pr, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || !(data = malloc((size_t)len)))
        return;
    glGetProgramBinary(pr, len, NULL, &fmt, data);
    f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[2];
        hdr[0] = (uint32_t)fmt; hdr[1] = (uint32_t)len;
        fwrite(hdr, 4, 2, f);
        fwrite(data, 1, (size_t)len, f);
        fclose(f);
    }
    free(data);
}

static GLuint link_with2(const char *vsrc, const char *fsrc);
static GlProg *zvar(GlProg *p, const NvGlState *st);
static void spec_purge(GlProg *base);
static GLuint link_with(const char *vsrc) { return link_with2(vsrc, FS); }

/* v103: FS without the gl_FragDepth write */
static const char *fs_nz(void)
{
    static char *src;
    if (!src) {
        const char *nl = strchr(FS, '\n') + 1;
        size_t n = strlen(FS) + 40;
        src = (char *)malloc(n + 40);
        if (!src)
            return FS;
#ifdef NV2A_GL_NZ_TEST
        snprintf(src, n + 40, "%.*s#define NO_FRAGDEPTH 1\n#define NZ_MARK 1\n%s", (int)(nl - FS), FS, nl);
#else
        snprintf(src, n, "%.*s#define NO_FRAGDEPTH 1\n%s", (int)(nl - FS), FS, nl);
#endif
    }
    return src;
}

static GLuint link_with2(const char *vsrc, const char *fsrc)
{
    GLuint vs, fs, pr;
    GLint ok = 0;
    char path[64];
    int cache = shader_cache_on();
    if (cache) {
        cache_path(path, sizeof path, vsrc, fsrc);
        pr = cache_load(path);
        if (pr) {
            s_cache_hits++;
            return pr;
        }
        s_cache_misses++;
    }
    uint64_t t0 = TM_NOW();
    vs = compile(GL_VERTEX_SHADER, vsrc);
    fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs)
        return 0;
    pr = glCreateProgram();
    glAttachShader(pr, vs);
    glAttachShader(pr, fs);
    if (cache)
        glProgramParameteri(pr, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(pr);
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(pr, sizeof log, NULL, log);
        fprintf(stderr, "[GL] shader program failed to link:\n%s\n", log);
        glDeleteProgram(pr);
        return 0;
    }
    if (cache)
        cache_save(path, pr);
    s_tm_compile += TM_NOW() - t0;
    return pr;
}

void nv2a_gl_frame_stats(uint64_t *compiles, uint64_t *compile_ticks, uint64_t *texups,
                         uint64_t *texup_ticks, uint64_t *wait_ticks, uint64_t *freq)
{
    *compiles = s_cache_misses;
    *compile_ticks = s_tm_compile;
    *texups = s_n_texup;
    *texup_ticks = s_tm[3];
    *wait_ticks = s_tm[1];
    *freq = SDL_GetPerformanceFrequency();
}

/* Uniform locations of a freshly linked program, and its texture units. */
static void get_locs(GlProg *p)
{
    GLuint id = p->id;
    int i;
    GlProg *was = P;
    glUseProgram(id);
#define U(v) p->L_##v = glGetUniformLocation(id, "u_" #v)
    U(bmat); U(bls); U(osi);
    U(scr); U(wsk); U(wsc); U(ws2d); U(vp); U(zs); U(zs2); U(zdirect); U(poff); U(rc_on); U(cicw); U(aicw); U(cocw);
    U(aocw); U(f0); U(f1); U(final0); U(final1); U(fc0); U(fc1); U(control); U(sp);
    U(clipmode); U(fogc); U(atest); U(afunc); U(aref); U(tscale); U(iscube); U(tvalid);
    U(vsmode); U(present); U(c); U(prog); U(nins); U(comp); U(mv); U(imv0); U(texm);
    U(skin); U(light); U(texm_on); U(texgen); U(texpl); U(samb); U(emis); U(malpha);
    U(cmat); U(lmask); U(spec); U(lamb); U(ldif); U(lspc); U(lhalf); U(ldir); U(lpos);
    U(latt); U(lrange); U(fog_en); U(fog_mode); U(fogp);
#undef U
    for (i = 0; i < 4; i++) {
        char n[16];
        snprintf(n, sizeof n, "u_s%d", i);
        p->L_s2d[i] = glGetUniformLocation(id, n);
        snprintf(n, sizeof n, "u_c%d", i);
        p->L_scube[i] = glGetUniformLocation(id, n);
        glUniform1i(p->L_s2d[i], i);        /* 2D on units 0-3 */
        glUniform1i(p->L_scube[i], 4 + i);  /* cube on units 4-7 */
    }
    /* v117: how much of u_c[] is live, and whether its elements' locations
     * run on from u_c[0], so a changed range can be sent by itself. A range
     * past the live size would land on whatever uniform comes next. */
    p->c_n = 0;
    if (p->L_c >= 0) {
        GLint nu = 0, k;
        glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &nu);
        for (k = 0; k < nu; k++) {
            char nm[64];
            GLint sz = 0;
            GLenum ty = 0;
            nm[0] = 0;
            glGetActiveUniform(id, (GLuint)k, sizeof nm, NULL, &sz, &ty, nm);
            if (!strcmp(nm, "u_c[0]") || !strcmp(nm, "u_c")) {
                p->c_n = ty == GL_FLOAT_VEC4 ? sz : -1;
                break;
            }
        }
        if (p->c_n > 1) {
            char b[16];
            GLint l1 = glGetUniformLocation(id, "u_c[1]"), lz;
            snprintf(b, sizeof b, "u_c[%d]", p->c_n - 1);
            lz = glGetUniformLocation(id, b);
            if (l1 != p->L_c + 1 || lz != p->L_c + p->c_n - 1)
                p->c_n = -1;
        }
    }
    p->ap_valid = 0;
    p->vs_const_gen = 0;
    p->vs_present = 0xFFFFFFFFu;
    p->vs_ff_valid = 0;
    p->vs_mode = -1;
    p->vs_fog[0] = p->vs_fog[1] = -1;
    p->vs_fogp[0] = p->vs_fogp[1] = -1e30f;
    if (was)
        glUseProgram(was->id);
    else
        glUseProgram(id);
}

/* v78: fixed-function variants -- bit 0 lighting, bit 1 texgen -- so a batch
 * that needs neither runs a shader without them. */
static GlProg s_p_ffv[4];
static const char *ff_source(int key)
{
    static char *src[4];
    if (!src[key]) {
        const char *body = strchr(VS_FF, '\n') + 1;     /* after #version */
        size_t n = strlen(body) + 96;
        src[key] = (char *)malloc(n);
        if (!src[key])
            return VS_FF;
        snprintf(src[key], n, "#version 330 core\n#define FF_LIGHT %d\n#define FF_TEXGEN %d\n%s",
                 key & 1, (key >> 1) & 1, body);
    }
    return src[key];
}

static GlProg *ff_prog(int key)
{
    GlProg *p = &s_p_ffv[key & 3];
    if (key == 3)
        return &s_p_ff;
    if (!p->id) {
        p->id = link_with(ff_source(key & 3));
        if (!p->id)
            return &s_p_ff;
        get_locs(p);
        p->vsrc = ff_source(key & 3);
    }
    return p;
}

static int build_program(void)
{
    s_p_old.id = link_with(VS_OLD);
    if (!s_p_old.id)
        return 0;
    get_locs(&s_p_old);
    s_p_old.vsrc = VS_OLD;
    if (!getenv("RECOMP_GL_CPU_VERTEX")) {
        GLint comps = 0;
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &comps);
        if (comps >= 1536) {
            s_p_ff.id = link_with(ff_source(3));      /* the general one */
            if (s_p_ff.id) {
                get_locs(&s_p_ff);
                s_p_ff.vsrc = ff_source(3);
            }
        } else {
            fprintf(stderr, "[GL] only %d vertex uniform components; vertices stay on"
                    " the CPU\n", comps);
        }
        s_gpu_vs = s_p_ff.id != 0;
        if (s_gpu_vs) {
            int k;
            NvGlState z;
            memset(&z, 0, sizeof z);
            z.zdirect_off = 1;
            for (k = 0; k < 3; k++)           /* the other variants, now not mid-game */
                zvar(ff_prog(k), &z);
            zvar(&s_p_ff, &z);
        }
    }
    {
        NvGlState z;
        memset(&z, 0, sizeof z);
        z.zdirect_off = 1;
        zvar(&s_p_old, &z);
    }
    use_prog(&s_p_old);
    fprintf(stderr, "[GL] vertex processing on the %s%s\n", s_gpu_vs ? "GPU" : "CPU",
            s_gpu_vs ? " (RECOMP_GL_CPU_VERTEX=1 for the CPU)" : "");
    return 1;
}

/* ── v78: a vertex program as straight-line GLSL ─────────────────────────
 *
 * v77 interpreted the program in the shader -- a loop over a uniform array
 * of instructions, registers in indexed arrays. It was right but slow on the
 * GPU: dynamic indexing puts the registers in scratch memory, and the frame
 * took longer to draw than to submit (run 77: "present" 1-2 s of every 10,
 * waiting on the GPU). Each program is now translated once into its own
 * shader, registers as plain variables, every swizzle and mask fixed. */
static void gsrc(char *b, size_t n, uint32_t s)
{
    static const char L[] = "xyzw";
    uint32_t mux = s & 3u, sw = (s >> 3) & 255u, reg = (s >> 12) & 255u;
    char base[48];
    if (mux == 1)
        snprintf(base, sizeof base, "R%u", reg <= 12 ? reg : 0);
    else if (mux == 2)
        snprintf(base, sizeof base, "i%u", reg & 15u);
    else if ((s >> 11) & 1u)
        snprintf(base, sizeof base, "ucon(a0 + %u)", reg);
    else
        snprintf(base, sizeof base, "u_c[%u]", reg < 192 ? reg : 0);
    snprintf(b, n, "%s%s.%c%c%c%c%s", ((s >> 2) & 1u) ? "(-" : "", base,
             L[(sw >> 6) & 3], L[(sw >> 4) & 3], L[(sw >> 2) & 3], L[sw & 3],
             ((s >> 2) & 1u) ? ")" : "");
}

static void gmask(char *b, uint32_t m)
{
    int k = 0;
    if (m & 8) b[k++] = 'x';
    if (m & 4) b[k++] = 'y';
    if (m & 2) b[k++] = 'z';
    if (m & 1) b[k++] = 'w';
    b[k] = 0;
}

static char *gen_vsh(const uint32_t (*prog)[4], int n)
{
    size_t cap = strlen(VSH_HEAD) + strlen(VSH_TAIL) + (size_t)n * 640 + 64, len = 0;
    char *out = (char *)malloc(cap);
    int k;
    if (!out)
        return NULL;
#define EMIT(...) do { len += (size_t)snprintf(out + len, cap - len, __VA_ARGS__); } while (0)
    EMIT("%s", VSH_HEAD);
    for (k = 0; k < n; k++) {
        const uint32_t *I = prog[k];
        uint32_t mac = I[3] & 15u, ilu = (I[3] >> 4) & 7u, mm = (I[3] >> 7) & 15u;
        uint32_t im = (I[3] >> 11) & 15u, td = (I[3] >> 15) & 15u, om = (I[3] >> 19) & 15u;
        uint32_t oa = (I[0] >> 20) & 255u, oreg = (I[0] >> 28) & 1u, oilu = (I[0] >> 29) & 1u;
        char a[64], b[64], c[64], m1[8];
        gsrc(a, sizeof a, I[0]); gsrc(b, sizeof b, I[1]); gsrc(c, sizeof c, I[2]);
        EMIT("  { vec4 a = %s, b = %s, c = %s, mr = vec4(0.0), ir = vec4(0.0);\n", a, b, c);
        switch (mac) {
        case 0: break;
        case 2: EMIT("    mr = a * b;\n"); break;
        case 3: EMIT("    mr = a + c;\n"); break;
        case 4: EMIT("    mr = a * b + c;\n"); break;
        case 5: EMIT("    mr = vec4(dot(a.xyz, b.xyz));\n"); break;
        case 6: EMIT("    mr = vec4(dot(a.xyz, b.xyz) + b.w);\n"); break;
        case 7: EMIT("    mr = vec4(dot(a, b));\n"); break;
        case 8: EMIT("    mr = vec4(1.0, a.y * b.y, a.z, b.w);\n"); break;
        case 9: EMIT("    mr = min_(a, b);\n"); break;
        case 10: EMIT("    mr = max_(a, b);\n"); break;
        case 11: EMIT("    mr = slt_(a, b);\n"); break;
        case 12: EMIT("    mr = sge_(a, b);\n"); break;
        case 13: EMIT("    mr = a; a0 = int(floor(a.x + 0.001));\n"); break;
        default: EMIT("    mr = a;\n"); break;              /* mov, and the unknown ones */
        }
        switch (ilu) {
        case 0: break;
        case 2: EMIT("    ir = vec4(c.x == 0.0 ? finf() : 1.0 / c.x);\n"); break;
        case 3: EMIT("    ir = vec4(rcc_(c.x));\n"); break;
        case 4: EMIT("    ir = vec4(1.0 / sqrt(abs(c.x)));\n"); break;
        case 5: EMIT("    ir = exp_(c.x);\n"); break;
        case 6: EMIT("    ir = log_(c.x);\n"); break;
        case 7: EMIT("    ir = lit_(c);\n"); break;
        default: EMIT("    ir = c;\n"); break;
        }
        if (mac && mac != 13 && mm && td <= 12) {
            gmask(m1, mm);
            EMIT("    R%u.%s = mr.%s;\n", td, m1, m1);
        }
        if (ilu && im) {
            uint32_t it = mac ? 1u : td;
            if (it <= 12) {
                gmask(m1, im);
                EMIT("    R%u.%s = ir.%s;\n", it, m1, m1);
            }
        }
        if (om && oreg) {
            gmask(m1, om);
            if (oa == 0)
                EMIT("    R12.%s = %s.%s;\n", m1, oilu ? "ir" : "mr", m1);
            else if (oa < 13)
                EMIT("    O%u.%s = %s.%s;\n", oa, m1, oilu ? "ir" : "mr", m1);
        }
        EMIT("  }\n");
        if (len + 700 > cap)
            break;
    }
    EMIT("%s", VSH_TAIL);
#undef EMIT
    return out;
}

static uint32_t vsh_hash(const uint32_t (*prog)[4], int n)
{
    uint32_t h = 2166136261u;
    int k, j;
    for (k = 0; k < n; k++)
        for (j = 0; j < 4; j++)
            h = (h ^ prog[k][j]) * 16777619u;
    return h ^ (uint32_t)n;
}

static GlProg *vsh_find(const uint32_t (*prog)[4], int n, int build)
{
    uint32_t h = vsh_hash(prog, n);
    int i, victim = 0;
    for (i = 0; i < NV_GL_VSH_PROGS; i++) {
        GlProg *p = &s_p_vsh[i];
        if (p->id && p->key_hash == h && p->key_n == (uint32_t)n
                && !memcmp(p->key, prog, (size_t)n * 16)) {
            p->used = ++s_prog_clock;
            return p;
        }
        if (!s_p_vsh[i].id || s_p_vsh[i].used < s_p_vsh[victim].used)
            victim = i;
    }
    if (!build)
        return NULL;
    {
        static int fails;
        GlProg *p = &s_p_vsh[victim];
        char *src = gen_vsh(prog, n);
        GLuint id;
        if (!src)
            return NULL;
        id = link_with(src);
        if (!id) {
            if (fails++ < 4) {
                fprintf(stderr, "[GL] vertex program (%d instructions) did not build;"
                        " it stays on the CPU. Source:\n%s\n", n, src);
            }
            free(src);
            return NULL;
        }
        if (p->id) {
            if (P == p || (p->nz && P == p->nz)) { glUseProgram(s_p_old.id); P = &s_p_old; }
            glDeleteProgram(p->id);
            free(p->key);
            if (p->nz && p->nz != p) {
                glDeleteProgram(p->nz->id);
                free(p->nz);
            }
            spec_purge(p);
            if (p->vsrc_owned)
                free((void *)p->vsrc);
        }
        memset(p, 0, sizeof *p);
        p->id = id;
        p->key = (uint32_t (*)[4])malloc((size_t)n * 16);
        if (p->key)
            memcpy(p->key, prog, (size_t)n * 16);
        p->key_hash = h;
        p->key_n = (uint32_t)n;
        p->used = ++s_prog_clock;
        get_locs(p);
        p->vsrc = src; p->vsrc_owned = 1;      /* kept for the twin */
        {
            static unsigned built;
            if (built++ < 64)
                fprintf(stderr, "[GL] vertex program %u ready (%d instructions; shader cache"
                        " %u loaded, %u compiled so far)\n", built, n, s_cache_hits, s_cache_misses);
        }
        return p;
    }
}

/* v103: the program to draw with -- p itself when the fragment shader is to
 * write the title's depth, else p's twin whose shader leaves depth to GL, so
 * the GPU tests depth before shading (see zdirect_wanted in nv2a_pb_exec.c).
 * RECOMP_GL_NZ_TWINS=0 always uses p (pre-v103). */
static GlProg *zvar(GlProg *p, const NvGlState *st)
{
    static int direct = -1, twins = -1;
    if (direct < 0) {
        const char *e = getenv("RECOMP_GL_FRAGDEPTH");
        direct = !(e && !strcmp(e, "0"));
        e = getenv("RECOMP_GL_NZ_TWINS");
        twins = !(e && !strcmp(e, "0"));
    }
    if (!twins || !p || !p->id || !p->vsrc || (direct && !st->zdirect_off))
        return p;
    if (!p->nz) {
        GlProg *q = (GlProg *)calloc(1, sizeof *q);
        p->nz = p;                              /* until it builds */
        if (q) {
            q->id = link_with2(p->vsrc, fs_nz());
            if (q->id) {
                static unsigned nb;
                get_locs(q);
                p->nz = q;
                if (nb++ == 0)
                    fprintf(stderr, "[GL] 3D batches leave depth to GL: the GPU tests it"
                            " before shading (RECOMP_GL_FRAGDEPTH=1 for the old way)\n");
            } else {
                free(q);
            }
        }
    }
    return p->nz;
}


/* v105: fragment shaders specialised to the combiner setup. The general
 * shader runs the register combiners as a loop over uniforms, reading and
 * writing its sixteen registers through run-time indices, and at 3x that
 * cost the GPU 4-5 ms a frame (run 104: with RECOMP_GL_SIMPLE_FS=1, the
 * frames came in even). Here the same source has the stage words as
 * constants and the stage loop written out, so every register index is
 * known when it compiles and the compiler folds the rest away. One program
 * per vertex shader, depth variant and combiner setup, built on first use
 * (and kept in the shader cache). RECOMP_GL_RC_SPECIAL=0 keeps the general
 * shader. */
/* v106: 1024 entries with a 16-slot probe was far too few: Shibuya
 * terminal needs thousands of vertex shader x combiner pairs, and entries
 * were evicted and loaded back from the shader cache all the time (run 105:
 * 18,000 cache loads in about a minute, frames at 55 ms). */
#define SPEC_N 16384
#define SPEC_PROBE 64
#define SPEC_KEY 37
typedef struct {
    GlProg *base; int nz;
    uint32_t key[SPEC_KEY];
    GlProg *prog;            /* NULL: built and failed */
    uint32_t used;
    int live;
} SpecEnt;
static SpecEnt s_spec[SPEC_N];
static unsigned s_spec_built, s_spec_failed, s_spec_evicted, s_spec_live, s_spec_new;
static uint32_t s_spec_clock;

static int spec_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_RC_SPECIAL");
        v = !(e && !strcmp(e, "0"));
    }
    return v;
}

static void spec_key(const NvGlState *st, uint32_t k[SPEC_KEY])
{
    uint32_t n = st->rc.control & 0xFF, i;
    if (n > 8) n = 8;
    memset(k, 0, SPEC_KEY * 4);
    k[0] = st->rc.control;
    k[1] = st->rc.final0;
    k[2] = st->rc.final1;
    k[3] = n;
    k[4] = 0;
    for (i = 0; i < n; i++) {
        k[5 + i] = st->rc.color_icw[i];
        k[13 + i] = st->rc.alpha_icw[i];
        k[21 + i] = st->rc.color_ocw[i];
        k[29 + i] = st->rc.alpha_ocw[i];
    }
}

/* text replace, once; returns a new string (or NULL) */
static char *str_rep(const char *src, const char *old, const char *new_)
{
    const char *at = strstr(src, old);
    size_t a, lo, ln, ls;
    char *out;
    if (!at) return NULL;
    a = (size_t)(at - src); lo = strlen(old); ln = strlen(new_); ls = strlen(src);
    out = (char *)malloc(ls - lo + ln + 1);
    if (!out) return NULL;
    memcpy(out, src, a);
    memcpy(out + a, new_, ln);
    memcpy(out + a + ln, at + lo, ls - a - lo + 1);
    return out;
}

static char *spec_src(const char *fs, const uint32_t k[SPEC_KEY])
{
    char buf[1024], *cur, *nx;
    const char *hdr = "    for (uint s = 0u; s < n; s++) {\n";
    const char *tail = "      R[0] = vec4(0.0);\n    }\n";
    const char *h, *t;
    size_t blen, cap, len;
    uint32_t n = k[3], i;
    char *body, *unrolled;

#define REPL(o, nw) do { nx = str_rep(cur, (o), (nw)); if (!nx) { free(cur); return NULL; } \
                         free(cur); cur = nx; } while (0)
    cur = (char *)malloc(strlen(fs) + 1);
    if (!cur) return NULL;
    strcpy(cur, fs);
    REPL("uniform int u_rc_on;\n", "const int u_rc_on = 1;\n");
    snprintf(buf, sizeof buf, "const uint u_cicw[8] = uint[8](%uu,%uu,%uu,%uu,%uu,%uu,%uu,%uu);"
             " const uint u_aicw[8] = uint[8](%uu,%uu,%uu,%uu,%uu,%uu,%uu,%uu);\n",
             k[5], k[6], k[7], k[8], k[9], k[10], k[11], k[12],
             k[13], k[14], k[15], k[16], k[17], k[18], k[19], k[20]);
    REPL("uniform uint u_cicw[8]; uniform uint u_aicw[8];\n", buf);
    snprintf(buf, sizeof buf, "const uint u_cocw[8] = uint[8](%uu,%uu,%uu,%uu,%uu,%uu,%uu,%uu);"
             " const uint u_aocw[8] = uint[8](%uu,%uu,%uu,%uu,%uu,%uu,%uu,%uu);\n",
             k[21], k[22], k[23], k[24], k[25], k[26], k[27], k[28],
             k[29], k[30], k[31], k[32], k[33], k[34], k[35], k[36]);
    REPL("uniform uint u_cocw[8]; uniform uint u_aocw[8];\n", buf);
    snprintf(buf, sizeof buf, "const uint u_final0 = %uu; const uint u_final1 = %uu;\n", k[1], k[2]);
    REPL("uniform uint u_final0; uniform uint u_final1;\n", buf);
    snprintf(buf, sizeof buf, "const uint u_control = %uu; uniform uint u_sp; uniform uint u_clipmode;\n", k[0]);
    REPL("uniform uint u_control; uniform uint u_sp; uniform uint u_clipmode;\n", buf);
#undef REPL
    /* the stage loop, written out */
    h = strstr(cur, hdr);
    t = h ? strstr(h, tail) : NULL;
    if (!h || !t) { free(cur); return NULL; }
    blen = (size_t)(t - (h + strlen(hdr))) + strlen("      R[0] = vec4(0.0);\n");
    body = (char *)malloc(blen + 1);
    if (!body) { free(cur); return NULL; }
    memcpy(body, h + strlen(hdr), blen); body[blen] = 0;
    cap = (blen + 64) * (n + 1) + 64;
    unrolled = (char *)malloc(cap);
    if (!unrolled) { free(body); free(cur); return NULL; }
    len = 0; unrolled[0] = 0;
    for (i = 0; i < n; i++)
        len += (size_t)snprintf(unrolled + len, cap - len, "    { const uint s = %uu;\n%s    }\n", i, body);
    free(body);
    {
        size_t pre = (size_t)(h - cur), post = strlen(t + strlen(tail));
        char *out = (char *)malloc(pre + len + post + 1);
        if (!out) { free(unrolled); free(cur); return NULL; }
        memcpy(out, cur, pre);
        memcpy(out + pre, unrolled, len);
        memcpy(out + pre + len, t + strlen(tail), post + 1);
        free(unrolled); free(cur);
        return out;
    }
}

static void spec_purge(GlProg *base)
{
    int i;
    for (i = 0; i < SPEC_N; i++)
        if (s_spec[i].live && s_spec[i].base == base) {
            if (s_spec[i].prog) {
                if (P == s_spec[i].prog) { glUseProgram(s_p_old.id); P = &s_p_old; }
                glDeleteProgram(s_spec[i].prog->id);
                free(s_spec[i].prog);
            }
            s_spec[i].live = 0;
            s_spec_live--;
        }
}

static GlProg *pick_prog(GlProg *p, const NvGlState *st)
{
    GlProg *z = zvar(p, st);
    uint32_t k[SPEC_KEY], h = 2166136261u;
    int nz = (z != p), i, slot, victim = -1;
    if (!spec_on() || !st->rc_seen || !p || !p->vsrc)
        return z;
    spec_key(st, k);
    for (i = 0; i < SPEC_KEY; i++) h = (h ^ k[i]) * 16777619u;
    h ^= (uint32_t)(uintptr_t)p * 2654435761u ^ (uint32_t)nz;
    for (i = 0; i < SPEC_PROBE; i++) {
        SpecEnt *e;
        slot = (int)((h + (uint32_t)i) & (SPEC_N - 1));
        e = &s_spec[slot];
        /* v107: an empty slot always beats evicting a live one (v105/v106
         * evicted a live entry whenever one came first in the probe, so the
         * table stayed almost empty while thousands were rebuilt) */
        if (!e->live) { if (victim < 0 || s_spec[victim].live) victim = slot; continue; }
        if (e->base == p && e->nz == nz && !memcmp(e->key, k, sizeof k)) {
            e->used = ++s_spec_clock;
            return e->prog ? e->prog : z;
        }
        if (victim < 0 || (s_spec[victim].live && e->used < s_spec[victim].used))
            victim = slot;
    }
    {
        SpecEnt *e = &s_spec[victim];
        char *src;
        GlProg *q;
        if (e->live && e->prog) {
            if (P == e->prog) { glUseProgram(s_p_old.id); P = &s_p_old; }
            glDeleteProgram(e->prog->id);
            free(e->prog);
        }
        if (e->live) s_spec_evicted++; else s_spec_live++;
        s_spec_new++;
        memset(e, 0, sizeof *e);
        e->live = 1; e->base = p; e->nz = nz; memcpy(e->key, k, sizeof k);
        e->used = ++s_spec_clock;
        src = spec_src(nz ? fs_nz() : FS, k);
        if (src && getenv("RECOMP_GL_DUMP_SPEC") && s_spec_built < 2)
            fprintf(stderr, "[GL] specialised fragment shader:\n%s\n", src);
        q = src ? (GlProg *)calloc(1, sizeof *q) : NULL;
        if (q) {
            q->id = link_with2(p->vsrc, src);
            if (q->id) {
                get_locs(q);
                e->prog = q;
                if (s_spec_built++ == 0)
                    fprintf(stderr, "[GL] fragment shaders specialised to each combiner setup"
                            " (RECOMP_GL_RC_SPECIAL=0 for the general one)\n");
            } else {
                free(q);
            }
        }
        if (!e->prog && s_spec_failed++ < 3)
            fprintf(stderr, "[GL] a specialised fragment shader did not build;"
                    " that setup keeps the general one\n");
        free(src);
        return e->prog ? e->prog : z;
    }
}

int nv2a_gl_prepare_vsh(const uint32_t (*prog)[4], int n)
{
    if (s_state != 1 || !s_gpu_vs || n <= 0 || !prog)
        return 0;
    return vsh_find(prog, n, 1) != NULL;
}

typedef NvGlVertex GlVert;

/* Vertex and index data stream through two ring buffers: each batch is
 * written behind the last one, unsynchronised, and the buffer is orphaned
 * (new storage, the old one released once the GPU is done with it) when the
 * ring is full. One glBufferData of the whole batch per draw made the driver
 * copy and synchronise every time. */
#define NV_GL_VB_BYTES (32u << 20)
#define NV_GL_IB_BYTES (8u << 20)
static size_t s_vb_off, s_ib_off;

/* v117: ...and, where the driver has ARB_buffer_storage, rings the CPU writes
 * straight into: mapped once, persistently and coherently, so a draw costs a
 * memcpy rather than two glBufferSubData calls (each a driver copy and its
 * bookkeeping, ~100-130k a second in play). The ring is cut into segments;
 * leaving one fences it, entering one waits for its fence -- which the GPU
 * has long passed, a ring holding ~15 frames. A batch too big for a segment
 * waits for the GPU to finish. RECOMP_GL_PERSIST=0 goes back to
 * glBufferSubData. */
#define NV_GL_SEGS 8
typedef struct {
    GLuint   buf;
    uint8_t *map;
    size_t   size, off;
    int      seg, seg_lo;             /* segments the current writes span */
    GLsync   fence[NV_GL_SEGS];
} GlRing;
static GlRing s_rv, s_rv2, s_ri;      /* CPU vertices, raw inputs, indices */
static int s_persist;
static uint64_t s_n_ring_waits, s_n_ring_big;

static int ring_create(GlRing *r, GLenum target, GLuint buf, size_t size)
{
    const GLbitfield f = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->size = size;
    r->seg = r->seg_lo = -1;
    glBindBuffer(target, buf);
    if (target == GL_ARRAY_BUFFER)
        s_bound_ab = buf;
    glBufferStorage(target, (GLsizeiptr)size, NULL, f);
    r->map = (uint8_t *)glMapBufferRange(target, 0, (GLsizeiptr)size, f);
    return r->map != NULL;
}

static void ring_wait(GLsync *f)
{
    if (!*f)
        return;
    if (glClientWaitSync(*f, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {
        s_n_ring_waits++;
        glClientWaitSync(*f, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
    }
    glDeleteSync(*f);
    *f = 0;
}

/* Room for `bytes` at a multiple of `align`, safe to write: no draw the GPU
 * has still to run reads it. Returns its offset in the ring. Every draw that
 * used the ring so far has been issued when this is called. */
static size_t ring_alloc(GlRing *r, size_t bytes, size_t align)
{
    size_t seg = r->size / NV_GL_SEGS;
    size_t off = (r->off + align - 1) / align * align;
    int k, lo, i;

    if (bytes + align > seg) {
        /* Bigger than a segment: everything in flight finishes, and the
         * batch goes at the start. */
        glFinish();
        for (i = 0; i < NV_GL_SEGS; i++)
            if (r->fence[i]) { glDeleteSync(r->fence[i]); r->fence[i] = 0; }
        s_n_ring_big++;
        off = 0;
        lo = 0;
        k = (int)((bytes - 1) / seg);
    } else {
        if (off + bytes > r->size) {
            off = 0;
        } else if (off / seg != (off + bytes - 1) / seg) {
            off = (off / seg + 1) * seg;
            off = (off + align - 1) / align * align;
            if (off + bytes > r->size)
                off = 0;
        }
        lo = k = (int)(off / seg);
    }
    if (k != r->seg || lo != r->seg_lo) {
        if (r->seg >= 0)
            for (i = r->seg_lo; i <= r->seg; i++) {
                if (r->fence[i]) glDeleteSync(r->fence[i]);
                r->fence[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }
        for (i = lo; i <= k; i++)
            ring_wait(&r->fence[i]);
        r->seg_lo = lo;
        r->seg = k;
    }
    r->off = off + bytes;
    return off;
}

static int init_gl(void)
{
    const char *r = getenv("RECOMP_RENDERER");
    static const uint32_t white = 0xFFFFFFFFu;
    int i;

    if (r && (!strcmp(r, "sw") || !strcmp(r, "software")))
        return 0;
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[GL] SDL video init failed: %s -- software renderer\n",
                SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    /* v88: double-buffered, so the context can present into the game's
     * window with SwapBuffers (gl_window_present); rendering itself still
     * goes to framebuffer objects. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* Rendering goes to framebuffer objects, so the window is only there to
     * own the context and never needs to be seen. */
    s_win = SDL_CreateWindow("JSRF GL", 0, 0, 16, 16,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!s_win) {
        fprintf(stderr, "[GL] no window for a context: %s -- software renderer\n",
                SDL_GetError());
        return 0;
    }
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "[GL] OpenGL 3.3 core context failed: %s -- software renderer\n",
                SDL_GetError());
        return 0;
    }
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "[GL] could not load GL functions -- software renderer\n");
        return 0;
    }
    fprintf(stderr, "[GL] %s on %s (%s) -- triangles are drawn on the GPU"
            " (RECOMP_RENDERER=sw for the software rasteriser)\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VENDOR));
    if (!build_program())
        return 0;
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        GLint mx = 0;
        s_scale = e ? atoi(e) : 1;
        if (s_scale < 1) s_scale = 1;
        if (s_scale > 8) s_scale = 8;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &mx);
        while (s_scale > 1 && 1024 * s_scale > mx) s_scale--;
        if (s_scale > 1)
            fprintf(stderr, "[GL] internal resolution x%d (640x480 renders at %dx%d)"
                    " -- RECOMP_GL_SCALE\n", s_scale, 640 * s_scale, 480 * s_scale);
    }

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao); s_cur_attr_ok = 0;
    {
        const char *pe = getenv("RECOMP_GL_PERSIST");
        s_persist = GLAD_GL_ARB_buffer_storage
                    && glad_glBufferStorage && !(pe && pe[0] == '0');
    }
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ebo);
    if (s_persist) {
        /* the element buffer is part of the VAO: bound with it current */
        if (!ring_create(&s_rv, GL_ARRAY_BUFFER, s_vbo, NV_GL_VB_BYTES)
                || !ring_create(&s_ri, GL_ELEMENT_ARRAY_BUFFER, s_ebo, NV_GL_IB_BYTES)) {
            /* storage is immutable now: start over with fresh buffers */
            glDeleteBuffers(1, &s_vbo); s_bound_ab = (GLuint)-1;
            glDeleteBuffers(1, &s_ebo);
            glGenBuffers(1, &s_vbo);
            glGenBuffers(1, &s_ebo);
            s_persist = 0;
            while (glGetError() != GL_NO_ERROR) {}
        }
    }
    if (!s_persist) {
        bind_ab(s_vbo);
        glBufferData(GL_ARRAY_BUFFER, NV_GL_VB_BYTES, NULL, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);   /* part of the VAO */
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, NV_GL_IB_BYTES, NULL, GL_STREAM_DRAW);
    }
    bind_ab(s_vbo);
    fprintf(stderr, "[GL] vertex and index data: %s\n", s_persist
            ? "persistent mapped rings (RECOMP_GL_PERSIST=0 for glBufferSubData)"
            : "glBufferSubData");
    {
        const GLsizei S = (GLsizei)sizeof(GlVert);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, S, (void *)offsetof(GlVert, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, S, (void *)offsetof(GlVert, d0));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, S, (void *)offsetof(GlVert, d1));
        for (i = 0; i < 4; i++) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, S,
                                  (void *)(offsetof(GlVert, t) + (size_t)i * 16));
        }
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, S, (void *)offsetof(GlVert, fog));
    }

    /* Stand-ins for a stage whose texture is unusable: the software sampler
     * reads such a stage as white, and so does this. */
    glGenTextures(1, &s_white2d);
    glBindTexture(GL_TEXTURE_2D, s_white2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &s_whitecube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_whitecube);
    for (i = 0; i < 6; i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &white);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* v77: the raw-input VAO for the GPU vertex path. Same index buffer;
     * its attribute arrays are pointed per batch (nv2a_gl_draw_raw). */
    if (s_gpu_vs) {
        glGenVertexArrays(1, &s_vao2);
        glBindVertexArray(s_vao2);
        glGenBuffers(1, &s_vbo2);
        if (!s_persist || !ring_create(&s_rv2, GL_ARRAY_BUFFER, s_vbo2, NV_GL_VB_BYTES)) {
            if (s_persist) {
                /* the other two rings stay; this one goes through SubData */
                glDeleteBuffers(1, &s_vbo2); s_bound_ab = (GLuint)-1;
                glGenBuffers(1, &s_vbo2);
                while (glGetError() != GL_NO_ERROR) {}
            }
            bind_ab(s_vbo2);
            glBufferData(GL_ARRAY_BUFFER, NV_GL_VB_BYTES, NULL, GL_STREAM_DRAW);
        }
        bind_ab(s_vbo2);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
        glBindVertexArray(s_vao); s_cur_attr_ok = 0;
        /* What an input with no array reads: (0,0,0,1), as fetch_attr gives. */
        for (i = 0; i < 16; i++)
            glVertexAttrib4f((GLuint)i, 0.0f, 0.0f, 0.0f, 1.0f); s_cur_attr_ok = 0;
    }
    glEnable(GL_DEPTH_CLAMP);
    if (old_clip() && !no_zcull()) {
        int ci;
        for (ci = 0; ci < 3; ci++)
            glEnable(GL_CLIP_DISTANCE0 + ci);   /* v103: the depth-range cull */
    }
    glEnable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    use_prog(&s_p_old);
    glBindVertexArray(s_vao); s_cur_attr_ok = 0;
    {
        /* v172: from here on the GL calls are made on the GL thread
         * (not with the opt-in DXGI presentation, which stays as it was) */
        const char *pm = getenv("RECOMP_GL_PRESENT");
        if (!(pm && !strcmp(pm, "dxgi")))
            gt_start(s_win, s_ctx);
    }
    return 1;
}

int nv2a_gl_active(void)
{
    if (s_state < 0) {
        s_state = init_gl();
        if (!s_state)
            fprintf(stderr, "[GL] using the software rasteriser\n");
    }
    return s_state;
}

/* ── hashing ──────────────────────────────────────────────────────────── */

/* Four independent lanes, so the multiplies overlap: textures up to 256 KB
 * are hashed on every bind (see bind_texture), and one serial chain made that
 * a visible share of the frame. */
/* v142: CRC32C (SSE4.2) in three independent streams, about three times the
 * old hash's speed; hash values are only ever compared with others from the
 * same function, so the change is invisible but for the time. The old
 * multiply-xorshift hash stays as the fallback for CPUs without SSE4.2
 * (RECOMP_GL_HASH=old forces it). */
#if defined(__x86_64__) || defined(_M_X64)
#include <nmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define HASH_CRC_TARGET
#else
#include <cpuid.h>
#define HASH_CRC_TARGET __attribute__((target("sse4.2")))
#endif
static int crc_ok(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_HASH");
        unsigned c = 0;
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, 1);
        c = (unsigned)r[2];
#else
        unsigned a, b, d;
        if (!__get_cpuid(1, &a, &b, &c, &d)) c = 0;
#endif
        v = ((c >> 20) & 1) && !(e && !strcmp(e, "old"));
    }
    return v;
}
static HASH_CRC_TARGET uint64_t crc_hash(const uint8_t *p, size_t n)
{
    uint64_t c0 = 0x9E3779B9u ^ n, c1 = 0x85EBCA6Bu, c2 = 0xC2B2AE35u, w0, w1, w2;
    size_t third = (n / 24) * 8, i;
    const uint8_t *a = p, *b = p + third, *c = p + 2 * third;
    for (i = 0; i < third; i += 8) {
        memcpy(&w0, a + i, 8); memcpy(&w1, b + i, 8); memcpy(&w2, c + i, 8);
        c0 = _mm_crc32_u64(c0, w0);
        c1 = _mm_crc32_u64(c1, w1);
        c2 = _mm_crc32_u64(c2, w2);
    }
    for (i = 3 * third; i + 8 <= n; i += 8) {
        memcpy(&w0, p + i, 8);
        c0 = _mm_crc32_u64(c0, w0);
    }
    for (; i < n; i++)
        c0 = _mm_crc32_u8((uint32_t)c0, p[i]);
    return (c0 << 32 | c1) ^ (c2 * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)n << 1);
}
#else
static int crc_ok(void) { return 0; }
static uint64_t crc_hash(const uint8_t *p, size_t n) { (void)p; (void)n; return 0; }
#endif

static uint64_t hash_bytes_old(const uint8_t *p, size_t n);
static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    return crc_ok() ? crc_hash(p, n) : hash_bytes_old(p, n);
}

static uint64_t hash_bytes_old(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    if (n >= 32) {
        uint64_t l0 = h, l1 = h ^ 0x51ED27u, l2 = h ^ 0xA5A5A5A5u, l3 = h ^ 0x0F0F0F0Fu;
        for (; i + 32 <= n; i += 32) {
            uint64_t w0, w1, w2, w3;
            memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8);
            memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
            l0 = (l0 ^ w0) * 0x100000001B3ull; l0 ^= l0 >> 29;
            l1 = (l1 ^ w1) * 0x100000001B3ull; l1 ^= l1 >> 29;
            l2 = (l2 ^ w2) * 0x100000001B3ull; l2 ^= l2 >> 29;
            l3 = (l3 ^ w3) * 0x100000001B3ull; l3 ^= l3 >> 29;
        }
        h = ((l0 * 31 + l1) * 31 + l2) * 31 + l3;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* v117: "are these guest bytes still what hashed to `want`?" The write-watch
 * answers without reading them when it can; otherwise the range is re-marked
 * (protected) and hashed as before, so the next check can be answered by the
 * watch. */
static uint64_t s_n_wskip, s_n_whash;
static int guest_same(uint32_t va, const uint8_t *p, uint32_t bytes, XboxGwMark *m,
                      uint64_t want)
{
    if (xbox_gw_clean(va, bytes, m)) {
        s_n_wskip++;
        return 1;
    }
    xbox_gw_mark(va, bytes, m);
    s_n_whash++;
    return hash_bytes(p, bytes) == want;
}

/* ── surfaces ─────────────────────────────────────────────────────────── */

#define NV_GL_SURFS 16
typedef struct {
    uint32_t va, pitch, bpp, w, h;
    int      alpha;
    uint8_t *host;
    GLuint   tex, fbo;
    GLuint   ds;              /* depth-stencil renderbuffer attached */
    int      dirty;           /* GPU holds newer pixels than guest memory */
    uint32_t draws;           /* draws and clears into it, for nv2a_gl_present */
    uint64_t hash;            /* guest bytes as of the last upload/readback */
    uint32_t used;
    uint32_t cpu_checked;     /* frame its guest copy was last checked for CPU writes */
    XboxGwMark wm;            /* v117: write-watch over its guest bytes, from `hash` */
} GlSurf;
static GlSurf s_surf[NV_GL_SURFS];
static GlSurf *s_cur;
static uint32_t s_use_clock;

/* Redundant-state filters. Only this file touches GL, so what it last set is
 * known: per texture unit the bound 2D and cube textures, the bound draw
 * framebuffer, viewport and scissor, and (apply_state) the pipeline state.
 * Surface uploads and creation bind on unit 15, which the stages never use,
 * so they leave the stage bindings alone. */
#define NV_GL_SCRATCH_UNIT 15
static GLuint s_unit2d[16], s_unitcube[16];
static int    s_active_unit = -1;
static GLuint s_bound_fbo = (GLuint)-1;
static int    s_bound_vp[2] = {-1, -1}, s_bound_sc[4] = {-1, -1, -1, -1};
static int    s_ap_valid;        /* apply_state's cache is usable */

/* v87: internal resolution scale. Every render target, its depth buffer, the
 * viewport and the scissor are RECOMP_GL_SCALE times the guest's size; the
 * guest keeps seeing its own resolution wherever it reads pixels back
 * (surf_readback scales down first) or writes them (surf_upload scales up),
 * and the window is handed the full-size frame. Vertex positions need
 * nothing: the shaders map guest pixels to clip space through u_vp, which
 * stays the guest size. 1 = off. */
#define SC(v) ((int)(v) * s_scale)
static GLuint s_dn_fbo, s_dn_rb, s_up_fbo, s_up_tex;
static uint32_t s_dn_w, s_dn_h, s_up_w, s_up_h;

/* Copy a whole surface between two framebuffers with no scissor or masks in
 * the way; restores the draw binding. */
static void bind_draw_fbo(GLuint fbo);
static void blit_fb(GLuint src, int sw, int sh, GLuint dst, int dw, int dh, GLenum filter)
{
    /* v172: no glIsEnabled / glGetBooleanv (each one a wait for the GL
     * thread): the scissor test is always on outside these copies, and the
     * colour mask is set again by the next draw */
    GLuint prev = s_bound_fbo;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    bind_draw_fbo(dst);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, filter);
    glEnable(GL_SCISSOR_TEST);
    s_ap_valid = 0;
    bind_draw_fbo(prev == (GLuint)-1 ? 0 : prev);
}

/* A framebuffer holding `fbo`'s image at w x h (the guest size): `fbo`
 * itself unscaled, else a scaled-down copy. For reading pixels back. */
static GLuint guest_res_fbo(GLuint fbo, uint32_t w, uint32_t h)
{
    if (s_scale == 1)
        return fbo;
    if (!s_dn_fbo) {
        glGenFramebuffers(1, &s_dn_fbo);
        glGenRenderbuffers(1, &s_dn_rb);
    }
    if (s_dn_w != w || s_dn_h != h) {
        GLuint prev = s_bound_fbo;
        glBindRenderbuffer(GL_RENDERBUFFER, s_dn_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        bind_draw_fbo(s_dn_fbo);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, s_dn_rb);
        bind_draw_fbo(prev == (GLuint)-1 ? 0 : prev);
        s_dn_w = w; s_dn_h = h;
    }
    blit_fb(fbo, SC(w), SC(h), s_dn_fbo, (int)w, (int)h, GL_LINEAR);
    return s_dn_fbo;
}

static void active_unit(int u)
{
    if (s_active_unit != u) {
        glActiveTexture(GL_TEXTURE0 + u);
        s_active_unit = u;
    }
}

static void unit_bind(int u, GLenum target, GLuint id)
{
    GLuint *slot = target == GL_TEXTURE_CUBE_MAP ? &s_unitcube[u] : &s_unit2d[u];
    if (*slot != id) {
        active_unit(u);
        glBindTexture(target, id);
        *slot = id;
    }
}

static void bind_draw_fbo(GLuint fbo)
{
    if (s_bound_fbo != fbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        s_bound_fbo = fbo;
    }
}

typedef struct { uint32_t key, w, h; GLuint rb; } GlZeta;
static GlZeta s_zeta[8];

static uint32_t *s_scratch;
static size_t s_scratch_n;

static uint32_t *scratch(size_t n)
{
    if (n > s_scratch_n) {
        free(s_scratch);
        s_scratch = (uint32_t *)malloc(n * 4);
        s_scratch_n = s_scratch ? n : 0;
    }
    return s_scratch;
}

static size_t surf_bytes(const GlSurf *s)
{
    return (size_t)s->pitch * s->h;
}

/* Guest pixels into the surface's texture: rows flipped, 16-bit 5:6:5 or
 * 32-bit A8R8G8B8 widened to A8R8G8B8. */
static void surf_upload(GlSurf *s)
{
    uint32_t *px = scratch((size_t)s->w * s->h), x, y;
    if (!px || !s->host)
        return;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = s->host + (size_t)y * s->pitch;
        uint32_t *dst = px + (size_t)(s->h - 1 - y) * s->w;
        if (s->bpp == 4) {
            memcpy(dst, row, (size_t)s->w * 4);
            if (!s->alpha)
                for (x = 0; x < s->w; x++) dst[x] |= 0xFF000000u;
        } else {
            const uint16_t *r16 = (const uint16_t *)row;
            for (x = 0; x < s->w; x++) {
                uint32_t t = r16[x];
                uint32_t r = (t >> 11) & 31, g = (t >> 5) & 63, b = t & 31;
                dst[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                       | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
        }
    }
    if (s_scale == 1) {
        unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, s->tex); active_unit(NV_GL_SCRATCH_UNIT);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->w, (GLsizei)s->h,
                        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    } else {
        /* guest size into a staging texture, then scaled up into the surface */
        if (!s_up_fbo) {
            glGenFramebuffers(1, &s_up_fbo);
            glGenTextures(1, &s_up_tex);
        }
        unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, s_up_tex); active_unit(NV_GL_SCRATCH_UNIT);
        if (s_up_w != s->w || s_up_h != s->h) {
            GLuint prev = s_bound_fbo;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s->w, (GLsizei)s->h, 0,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            bind_draw_fbo(s_up_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_up_tex, 0);
            bind_draw_fbo(prev == (GLuint)-1 ? 0 : prev);
            s_up_w = s->w; s_up_h = s->h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->w, (GLsizei)s->h,
                            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        }
        blit_fb(s_up_fbo, (int)s->w, (int)s->h, s->fbo, SC(s->w), SC(s->h), GL_LINEAR);
    }
    xbox_gw_mark(s->va, (uint32_t)surf_bytes(s), &s->wm);
    s->hash = hash_bytes(s->host, surf_bytes(s));
    s_n_uploads++;
}

static void textures_invalidate_range(uint32_t va, uint32_t bytes);

static void surf_store(GlSurf *s, const uint32_t *px);

static void surf_readback(GlSurf *s)
{
    uint32_t *px;
    if (!s->dirty)
        return;
    s->dirty = 0;
    if (!s->host)
        return;
    px = scratch((size_t)s->w * s->h);
    if (!px)
        return;
    uint64_t t0 = TM_NOW();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, guest_res_fbo(s->fbo, s->w, s->h));
    glReadPixels(0, 0, (GLsizei)s->w, (GLsizei)s->h, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, px);
    surf_store(s, px);
    s_n_readbacks++;
    s_tm[1] += TM_NOW() - t0;
}

/* GL rows (bottom-up, A8R8G8B8) into the surface's guest memory. */
static void surf_store(GlSurf *s, const uint32_t *px)
{
    uint32_t x, y;
    /* v117: one unprotect for the whole store rather than a fault per
     * write-watched page */
    xbox_gw_host_write(s->host, surf_bytes(s), 1);
    for (y = 0; y < s->h; y++) {
        uint8_t *row = s->host + (size_t)y * s->pitch;
        const uint32_t *src = px + (size_t)(s->h - 1 - y) * s->w;
        if (s->bpp == 4) {
            memcpy(row, src, (size_t)s->w * 4);
        } else {
            uint16_t *r16 = (uint16_t *)row;
            for (x = 0; x < s->w; x++) {
                uint32_t c = src[x];
                r16[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    }
    xbox_gw_host_write(s->host, surf_bytes(s), 0);
    xbox_gw_mark(s->va, (uint32_t)surf_bytes(s), &s->wm);
    s->hash = hash_bytes(s->host, surf_bytes(s));
    textures_invalidate_range(s->va, (uint32_t)surf_bytes(s));
}

static GLuint zeta_rb(uint32_t key, uint32_t w, uint32_t h)
{
    int i, lru = 0;
    for (i = 0; i < 8; i++)
        if (s_zeta[i].rb && s_zeta[i].key == key && s_zeta[i].w == w
                && s_zeta[i].h == h)
            return s_zeta[i].rb;
    for (i = 0; i < 8; i++)
        if (!s_zeta[i].rb) { lru = i; break; }
    if (i == 8)
        lru = (int)(s_use_clock % 8);       /* rare: evict something */
    if (!s_zeta[lru].rb)
        glGenRenderbuffers(1, &s_zeta[lru].rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_zeta[lru].rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    s_zeta[lru].key = key; s_zeta[lru].w = w; s_zeta[lru].h = h;
    /* A new depth buffer starts at "far" with stencil 0, as the software
     * one does. */
    {
        GLuint tmp;
        glGenFramebuffers(1, &tmp);
        bind_draw_fbo(tmp);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, s_zeta[lru].rb);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        bind_draw_fbo(0);
        glDeleteFramebuffers(1, &tmp);
        s_ap_valid = 0;             /* depth and stencil masks changed */
    }
    return s_zeta[lru].rb;
}

static void present_finish_if(GlSurf *s);
static int surf_cpu_wrote(GlSurf *s);

static GlSurf *surf_get(const NvGlState *st)
{
    GlSurf *s = NULL, *victim = &s_surf[0];
    int i;

    for (i = 0; i < NV_GL_SURFS; i++) {
        GlSurf *c = &s_surf[i];
        if (c->fbo && c->va == st->surf_va && c->pitch == st->pitch
                && c->bpp == st->bpp && c->w == st->width && c->h == st->height
                && c->alpha == st->surf_has_alpha) {
            s = c;
            break;
        }
        if (!c->fbo || c->used < victim->used)
            victim = c;
    }
    if (!s) {
        /* A surface at the same address in another shape is the same memory:
         * write it back before reusing the address. */
        for (i = 0; i < NV_GL_SURFS; i++)
            if (s_surf[i].fbo && s_surf[i].va == st->surf_va)
                surf_readback(&s_surf[i]);
        s = victim;
        present_finish_if(s);
        surf_readback(s);
        if (!s->fbo) {
            glGenFramebuffers(1, &s->fbo);
            glGenTextures(1, &s->tex);
        }
        s->va = st->surf_va; s->pitch = st->pitch; s->bpp = st->bpp;
        s->w = st->width; s->h = st->height; s->alpha = st->surf_has_alpha;
        s->ds = 0;
        unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, s->tex); active_unit(NV_GL_SCRATCH_UNIT);
        glTexImage2D(GL_TEXTURE_2D, 0, s->alpha ? GL_RGBA8 : GL_RGB8,
                     (GLsizei)SC(s->w), (GLsizei)SC(s->h), 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        bind_draw_fbo(s->fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s->tex, 0);
        s->hash = ~0ull;                /* force the first upload */
        s->wm.ok = 0;
        s->dirty = 0;
    }
    s->host = st->surf_host;
    s->used = ++s_use_clock;
    return s;
}

/* Make `st`'s surface the GL target, bringing guest memory in if it changed
 * since the GPU last saw it. Returns NULL when there is nowhere to draw. */
static GlSurf *bind_surface(const NvGlState *st)
{
    GlSurf *s;
    GLuint rb;

    if (!st->surf_va || !st->width || !st->height
            || (st->bpp != 2 && st->bpp != 4))
        return NULL;
    s = surf_get(st);
    /* Leaving a surface used to write it back at once. Now it stays on the
     * GPU until something reads the guest copy: a texture over its memory
     * (bind_texture), the presentation at a flip, the software rasteriser,
     * or eviction. JSRF switches to its render targets and back several
     * times a frame, and each switch was a synchronous full-screen
     * glReadPixels (run 53: 1.3 s of every 10). RECOMP_GL_EAGER_READBACK=1
     * restores the old behaviour, for a title whose CPU reads a render
     * target it drew. */
    {
        static int eager = -1;
        if (eager < 0)
            eager = getenv("RECOMP_GL_EAGER_READBACK") != NULL;
        if (eager && s_cur && s_cur != s)
            surf_readback(s_cur);
    }
    if (s_cur != s)
        surf_cpu_wrote(s);
    s_cur = s;
    if (!s->dirty && s->host
            && !guest_same(s->va, s->host, (uint32_t)surf_bytes(s), &s->wm, s->hash))
        surf_upload(s);
    /* zeta_rb may bind a framebuffer of its own to clear a new buffer, so
     * it goes before this surface's is bound. */
    rb = zeta_rb(st->zeta_key, (uint32_t)SC(s->w), (uint32_t)SC(s->h));
    bind_draw_fbo(s->fbo);
    if (rb != s->ds) {
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, rb);
        s->ds = rb;
        {
            /* v172: a wait for the GL thread each time; only checked without it */
            GLenum fs = s_gt_on ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (fs != GL_FRAMEBUFFER_COMPLETE) {
                static int said;
                if (said++ < 4)
                    fprintf(stderr, "[GL] framebuffer for surface %08X (%ux%u, %ubpp)"
                            " incomplete: 0x%X\n", s->va, s->w, s->h, s->bpp, fs);
            }
        }
    }
    if (s_bound_vp[0] != SC(s->w) || s_bound_vp[1] != SC(s->h)) {
        glViewport(0, 0, (GLsizei)SC(s->w), (GLsizei)SC(s->h));
        s_bound_vp[0] = SC(s->w); s_bound_vp[1] = SC(s->h);
    }
    {
        int x = (int)st->clip_x, y = (int)st->clip_y;
        int w = (int)st->clip_w, h = (int)st->clip_h;
        int sy;
        if (!w || !h) { x = 0; y = 0; w = (int)s->w; h = (int)s->h; }
        sy = (int)s->h - (y + h);
        x = SC(x); sy = SC(sy); w = SC(w); h = SC(h);
        if (s_bound_sc[0] != x || s_bound_sc[1] != sy || s_bound_sc[2] != w
                || s_bound_sc[3] != h) {
            glScissor(x, sy, w, h);
            s_bound_sc[0] = x; s_bound_sc[1] = sy; s_bound_sc[2] = w; s_bound_sc[3] = h;
        }
    }
    return s;
}

/* Presenting without stalling. The frame used to be read back with a plain
 * glReadPixels at every flip, which waits for the GPU to finish the whole
 * frame before the executor can queue the next -- CPU and GPU took turns
 * (run 61: about 1-1.5 s of every 10 in "present"). Now the flip starts an
 * asynchronous read into a pixel-pack buffer and shows the frame the
 * previous flip started, which by then has arrived. One frame of latency.
 * A surface drawn again before its read is collected stays GPU-side.
 * RECOMP_GL_SYNC_PRESENT=1 restores the synchronous path. */
static struct { GLuint pbo; size_t bytes; GlSurf *surf; uint32_t draws; int win;
                uint32_t pw, ph; } s_pend;

/* v67: the finished frame goes from the pixel-pack buffer straight to the
 * window, not into guest memory first. Storing it there (16-bit conversion,
 * hash) and having the window convert it back was most of "present", and
 * left the surface clean, so the next texture made from it (JSRF samples its
 * previous frame every frame) was a synchronous glReadPixels. Now the surface
 * stays on the GPU until something on the CPU side needs it.
 * RECOMP_GL_PRESENT_GUEST=1 restores the old path. */
extern int xbox_FramebufferWindowAccepts(uint32_t w, uint32_t h);
extern int xbox_FramebufferWindowPresentPixels(const uint32_t *px, uint32_t w, uint32_t h);

static int present_direct(void)
{
    static int v = -1;
    if (v < 0)
        v = !getenv("RECOMP_GL_PRESENT_GUEST") && !getenv("RECOMP_FB_DUMP_FLIPS");
    return v;
}

/* A surface the GPU owns whose guest copy changed anyway: the title's CPU
 * wrote it (a movie frame, a software clear). The write wins, as it would on
 * the hardware, and the GPU copy is refreshed from it. Checked at most once a
 * frame per surface, when it is presented or becomes the target. */
static int surf_cpu_wrote(GlSurf *s)
{
    if (!s->dirty || !s->host || s->cpu_checked == s_frame)
        return 0;
    s->cpu_checked = s_frame;
    if (guest_same(s->va, s->host, (uint32_t)surf_bytes(s), &s->wm, s->hash))
        return 0;
    {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "[GL] surface %08X written by the CPU while on the GPU;"
                    " taking the CPU's copy\n", s->va);
    }
    s->dirty = 0;
    surf_upload(s);
    s_n_cpu_taken++;
    return 1;
}

static void present_collect(void)
{
    GlSurf *s = s_pend.surf;
    const uint32_t *px;
    uint64_t t0;
    if (!s)
        return;
    s_pend.surf = NULL;
    t0 = TM_NOW();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pend.pbo);
    px = (const uint32_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                            (GLsizeiptr)((size_t)s_pend.pw * s_pend.ph * 4),
                                            GL_MAP_READ_BIT);
    /* Not if it was read back synchronously after more drawing: guest
     * memory is newer than this. */
    if (px && s_pend.win) {
        xbox_FramebufferWindowPresentPixels(px, s_pend.pw, s_pend.ph);
    } else if (px && s->host && !(s->draws != s_pend.draws && !s->dirty)) {
        surf_store(s, px);
        if (s->draws == s_pend.draws)
            s->dirty = 0;                 /* nothing drawn since: guest is current */
    }
    if (px)
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_n_present_reads++;
    s_tm[1] += TM_NOW() - t0;
}

static void present_finish_if(GlSurf *s)
{
    if (s_pend.surf == s)
        present_collect();
}

static void start_present_read(GlSurf *s, int win);

/* v88: the finished frame straight into the game's window on the GPU.
 *
 * The window used to receive every frame through the CPU: an asynchronous
 * read of the whole render target, a row flip into a buffer, a copy, and a
 * GDI StretchDIBits. At 640x480 that was cheap; at RECOMP_GL_SCALE=3 it is
 * 11 MB a frame three times over, the window thread fell behind, and frames
 * were skipped while the counter (which counts flips) still said 60 -- "feels
 * like 30" (run 87: 1.4 s of every 10 in the read alone).
 *
 * Now this context draws into the window itself: its DC is given the pixel
 * format of the context's own (hidden) window, the context is made current
 * on it just long enough to blit the render target, scaled to the client
 * area in 4:3, and SwapBuffers, then goes back. Nothing crosses to the CPU.
 * The window thread stops painting once this has succeeded. Any failure
 * falls back to the old path for the rest of the run. RECOMP_GL_WINDOW_CPU=1
 * keeps the old path; RECOMP_GL_VSYNC=1 waits for vertical blank. */
#if defined(_WIN32)
extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowGLOwned(void);
typedef BOOL  (WINAPI *wgl_make_t)(HDC, HGLRC);
typedef HGLRC (WINAPI *wgl_getctx_t)(void);
typedef HDC   (WINAPI *wgl_getdc_t)(void);
typedef PROC  (WINAPI *wgl_proc_t)(LPCSTR);
typedef int   (WINAPI *gdi_getpf_t)(HDC);
typedef int   (WINAPI *gdi_descpf_t)(HDC, int, UINT, LPPIXELFORMATDESCRIPTOR);
typedef BOOL  (WINAPI *gdi_setpf_t)(HDC, int, const PIXELFORMATDESCRIPTOR *);
typedef BOOL  (WINAPI *gdi_swap_t)(HDC);
/* v89: how the window path is really doing. A fence goes in after each
 * present; at the next present, the fences not yet passed say how many
 * frames the GPU is still behind (2+ all the time = the GPU is the limit,
 * and each frame is shown that much later). The CPU-side interval between
 * SwapBuffers calls is the pacing the eye sees. */
#define GW_F 8
static struct {
    GLsync f[GW_F]; int fn;
    uint64_t frames, behind_sum, behind_max, behind2;
    uint64_t last_qpc, iv_max_us, iv_over20, iv_over34;
} s_gwt;

static void gw_timing_swap(void)
{
    uint64_t now = SDL_GetPerformanceCounter(), fq = SDL_GetPerformanceFrequency();
    int i, behind = 0;
    if (s_gwt.last_qpc) {
        uint64_t iv = (now - s_gwt.last_qpc) * 1000000ull / fq;
        if (iv > s_gwt.iv_max_us) s_gwt.iv_max_us = iv;
        if (iv > 20000) s_gwt.iv_over20++;
        if (iv > 34000) s_gwt.iv_over34++;
    }
    s_gwt.last_qpc = now;
    for (i = 0; i < GW_F; i++)
        if (s_gwt.f[i]) {
            if (glClientWaitSync(s_gwt.f[i], 0, 0) == GL_TIMEOUT_EXPIRED) {
                behind++;
            } else {
                glDeleteSync(s_gwt.f[i]);
                s_gwt.f[i] = 0;
            }
        }
    s_gwt.behind_sum += (uint64_t)behind;
    if ((uint64_t)behind > s_gwt.behind_max) s_gwt.behind_max = (uint64_t)behind;
    if (behind >= 2) s_gwt.behind2++;
    i = s_gwt.fn++ % GW_F;
    if (s_gwt.f[i]) glDeleteSync(s_gwt.f[i]);
    s_gwt.f[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s_gwt.frames++;
}

/* v101: when the GPU actually gets to each present. A timestamp query goes
 * in just before SwapBuffers and another at the frame's first draw; read
 * back when ready (never waited on), they give the GPU-side spacing of
 * finished frames -- the cadence the screen can show, which the CPU-side
 * present intervals above do not see -- and the span from the first draw to
 * the present on the GPU (when the GPU keeps up, about the CPU's submission
 * time; when it is the limit, its own time for the frame).
 * v102: no more glGetInteger64v(GL_TIMESTAMP) beside each present: it is a
 * round trip to the GPU that waits for it, and cost 6-11 ms a frame (run
 * 101: "present" 3.5-7 s of every 10, against 0.2-0.5 s before). */
#define GQ_N 16
static struct {
    GLuint q[GQ_N], qs[GQ_N]; unsigned char has_start[GQ_N];
    unsigned issued, done; int ok, need_start;
    GLuint64 last;
    uint64_t n, iv_sum_us, iv_max_us, iv20, iv34, iv_short;
    uint64_t sn, span_sum_us, span_max_us, span16;
    double iv_sq;
} s_gq;

static void gq_poll(void)
{
    while (s_gq.done < s_gq.issued) {
        unsigned k = s_gq.done % GQ_N;
        GLuint avail = 0;
        GLuint64 t = 0;
        glGetQueryObjectuiv(s_gq.q[k], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail)
            break;
        glGetQueryObjectui64v(s_gq.q[k], GL_QUERY_RESULT, &t);
        if (s_gq.has_start[k]) {
            GLuint64 t0 = 0;
            glGetQueryObjectui64v(s_gq.qs[k], GL_QUERY_RESULT, &t0);
            if (t > t0) {
                uint64_t sp = (uint64_t)(t - t0) / 1000u;
                s_gq.sn++;
                s_gq.span_sum_us += sp;
                if (sp > s_gq.span_max_us) s_gq.span_max_us = sp;
                if (sp > 16000) s_gq.span16++;
            }
        }
        if (s_gq.last && t > s_gq.last) {
            uint64_t iv = (uint64_t)(t - s_gq.last) / 1000u;
            s_gq.n++;
            s_gq.iv_sum_us += iv;
            s_gq.iv_sq += (double)iv * (double)iv;
            if (iv > s_gq.iv_max_us) s_gq.iv_max_us = iv;
            if (iv > 20000) s_gq.iv20++;
            if (iv > 34000) s_gq.iv34++;
            if (iv < 12000) s_gq.iv_short++;
        }
        s_gq.last = t;
        s_gq.done++;
    }
}

/* at each draw: the first one of a frame gets its start stamp */
static void gq_frame_start(void)
{
    if (s_gq.ok && s_gq.need_start && s_gq.issued - s_gq.done < GQ_N) {
        unsigned k = s_gq.issued % GQ_N;
        glQueryCounter(s_gq.qs[k], GL_TIMESTAMP);
        s_gq.has_start[k] = 1;
        s_gq.need_start = 0;
    }
}

static void gq_mark(void)
{
    unsigned k;
    if (!s_gq.ok) {
        if (getenv("RECOMP_GL_NO_GPU_TIMING") || !glad_glQueryCounter || s_gt_on)
            return;
        glGenQueries(GQ_N, s_gq.q);
        glGenQueries(GQ_N, s_gq.qs);
        s_gq.ok = 1;
    }
    gq_poll();
    if (s_gq.issued - s_gq.done >= GQ_N) {
        /* 16 frames unread: the GPU is that far behind; skip a mark rather
         * than wait for it */
        s_gq.need_start = 1;
        return;
    }
    k = s_gq.issued % GQ_N;
    glQueryCounter(s_gq.q[k], GL_TIMESTAMP);
    s_gq.issued++;
    s_gq.has_start[s_gq.issued % GQ_N] = 0;
    s_gq.need_start = 1;
}

int nv2a_gl_gpu_frame_stats(char *buf, size_t n)
{
    double avg, sd;
    if (!s_gq.ok)
        return 0;
    gq_poll();
    if (!s_gq.n)
        return 0;
    avg = (double)s_gq.iv_sum_us / (double)s_gq.n;
    sd = s_gq.iv_sq / (double)s_gq.n - avg * avg;
    sd = sd > 0 ? sqrt(sd) : 0;
    snprintf(buf, n, "%llu frames finished; spacing %.1f ms average, %.1f ms spread,"
             " up to %.1f ms, %llu under 12 ms, %llu over 20 ms, %llu over 34 ms;"
             " first draw to present %.1f ms average, up to %.1f ms, %llu over 16 ms",
             (unsigned long long)s_gq.n, avg / 1000.0, sd / 1000.0,
             s_gq.iv_max_us / 1000.0, (unsigned long long)s_gq.iv_short,
             (unsigned long long)s_gq.iv20, (unsigned long long)s_gq.iv34,
             s_gq.sn ? s_gq.span_sum_us / 1000.0 / (double)s_gq.sn : 0.0,
             s_gq.span_max_us / 1000.0, (unsigned long long)s_gq.span16);
    {
        size_t l = strlen(buf);
        snprintf(buf + l, n > l ? n - l : 0, "; specialised shaders %u in use, %u new, %u evicted",
                 s_spec_live, s_spec_new, s_spec_evicted);
        s_spec_new = s_spec_evicted = 0;
    }
    s_gq.n = s_gq.iv_sum_us = s_gq.iv_max_us = s_gq.iv20 = s_gq.iv34 = 0;
    s_gq.iv_short = s_gq.sn = s_gq.span_sum_us = s_gq.span_max_us = s_gq.span16 = 0;
    s_gq.iv_sq = 0;
    return 1;
}

void nv2a_gl_window_stats(uint64_t *frames, double *behind_avg, uint64_t *behind_max,
                          uint64_t *behind2, double *iv_max_ms, uint64_t *iv20,
                          uint64_t *iv34)
{
    *frames = s_gwt.frames;
    *behind_avg = s_gwt.frames ? (double)s_gwt.behind_sum / (double)s_gwt.frames : 0.0;
    *behind_max = s_gwt.behind_max;
    *behind2 = s_gwt.behind2;
    *iv_max_ms = s_gwt.iv_max_us / 1000.0;
    *iv20 = s_gwt.iv_over20;
    *iv34 = s_gwt.iv_over34;
    s_gwt.frames = s_gwt.behind_sum = s_gwt.behind_max = s_gwt.behind2 = 0;
    s_gwt.iv_max_us = s_gwt.iv_over20 = s_gwt.iv_over34 = 0;
}

/* v108: presenting through a DXGI flip-model swap chain. GL still renders;
 * the finished frame is blitted into a D3D11 texture shared with GL
 * (WGL_NV_DX_interop2), copied to the swap chain's back buffer and shown
 * with Present. A flip-model swap chain is what Windows and the driver
 * treat as a game's output: it gets independent flip and the game power
 * profile, presents are paced by DXGI rather than by GL's SwapBuffers on a
 * borrowed window DC, and vsync (RECOMP_GL_VSYNC=1) waits for at most one
 * refresh with one frame queued (SetMaximumFrameLatency 1). Without vsync
 * it presents with tearing allowed where the system supports it.
 * RECOMP_GL_PRESENT=gl keeps v88's SwapBuffers path, which is also the
 * fallback when anything here is unavailable. */
#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif
#define DXP_TEARING_FLAG 2048u          /* DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING */
#define DXP_PRESENT_TEARING 0x200u      /* DXGI_PRESENT_ALLOW_TEARING */
#define WGL_ACCESS_WRITE_DISCARD_NV_ 0x0002
static const GUID dxp_IID_IDXGIDevice  = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const GUID dxp_IID_IDXGIDevice1 = {0x77db970f,0x6276,0x48ba,{0xba,0x28,0x07,0x01,0x43,0xb4,0x39,0x2c}};
static const GUID dxp_IID_IDXGIFactory2 = {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};
static const GUID dxp_IID_IDXGIFactory5 = {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}};
static const GUID dxp_IID_ID3D11Texture2D = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
typedef HANDLE (WINAPI *dxo_t)(void *);
typedef BOOL   (WINAPI *dxc_t)(HANDLE);
typedef HANDLE (WINAPI *dxr_t)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL   (WINAPI *dxu_t)(HANDLE, HANDLE);
typedef BOOL   (WINAPI *dxl_t)(HANDLE, GLint, HANDLE *);
typedef HRESULT (WINAPI *d3dcreate_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                       const D3D_FEATURE_LEVEL *, UINT, UINT,
                                       ID3D11Device **, D3D_FEATURE_LEVEL *,
                                       ID3D11DeviceContext **);
static struct {
    int state;                          /* 0 untried, 1 working, -1 off */
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    IDXGISwapChain1 *sc; ID3D11Texture2D *tex;
    HANDLE hdev, hobj;
    GLuint rb, fbo;
    int w, h, tearing, vsync;
    UINT scflags;
    dxo_t open; dxc_t close; dxr_t reg; dxu_t unreg; dxl_t lock, unlock;
} s_dx;

static void dxp_drop_tex(void)
{
    if (s_dx.hobj) { s_dx.unreg(s_dx.hdev, s_dx.hobj); s_dx.hobj = NULL; }
    if (s_dx.tex) { ID3D11Texture2D_Release(s_dx.tex); s_dx.tex = NULL; }
}

static int dxp_make_tex(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dx.dev, &td, NULL, &s_dx.tex)))
        return 0;
    if (!s_dx.rb) glGenRenderbuffers(1, &s_dx.rb);
    if (!s_dx.fbo) glGenFramebuffers(1, &s_dx.fbo);
    s_dx.hobj = s_dx.reg(s_dx.hdev, s_dx.tex, s_dx.rb, GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV_);
    if (!s_dx.hobj)
        return 0;
    if (!s_dx.lock(s_dx.hdev, 1, &s_dx.hobj))
        return 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_dx.fbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_dx.rb);
    {
        GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        s_dx.unlock(s_dx.hdev, 1, &s_dx.hobj);
        if (st != GL_FRAMEBUFFER_COMPLETE)
            return 0;
    }
    s_dx.w = w; s_dx.h = h;
    return 1;
}

static int dxp_fail(const char *why)
{
    fprintf(stderr, "[GL] DXGI presentation unavailable (%s) -- presenting with SwapBuffers\n", why);
    s_dx.state = -1;
    return 0;
}

static int dxp_init(HWND hwnd, int cw, int ch)
{
    HMODULE gl = GetModuleHandleA("opengl32.dll"), d3d;
    PROC (WINAPI *gp)(LPCSTR) = gl ? (PROC (WINAPI *)(LPCSTR))(void *)GetProcAddress(gl, "wglGetProcAddress") : NULL;
    d3dcreate_t create;
    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *f2 = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    const char *e = getenv("RECOMP_GL_PRESENT");
    /* v109: opt-in. Handing the frame from GL to D3D (wglDXUnlockObjectsNV)
     * waits for GL to finish it, so each present waited out the frame's GPU
     * work -- "present" 4.8 s of every 10 and frames 20+ ms apart (run 108),
     * where SwapBuffers holds 60. RECOMP_GL_PRESENT=dxgi turns it on. */
    if (!(e && !strcmp(e, "dxgi"))) {
        s_dx.state = -1;
        return 0;
    }
    if (!gp)
        return dxp_fail("no wglGetProcAddress");
    s_dx.open = (dxo_t)(void *)gp("wglDXOpenDeviceNV");
    s_dx.close = (dxc_t)(void *)gp("wglDXCloseDeviceNV");
    s_dx.reg = (dxr_t)(void *)gp("wglDXRegisterObjectNV");
    s_dx.unreg = (dxu_t)(void *)gp("wglDXUnregisterObjectNV");
    s_dx.lock = (dxl_t)(void *)gp("wglDXLockObjectsNV");
    s_dx.unlock = (dxl_t)(void *)gp("wglDXUnlockObjectsNV");
    if (!s_dx.open || !s_dx.reg || !s_dx.unreg || !s_dx.lock || !s_dx.unlock)
        return dxp_fail("the driver has no WGL_NV_DX_interop");
    d3d = LoadLibraryA("d3d11.dll");
    create = d3d ? (d3dcreate_t)(void *)GetProcAddress(d3d, "D3D11CreateDevice") : NULL;
    if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 NULL, 0, D3D11_SDK_VERSION, &s_dx.dev, NULL, &s_dx.ctx)))
        return dxp_fail("no D3D11 device");
    if (FAILED(ID3D11Device_QueryInterface(s_dx.dev, &dxp_IID_IDXGIDevice, (void **)&xd))
            || FAILED(IDXGIDevice_GetAdapter(xd, &ad))
            || FAILED(IDXGIAdapter_GetParent(ad, &dxp_IID_IDXGIFactory2, (void **)&f2))) {
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        return dxp_fail("no DXGI 1.2 factory");
    }
    {
        IDXGIFactory5 *f5 = NULL;
        BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory2_QueryInterface(f2, &dxp_IID_IDXGIFactory5, (void **)&f5))) {
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, (DXGI_FEATURE)0, &t, sizeof t)) && t)
                s_dx.tearing = 1;
            IDXGIFactory5_Release(f5);
        }
    }
    {
        const char *v = getenv("RECOMP_GL_VSYNC");
        s_dx.vsync = v && v[0] == '1';
    }
    s_dx.scflags = s_dx.tearing ? DXP_TEARING_FLAG : 0;
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)cw; sd.Height = (UINT)ch;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = s_dx.scflags;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)s_dx.dev, hwnd, &sd, NULL, NULL, &s_dx.sc))) {
        IDXGIFactory2_Release(f2); IDXGIAdapter_Release(ad); IDXGIDevice_Release(xd);
        return dxp_fail("CreateSwapChainForHwnd failed");
    }
    IDXGIFactory2_MakeWindowAssociation(f2, hwnd, DXGI_MWA_NO_ALT_ENTER);
    {
        IDXGIDevice1 *d1 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dx.dev, &dxp_IID_IDXGIDevice1, (void **)&d1))) {
            IDXGIDevice1_SetMaximumFrameLatency(d1, 1);
            IDXGIDevice1_Release(d1);
        }
    }
    IDXGIFactory2_Release(f2); IDXGIAdapter_Release(ad); IDXGIDevice_Release(xd);
    s_dx.hdev = s_dx.open(s_dx.dev);
    if (!s_dx.hdev)
        return dxp_fail("wglDXOpenDeviceNV failed");
    if (!dxp_make_tex(cw, ch))
        return dxp_fail("could not share a D3D11 texture with GL");
    s_dx.state = 1;
    fprintf(stderr, "[GL] frames presented through a DXGI flip-model swap chain (%s%s;"
            " without RECOMP_GL_PRESENT=dxgi it is SwapBuffers)\n",
            s_dx.vsync ? "vsync" : "no vsync",
            !s_dx.vsync ? (s_dx.tearing ? ", tearing allowed" : ", tearing not supported") : "");
    return 1;
}

/* Present s through DXGI; 0 = not available (caller falls back). */
static int dxp_present(GlSurf *s, HWND hwnd, int cw, int ch, int dx, int dy, int dw, int dh)
{
    ID3D11Texture2D *bb = NULL;
    if (s_dx.state < 0)
        return 0;
    if (s_dx.state == 0 && !dxp_init(hwnd, cw, ch))
        return 0;
    if (cw != s_dx.w || ch != s_dx.h) {
        dxp_drop_tex();
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_dx.sc, 0, (UINT)cw, (UINT)ch,
                                                 DXGI_FORMAT_UNKNOWN, s_dx.scflags))
                || !dxp_make_tex(cw, ch)) {
            fprintf(stderr, "[GL] DXGI resize to %dx%d failed\n", cw, ch);
            return 1;                       /* skip this frame */
        }
    }
    if (!s_dx.lock(s_dx.hdev, 1, &s_dx.hobj))
        return 1;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_dx.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    /* D3D's row 0 is the top: flip vertically on the way in */
    glBlitFramebuffer(0, 0, SC(s->w), SC(s->h), dx, dy + dh, dx + dw, dy,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gq_mark();
    s_dx.unlock(s_dx.hdev, 1, &s_dx.hobj);
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(s_dx.sc, 0, &dxp_IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11DeviceContext_CopyResource(s_dx.ctx, (ID3D11Resource *)bb, (ID3D11Resource *)s_dx.tex);
        ID3D11Texture2D_Release(bb);
    }
    IDXGISwapChain1_Present(s_dx.sc, s_dx.vsync ? 1 : 0,
                            (!s_dx.vsync && s_dx.tearing) ? DXP_PRESENT_TEARING : 0);
    gw_timing_swap();
    glEnable(GL_SCISSOR_TEST);
    s_bound_fbo = (GLuint)-1;
    s_bound_vp[0] = s_bound_vp[1] = -1;
    s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
    s_ap_valid = 0;
    return 1;
}

static struct {
    int state;                 /* 0 untried, 1 working, -1 off */
    HWND hwnd; HDC hdc, own_hdc; HGLRC ctx;
    wgl_make_t make; gdi_swap_t swap;
} s_gw;
static int s_gw_tries, s_gw_wait;   /* v147: retries of the window setup */

static int gl_window_present_impl(GlSurf *s)
{
    RECT cr;
    int cw, ch, dw, dh, dx, dy;
    if (s_gw.state < 0)
        return 0;
    /* v108: DXGI first; the window's own GL setup only if that is off */
    if (s_dx.state >= 0) {
        HWND hw = (HWND)xbox_FramebufferWindowHwnd();
        if (!hw)
            return 0;
        if (GetClientRect(hw, &cr)) {
            cw = cr.right - cr.left; ch = cr.bottom - cr.top;
            if (cw <= 0 || ch <= 0)
                return 1;
            xbox_PresentFit(cw, ch, &dw, &dh);   /* v187: 4:3 or widescreen */
            dx = (cw - dw) / 2; dy = (ch - dh) / 2;
            if (dxp_present(s, hw, cw, ch, dx, dy, dw, dh)) {
                if (s_dx.state == 1 && !s_gw.state) {
                    s_gw.state = 2;            /* GL never touches the window */
                    xbox_FramebufferWindowGLOwned();
                }
                return 1;
            }
        }
        if (s_dx.state >= 0)
            return 0;
    }
    if (s_gw.state == 2)
        return 0;
    if (s_gw.state == 0 && s_gw_wait > 0) {
        s_gw_wait--;                        /* v147: a retry is due later */
        return 0;
    }
    if (s_gw.state == 0) {
        HMODULE gl = LoadLibraryA("opengl32.dll"), gdi = LoadLibraryA("gdi32.dll");
        wgl_getctx_t getctx = gl ? (wgl_getctx_t)GetProcAddress(gl, "wglGetCurrentContext") : NULL;
        wgl_getdc_t getdc = gl ? (wgl_getdc_t)GetProcAddress(gl, "wglGetCurrentDC") : NULL;
        wgl_proc_t getproc = gl ? (wgl_proc_t)GetProcAddress(gl, "wglGetProcAddress") : NULL;
        gdi_getpf_t getpf = gdi ? (gdi_getpf_t)GetProcAddress(gdi, "GetPixelFormat") : NULL;
        gdi_descpf_t descpf = gdi ? (gdi_descpf_t)GetProcAddress(gdi, "DescribePixelFormat") : NULL;
        gdi_setpf_t setpf = gdi ? (gdi_setpf_t)GetProcAddress(gdi, "SetPixelFormat") : NULL;
        PIXELFORMATDESCRIPTOR pfd;
        int fmt;
        s_gw.make = gl ? (wgl_make_t)GetProcAddress(gl, "wglMakeCurrent") : NULL;
        s_gw.swap = gdi ? (gdi_swap_t)GetProcAddress(gdi, "SwapBuffers") : NULL;
        if (getenv("RECOMP_GL_WINDOW_CPU") || !getctx || !getdc || !getpf || !descpf
                || !setpf || !s_gw.make || !s_gw.swap) {
            s_gw.state = -1;
            return 0;
        }
        s_gw.hwnd = (HWND)xbox_FramebufferWindowHwnd();
        if (!s_gw.hwnd)
            return 0;                       /* window not open yet: next flip */
        s_gw.ctx = getctx();
        s_gw.own_hdc = getdc();
        s_gw.hdc = GetDC(s_gw.hwnd);
        fmt = s_gw.own_hdc ? getpf(s_gw.own_hdc) : 0;
        memset(&pfd, 0, sizeof pfd);
        {
            /* v147: under Proton this failed now and then at startup (run
             * 146b: SetPixelFormat refused, every frame then read back
             * through the CPU, 2-4.6 s in 10). A window that already has
             * another format may still take the context, so that is tried;
             * and a failure is retried a few times, a second apart, before
             * giving up on the GPU path. */
            int wfmt = s_gw.hdc ? getpf(s_gw.hdc) : 0, ok = 1;
            unsigned long err = 0;
            const char *why = NULL;
            if (!s_gw.ctx || !s_gw.hdc || !fmt
                    || !descpf(s_gw.own_hdc, fmt, sizeof pfd, &pfd)
                    || !(pfd.dwFlags & PFD_DOUBLEBUFFER)) {
                ok = 0; why = "no usable format";
            } else if (wfmt != fmt && !setpf(s_gw.hdc, fmt, &pfd)) {
                err = GetLastError();
                if (!wfmt) { ok = 0; why = "SetPixelFormat failed"; }
                /* else: the window has format wfmt; try the context on it */
            }
            if (ok && !s_gw.make(s_gw.hdc, s_gw.ctx)) {
                err = GetLastError();
                s_gw.make(s_gw.own_hdc, s_gw.ctx);
                ok = 0; why = "wglMakeCurrent failed";
            }
            if (!ok) {
                fprintf(stderr, "[GL] cannot draw into the game window yet (%s; format %d,"
                        " window format %d, flags 0x%lX, error %lu)%s\n", why, fmt, wfmt,
                        (unsigned long)pfd.dwFlags, err,
                        s_gw_tries < 9 ? " -- trying again shortly" : " -- frames go through the CPU");
                if (s_gw.hdc) ReleaseDC(s_gw.hwnd, s_gw.hdc);
                s_gw.hdc = NULL;
                if (++s_gw_tries < 10) {
                    s_gw_wait = 60;          /* flips: about a second */
                    return 0;
                }
                s_gw.state = -1;
                return 0;
            }
            if (wfmt && wfmt != fmt)
                fprintf(stderr, "[GL] game window keeps its format %d (the context's is %d)\n",
                        wfmt, fmt);
        }
        {
            typedef BOOL (WINAPI *swapint_t)(int);
            swapint_t si = getproc ? (swapint_t)getproc("wglSwapIntervalEXT") : NULL;
            const char *v = getenv("RECOMP_GL_VSYNC");
            if (si) si(v && v[0] == '1' ? 1 : 0);
        }
        s_gw.make(s_gw.own_hdc, s_gw.ctx);
        s_gw.state = 1;
        xbox_FramebufferWindowGLOwned();
        fprintf(stderr, "[GL] frames drawn into the game window on the GPU\n");
    }
    if (!GetClientRect(s_gw.hwnd, &cr))
        return 0;
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0)
        return 1;                           /* minimised: nothing to show */
    xbox_PresentFit(cw, ch, &dw, &dh);   /* v187: 4:3 or widescreen */
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    if (!s_gw.make(s_gw.hdc, s_gw.ctx)) {
        s_gw.make(s_gw.own_hdc, s_gw.ctx);
        return 0;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, SC(s->w), SC(s->h), dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gq_mark();
    s_gw.swap(s_gw.hdc);
    gw_timing_swap();
    s_gw.make(s_gw.own_hdc, s_gw.ctx);
    /* everything cached about bindings and fixed state is stale now */
    glEnable(GL_SCISSOR_TEST);
    s_bound_fbo = (GLuint)-1;
    s_bound_vp[0] = s_bound_vp[1] = -1;
    s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
    s_ap_valid = 0;
    return 1;
}

/* v172: with the GL thread, a frame into the game window once that is set
 * up: the blit and SwapBuffers on the GL thread (queued); the caches of
 * what is bound are dropped here, where they are kept */
typedef struct { GLuint fbo; uint32_t w, h; uint64_t queued; } GwFrame;
/* v172b: how long a finished frame waited in the queue before SwapBuffers --
 * the delay the GL thread adds to what the player sees */
static volatile uint64_t s_gt_lat_sum, s_gt_lat_max, s_gt_lat_n;
static void gw_present_gl(void *arg)
{
    const GwFrame *f = (const GwFrame *)arg;
    RECT cr;
    int cw, ch, dw, dh, dx, dy;
    if (!GetClientRect(s_gw.hwnd, &cr))
        return;
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0)
        return;                             /* minimised: nothing to show */
    xbox_PresentFit(cw, ch, &dw, &dh);   /* v187: 4:3 or widescreen */
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    if (!s_gw.make(s_gw.hdc, s_gw.ctx)) {
        s_gw.make(s_gw.own_hdc, s_gw.ctx);
        return;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, SC(f->w), SC(f->h), dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    s_gw.swap(s_gw.hdc);
    {
        uint64_t d = TM_NOW() - f->queued;
        s_gt_lat_sum += d;
        if (d > s_gt_lat_max) s_gt_lat_max = d;
        s_gt_lat_n++;
    }
    gw_timing_swap();
    s_gw.make(s_gw.own_hdc, s_gw.ctx);
    glEnable(GL_SCISSOR_TEST);
}
typedef struct { GlSurf *s; int r; } GwSync;
static void gw_present_sync(void *arg)
{
    GwSync *a = (GwSync *)arg;
    a->r = gl_window_present_impl(a->s);
}
static int gl_window_present(GlSurf *s)
{
    if (s_gt_on && t_gt_mode == 1) {
        if (s_gw.state == 1) {
            GwFrame f;
            f.fbo = s->fbo; f.w = s->w; f.h = s->h;
            f.queued = TM_NOW();
            gt_call(gw_present_gl, &f, sizeof f);
            s_bound_fbo = (GLuint)-1;
            s_bound_vp[0] = s_bound_vp[1] = -1;
            s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
            s_ap_valid = 0;
            return 1;
        }
        {
            /* setting up (or not possible): all of it there, waited for */
            GwSync a;
            a.s = s; a.r = 0;
            gt_call_sync(gw_present_sync, &a);
            return a.r;
        }
    }
    return gl_window_present_impl(s);
}
#else
static int gl_window_present(GlSurf *s) { (void)s; return 0; }
void nv2a_gl_window_stats(uint64_t *frames, double *behind_avg, uint64_t *behind_max,
                          uint64_t *behind2, double *iv_max_ms, uint64_t *iv20,
                          uint64_t *iv34)
{ *frames = *behind_max = *behind2 = *iv20 = *iv34 = 0; *behind_avg = *iv_max_ms = 0; }
int nv2a_gl_gpu_frame_stats(char *buf, size_t n) { (void)buf; (void)n; return 0; }
static void gq_frame_start(void) {}
#endif

uint32_t nv2a_gl_present(uint32_t surf_va)
{
    static int sync = -1;
    GlSurf *s = NULL;
    uint32_t shown = 0;
    int i;

    if (s_state != 1)
        return surf_va;
    if (sync < 0)
        sync = getenv("RECOMP_GL_SYNC_PRESENT") != NULL;
    for (i = 0; i < NV_GL_SURFS; i++)
        if (s_surf[i].fbo && s_surf[i].va == surf_va)
            s = &s_surf[i];
    {
        int win = !sync && s && s->host && s->dirty && present_direct()
               && xbox_FramebufferWindowAccepts((uint32_t)SC(s->w), (uint32_t)SC(s->h));
        if (win && surf_cpu_wrote(s))
            win = 0;                      /* guest is current again: show it */
        if (win && gl_window_present(s)) {
            present_collect();            /* a read still in flight, if any */
            return 0;                     /* shown already */
        }
        if (win) {
            present_collect();            /* the previous frame, to the window */
            start_present_read(s, 1);
            return 0;                     /* nothing for the caller to show */
        }
    }
    if (sync || !s || !s->dirty || !s->host) {
        present_collect();
        nv2a_gl_sync_to_guest(surf_va);
        return surf_va;
    }
    if (s_pend.surf) {
        shown = s_pend.win ? 0 : s_pend.surf->va;
        present_collect();
    }
    start_present_read(s, 0);
    return shown;
}

static void start_present_read(GlSurf *s, int win)
{
    {
        uint32_t pw = win ? (uint32_t)SC(s->w) : s->w, ph = win ? (uint32_t)SC(s->h) : s->h;
        size_t bytes = (size_t)pw * ph * 4;
        if (!s_pend.pbo)
            glGenBuffers(1, &s_pend.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pend.pbo);
        if (s_pend.bytes < bytes) {
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
            s_pend.bytes = bytes;
        }
        /* the window takes the full-size frame; guest memory its own size */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, win ? s->fbo : guest_res_fbo(s->fbo, s->w, s->h));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pend.pbo);
        glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, (void *)0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        s_pend.surf = s;
        s_pend.draws = s->draws;
        s_pend.win = win;
        s_pend.pw = pw; s_pend.ph = ph;
    }
}

void nv2a_gl_sync_to_guest(uint32_t surf_va)
{
    int i;
    if (s_state != 1)
        return;
    if (s_pend.surf && (!surf_va || s_pend.surf->va == surf_va))
        present_collect();
    for (i = 0; i < NV_GL_SURFS; i++)
        if (s_surf[i].fbo && s_surf[i].dirty
                && (!surf_va || s_surf[i].va == surf_va))
            surf_readback(&s_surf[i]);
}

/* ── textures ─────────────────────────────────────────────────────────── */

#define NV_GL_TEXTURES 512
typedef struct {
    uint32_t va, w, h, pitch, color, palette, cube, bytes;
    uint64_t hash;
    uint32_t checked_frame;   /* hash verified this frame */
    uint64_t shash;           /* v143: sparse hash, see tex_sparse */
    uint32_t full_frame;      /* v143: frame of the last full hash */
    uint32_t used;
    int      mips;
    GLuint   id;
    GLint    p_mag, p_min, p_ws, p_wt;   /* parameters last set on id; 0 unknown */
    GLint    p_aniso;                    /* v187v: anisotropy last set (0 unknown) */
    int      rewritten;  /* seen changing between two binds of one frame */
    int      alloc;      /* storage allocated (w x h RGBA8) */
    int      from_gpu;   /* contents copied from a render target on the GPU */
    GLuint   src_fbo;    /* ...from this surface */
    uint32_t src_draws;  /* ...as of this many draws into it */
    int      comp;       /* v80: uploaded as S3TC with the guest's mip chain */
    int      nlev;       /* ...this many levels of it */
    GLint    p_maxlev;   /* GL_TEXTURE_MAX_LEVEL last set (GL default 1000) */
    XboxGwMark wm, wmp;  /* v117: write-watch over its bytes and its palette */
    uint32_t w_dirty_frame, w_ban;   /* ...last frame it reported a write; off until */
    int      w_streak;   /* ...frames in a row it did */
    uint32_t probe_frame;   /* v167: frame of the mid-frame re-checks below */
    int      probes;        /* ...how many of them so far */
} GlTex;
static int s_dxt_ok = -1;      /* S3TC uploads usable */
static uint32_t s_n_texcomp;   /* of the uploads, how many went up compressed */

/* The S3TC format and block bytes for a guest DXT colour format, or 0. */
static GLenum dxt_gl_format(uint32_t color, uint32_t *block)
{
    switch (color) {
    case 0x0C: *block = 8;  return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    case 0x0E: *block = 16; return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    case 0x0F: *block = 16; return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }
    return 0;
}
static GlTex s_tex[NV_GL_TEXTURES];
static uint32_t s_tex_clock;
#define NV_GL_TEX_HINTS 2048
static uint16_t s_tex_hint[NV_GL_TEX_HINTS];   /* index + 1 into s_tex */

static void textures_invalidate_range(uint32_t va, uint32_t bytes)
{
    int i;
    for (i = 0; i < NV_GL_TEXTURES; i++)
        if (s_tex[i].id && s_tex[i].va < va + bytes && va < s_tex[i].va + s_tex[i].bytes) {
            s_tex[i].checked_frame = 0;
            s_tex[i].wm.ok = 0;
        }
}

/* v143: without the write-watch (Wine/Proton), every texture bound in a
 * frame was hashed in full every frame: ~100k hashes, 0.7 s in 10 s on a
 * Steam Deck. Now a texture of 16 KB or more is checked each frame by a
 * sparse hash (8 bytes every 512, plus the last 8 and the palette), and in
 * full every 16 frames or when the sparse hash changes. New data loaded over
 * a texture changes nearly every sampled word and is caught at once; an edit
 * that misses every sample shows within 16 frames. Textures seen changing
 * within a frame keep the full check on every bind.
 * RECOMP_GL_TEX_FULLCHECK=1 hashes everything in full, as before. */
static uint64_t s_n_sparse;            /* checks answered by tex_sparse */
static int tex_sparse_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_TEX_FULLCHECK");
        v = !(e && e[0] == '1');
    }
    return v;
}
static uint64_t tex_sparse(const NvGlTexture *t, const uint8_t *mem)
{
    const uint8_t *p = mem + t->offset;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ t->bytes, w;
    uint32_t i;
    for (i = 0; i + 8 <= t->bytes; i += 512) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    if (t->bytes >= 8) {
        memcpy(&w, p + t->bytes - 8, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    if (t->palette)
        h ^= hash_bytes(mem + t->palette, 1024) * 31u;
    return h;
}

static uint64_t tex_hash(const NvGlTexture *t, const uint8_t *mem)
{
    uint64_t h = hash_bytes(mem + t->offset, t->bytes);
    if (t->palette)
        h ^= hash_bytes(mem + t->palette, 1024) * 31u;
    return h;
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

static GLenum gl_wrap(uint32_t mode)
{
    switch (mode) {
    case 1:  return GL_REPEAT;
    case 2:  return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}

/* Can `g`'s GL texture stand for texture `t`? Same memory from its first
 * byte, same row pitch, inside the surface, a linear format whose decode of
 * what the surface would hold is what the GL copy holds. */
static int tex_from_surface_ok(const GlSurf *g, const NvGlTexture *t)
{
    if (t->cube || t->palette || !t->width || !t->height)
        return 0;
    if (t->offset != g->va || t->pitch != g->pitch
            || t->width > g->w || t->height > g->h)
        return 0;
    switch (t->color) {
    case 0x11: return g->bpp == 2;              /* LIN_R5G6B5 */
    case 0x12:                                  /* LIN_A8R8G8B8 */
    case 0x1E: return g->bpp == 4;              /* LIN_X8R8G8B8 */
    }
    return 0;
}

static GLuint s_copy_fbo;

/* Guest rows run top-down; a surface's GL rows bottom-up (surf_upload), a
 * texture's top-down (bind_texture). So the blit flips. */
static void tex_copy_from_surface(GlTex *e, const NvGlTexture *t, GlSurf *g)
{
    GLuint prev = s_bound_fbo;
    if (!s_copy_fbo)
        glGenFramebuffers(1, &s_copy_fbo);
    /* alloc 2: storage at the scaled size (v87); a CPU upload in between
     * re-specified it at the guest size (alloc 1) */
    if (s_scale == 1 ? !e->alloc : e->alloc != 2) {
        unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, e->id); active_unit(NV_GL_SCRATCH_UNIT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)SC(t->width), (GLsizei)SC(t->height),
                     0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        if (t->color == 0x1E)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        e->alloc = s_scale == 1 ? 1 : 2;
    }
    bind_draw_fbo(s_copy_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           e->id, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo);
    glDisable(GL_SCISSOR_TEST);             /* v172: as blit_fb */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, SC(g->h - t->height), SC(t->width), SC(g->h),
                      0, SC(t->height), SC(t->width), 0,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    s_ap_valid = 0;
    bind_draw_fbo(prev == (GLuint)-1 ? 0 : prev);
    e->from_gpu = 1;
    e->src_fbo = g->fbo;
    e->src_draws = g->draws;
    e->hash = 0;            /* guest bytes are not what it holds */
    e->wm.ok = 0;
    e->checked_frame = s_frame;
    e->mips = -1;
    s_n_gpucopy++;
}

/* Bind stage `stage`'s texture to its unit, uploading it when its bytes have
 * changed. */
/* RECOMP_GL_TEX_PROBES: mid-frame re-checks per small texture on every 8th
 * frame (v167, default 3); a large value restores the old every-bind check */
static int s_tex_probes = -1;
/* v187t: a surface the GPU drew whose memory the CPU has since rewritten is
 * no longer that surface: the title freed it and loaded something else
 * there. Leaving graffiti select, the garage's textures were read from disc
 * into memory its previews had been drawn in; the first texture bound over
 * it read the stale previews back on top of them (the scrambled garage,
 * run 187s, 4:3 as well). The CPU's write wins: the GPU copy is dropped. */
static int surf_overwritten(GlSurf *g)
{
    if (!g->dirty || !g->host)
        return 0;
    if (guest_same(g->va, g->host, (uint32_t)surf_bytes(g), &g->wm, g->hash))
        return 0;
    {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "[GL] surface %08X rewritten by the CPU after it was drawn;"
                    " its GPU copy is dropped\n", g->va);
    }
    g->dirty = 0;
    xbox_gw_mark(g->va, (uint32_t)surf_bytes(g), &g->wm);
    g->hash = hash_bytes(g->host, surf_bytes(g));
    return 1;
}

static void bind_texture(int stage, const NvGlTexture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    GlTex *e = NULL, *victim = &s_tex[0];
    if (s_tex_probes < 0) {
        const char *ev = getenv("RECOMP_GL_TEX_PROBES");
        s_tex_probes = ev ? atoi(ev) : 3;
    }
    GLenum target = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int i, linear, mips, gpu_now = 0;

#define TEX_MATCH(c) ((c)->id && (c)->va == t->offset && (c)->w == t->width \
        && (c)->h == t->height && (c)->color == t->color && (c)->palette == t->palette \
        && (c)->cube == (uint32_t)t->cube && (c)->pitch == t->pitch)
    /* v117: a direct-mapped hint before the scan of all 512 */
    uint32_t hk = ((t->offset >> 7) ^ (t->offset >> 17) ^ (t->width * 31u)
                   ^ (t->color * 131u)) & (NV_GL_TEX_HINTS - 1);
    if (s_tex_hint[hk] && TEX_MATCH(&s_tex[s_tex_hint[hk] - 1]))
        e = &s_tex[s_tex_hint[hk] - 1];
    for (i = 0; !e && i < NV_GL_TEXTURES; i++) {
        GlTex *c = &s_tex[i];
        if (TEX_MATCH(c)) {
            e = c;
            break;
        }
        if (!c->id || c->used < victim->used)
            victim = c;
    }
#undef TEX_MATCH
    if (!e) {
        e = victim;
        if (!e->id) {
            glGenTextures(1, &e->id);
            e->p_mag = e->p_min = e->p_ws = e->p_wt = 0;
            e->p_aniso = 0;
            e->p_maxlev = 1000;
        }
        e->va = t->offset; e->w = t->width; e->h = t->height;
        e->color = t->color; e->palette = t->palette; e->cube = (uint32_t)t->cube;
        e->pitch = t->pitch; e->bytes = t->bytes;
        e->hash = 0; e->checked_frame = 0; e->mips = -1; e->rewritten = 0;
        e->alloc = 0; e->from_gpu = 0; e->src_fbo = 0; e->src_draws = 0;
        e->comp = 0; e->nlev = 0;
        e->wm.ok = e->wmp.ok = 0; e->w_streak = 0; e->w_ban = 0;
    }
    s_tex_hint[hk] = (uint16_t)(e - s_tex + 1);
    e->used = ++s_tex_clock;

    /* A texture over a surface the GPU has drawn and not written back reads
     * the guest copy, so bring that up to date first (see bind_surface).
     *
     * v67: when the texture is exactly that surface's memory in a format the
     * GPU copy can stand for, copy it texture-to-texture on the GPU instead.
     * The read-back was a synchronous glReadPixels (CPU waits for the whole
     * queue, GPU then idles), plus a store, a hash, a decode and an upload:
     * run 66 spent 1.1-1.9 s of every 10 there, JSRF sampling its previous
     * frame. RECOMP_GL_TEX_READBACK=1 restores the read-back. */
    {
        static int no_copy = -1;
        int k, n = 0;
        GlSurf *only = NULL;
        if (no_copy < 0)
            no_copy = getenv("RECOMP_GL_TEX_READBACK") != NULL;
        for (k = 0; k < NV_GL_SURFS; k++) {
            GlSurf *g = &s_surf[k];
            if (g->fbo && g->dirty && g->va < t->offset + t->bytes
                    && t->offset < g->va + (uint32_t)surf_bytes(g)
                    && !surf_overwritten(g)) {
                n++;
                only = g;
            }
        }
        if (n == 1 && !no_copy && tex_from_surface_ok(only, t)) {
            if (!e->from_gpu || e->src_fbo != only->fbo || e->src_draws != only->draws)
                tex_copy_from_surface(e, t, only);
            gpu_now = 1;
        } else if (n) {
            for (k = 0; k < NV_GL_SURFS; k++) {
                GlSurf *g = &s_surf[k];
                if (g->fbo && g->dirty && g->va < t->offset + t->bytes
                        && t->offset < g->va + (uint32_t)surf_bytes(g))
                    surf_readback(g);
            }
        }
    }

    unit_bind(t->cube ? 4 + stage : stage, target, e->id);
    /* v169: the unit is made active only before a call that needs it (an
     * upload, a parameter change) -- most binds need none, and making it
     * active every time was 1.5 glActiveTexture calls a draw */
    const int bt_unit = t->cube ? 4 + stage : stage;
    /* Small textures are re-checked on every bind: a title that streams
     * decals through one buffer rewrites it between draws of a frame, and
     * checking once a frame drew every one of them with the first image --
     * the flickering textures. Big ones (stage textures) once a frame. */
    /* ...but only the ones that have been caught doing it. Hashing every
     * small texture on every bind was 1.1 s of every 10 in play (run 58).
     * Every 8th frame all of them are checked on every bind; one whose bytes
     * change between two binds of the same frame is marked and from then on
     * always checked. The rest are checked once a frame. */
    /* v117: ...unless the write-watch has its pages. Then it is checked on
     * every bind, at no cost unless something wrote to the window since the
     * last check, and is hashed only when one of its pages was written. A
     * texture whose pages are written frame after frame (sharing them with
     * data the title rewrites) is left to the old policy for ten seconds at
     * a time, so it is not protected and faulted every frame. */
    if (e->bytes != t->bytes) {           /* same key, other level count */
        e->bytes = t->bytes;
        e->wm.ok = 0;
    }
    int watched = !gpu_now && e->wm.ok && (!t->palette || e->wmp.ok);
    int need;
    if (watched) {
        uint64_t t0 = TM_NOW();
        need = !(xbox_gw_clean(t->offset, t->bytes, &e->wm)
                 && (!t->palette || xbox_gw_clean(t->palette, 1024, &e->wmp)));
        s_tm[2] += TM_NOW() - t0;
        if (!need) {
            s_n_wskip++;
        } else {
            if (s_frame - e->w_dirty_frame <= 2) e->w_streak++;
            else e->w_streak = 1;
            e->w_dirty_frame = s_frame;
            if (e->w_streak >= 16) {
                e->w_ban = s_frame + 600;
                e->w_streak = 0;
            }
        }
    } else {
        /* v167: on the every-8th-frame pass, a few re-checks per texture
         * rather than one on every bind: one change between two binds of the
         * frame is enough to mark it rewritten, and those are then checked on
         * every bind as before. Hashing it at each of its binds that frame
         * was most of the texture-check time in busy areas. */
        int probe = 0;
        if (!gpu_now && e->checked_frame == s_frame && t->bytes <= 256u * 1024u
                && !e->rewritten && (s_frame & 7) == 0) {
            if (e->probe_frame != s_frame) {
                e->probe_frame = s_frame;
                e->probes = 0;
            }
            probe = e->probes < s_tex_probes;
            if (probe)
                e->probes++;
        }
        need = !gpu_now && (e->checked_frame != s_frame
            || (t->bytes <= 256u * 1024u && e->rewritten) || probe);
        if (need && !e->rewritten && e->hash && t->bytes >= 16384u && tex_sparse_on()) {
            uint64_t t0 = TM_NOW();
            uint64_t sh = tex_sparse(t, mem);
            if (sh == e->shash && (uint32_t)(s_frame - e->full_frame) < 16u) {
                need = 0;
                e->checked_frame = s_frame;
                s_n_sparse++;
            }
            s_tm[2] += TM_NOW() - t0;
        }
    }
    if (need) {
        uint64_t t0 = TM_NOW();
        active_unit(bt_unit);
        uint64_t h;
        int same_frame = e->checked_frame == s_frame;
        if ((int32_t)(e->w_ban - s_frame) <= 0) {
            xbox_gw_mark(t->offset, t->bytes, &e->wm);
            if (t->palette)
                xbox_gw_mark(t->palette, 1024, &e->wmp);
        } else {
            e->wm.ok = 0;
        }
        h = tex_hash(t, mem);
        s_n_whash++;
        e->checked_frame = s_frame;
        e->full_frame = s_frame;
        if (t->bytes >= 16384u && tex_sparse_on())
            e->shash = tex_sparse(t, mem);
        s_tm[2] += TM_NOW() - t0;
        if (h != e->hash && same_frame && e->hash)
            e->rewritten = 1;
        if (h != e->hash) {
            uint32_t faces = t->cube ? 6 : 1, f, block = 0;
            GLenum cfmt = dxt_gl_format(t->color, &block);
            uint32_t *px;
            /* v80: DXT textures go up as they are. Decoding every block to
             * RGBA8 on the CPU, uploading four to eight times the bytes and
             * then generating mips was the other half of the area-entry
             * hitches (run 79: 96-120 uploads, 180-220 ms in one frame). The
             * GPU reads S3TC natively and the guest's own mip chain follows
             * level 0 in memory, so there is nothing to decode or generate.
             * RECOMP_GL_DXT=0 restores the CPU decode. */
            if (s_dxt_ok < 0) {
                const char *ev = getenv("RECOMP_GL_DXT");
                s_dxt_ok = GLAD_GL_EXT_texture_compression_s3tc
                           && !(ev && ev[0] == '0');
                fprintf(stderr, "[GL] DXT textures uploaded %s\n",
                        s_dxt_ok ? "compressed (S3TC)" : "decoded on the CPU");
            }
            if (cfmt && s_dxt_ok && !t->cube && !t->palette
                    && !(t->width & 3) && !(t->height & 3)) {
                uint32_t lv, levels = t->levels ? t->levels : 1, n = 0;
                uint32_t w = t->width, hh = t->height, off = t->offset;
                uint32_t limit = t->offset + 2u * t->bytes;  /* chain < 4/3 */
                /* v172: the error checks are waits for the GL thread; once 16
                 * uploads went through without one, they are taken as good */
                static int dxt_good;
                int check = !s_gt_on || dxt_good < 16;
                if (check) { int q = 8; while (q-- && glGetError() != GL_NO_ERROR) {} }
                for (lv = 0; lv < levels && lv < 16; lv++) {
                    uint32_t bw = (w + 3) / 4, bh = (hh + 3) / 4;
                    uint32_t sz = (bw ? bw : 1) * (bh ? bh : 1) * block;
                    if (off + sz > limit)
                        break;
                    glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)lv, cfmt,
                                           (GLsizei)w, (GLsizei)hh, 0,
                                           (GLsizei)sz, mem + off);
                    off += sz;
                    n++;
                    if (w == 1 && hh == 1)
                        break;
                    w = w > 1 ? w >> 1 : 1;
                    hh = hh > 1 ? hh >> 1 : 1;
                }
                if (n && (!check || glGetError() == GL_NO_ERROR)) {
                    if (check) dxt_good++;
                    e->comp = 1;
                    e->nlev = (int)n;
                    e->hash = h;
                    e->mips = -1;
                    e->alloc = 1;
                    e->from_gpu = 0;
                    s_n_texup++;
                    s_n_texcomp++;
                    s_tm[3] += TM_NOW() - t0;
                    goto uploaded;
                }
                /* the driver refused it: the CPU decode below */
            }
            e->comp = 0;
            e->nlev = 0;
            px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
            if (px) {
                for (f = 0; f < faces; f++) {
                    if (!nv2a_exec_decode_texture(stage, f, px))
                        memset(px, 0xFF, (size_t)t->width * t->height * 4);
                    glTexImage2D(t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f
                                         : GL_TEXTURE_2D,
                                 0, GL_RGBA8, (GLsizei)t->width,
                                 (GLsizei)t->height, 0, GL_BGRA,
                                 GL_UNSIGNED_INT_8_8_8_8_REV, px);
                }
                free(px);
            }
            e->hash = h;
            e->mips = -1;
            e->alloc = 1;
            e->from_gpu = 0;
            s_n_texup++;
            s_tm[3] += TM_NOW() - t0;
        }
    }
uploaded:
    /* Mip-mapped minification when the title asks for it. The NV2A would
     * read the title's own mip chain; generating one from level 0 is close
     * and is what stops distant textures shimmering into noise. */
    linear = (mag == 2 || min == 2 || min == 4 || min == 6);
    mips = (min >= 3 && min <= 6) && !(t->width & (t->width - 1))
        && !(t->height & (t->height - 1)) && t->normalised;
    if (e->comp) {
        /* the guest's chain: only as many levels as it has, and with one
         * level no mip filter at all (what the NV2A does: LOD clamps to the
         * levels in FORMAT) */
        GLint want = e->nlev - 1;
        if (want < 0) want = 0;
        mips = mips && e->nlev > 1;
        if (e->p_maxlev != want) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, want);
            e->p_maxlev = want;
        }
    } else {
        if (e->p_maxlev != 1000) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 1000);
            e->p_maxlev = 1000;
        }
        if (mips && e->mips != 1) {
            active_unit(bt_unit); glGenerateMipmap(target);
            e->mips = 1;
        }
    }
    {
        GLint want_mag = mag == 2 ? GL_LINEAR : GL_NEAREST;
        GLint want_min = mips ? ((min == 3) ? GL_NEAREST_MIPMAP_NEAREST
                               : (min == 4) ? GL_LINEAR_MIPMAP_NEAREST
                               : (min == 5) ? GL_NEAREST_MIPMAP_LINEAR
                                            : GL_LINEAR_MIPMAP_LINEAR)
                              : (linear ? GL_LINEAR : GL_NEAREST);
        GLint ws = (GLint)gl_wrap(t->addr_u), wt = (GLint)gl_wrap(t->addr_v);
        if (e->p_mag != want_mag) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_MAG_FILTER, want_mag);
            e->p_mag = want_mag;
        }
        if (e->p_min != want_min) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_MIN_FILTER, want_min);
            e->p_min = want_min;
        }
        {
            /* v187v: RECOMP_GL_ANISO=2/4/8/16 -- anisotropic filtering of
             * mipmapped textures (sharper ground and walls at an angle);
             * the original is off (1). Capped at what the driver offers. */
            static GLint aniso = -1;
            GLint want_a;
            if (aniso < 0) {
                const char *ev = getenv("RECOMP_GL_ANISO");
                aniso = ev ? atoi(ev) : 1;
                if (aniso > 1 && (GLAD_GL_ARB_texture_filter_anisotropic
                                  || GLAD_GL_EXT_texture_filter_anisotropic)) {
                    GLfloat mx = 0.0f;
                    GLint mi = 0;
                    while (glGetError() != GL_NO_ERROR) ;
                    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &mx);
                    if (!(mx >= 2.0f)) {
                        glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &mi);
                        mx = (GLfloat)mi;
                    }
                    /* v187w: run 187v's AMD driver answered 1 here (an RX 6900
                     * XT, which does 16x), and 1x was used. A maximum under 2
                     * is not believed: the request is used (GL clamps it). */
                    if (mx >= 2.0f && aniso > (GLint)mx) aniso = (GLint)mx;
                    if (aniso > 16) aniso = 16;
                    fprintf(stderr, "[GL] anisotropic filtering %dx (RECOMP_GL_ANISO; driver"
                            " maximum reported %.0f)\n", aniso, mx);
                } else {
                    if (aniso > 1)
                        fprintf(stderr, "[GL] anisotropic filtering not offered by the driver\n");
                    aniso = 1;
                }
            }
            want_a = mips ? aniso : 1;
            if (aniso > 1 && e->p_aniso != want_a) {
                static int checked;
                active_unit(bt_unit);
                glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, (GLfloat)want_a);
                e->p_aniso = want_a;
                if (!checked && want_a > 1) {
                    GLfloat got = 0.0f;
                    GLenum err = glGetError();
                    glGetTexParameterfv(target, GL_TEXTURE_MAX_ANISOTROPY, &got);
                    checked = 1;
                    fprintf(stderr, "[GL] anisotropy on the first mipmapped texture: asked %d,"
                            " the driver has %.0f%s\n", want_a, got,
                            err != GL_NO_ERROR ? " (it reported an error setting it)" : "");
                }
            }
        }
        if (e->p_ws != ws) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_WRAP_S, ws);
            e->p_ws = ws;
        }
        if (e->p_wt != wt) {
            active_unit(bt_unit); glTexParameteri(target, GL_TEXTURE_WRAP_T, wt);
            if (t->cube)
                glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            e->p_wt = wt;
        }
    }
}

/* v118: a safety net under the write-watch. Each frame one texture the watch
 * calls unchanged is hashed anyway, in rotation; a mismatch means some write
 * reached it without the watch seeing it. It is logged ([GW] missed) and the
 * texture re-checked at its next bind, so a hole costs a few frames of a
 * stale image rather than a stale area. RECOMP_GL_TEX_VERIFY=0 turns it off. */
static void fx_frame(void);   /* v122, with the draws below */
static uint64_t s_n_wmissed;
static void watch_verify_one(void)
{
    static int on = -1;
    static int cur;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int k;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_TEX_VERIFY");
        on = !(e && e[0] == '0');
    }
    if (!on || !xbox_gw_enabled())
        return;
    for (k = 0; k < NV_GL_TEXTURES; k++) {
        GlTex *e = &s_tex[cur];
        cur = (cur + 1) % NV_GL_TEXTURES;
        if (!e->id || !e->wm.ok || e->from_gpu || !e->hash || e->bytes > (4u << 20))
            continue;
        if (!xbox_gw_clean(e->va, e->bytes, &e->wm)
                || (e->palette && !xbox_gw_clean(e->palette, 1024, &e->wmp)))
            return;                     /* already known changed: its bind handles it */
        {
            uint64_t h = hash_bytes(mem + e->va, e->bytes);
            if (e->palette)
                h ^= hash_bytes(mem + e->palette, 1024) * 31u;
            if (h != e->hash) {
                if (s_n_wmissed++ < 20)
                    fprintf(stderr, "[GW] missed a write: texture %08X %ux%u fmt %02X"
                            " (%u bytes) changed with its pages still marked clean\n",
                            e->va, e->w, e->h, e->color, e->bytes);
                e->wm.ok = 0;
                e->checked_frame = 0;
            }
        }
        return;
    }
}

void nv2a_gl_end_frame(void)
{
    if (s_state == 1) {
        watch_verify_one();
        fx_frame();
    }
    s_bound_ab = (GLuint)-1;   /* v169: bound again once a frame, whatever else ran */
    gt_frame_end();            /* v172: at most RECOMP_GL_THREAD_LAG frames queued */
    if (++s_frame == 0)
        s_frame = 1;
    if (s_win)
        SDL_PumpEvents();
}

/* ── state ────────────────────────────────────────────────────────────── */

static void unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

static NvGlState s_ap;            /* what apply_state last set */
static int s_ap_alpha;

#define RANGE_SAME(a, b, first, end) \
    (!memcmp(&(a)->first, &(b)->first, offsetof(NvGlState, end) - offsetof(NvGlState, first)))

static void apply_state(const NvGlState *st, const GlSurf *s)
{
    int i, all = !s_ap_valid;
    float f0[8][4], f1[8][4], c[4];
    GLint isc[4], tv[4];
    float tsc[4][2];

    /* Fixed-function state, each group only when it changed (v152: the
     * whole set used to go whenever any of it changed -- ~15 GL calls, each
     * a crossing into the Linux driver under Proton). Anything else that
     * touches this GL state clears s_ap_valid, as before. */
#define FF_CH(first, end) (all || !RANGE_SAME(st, &s_ap, first, end))
    if (FF_CH(depth_test, stencil_test)) {
        if (st->depth_test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(st->depth_func ? st->depth_func : GL_LESS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(st->depth_mask ? GL_TRUE : GL_FALSE);
    }
    if (FF_CH(stencil_test, blend_enable)) {
        if (st->stencil_test) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(st->stencil_func ? st->stencil_func : GL_ALWAYS,
                          (GLint)(st->stencil_ref & 0xFF), st->stencil_mask & 0xFF);
            /* 0 is GL_ZERO for the ops, a real op, so they pass as they are. */
            glStencilOp(st->stencil_fail, st->stencil_zfail, st->stencil_zpass);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        glStencilMask(st->stencil_wmask & 0xFF);
    }
    if (FF_CH(blend_enable, color_mask)) {
        if (st->blend_enable) {
            glEnable(GL_BLEND);
            glBlendFunc(st->blend_sfactor, st->blend_dfactor);
            glBlendEquation(st->blend_equation ? st->blend_equation : GL_FUNC_ADD);
            unpack(st->blend_color, c);
            glBlendColor(c[0], c[1], c[2], c[3]);
        } else {
            glDisable(GL_BLEND);
        }
    }
    if (FF_CH(color_mask, cull_enable) || s_ap_alpha != s->alpha)
        glColorMask((st->color_mask & 0x00010000u) != 0, (st->color_mask & 0x00000100u) != 0,
                    (st->color_mask & 0x00000001u) != 0,
                    (st->color_mask & 0x01000000u) != 0 && s->alpha);

    /* Depth bias. The NV2A's units are depth-buffer units, as GL's are
     * (the smallest resolvable step), so the values pass through. Depth is
     * written by the fragment shader straight from the title's depth-buffer
     * value (u_zdirect, run 63); the polygon offset then has to be applied
     * in the shader too, GL's own does not touch gl_FragDepth.
     * RECOMP_GL_FRAGDEPTH=0 goes back to GL's depth. Not for a batch with a
     * vertex behind the eye (zdirect_off): GL clips those triangles, and
     * JSRF's shadow volumes are extruded that far (run 64). */
    if (FF_CH(poly_offset, screen_space)) {
        static int direct = -1;
        int use;
        if (direct < 0) {
            const char *e = getenv("RECOMP_GL_FRAGDEPTH");
            direct = !(e && !strcmp(e, "0"));
        }
        use = direct && !st->zdirect_off;
        if (use) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        } else if (st->poly_offset) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(st->poly_factor, st->poly_units);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    if (FF_CH(cull_enable, poly_offset)) {
        if (st->cull_enable) {
            glEnable(GL_CULL_FACE);
            glCullFace(st->cull_face ? st->cull_face : GL_BACK);
            glFrontFace(st->front_face ? st->front_face : GL_CCW);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
#undef FF_CH

    memcpy(&s_ap, st, sizeof s_ap);
    s_ap_alpha = s->alpha;
    s_ap_valid = 1;

    /* v78: the uniforms, against what this program last had (each program
     * keeps its own values). */
    all = !P->ap_valid;
    {
        static int direct = -1;
        int use;
        float po[3];
        if (direct < 0) {
            const char *e = getenv("RECOMP_GL_FRAGDEPTH");
            direct = !(e && !strcmp(e, "0"));
        }
        use = direct && !st->zdirect_off;
        po[0] = st->poly_offset ? 1.0f : 0.0f; po[1] = st->poly_factor; po[2] = st->poly_units;
        if (all || use != P->ap_zdirect) {
            glUniform1i(u_zdirect, use);
            P->ap_zdirect = use;
        }
        if (use && (all || memcmp(po, P->ap_poff, sizeof po))) {
            glUniform3fv(u_poff, 1, po);
            memcpy(P->ap_poff, po, sizeof po);
        }
    }
    if (all || P->ap_scr != (int)st->screen_space) {
        glUniform1i(u_scr, (GLint)st->screen_space);
        P->ap_scr = (int)st->screen_space;
    }
    {
        float k = st->ws_k > 0.0f ? st->ws_k : 1.0f;     /* v187 */
        int d = k != 1.0f && st->ws_2d;
        if (all || P->ap_wsk != k) {
            glUniform1f(u_wsk, k);
            P->ap_wsk = k;
        }
        if (all || P->ap_wsc != st->ws_c) {
            glUniform1f(u_wsc, st->ws_c);
            P->ap_wsc = st->ws_c;
        }
        if (all || P->ap_ws2d != d) {
            glUniform1i(u_ws2d, d);
            P->ap_ws2d = d;
        }
    }
    if (all || P->ap_w != (int)s->w || P->ap_h != (int)s->h || P->ap.zmax != st->zmax) {
        glUniform2f(u_vp, (float)s->w, (float)s->h);
        glUniform1f(u_zs, st->zmax > 0.0f ? 1.0f / st->zmax : 1.0f / 16777215.0f);
        glUniform1f(u_zs2, st->zmax > 0.0f ? 1.0f / st->zmax : 1.0f / 16777215.0f);
    }
    /* Combiners, fog and alpha test: alpha_test through fog_color, each
     * uniform only when its own value changed for this program (v152: all
     * ~23 went whenever any of them changed -- a constant colour, usually). */
    if (all || !RANGE_SAME(st, &P->ap, alpha_test, tex)) {
#define RC_CH(f) (all || memcmp(&st->f, &P->ap.f, sizeof st->f))
        if (RC_CH(rc_seen)) glUniform1i(u_rc_on, st->rc_seen);
        if (RC_CH(rc.color_icw)) glUniform1uiv(u_cicw, 8, st->rc.color_icw);
        if (RC_CH(rc.alpha_icw)) glUniform1uiv(u_aicw, 8, st->rc.alpha_icw);
        if (RC_CH(rc.color_ocw)) glUniform1uiv(u_cocw, 8, st->rc.color_ocw);
        if (RC_CH(rc.alpha_ocw)) glUniform1uiv(u_aocw, 8, st->rc.alpha_ocw);
        if (RC_CH(rc.factor0)) {
            for (i = 0; i < 8; i++) unpack(st->rc.factor0[i], f0[i]);
            glUniform4fv(u_f0, 8, &f0[0][0]);
        }
        if (RC_CH(rc.factor1)) {
            for (i = 0; i < 8; i++) unpack(st->rc.factor1[i], f1[i]);
            glUniform4fv(u_f1, 8, &f1[0][0]);
        }
        if (RC_CH(rc.final0)) glUniform1ui(u_final0, st->rc.final0);
        if (RC_CH(rc.final1)) glUniform1ui(u_final1, st->rc.final1);
        if (RC_CH(rc.final_c0)) { unpack(st->rc.final_c0, c); glUniform4fv(u_fc0, 1, c); }
        if (RC_CH(rc.final_c1)) { unpack(st->rc.final_c1, c); glUniform4fv(u_fc1, 1, c); }
        if (RC_CH(rc.control)) glUniform1ui(u_control, st->rc.control);
        if (RC_CH(rc.stage_program)) glUniform1ui(u_sp, st->rc.stage_program);
        if (RC_CH(rc.bump_mat)) glUniform4fv(u_bmat, 4, &st->rc.bump_mat[0][0]);
        if (RC_CH(rc.bump_ls)) glUniform2fv(u_bls, 4, &st->rc.bump_ls[0][0]);
        if (RC_CH(rc.other_input)) glUniform1ui(u_osi, st->rc.other_input);
        if (RC_CH(clip_plane_mode)) glUniform1ui(u_clipmode, st->clip_plane_mode);
        if (RC_CH(fog_color)) glUniform4fv(u_fogc, 1, st->fog_color);
        if (RC_CH(alpha_test)) glUniform1i(u_atest, st->alpha_test ? 1 : 0);
        if (RC_CH(alpha_func)) glUniform1ui(u_afunc, st->alpha_func);
        if (RC_CH(alpha_ref)) glUniform1i(u_aref, (GLint)(st->alpha_ref & 0xFF));
#undef RC_CH
    }

    for (i = 0; i < 4; i++) {
        const NvGlTexture *t = &st->tex[i];
        /* Without combiners only stage 0 is read; with them, only stages the
         * stage program uses. */
        int used = st->rc_seen ? (((st->rc.stage_program >> (i * 5)) & 0x1F) != 0)
                               : (i == 0);
        tv[i] = t->valid && used;
        isc[i] = t->cube;
        tsc[i][0] = t->normalised ? 1.0f : (t->width ? 1.0f / (float)t->width : 1.0f);
        tsc[i][1] = t->normalised ? 1.0f : (t->height ? 1.0f / (float)t->height : 1.0f);
        if (tv[i]) {
            /* Always: bind_texture is also where a changed image is noticed. */
            bind_texture(i, t);
            /* The other binding point of the stage gets the stand-in. */
            if (t->cube)
                unit_bind(i, GL_TEXTURE_2D, s_white2d);
            else
                unit_bind(4 + i, GL_TEXTURE_CUBE_MAP, s_whitecube);
        } else {
            unit_bind(i, GL_TEXTURE_2D, s_white2d);
            unit_bind(4 + i, GL_TEXTURE_CUBE_MAP, s_whitecube);
        }
    }
    {
        if (all || memcmp(P->ap_isc, isc, sizeof isc)) {
            glUniform1iv(u_iscube, 4, isc);
            memcpy(P->ap_isc, isc, sizeof isc);
        }
        if (all || memcmp(P->ap_tv, tv, sizeof tv)) {
            glUniform1iv(u_tvalid, 4, tv);
            memcpy(P->ap_tv, tv, sizeof tv);
        }
        if (all || memcmp(P->ap_tsc, tsc, sizeof tsc)) {
            glUniform2fv(u_tscale, 4, &tsc[0][0]);
            memcpy(P->ap_tsc, tsc, sizeof tsc);
        }
    }
    memcpy(&P->ap, st, sizeof P->ap);
    P->ap_w = (int)s->w; P->ap_h = (int)s->h;
    P->ap_valid = 1;
}



/* v123: the title's visibility tests (D3D Begin/EndVisibilityTest: NV2A
 * CLEAR_REPORT_VALUE / SET_ZPASS_PIXEL_COUNT_ENABLE / GET_REPORT) on the GPU.
 * Only the software rasteriser ever counted the pixels, so on the GL path
 * every test answered 0 and the title took whatever it tested as hidden --
 * JSRF tests the spot it would put an effect (one point per glow, the
 * skates' sparks and smoke when braking) before drawing it. While counting
 * is on, the draws run inside a GL_SAMPLES_PASSED query; GET_REPORT waits
 * for the queries since the last clear and answers in guest pixels.
 * RECOMP_GL_VISTEST=0 answers 0 again (pre-v123). */
#define NV_GL_VQ 64
static GLuint s_vq[NV_GL_VQ];
static int s_vq_open = -1;           /* index of the open query, or -1 */
static int s_vq_pend[NV_GL_VQ], s_vq_npend;
static int s_vq_next;
static uint64_t s_vq_sum;            /* samples counted since the clear, read */
static uint64_t s_n_vtests, s_vtest_ticks;

static int vis_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GL_VISTEST");
        v = !(e && e[0] == '0');
    }
    return v && s_state == 1;
}

static void vq_close(void)
{
    if (s_vq_open < 0)
        return;
    glEndQuery(GL_SAMPLES_PASSED);
    if (s_vq_npend < NV_GL_VQ)
        s_vq_pend[s_vq_npend++] = s_vq_open;
    s_vq_open = -1;
}

static void vq_collect(void)
{
    int i;
    for (i = 0; i < s_vq_npend; i++) {
        GLuint v = 0;
        glGetQueryObjectuiv(s_vq[s_vq_pend[i]], GL_QUERY_RESULT, &v);
        s_vq_sum += v;
    }
    s_vq_npend = 0;
}

void nv2a_gl_vis_enable(int on)
{
    if (!vis_on())
        return;
    if (!s_vq[0])
        glGenQueries(NV_GL_VQ, s_vq);
    if (on && s_vq_open < 0) {
        if (s_vq_npend >= NV_GL_VQ - 1)
            vq_collect();
        s_vq_open = s_vq_next;
        s_vq_next = (s_vq_next + 1) % NV_GL_VQ;
        glBeginQuery(GL_SAMPLES_PASSED, s_vq[s_vq_open]);
    } else if (!on) {
        vq_close();
    }
}

void nv2a_gl_vis_clear(void)
{
    int was;
    if (!vis_on() || !s_vq[0])
        return;
    was = s_vq_open >= 0;
    vq_close();
    vq_collect();                    /* results before the clear are dropped */
    s_vq_sum = 0;
    if (was)
        nv2a_gl_vis_enable(1);
}

uint32_t nv2a_gl_vis_count(void)
{
    uint64_t t0 = TM_NOW(), px;
    int was = s_vq_open >= 0;
    if (!vis_on() || !s_vq[0])
        return 0;
    vq_close();
    vq_collect();
    if (was)
        nv2a_gl_vis_enable(1);
    s_n_vtests++;
    s_vtest_ticks += TM_NOW() - t0;
    px = s_vq_sum / (uint64_t)(s_scale * s_scale);
    return (uint32_t)(px > 0xFFFFFFFFu ? 0xFFFFFFFFu : px);
}

int nv2a_gl_vis_active(void) { return s_vq_open >= 0; }

/* v121: on a traced frame, how many samples each draw actually wrote
 * (GL_SAMPLES_PASSED), so a draw that is issued but never seen -- depth,
 * clipping, alpha -- tells itself apart from one that is never issued. */
#define NV_GL_TQ 4096
static int s_tq_on, s_tq_cur, s_tq_n;
static GLuint s_tq_ids[NV_GL_TQ];
static int s_tq_draw[NV_GL_TQ];

void nv2a_gl_trace_frame(int on)
{
    if (s_state != 1)
        return;
    if (on && !s_tq_ids[0])
        glGenQueries(NV_GL_TQ, s_tq_ids);
    s_tq_on = on;
    s_tq_n = 0;
    s_tq_cur = 0;
}

void nv2a_gl_trace_draw(int draw_no)
{
    s_tq_cur = draw_no;
}

/* v122: RECOMP_GL_EFFECTLOG=1: every frame, each additive draw (blend
 * dst factor ONE: sparks, glows) with its texture, triangles and the samples
 * it wrote, read back a frame later. One [FX] line per frame that has any. */
#define NV_GL_FXQ 64
static int s_fx_on = -1, s_fx_n[2], s_fx_cur;
static GLuint s_fx_q[2][NV_GL_FXQ];
static uint32_t s_fx_tex[2][NV_GL_FXQ], s_fx_tris[2][NV_GL_FXQ];
static uint64_t s_fx_ms[2];

static int fx_begin(const NvGlState *st, uint32_t tris)
{
    int b, i;
    if (s_fx_on < 0) {
        s_fx_on = getenv("RECOMP_GL_EFFECTLOG") != NULL;
        if (s_fx_on) {
            glGenQueries(NV_GL_FXQ, s_fx_q[0]);
            glGenQueries(NV_GL_FXQ, s_fx_q[1]);
        }
    }
    if (!s_fx_on || !st->blend_enable || st->blend_dfactor != GL_ONE || s_tq_on
            || s_vq_open >= 0)
        return 0;
    b = s_fx_cur;
    i = s_fx_n[b];
    if (i >= NV_GL_FXQ)
        return 0;
    s_fx_tex[b][i] = st->tex[0].valid ? st->tex[0].offset : 0;
    s_fx_tris[b][i] = tris;
    glBeginQuery(GL_SAMPLES_PASSED, s_fx_q[b][i]);
    return 1;
}

static void fx_end(int began)
{
    if (!began)
        return;
    glEndQuery(GL_SAMPLES_PASSED);
    s_fx_n[s_fx_cur]++;
}

/* at a frame's end: report the frame before (its queries are done by now),
 * then start this one's set */
static void fx_frame(void)
{
    int prev = s_fx_cur ^ 1, i;
    if (s_fx_on != 1)
        return;
    if (s_fx_n[prev]) {
        fprintf(stderr, "[FX] t=%llu ms:", (unsigned long long)s_fx_ms[prev]);
        for (i = 0; i < s_fx_n[prev]; i++) {
            GLuint v = 0;
            glGetQueryObjectuiv(s_fx_q[prev][i], GL_QUERY_RESULT, &v);
            fprintf(stderr, " %08X %u tris %u px%s", s_fx_tex[prev][i], s_fx_tris[prev][i],
                    v / (GLuint)(s_scale * s_scale), i + 1 < s_fx_n[prev] ? " |" : "");
        }
        fputc('\n', stderr);
    }
    s_fx_n[prev] = 0;
#ifdef _WIN32
    s_fx_ms[prev] = GetTickCount64();
#else
    s_fx_ms[prev] = SDL_GetTicks();
#endif
    s_fx_cur = prev;
}

static int tq_begin(void)
{
    if (!s_tq_on || !s_tq_cur || s_tq_n >= NV_GL_TQ || s_vq_open >= 0)
        return 0;
    glBeginQuery(GL_SAMPLES_PASSED, s_tq_ids[s_tq_n]);
    return 1;
}

static void tq_end(int began)
{
    if (!began)
        return;
    glEndQuery(GL_SAMPLES_PASSED);
    s_tq_draw[s_tq_n++] = s_tq_cur;
    s_tq_cur = 0;
}

void nv2a_gl_trace_report(void)
{
    int i, col = 0;
    if (s_state != 1 || !s_tq_n)
        return;
    fprintf(stderr, "[FTRACE] samples written per draw (x%d scale: %d per guest pixel):", s_scale,
            s_scale * s_scale);
    for (i = 0; i < s_tq_n; i++) {
        GLuint v = 0;
        glGetQueryObjectuiv(s_tq_ids[i], GL_QUERY_RESULT, &v);
        if (col++ % 12 == 0)
            fprintf(stderr, "\n[FTRACE]   ");
        fprintf(stderr, " %d:%u", s_tq_draw[i], v);
    }
    fputc('\n', stderr);
    s_tq_n = 0;
}

/* ── draws ────────────────────────────────────────────────────────────── */

static GlVert *s_vbuf;          /* staging, when a buffer cannot be mapped */
static size_t s_vbuf_n;
static uint32_t s_map_n;        /* vertices handed out by nv2a_gl_map_vertices */
static int s_mapped;            /* 1 mapped, 2 staged in s_vbuf, 0 none */

NvGlVertex *nv2a_gl_map_vertices(uint32_t n)
{
    size_t bytes = (size_t)n * sizeof(GlVert);
    void *p;

    if (s_state != 1 || !n || bytes > NV_GL_VB_BYTES)
        return NULL;
    if (s_persist)
        goto staged;                  /* vertices_done copies into the ring */
    bind_ab(s_vbo);
    if (s_vb_off + bytes > NV_GL_VB_BYTES) {
        glBufferData(GL_ARRAY_BUFFER, NV_GL_VB_BYTES, NULL, GL_STREAM_DRAW);
        s_vb_off = 0;
    }
    /* Staged in CPU memory and sent with one glBufferSubData by default: a
     * map/unmap pair for the vertices and another for the indices of every
     * draw cost ~100 us a draw on the AMD driver (run 62: 2.4 s of every 10
     * in "GL draws" at ~650 draws a frame). RECOMP_GL_MAP=1 maps instead. */
    {
        static int use_map = -1;
        if (use_map < 0)
            use_map = getenv("RECOMP_GL_MAP") != NULL;
        p = use_map ? glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)s_vb_off, (GLsizeiptr)bytes,
                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT
                                       | GL_MAP_UNSYNCHRONIZED_BIT)
                    : NULL;
    }
    s_map_n = n;
    if (p) {
        s_mapped = 1;
        return (NvGlVertex *)p;
    }
staged:
    s_map_n = n;
    if (n > s_vbuf_n) {
        free(s_vbuf);
        s_vbuf = (GlVert *)malloc(bytes);
        s_vbuf_n = s_vbuf ? n : 0;
        if (!s_vbuf)
            return NULL;
    }
    s_mapped = 2;
    return s_vbuf;
}

/* Finish the vertices nv2a_gl_map_vertices handed out. Returns the base
 * vertex they start at in the buffer. */
static GLint vertices_done(void)
{
    GLint base = (GLint)(s_vb_off / sizeof(GlVert));
    size_t bytes = (size_t)s_map_n * sizeof(GlVert);

    if (s_persist) {
        size_t off;
        if (s_mapped != 2) {
            s_mapped = 0;
            return 0;
        }
        off = ring_alloc(&s_rv, bytes, sizeof(GlVert));
        memcpy(s_rv.map + off, s_vbuf, bytes);
        s_mapped = 0;
        return (GLint)(off / sizeof(GlVert));
    }
    bind_ab(s_vbo);
    if (s_mapped == 1)
        glUnmapBuffer(GL_ARRAY_BUFFER);
    else if (s_mapped == 2)
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)s_vb_off, (GLsizeiptr)bytes, s_vbuf);
    s_mapped = 0;
    s_vb_off += bytes;
    return base;
}

void nv2a_gl_unmap_vertices(void)
{
    if (s_mapped)
        (void)vertices_done();
}

void nv2a_gl_draw_mapped(const NvGlState *st, const uint32_t *idx, uint32_t n_idx)
{
    GlSurf *s;
    GLint base;
    size_t ib = (size_t)n_idx * 4;
    void *p;

    uint64_t t0 = TM_NOW();
    if (s_state != 1 || !s_mapped)
        return;
    if (s_cur_vao != 1) {
        glBindVertexArray(s_vao); s_cur_attr_ok = 0;
        s_cur_vao = 1;
    }
    use_prog(pick_prog(&s_p_old, st));
    base = vertices_done();
    if (n_idx < 3 || ib > NV_GL_IB_BYTES)
        return;
    s = bind_surface(st);
    if (!s)
        return;
    apply_state(st, s);
    if (s_persist) {
        size_t ioff = ring_alloc(&s_ri, ib, 4);
        int tq = tq_begin(), fx = fx_begin(st, n_idx / 3);
        memcpy(s_ri.map + ioff, idx, ib);
        glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)n_idx, GL_UNSIGNED_INT,
                                 (void *)(uintptr_t)ioff, base);
        tq_end(tq); fx_end(fx);
        goto drawn;
    }
    if (s_ib_off + ib > NV_GL_IB_BYTES) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, NV_GL_IB_BYTES, NULL, GL_STREAM_DRAW);
        s_ib_off = 0;
    }
    static int ib_map = -1;
    if (ib_map < 0)
        ib_map = getenv("RECOMP_GL_MAP") != NULL;
    p = ib_map
        ? glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)s_ib_off, (GLsizeiptr)ib,
                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT
                           | GL_MAP_UNSYNCHRONIZED_BIT)
        : NULL;
    if (p) {
        memcpy(p, idx, ib);
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)s_ib_off, (GLsizeiptr)ib, idx);
    }
    {
        int tq = tq_begin(), fx = fx_begin(st, n_idx / 3);
        glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)n_idx, GL_UNSIGNED_INT,
                                 (void *)(uintptr_t)s_ib_off, base);
        tq_end(tq); fx_end(fx);
    }
    s_ib_off += ib;
drawn:
    s->dirty = 1;
    s->draws++;
    s_n_draws++;
    gq_frame_start();
    s_tm[0] += TM_NOW() - t0;
    s_n_tris += n_idx / 3;
    {
        static int checks;
        if (checks < 8) {
            GLenum e = glGetError();
            checks++;
            if (e != GL_NO_ERROR)
                fprintf(stderr, "[GL] error 0x%X after draw %llu\n", e,
                        (unsigned long long)s_n_draws);
        }
    }
}

/* ── v77: vertices processed on the GPU ──────────────────────────────── */

int nv2a_gl_gpu_vs_ok(void)
{
    return s_state == 1 && s_gpu_vs;
}

static float   *s_rbuf;          /* staged raw inputs */
static size_t   s_rbuf_bytes;
static uint32_t s_raw_n, s_raw_k;
static int      s_raw_direct;    /* v144: this batch was written into s_rv2 */
static size_t   s_raw_off;
static size_t   s_vb2_off;
static int      s_raw_mapped;
/* v147: the batch went into the ring as the title stored it */
static int      s_raw_nat;
static GLint    s_nat_base;           /* base vertex (one stream), else 0 */
static uint32_t s_nat_ns;
static NvGlNatAttr s_nat_a[16];
static size_t   s_nat_ptr[16];        /* each input's offset in the ring */
static uint32_t s_nat_stride[16];
static float    s_nat_cval[16][4];    /* v149: values of the constant inputs */
static int      s_nat_reset;          /* the float layout replaced the pointers */
/* v169: per stream, for the attribute-binding path of several streams */
static size_t   s_nat_at[16];
static uint32_t s_nat_sstr[16];
static int      s_vab_ok;             /* the binding path's formats are in place */

float *nv2a_gl_map_raw(uint32_t n, uint32_t k)
{
    size_t bytes = (size_t)n * k * 16;
    if (s_state != 1 || !s_gpu_vs || !n || !k || k > 16 || bytes > NV_GL_VB_BYTES / 2)
        return NULL;
    if (bytes > s_rbuf_bytes) {
        free(s_rbuf);
        s_rbuf = (float *)malloc(bytes);
        s_rbuf_bytes = s_rbuf ? bytes : 0;
        if (!s_rbuf)
            return NULL;
    }
    s_raw_n = n; s_raw_k = k;
    s_raw_mapped = 1;
    s_raw_nat = 0;
    /* v144: straight into the persistent ring when there is one, instead of
     * staging in s_rbuf and copying (0.45 s in 10 s on a Steam Deck). The
     * fill only ever writes the memory, so a write-combined mapping is fine.
     * RECOMP_GL_RAW_STAGE=1 stages as before. */
    {
        static int stage = -1;
        if (stage < 0) {
            /* v145: off by default again: on the Deck the fill into the
             * mapped ring cost more than the copy it saved (run 144:
             * vertex staging +0.4 s, draws -0.3 s). RECOMP_GL_RAW_DIRECT=1 */
            const char *e = getenv("RECOMP_GL_RAW_DIRECT");
            stage = !(e && e[0] == '1');
        }
        s_raw_direct = 0;
        if (!stage && s_rv2.map) {
            size_t stride = (size_t)k * 16;
            bind_ab(s_vbo2);
            s_raw_off = ring_alloc(&s_rv2, bytes, stride);
            s_raw_direct = 1;
            return (float *)(s_rv2.map + s_raw_off);
        }
    }
    return s_rbuf;
}

void nv2a_gl_unmap_raw(void)
{
    s_raw_mapped = 0;
    s_raw_nat = 0;
}

int nv2a_gl_map_native(uint32_t k, const NvGlNatAttr *a, const float (*cval)[4],
                       uint32_t ns, const uint8_t *const *src, const uint32_t *len,
                       const uint32_t *stride)
{
    size_t total = 0, off, at[16];
    uint32_t i;
    if (s_state != 1 || !s_gpu_vs || !s_rv2.map || !k || k > 16 || !ns || ns > 16)
        return 0;
    for (i = 0; i < ns; i++)
        total += ((size_t)len[i] + 15) & ~(size_t)15;
    if (total > NV_GL_VB_BYTES / 16)
        return 0;
    /* One allocation for all the streams (the ring fences per allocation).
     * A single stream sits at a multiple of its stride, so the base vertex
     * reaches it and the attribute pointers stay as they were. */
    if (ns == 1) {
        off = ring_alloc(&s_rv2, len[0], stride[0]);
        at[0] = off;
        s_nat_base = (GLint)(off / stride[0]);
    } else {
        off = ring_alloc(&s_rv2, total, 16);
        for (i = 0; i < ns; i++) {
            at[i] = off;
            off += ((size_t)len[i] + 15) & ~(size_t)15;
        }
        s_nat_base = 0;
    }
    for (i = 0; i < ns; i++)
        memcpy(s_rv2.map + at[i], src[i], len[i]);
    for (i = 0; i < ns; i++) {
        s_nat_at[i] = at[i];
        s_nat_sstr[i] = stride[i];
    }
    for (i = 0; i < k; i++) {
        s_nat_a[i] = a[i];
        if (a[i].type == NV_GL_NAT_CONST) {
            s_nat_stride[i] = 0;
            s_nat_ptr[i] = 0;
            memcpy(s_nat_cval[i], cval[i], sizeof s_nat_cval[i]);
            continue;
        }
        s_nat_stride[i] = stride[a[i].stream];
        s_nat_ptr[i] = (ns == 1 ? 0 : at[a[i].stream]) + a[i].rel;
    }
    s_nat_ns = ns;
    s_raw_k = k;
    s_raw_n = 0;
    s_raw_mapped = 1;
    s_raw_nat = 1;
    return 1;
}

static int s_full_consts;      /* RECOMP_GL_FULL_CONSTS=1: all 192 on any change */
static uint64_t s_n_cvec;      /* constant vectors uploaded */
/* The vertex shader's uniforms, sent only when they changed (per program). */
static void upload_vs(const NvGlVsState *vs)
{
    static int init;
    if (!init) {
        init = 1;
        s_full_consts = getenv("RECOMP_GL_FULL_CONSTS") != NULL;
    }
    if (vs->mode == NV_GL_VS_FF && vs->present != P->vs_present) {
        glUniform1ui(u_present, vs->present);
        P->vs_present = vs->present;
    }
    if (vs->consts && vs->const_gen != P->vs_const_gen) {
        /* v117: only the ones changed since this program last had them --
         * usually a matrix or two of the 192. A program that has not run for
         * 2^30 changes gets them all, so the generations cannot wrap past it. */
        uint32_t lo = 0, hi = NV2A_VSH_CONSTANTS;
        if (P->c_n <= 0 || s_full_consts) {
            glUniform4fv(u_c, NV2A_VSH_CONSTANTS, vs->consts);
        } else {
            if (P->vs_const_gen && (uint32_t)(vs->const_gen - P->vs_const_gen) < (1u << 30)) {
                if (!nv2a_vsh_constants_changed(P->vs_const_gen, &lo, &hi))
                    lo = hi = 0;
            }
            if (hi > (uint32_t)P->c_n)
                hi = (uint32_t)P->c_n;
            if (hi > lo)
                glUniform4fv(u_c + (GLint)lo, (GLsizei)(hi - lo), vs->consts + (size_t)lo * 4);
            else
                hi = lo;
        }
        s_n_cvec += hi - lo;
        P->vs_const_gen = vs->const_gen;
    }
    if (vs->mode == NV_GL_VS_FF) {
        typedef struct NvGlFF FF;
        const size_t split = offsetof(FF, scene_ambient);
        if (!P->vs_ff_valid || memcmp(&vs->ff, &P->vs_ff, split)) {
            /* v167: each group only when it changed (from one object to the
             * next it is usually the model-view and composite matrices; all
             * nine went every time, each a call into the driver) */
            int all = !P->vs_ff_valid;
#define FFCH(f) (all || memcmp(&vs->ff.f, &P->vs_ff.f, sizeof vs->ff.f))
            if (FFCH(composite)) glUniformMatrix4fv(u_comp, 1, GL_TRUE, vs->ff.composite);
            if (FFCH(mv))        glUniformMatrix4fv(u_mv, 4, GL_TRUE, &vs->ff.mv[0][0]);
            if (FFCH(inv_mv0))   glUniformMatrix4fv(u_imv0, 1, GL_TRUE, vs->ff.inv_mv0);
            if (FFCH(texmat))    glUniformMatrix4fv(u_texm, 4, GL_TRUE, &vs->ff.texmat[0][0]);
            if (FFCH(texplane))  glUniform4fv(u_texpl, 16, &vs->ff.texplane[0][0][0]);
            if (FFCH(skin_mode)) glUniform1i(u_skin, vs->ff.skin_mode);
            if (FFCH(lighting))  glUniform1i(u_light, vs->ff.lighting);
            if (FFCH(texmat_on)) glUniform1iv(u_texm_on, 4, vs->ff.texmat_on);
            if (FFCH(texgen))    glUniform1iv(u_texgen, 16, &vs->ff.texgen[0][0]);
#undef FFCH
            memcpy(&P->vs_ff, &vs->ff, split);
            if (!P->vs_ff_valid)
                P->vs_ff_valid = 1;
        }
        if (vs->ff.lighting && (P->vs_ff_valid < 2
                || memcmp((const char *)&vs->ff + split, (const char *)&P->vs_ff + split,
                          sizeof(FF) - split))) {
            int all = P->vs_ff_valid < 2;          /* v167: per group, as above */
#define FFCH(f) (all || memcmp(&vs->ff.f, &P->vs_ff.f, sizeof vs->ff.f))
            if (FFCH(scene_ambient))  glUniform3fv(u_samb, 1, vs->ff.scene_ambient);
            if (FFCH(emission))       glUniform3fv(u_emis, 1, vs->ff.emission);
            if (FFCH(mat_alpha))      glUniform1f(u_malpha, vs->ff.mat_alpha);
            if (FFCH(color_material)) glUniform1ui(u_cmat, vs->ff.color_material);
            if (FFCH(light_enable))   glUniform1ui(u_lmask, vs->ff.light_enable);
            if (FFCH(specular_on))    glUniform1i(u_spec, (int)vs->ff.specular_on);
            if (FFCH(l_amb))          glUniform3fv(u_lamb, 8, &vs->ff.l_amb[0][0]);
            if (FFCH(l_dif))          glUniform3fv(u_ldif, 8, &vs->ff.l_dif[0][0]);
            if (FFCH(l_spc))          glUniform3fv(u_lspc, 8, &vs->ff.l_spc[0][0]);
            if (FFCH(l_half))         glUniform3fv(u_lhalf, 8, &vs->ff.l_half[0][0]);
            if (FFCH(l_dir))          glUniform3fv(u_ldir, 8, &vs->ff.l_dir[0][0]);
            if (FFCH(l_pos))          glUniform3fv(u_lpos, 8, &vs->ff.l_pos[0][0]);
            if (FFCH(l_att))          glUniform3fv(u_latt, 8, &vs->ff.l_att[0][0]);
            if (FFCH(l_range))        glUniform1fv(u_lrange, 8, vs->ff.l_range);
#undef FFCH
            memcpy((char *)&P->vs_ff + split, (const char *)&vs->ff + split, sizeof(FF) - split);
            P->vs_ff_valid = 2;
        }
    }
    {
        /* v130: 2 = fixed function, whose fog distance (|eye z|) the fragment
         * shader takes the absolute value of per pixel */
        int fe = vs->fog_enable ? (vs->mode == NV_GL_VS_FF ? 2 : 1) : 0;
        if (fe != P->vs_fog[0] || (int32_t)vs->fog_mode != P->vs_fog[1]) {
            glUniform1i(u_fog_en, fe);
            glUniform1ui(u_fog_mode, vs->fog_mode);
            P->vs_fog[0] = fe; P->vs_fog[1] = (int32_t)vs->fog_mode;
        }
    }
    if (vs->fog_param[0] != P->vs_fogp[0] || vs->fog_param[1] != P->vs_fogp[1]) {
        glUniform2f(u_fogp, vs->fog_param[0], vs->fog_param[1]);
        P->vs_fogp[0] = vs->fog_param[0]; P->vs_fogp[1] = vs->fog_param[1];
    }
}

/* v171: one vertex array object per vertex layout of the stored-format
 * path. Run 170 still made 1.3 glVertexAttribPointer and 0.6 enable/disable
 * calls a draw: consecutive objects alternate between a few layouts, and
 * every change re-pointed each input of the single VAO. Here each layout
 * (input formats and offsets, stream strides, which inputs are arrays) has
 * its own VAO, set up once with vertex attribute binding; changing layout is
 * one glBindVertexArray. One stream sits at offset 0 (the base vertex reaches
 * the batch), several are moved with one glBindVertexBuffers when they move.
 * 64 layouts, least recently used replaced. RECOMP_GL_VAO_CACHE=0 for the
 * single VAO. */
#define NV_VAOC 64
typedef struct {
    GLuint vao;
    uint32_t hash, k, ns, en, used;
    NvGlNatAttr a[16];
    uint32_t str[16];
    GLintptr off[16];
    int offs_ok;
} NvVaoC;
static NvVaoC s_vaoc[NV_VAOC];
static uint32_t s_vaoc_tick;
static int s_vaoc_on = -1;
static uint64_t s_vaoc_miss;

static int nat_vao_bind(uint32_t k, uint32_t en)
{
    NvGlNatAttr ka[16];
    uint32_t i, h = 2166136261u, ns = s_nat_ns;
    NvVaoC *e = NULL, *lru = NULL;
    int idx;
    if (s_vaoc_on < 0) {
        const char *ev = getenv("RECOMP_GL_VAO_CACHE");
        s_vaoc_on = !(ev && ev[0] == '0') && GLAD_GL_ARB_vertex_attrib_binding && GLAD_GL_ARB_multi_bind
                    && glad_glBindVertexBuffers && glad_glVertexAttribFormat && glad_glVertexAttribBinding;
        fprintf(stderr, "[GL] vertex layouts: %s\n", s_vaoc_on
                ? "one vertex array object each (RECOMP_GL_VAO_CACHE=0 for one shared)" : "one shared vertex array object");
    }
    if (!s_vaoc_on || !k || k > 16 || !ns || ns > 16)
        return 0;
    for (i = 0; i < ns; i++)
        if (!s_nat_sstr[i] || s_nat_sstr[i] > 2048)
            return 0;
    memset(ka, 0, sizeof ka);
    for (i = 0; i < k; i++) {
        if (s_nat_a[i].type == NV_GL_NAT_CONST) {
            ka[i].loc = s_nat_a[i].loc;
            ka[i].type = NV_GL_NAT_CONST;
        } else {
            if (s_nat_a[i].rel > 2047 || s_nat_a[i].stream >= ns)
                return 0;
            ka[i] = s_nat_a[i];
        }
    }
    {
        const uint8_t *b = (const uint8_t *)ka;
        size_t n = k * sizeof *ka;
        for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
        b = (const uint8_t *)s_nat_sstr;
        for (i = 0; i < ns * sizeof *s_nat_sstr; i++) h = (h ^ b[i]) * 16777619u;
        h = (h ^ k ^ (ns << 8) ^ (en << 16) ^ (en >> 16)) * 16777619u;
    }
    for (idx = 0; idx < NV_VAOC; idx++) {
        NvVaoC *c = &s_vaoc[idx];
        if (c->vao && c->hash == h && c->k == k && c->ns == ns && c->en == en
                && !memcmp(c->a, ka, k * sizeof *ka) && !memcmp(c->str, s_nat_sstr, ns * sizeof *c->str)) {
            e = c;
            break;
        }
        if (!lru || !c->vao || (lru->vao && c->used < lru->used))
            lru = c;
    }
    if (!e) {
        uint32_t old_en;
        e = lru;
        idx = (int)(e - s_vaoc);
        s_vaoc_miss++;
        old_en = e->vao ? e->en : 0;
        if (!e->vao)
            glGenVertexArrays(1, &e->vao);
        glBindVertexArray(e->vao);
        s_cur_vao = 3 + idx;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
        for (i = 0; i < k; i++) {
            const NvGlNatAttr *na = &ka[i];
            if (na->type == NV_GL_NAT_CONST)
                continue;
            switch (na->type) {
            case 0:  glVertexAttribFormat(na->loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, na->rel); break;
            case 2:  glVertexAttribFormat(na->loc, na->size, GL_FLOAT, GL_FALSE, na->rel); break;
            case 4:  glVertexAttribFormat(na->loc, na->size, GL_UNSIGNED_BYTE, GL_TRUE, na->rel); break;
            default: glVertexAttribFormat(na->loc, na->size, GL_SHORT, GL_FALSE, na->rel); break;
            }
            glVertexAttribBinding(na->loc, na->stream);
        }
        for (i = 0; i < 16; i++) {
            uint32_t b = 1u << i;
            if ((en & b) && !(old_en & b)) glEnableVertexAttribArray(i);
            else if (!(en & b) && (old_en & b)) glDisableVertexAttribArray(i);
        }
        e->hash = h; e->k = k; e->ns = ns; e->en = en;
        memcpy(e->a, ka, sizeof e->a);
        memcpy(e->str, s_nat_sstr, sizeof e->str);
        e->offs_ok = 0;
    } else {
        idx = (int)(e - s_vaoc);
        if (s_cur_vao != 3 + idx) {
            glBindVertexArray(e->vao);
            s_cur_vao = 3 + idx;
        }
    }
    e->used = ++s_vaoc_tick;
    {
        GLuint bufs[16];
        GLintptr offs[16];
        GLsizei strs[16];
        for (i = 0; i < ns; i++) {
            bufs[i] = s_vbo2;
            offs[i] = ns == 1 ? 0 : (GLintptr)s_nat_at[i];
            strs[i] = (GLsizei)s_nat_sstr[i];
        }
        if (!e->offs_ok || memcmp(e->off, offs, ns * sizeof *offs)) {
            glBindVertexBuffers(0, (GLsizei)ns, bufs, offs, strs);
            memcpy(e->off, offs, ns * sizeof *offs);
            e->offs_ok = 1;
        }
    }
    return 1;
}

/* v142: where nv2a_gl_draw_raw's time goes (RECOMP_PROFILE): program,
 * surface, state, vertices, constants, the draw itself */
static uint64_t s_td[6], s_td_t;
static int s_td_on = -1;
#define TD_MARK(k) do { \
        if (s_td_on < 0) s_td_on = getenv("RECOMP_PROFILE") != NULL; \
        if (s_td_on) { uint64_t _n = TM_NOW(); if ((k) >= 0) s_td[(k)] += _n - s_td_t; s_td_t = _n; } \
    } while (0)

void nv2a_gl_draw_raw(const NvGlState *st, const NvGlVsState *vs,
                      const uint8_t *slots, uint32_t k,
                      const uint32_t *idx, uint32_t n_idx)
{
    static uint8_t l_slots[16];
    static uint32_t l_k, l_enabled;
    GlSurf *s;
    size_t ib = (size_t)n_idx * 4, stride = (size_t)k * 16, bytes;
    GLint base;
    uint32_t i, en = 0;
    uint64_t t0 = TM_NOW();

    if (s_state != 1 || !s_raw_mapped)
        return;
    s_raw_mapped = 0;
    if (n_idx < 3 || ib > NV_GL_IB_BYTES || k != s_raw_k)
        return;
    TD_MARK(-1);
    {
        GlProg *pr;
        if (vs->mode == NV_GL_VS_PROGRAM) {
            pr = vsh_find(vs->prog, vs->nins, 1);
        } else {
            int key = vs->ff.lighting ? 1 : 0, i, c;
            for (i = 0; i < 4; i++)
                for (c = 0; c < 4; c++)
                    if (vs->ff.texgen[i][c] >= 0x2400 && vs->ff.texgen[i][c] != 0)
                        key |= 2;
            pr = ff_prog(key);
        }
        if (!pr || !pr->id)
            return;
        use_prog(pick_prog(pr, st));
    }
    TD_MARK(0);
    s = bind_surface(st);
    if (!s)
        return;
    TD_MARK(1);
    apply_state(st, s);
    TD_MARK(2);
    /* Vertices: behind the last batch, at a multiple of this stride so the
     * base vertex lands on them. */
    bytes = (size_t)s_raw_n * stride;
    bind_ab(s_vbo2);
    if (s_raw_nat) {
        base = s_nat_base;
    } else if (s_raw_direct) {
        base = (GLint)(s_raw_off / stride);
    } else if (s_rv2.map) {
        size_t off = ring_alloc(&s_rv2, bytes, stride);
        memcpy(s_rv2.map + off, s_rbuf, bytes);
        base = (GLint)(off / stride);
    } else {
    s_vb2_off = (s_vb2_off + stride - 1) / stride * stride;
    if (s_vb2_off + bytes > NV_GL_VB_BYTES) {
        glBufferData(GL_ARRAY_BUFFER, NV_GL_VB_BYTES, NULL, GL_STREAM_DRAW);
        s_vb2_off = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)s_vb2_off, (GLsizeiptr)bytes, s_rbuf);
    base = (GLint)(s_vb2_off / stride);
    s_vb2_off += bytes;
    }
    for (i = 0; i < k; i++)
        en |= 1u << slots[i];
    if (s_raw_nat) {
        /* v149: constant inputs are not arrays; GL's current value is theirs */
        /* v169: GL keeps a disabled input's current value, so it is only
         * sent when it changed (1.75 calls a draw before). The cache for an
         * input is dropped whenever it is drawn from an array (its value is
         * then the driver's business) and when the other VAO is used. */
        for (i = 0; i < k; i++)
            if (s_nat_a[i].type == NV_GL_NAT_CONST) {
                uint32_t loc = s_nat_a[i].loc & 15u;
                en &= ~(1u << loc);
                if (!(s_cur_attr_ok & (1u << loc))
                        || memcmp(s_cur_attr[loc], s_nat_cval[i], sizeof s_cur_attr[loc])) {
                    glVertexAttrib4fv(s_nat_a[i].loc, s_nat_cval[i]);
                    memcpy(s_cur_attr[loc], s_nat_cval[i], sizeof s_cur_attr[loc]);
                    s_cur_attr_ok |= 1u << loc;
                }
            }
    }
    /* v171: a vertex array object per layout (see nat_vao_bind) */
    if (s_raw_nat && nat_vao_bind(k, en)) {
        s_raw_nat = 0;
        l_k = 0;                       /* vao2's float layout is set again next time */
        s_cur_attr_ok &= ~en;
        goto arrays_set;
    }
    if (s_cur_vao != 2) {
        glBindVertexArray(s_vao2);
        s_cur_vao = 2;
    }
    if (s_raw_nat) {
        /* v147: the stored formats; with one stream the pointers repeat from
         * batch to batch and are only set when the layout changes */
        static NvGlNatAttr l_a[16];
        static size_t l_p[16];
        static uint32_t l_s[16], l_nk;
        /* v169: with several streams the pointers move every batch (k
         * glVertexAttribPointer calls a draw). With vertex attribute binding
         * the formats stay and one glBindVertexBuffers moves all the streams.
         * RECOMP_GL_VAB=0 for the pointers. */
        static int vab = -1;
        int use_vab = 0;
        if (vab < 0) {
            const char *e = getenv("RECOMP_GL_VAB");
            vab = !(e && e[0] == '0') && GLAD_GL_ARB_vertex_attrib_binding && GLAD_GL_ARB_multi_bind
                  && glad_glBindVertexBuffers && glad_glVertexAttribFormat && glad_glVertexAttribBinding;
            fprintf(stderr, "[GL] several vertex streams: %s\n",
                    vab ? "attribute binding (RECOMP_GL_VAB=0 for pointers)" : "attribute pointers");
        }
        if (vab && s_nat_ns > 1) {
            use_vab = 1;
            for (i = 0; i < s_nat_ns; i++)
                if (!s_nat_sstr[i] || s_nat_sstr[i] > 2048)
                    use_vab = 0;
            for (i = 0; i < k; i++)
                if (s_nat_a[i].type != NV_GL_NAT_CONST && s_nat_a[i].rel > 2047)
                    use_vab = 0;
        }
        if (use_vab) {
            static NvGlNatAttr b_a[16];
            static uint32_t b_nk, b_ns, b_str[16];
            static GLintptr b_off[16];
            GLuint bufs[16];
            GLintptr offs[16];
            GLsizei strs[16];
            uint32_t q;
            int fsame = s_vab_ok && b_nk == k && b_ns == s_nat_ns
                        && !memcmp(b_a, s_nat_a, k * sizeof *b_a)
                        && !memcmp(b_str, s_nat_sstr, s_nat_ns * sizeof *b_str);
            if (!fsame) {
                for (i = 0; i < k; i++) {
                    const NvGlNatAttr *na = &s_nat_a[i];
                    if (na->type == NV_GL_NAT_CONST)
                        continue;
                    switch (na->type) {
                    case 0:  glVertexAttribFormat(na->loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, na->rel); break;
                    case 2:  glVertexAttribFormat(na->loc, na->size, GL_FLOAT, GL_FALSE, na->rel); break;
                    case 4:  glVertexAttribFormat(na->loc, na->size, GL_UNSIGNED_BYTE, GL_TRUE, na->rel); break;
                    default: glVertexAttribFormat(na->loc, na->size, GL_SHORT, GL_FALSE, na->rel); break;
                    }
                    glVertexAttribBinding(na->loc, na->stream);
                }
                memcpy(b_a, s_nat_a, k * sizeof *b_a);
                memcpy(b_str, s_nat_sstr, s_nat_ns * sizeof *b_str);
                b_nk = k;
                b_ns = s_nat_ns;
                s_vab_ok = 1;
            }
            for (q = 0; q < s_nat_ns; q++) {
                bufs[q] = s_vbo2;
                offs[q] = (GLintptr)s_nat_at[q];
                strs[q] = (GLsizei)s_nat_sstr[q];
            }
            if (!fsame || memcmp(b_off, offs, s_nat_ns * sizeof *offs)) {
                glBindVertexBuffers(0, (GLsizei)s_nat_ns, bufs, offs, strs);
                memcpy(b_off, offs, s_nat_ns * sizeof *offs);
            }
            l_nk = 0;                  /* the pointer layout is set again next time */
        }
        int same = use_vab || (!s_nat_reset && l_nk == k && !memcmp(l_a, s_nat_a, k * sizeof *l_a)
                   && !memcmp(l_p, s_nat_ptr, k * sizeof *l_p)
                   && !memcmp(l_s, s_nat_stride, k * sizeof *l_s));
        if (!same) {
            s_vab_ok = 0;              /* v169: pointers replace the bindings */
            for (i = 0; i < k; i++) {
                const NvGlNatAttr *na = &s_nat_a[i];
                GLsizei st2 = (GLsizei)s_nat_stride[i];
                void *ptr = (void *)(uintptr_t)s_nat_ptr[i];
                if (na->type == NV_GL_NAT_CONST)
                    continue;
                switch (na->type) {
                case 0:  glVertexAttribPointer(na->loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, st2, ptr); break;
                case 2:  glVertexAttribPointer(na->loc, na->size, GL_FLOAT, GL_FALSE, st2, ptr); break;
                case 4:  glVertexAttribPointer(na->loc, na->size, GL_UNSIGNED_BYTE, GL_TRUE, st2, ptr); break;
                default: glVertexAttribPointer(na->loc, na->size, GL_SHORT, GL_FALSE, st2, ptr); break;
                }
            }
            memcpy(l_a, s_nat_a, k * sizeof *l_a);
            memcpy(l_p, s_nat_ptr, k * sizeof *l_p);
            memcpy(l_s, s_nat_stride, k * sizeof *l_s);
            l_nk = k;
            s_nat_reset = 0;
        }
        l_k = 0;                       /* the float layout is set again next time */
        s_raw_nat = 0;
    } else if (k != l_k || memcmp(slots, l_slots, k)) {
        for (i = 0; i < k; i++)
            glVertexAttribPointer(slots[i], 4, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                  (void *)(uintptr_t)(i * 16));
        s_vab_ok = 0;
        memcpy(l_slots, slots, k);
        l_k = k;
        s_nat_reset = 1;
    }
    s_cur_attr_ok &= ~en;            /* v169: drawn from arrays */
    if (en != l_enabled) {
        for (i = 0; i < 16; i++) {
            uint32_t b = 1u << i;
            if ((en & b) && !(l_enabled & b)) glEnableVertexAttribArray(i);
            else if (!(en & b) && (l_enabled & b)) glDisableVertexAttribArray(i);
        }
        l_enabled = en;
    }
arrays_set:
    TD_MARK(3);
    upload_vs(vs);
    TD_MARK(4);
    if (s_persist) {
        size_t ioff = ring_alloc(&s_ri, ib, 4);
        int tq = tq_begin(), fx = fx_begin(st, n_idx / 3);
        memcpy(s_ri.map + ioff, idx, ib);
        glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)n_idx, GL_UNSIGNED_INT,
                                 (void *)(uintptr_t)ioff, base);
        tq_end(tq); fx_end(fx);
    } else {
    if (s_ib_off + ib > NV_GL_IB_BYTES) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, NV_GL_IB_BYTES, NULL, GL_STREAM_DRAW);
        s_ib_off = 0;
    }
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)s_ib_off, (GLsizeiptr)ib, idx);
    {
        int tq = tq_begin(), fx = fx_begin(st, n_idx / 3);
        glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)n_idx, GL_UNSIGNED_INT,
                                 (void *)(uintptr_t)s_ib_off, base);
        tq_end(tq); fx_end(fx);
    }
    s_ib_off += ib;
    }
    TD_MARK(5);
    s->dirty = 1;
    s->draws++;
    s_n_draws++;
    gq_frame_start();
    s_tm[0] += TM_NOW() - t0;
    s_n_tris += n_idx / 3;
    {
        static int checks;
        if (checks < 8) {
            GLenum e = glGetError();
            checks++;
            if (e != GL_NO_ERROR)
                fprintf(stderr, "[GL] error 0x%X after GPU-vertex draw %llu\n", e,
                        (unsigned long long)s_n_draws);
        }
    }
}

void nv2a_gl_draw(const NvGlState *st, const Nv2aVshOutput *v,
                  const float *fogf, const uint32_t *idx, uint32_t n_idx)
{
    static uint32_t *seq;
    static uint32_t seq_n;
    GlVert *g;
    uint32_t i;

    if (s_state != 1 || n_idx < 3)
        return;
    if (n_idx > seq_n) {
        free(seq);
        seq = (uint32_t *)malloc((size_t)n_idx * 4);
        seq_n = seq ? n_idx : 0;
        if (!seq)
            return;
        for (i = 0; i < n_idx; i++)
            seq[i] = i;
    }
    g = nv2a_gl_map_vertices(n_idx);
    if (!g)
        return;
    for (i = 0; i < n_idx; i++)
        nv2a_gl_vertex_from(&g[i], &v[idx[i]], fogf ? fogf[idx[i]] : 1.0f);
    nv2a_gl_draw_mapped(st, seq, n_idx);
}

static int s_clr_rect[4] = {0, 0x7FFF, 0, 0x7FFF};

void nv2a_gl_clear_rect(const NvGlState *st, uint32_t param, uint32_t argb,
                        float zclear, uint32_t sclear,
                        int x0, int x1, int y0, int y1)
{
    s_clr_rect[0] = x0; s_clr_rect[1] = x1; s_clr_rect[2] = y0; s_clr_rect[3] = y1;
    nv2a_gl_clear(st, param, argb, zclear, sclear);
    s_clr_rect[0] = 0; s_clr_rect[1] = 0x7FFF; s_clr_rect[2] = 0; s_clr_rect[3] = 0x7FFF;
}

void nv2a_gl_clear(const NvGlState *st, uint32_t param, uint32_t argb,
                   float zclear, uint32_t sclear)
{
    GlSurf *s;
    GLbitfield bits = 0;
    float c[4];

    if (s_state != 1)
        return;
    s = bind_surface(st);
    if (!s)
        return;
    /* Only the clear rectangle. JSRF's graffiti picker renders five previews
     * into tiles of one 1024x1024 target, clearing just the tile before each;
     * clearing the whole surface every time left only the last tile, the XL
     * one, and the other four came out black (run 62). The rectangle is
     * intersected with the clip the scissor already holds. */
    {
        int x0 = s_bound_sc[0], y0b = s_bound_sc[1];
        int x1 = x0 + s_bound_sc[2], y1b = y0b + s_bound_sc[3];  /* exclusive, GL y-up */
        int rx0 = SC(s_clr_rect[0]), rx1 = SC(s_clr_rect[1] + 1);
        int ry0 = SC((int)s->h - (s_clr_rect[3] + 1)), ry1 = SC((int)s->h - s_clr_rect[2]);
        if (rx0 > x0) x0 = rx0;
        if (rx1 < x1) x1 = rx1;
        if (ry0 > y0b) y0b = ry0;
        if (ry1 < y1b) y1b = ry1;
        if (x1 <= x0 || y1b <= y0b)
            return;
        if (x0 != s_bound_sc[0] || y0b != s_bound_sc[1] || x1 - x0 != s_bound_sc[2]
                || y1b - y0b != s_bound_sc[3]) {
            glScissor(x0, y0b, x1 - x0, y1b - y0b);
            s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
        }
    }
    if (param & 0xF0) {
        unpack(argb, c);
        glClearColor(c[0], c[1], c[2], s->alpha ? c[3] : 1.0f);
        /* The software clear writes every channel whenever any colour bit is
         * set; match it. */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, s->alpha ? GL_TRUE : GL_FALSE);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (param & 0x01) {
        glDepthMask(GL_TRUE);
        glClearDepth(st->zmax > 0.0f ? zclear / st->zmax : 1.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (param & 0x02) {
        glStencilMask(0xFF);
        glClearStencil((GLint)(sclear & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits)
        glClear(bits);
    s_ap_valid = 0;                 /* masks changed above */
    if (bits & GL_COLOR_BUFFER_BIT) {
        s->dirty = 1;
        s->draws++;
    }
}

void nv2a_gl_stats2(uint64_t *gpu_copies, uint64_t *present_reads, uint64_t *cpu_taken)
{
    *gpu_copies = s_n_gpucopy;
    *present_reads = s_n_present_reads;
    *cpu_taken = s_n_cpu_taken;
}

/* v92: for frame traces -- the colour at five points across the top of the
 * current render target, as the GPU has it after the draws so far
 * (synchronous: trace frames only). Lets a trace say which draw put
 * something at the top of the screen. */
void nv2a_gl_probe_top(uint32_t out[5])
{
    static const int px[5] = {40, 160, 320, 480, 600}, py[5] = {20, 50, 20, 50, 20};
    int i;
    for (i = 0; i < 5; i++) out[i] = 0;
    if (s_state != 1 || !s_cur || !s_cur->fbo)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_cur->fbo);
    for (i = 0; i < 5; i++) {
        uint32_t c = 0;
        int x = px[i] < (int)s_cur->w ? px[i] : (int)s_cur->w - 1;
        int y = py[i] < (int)s_cur->h ? py[i] : (int)s_cur->h - 1;
        glReadPixels(SC(x), SC((int)s_cur->h - 1 - y), 1, 1, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, &c);
        out[i] = c & 0xFFFFFFu;
    }
}

/* v99: the depth-buffer values at the same points (0..1), -1 if none */
static float s_probe_z[16];
float nv2a_gl_probe_z(int i) { return (i >= 0 && i < 16) ? s_probe_z[i] : -1.0f; }

/* v98: the same at any points (guest pixels, y down). */
void nv2a_gl_probe_at(const int *x, const int *y, int n, uint32_t *out)
{
    int i;
    for (i = 0; i < n; i++) out[i] = 0;
    for (i = 0; i < 16; i++) s_probe_z[i] = -1.0f;
    if (s_state != 1 || !s_cur || !s_cur->fbo)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_cur->fbo);
    for (i = 0; i < n; i++) {
        uint32_t c = 0;
        int xx = x[i] < 0 ? 0 : x[i] < (int)s_cur->w ? x[i] : (int)s_cur->w - 1;
        int yy = y[i] < 0 ? 0 : y[i] < (int)s_cur->h ? y[i] : (int)s_cur->h - 1;
        glReadPixels(SC(xx), SC((int)s_cur->h - 1 - yy), 1, 1, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, &c);
        out[i] = c & 0xFFFFFFu;
        if (i < 16) {
            float z = -1.0f;
            glReadPixels(SC(xx), SC((int)s_cur->h - 1 - yy), 1, 1, GL_DEPTH_COMPONENT,
                         GL_FLOAT, &z);
            if (glGetError() == GL_NO_ERROR)
                s_probe_z[i] = z;
        }
    }
}

void nv2a_gl_stats3(uint64_t *tex_compressed)
{
    *tex_compressed = s_n_texcomp;
}

void nv2a_gl_stats(uint64_t *draws, uint64_t *tris, uint64_t *uploads,
                   uint64_t *readbacks, uint64_t *tex_uploads)
{
    *draws = s_n_draws; *tris = s_n_tris; *uploads = s_n_uploads;
    *readbacks = s_n_readbacks; *tex_uploads = s_n_texup;
}

void nv2a_gl_times(uint64_t t[4], uint64_t *freq)
{
    memcpy(t, s_tm, sizeof s_tm);
    *freq = s_state == 1 ? SDL_GetPerformanceFrequency() : 0;
}

/* v117: what the write-watch, the rings and the constant ranges did, since
 * the last call: one line for [PERF]. Returns 0 before GL is up. */
/* v169: the GL calls made since the last report: per draw, and the ones
 * made most (each is a driver call; under Proton a crossing into Wine's
 * unix side, which the executor profile cannot see into) */
int nv2a_gl_call_stats(char *buf, size_t n)
{
#ifndef NV2A_GL_NO_COUNT
    static unsigned long long l_c[NV2A_GLC_N], l_d;
    unsigned long long tot = 0, d = s_n_draws - l_d;
    int top[8], nt = 0, i, j;
    size_t k;
    if (s_state != 1 || !buf || !n)
        return 0;
    for (i = 0; i < NV2A_GLC_N; i++) {
        unsigned long long c = s_glc[i] - l_c[i];
        tot += c;
        for (j = nt; j > 0 && s_glc[top[j - 1]] - l_c[top[j - 1]] < c; j--)
            if (j < 8) top[j] = top[j - 1];
        if (j < 8) { top[j] = i; if (nt < 8) nt++; }
    }
    {
        static uint64_t l_vm;
        snprintf(buf, n, "%llu in all, %.1f a draw (%llu new vertex layouts); most:", tot,
                 d ? (double)tot / (double)d : 0.0, (unsigned long long)(s_vaoc_miss - l_vm));
        l_vm = s_vaoc_miss;
    }
    for (i = 0; i < nt; i++) {
        unsigned long long c = s_glc[top[i]] - l_c[top[i]];
        if (!c) break;
        k = strlen(buf);
        snprintf(buf + k, n - k, " %s %llu", s_glc_name[top[i]] + 2, c);
    }
    if (s_gt_on) {
        /* v172: how often the executor had to wait for the GL thread */
        static uint64_t l_sy, l_pw, l_cm;
        k = strlen(buf);
        snprintf(buf + k, n - k, "; GL thread: %llu queued, %llu waited for, %llu waits for room/frames/fences",
                 (unsigned long long)(s_gt_n_cmds - l_cm), (unsigned long long)(s_gt_n_sync - l_sy),
                 (unsigned long long)(s_gt_n_pwait - l_pw));
        l_sy = s_gt_n_sync; l_pw = s_gt_n_pwait; l_cm = s_gt_n_cmds;
#if defined(_WIN32)
        {
            static uint64_t l_ls, l_ln;
            uint64_t ln = s_gt_lat_n - l_ln;
            double fq = 1000.0 / (double)SDL_GetPerformanceFrequency();
            if (ln) {
                k = strlen(buf);
                snprintf(buf + k, n - k, "; frames shown %.2f ms after they were finished"
                         " on average, %.2f ms at most", (double)(s_gt_lat_sum - l_ls) * fq / (double)ln,
                         (double)s_gt_lat_max * fq);
            }
            l_ls = s_gt_lat_sum; l_ln = s_gt_lat_n;
            s_gt_lat_max = 0;
        }
#endif
    }
    memcpy(l_c, s_glc, sizeof l_c);
    l_d = s_n_draws;
    return 1;
#else
    (void)buf; (void)n;
    return 0;
#endif
}

int nv2a_gl_submit_stats(char *buf, size_t n)
{
    static uint64_t l_sk, l_h, l_f, l_p, l_cv, l_d, l_w, l_b, l_vt, l_vtt;
    uint64_t f = 0, pr = 0, m = 0, d = s_n_draws - l_d;
    if (s_state != 1 || !buf || !n)
        return 0;
    xbox_gw_stats(&f, &pr, &m);
    snprintf(buf, n, "%llu checks answered by the write-watch, %llu hashed, %llu missed;"
             " %llu watched pages written, %llu protects; %.1f constant vectors a draw;"
             " %s%s%llu ring waits, %llu oversized batches; %llu visibility tests %.1f ms",
             (unsigned long long)(s_n_wskip - l_sk), (unsigned long long)(s_n_whash - l_h),
             (unsigned long long)s_n_wmissed,
             (unsigned long long)(f - l_f), (unsigned long long)(pr - l_p),
             d ? (double)(s_n_cvec - l_cv) / (double)d : 0.0,
             s_persist ? "rings" : "SubData", s_persist ? ": " : " (",
             (unsigned long long)(s_n_ring_waits - l_w), (unsigned long long)(s_n_ring_big - l_b),
             (unsigned long long)(s_n_vtests - l_vt),
             (double)(s_vtest_ticks - l_vtt) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    if (!s_persist) {
        size_t k = strlen(buf);
        if (k + 1 < n) { buf[k] = ')'; buf[k + 1] = 0; }
    }
    if (s_td_on > 0) {
        static uint64_t l_td[6];
        double fq = 1000.0 / (double)SDL_GetPerformanceFrequency();
        size_t k = strlen(buf);
        static uint64_t l_sp;
        snprintf(buf + k, n - k, "; %llu checks answered by the sparse hash",
                 (unsigned long long)(s_n_sparse - l_sp));
        l_sp = s_n_sparse;
        k = strlen(buf);
        snprintf(buf + k, n - k, "; GPU-vertex draws: program %.0f ms, surface %.0f,"
                 " state %.0f, vertices %.0f, constants %.0f, draw %.0f",
                 (s_td[0] - l_td[0]) * fq, (s_td[1] - l_td[1]) * fq, (s_td[2] - l_td[2]) * fq,
                 (s_td[3] - l_td[3]) * fq, (s_td[4] - l_td[4]) * fq, (s_td[5] - l_td[5]) * fq);
        memcpy(l_td, s_td, sizeof l_td);
    }
    (void)m;
    l_sk = s_n_wskip; l_h = s_n_whash; l_f = f; l_p = pr;
    l_cv = s_n_cvec; l_d = s_n_draws; l_w = s_n_ring_waits; l_b = s_n_ring_big;
    l_vt = s_n_vtests; l_vtt = s_vtest_ticks;
    return 1;
}
