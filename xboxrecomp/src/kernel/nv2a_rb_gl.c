/* nv2a_rb_gl.c: the OpenGL backend (nv2a_rb.h).
 *
 * While step 1 of the backends plan moves the GL code here piece by piece,
 * this file is compiled as part of nv2a_gl.c (included after the GL thread's
 * headers, so its calls go through the GL thread like the rest); when every
 * GL call is here, it becomes a file of its own and nv2a_gl.c has none. */

static RbFixed s_rbf;                /* what GL has now */
static int s_rbf_valid;

void rb_fixed_lost(void) { s_rbf_valid = 0; }

#define RBF_CH(a, b) (all || memcmp(&f->a, &s_rbf.a, sizeof f->a) || (b))
void rb_fixed(const RbFixed *f)
{
    int all = !s_rbf_valid;
    if (all || f->depth_test != s_rbf.depth_test || f->depth_func != s_rbf.depth_func
            || f->depth_mask != s_rbf.depth_mask) {
        if (f->depth_test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(f->depth_func);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(f->depth_mask ? GL_TRUE : GL_FALSE);
    }
    if (all || f->stencil_test != s_rbf.stencil_test || f->stencil_func != s_rbf.stencil_func
            || f->stencil_ref != s_rbf.stencil_ref || f->stencil_mask != s_rbf.stencil_mask
            || f->stencil_fail != s_rbf.stencil_fail || f->stencil_zfail != s_rbf.stencil_zfail
            || f->stencil_zpass != s_rbf.stencil_zpass || f->stencil_wmask != s_rbf.stencil_wmask) {
        if (f->stencil_test) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(f->stencil_func, (GLint)f->stencil_ref, f->stencil_mask);
            glStencilOp(f->stencil_fail, f->stencil_zfail, f->stencil_zpass);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        glStencilMask(f->stencil_wmask);
    }
    if (all || f->blend != s_rbf.blend || f->blend_src != s_rbf.blend_src
            || f->blend_dst != s_rbf.blend_dst || f->blend_eq != s_rbf.blend_eq
            || memcmp(f->blend_color, s_rbf.blend_color, sizeof f->blend_color)) {
        if (f->blend) {
            glEnable(GL_BLEND);
            glBlendFunc(f->blend_src, f->blend_dst);
            glBlendEquation(f->blend_eq);
            glBlendColor(f->blend_color[0], f->blend_color[1], f->blend_color[2], f->blend_color[3]);
        } else {
            glDisable(GL_BLEND);
        }
    }
    if (all || f->cmask != s_rbf.cmask)
        glColorMask((f->cmask & 1) != 0, (f->cmask & 2) != 0, (f->cmask & 4) != 0, (f->cmask & 8) != 0);
    if (all || f->poly_offset != s_rbf.poly_offset || f->poly_factor != s_rbf.poly_factor
            || f->poly_units != s_rbf.poly_units) {
        if (f->poly_offset) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(f->poly_factor, f->poly_units);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    if (all || f->cull != s_rbf.cull || f->cull_face != s_rbf.cull_face
            || f->front_face != s_rbf.front_face) {
        if (f->cull) {
            glEnable(GL_CULL_FACE);
            glCullFace(f->cull_face);
            glFrontFace(f->front_face);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
    s_rbf = *f;
    s_rbf_valid = 1;
}
#undef RBF_CH

/* ── programs (moved from nv2a_gl.c: compile, the program-binary cache, link) ── */

static const char *(*s_rb_defs)(void);
static int (*s_rb_salt)(const char **out, int max);

void rb_shader_options(const char *(*defs)(void), int (*salt)(const char **out, int max))
{
    s_rb_defs = defs;
    s_rb_salt = salt;
}

static GLuint rbg_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    const char *srcs[3];
    const char *nl = strchr(src, '\n');
    const char *defs = s_rb_defs ? s_rb_defs() : NULL;
    if (defs && nl && !strncmp(src, "#version", 8)) {
        /* after the #version line, which has to come first */
        static char ver[64];
        size_t n = (size_t)(nl - src) + 1;
        if (n >= sizeof ver) n = sizeof ver - 1;
        memcpy(ver, src, n); ver[n] = 0;
        srcs[0] = ver;
        srcs[1] = defs;
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
    if (s_rb_salt) {
        const char *salt[16];
        int k, ns = s_rb_salt(salt, 16);
        for (k = 0; k < ns; k++)
            h = fnv64(h, salt[k]);
    }
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

GLuint rb_program_build(const char *vsrc, const char *fsrc)
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
    vs = rbg_compile(GL_VERTEX_SHADER, vsrc);
    fs = rbg_compile(GL_FRAGMENT_SHADER, fsrc);
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


GLuint rb_shader_compile(int vertex, const char *src)
{
    return rbg_compile(vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, src);
}

void rb_program_stats(unsigned *hits, unsigned *misses, uint64_t *ticks)
{
    *hits = s_cache_hits;
    *misses = s_cache_misses;
    *ticks = s_tm_compile;
}

/* ── uniforms ───────────────────────────────────────────────────────────── */

void rb_use_program(uint32_t p) { glUseProgram(p); }
void rb_program_delete(uint32_t p) { glDeleteProgram(p); }
int rb_uniform_loc(uint32_t p, const char *name) { return glGetUniformLocation(p, name); }

/* v117: how much of a vec4 array is live, and whether its elements'
 * locations run on from element 0 (loc0), so a changed range can be sent by
 * itself: the live length, -1 when they don't, 0 when it isn't there */
int rb_uniform_vec4_len(uint32_t id, const char *name, int loc0)
{
    GLint nu = 0, k;
    int n = 0;
    char n0[64];
    snprintf(n0, sizeof n0, "%s[0]", name);
    glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &nu);
    for (k = 0; k < nu; k++) {
        char nm[64];
        GLint sz = 0;
        GLenum ty = 0;
        nm[0] = 0;
        glGetActiveUniform(id, (GLuint)k, sizeof nm, NULL, &sz, &ty, nm);
        if (!strcmp(nm, n0) || !strcmp(nm, name)) {
            n = ty == GL_FLOAT_VEC4 ? sz : -1;
            break;
        }
    }
    if (n > 1) {
        char b[64];
        GLint l1, lz;
        snprintf(b, sizeof b, "%s[1]", name);
        l1 = glGetUniformLocation(id, b);
        snprintf(b, sizeof b, "%s[%d]", name, n - 1);
        lz = glGetUniformLocation(id, b);
        if (l1 != loc0 + 1 || lz != loc0 + n - 1)
            n = -1;
    }
    return n;
}

/* the current program's uniforms (OpenGL: each program keeps its own) */
void rb_u1i(int l, int a) { glUniform1i(l, a); }
void rb_u1ui(int l, uint32_t a) { glUniform1ui(l, a); }
void rb_u1f(int l, float a) { glUniform1f(l, a); }
void rb_u2f(int l, float a, float b) { glUniform2f(l, a, b); }
void rb_u3f(int l, float a, float b, float c) { glUniform3f(l, a, b, c); }
void rb_u4f(int l, float a, float b, float c, float d) { glUniform4f(l, a, b, c, d); }
void rb_u1iv(int l, int n, const int32_t *v) { glUniform1iv(l, n, (const GLint *)v); }
void rb_u1uiv(int l, int n, const uint32_t *v) { glUniform1uiv(l, n, v); }
void rb_u1fv(int l, int n, const float *v) { glUniform1fv(l, n, v); }
void rb_u2fv(int l, int n, const float *v) { glUniform2fv(l, n, v); }
void rb_u3fv(int l, int n, const float *v) { glUniform3fv(l, n, v); }
void rb_u4fv(int l, int n, const float *v) { glUniform4fv(l, n, v); }
void rb_umat4(int l, int n, int transpose, const float *v) { glUniformMatrix4fv(l, n, transpose ? GL_TRUE : GL_FALSE, v); }

/* ── bindings: what GL has bound, cached (moved from nv2a_gl.c) ──────────── */

static GLuint s_bound_ab = (GLuint)-1;
void rb_bind_vbuf(uint32_t b)
{
    if (b != s_bound_ab) {
        glBindBuffer(GL_ARRAY_BUFFER, b);
        s_bound_ab = b;
    }
}
static GLuint s_unit2d[16], s_unitcube[16];
static int    s_active_unit = -1;
static GLuint s_bound_fbo = (GLuint)-1;
static int    s_bound_vp[2] = {-1, -1}, s_bound_sc[4] = {-1, -1, -1, -1};
void rb_copy_target(uint32_t src, int sw, int sh, uint32_t dst, int dw, int dh, GLenum filter)
{
    /* v172: no glIsEnabled / glGetBooleanv (each one a wait for the GL
     * thread): the scissor test is always on outside these copies, and the
     * colour mask is set again by the next draw */
    GLuint prev = s_bound_fbo;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    rb_bind_draw(dst);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, filter);
    glEnable(GL_SCISSOR_TEST);
    rb_fixed_lost();
    rb_bind_draw(prev == (GLuint)-1 ? 0 : prev);
}
void rb_active_unit(int u)
{
    if (s_active_unit != u) {
        glActiveTexture(GL_TEXTURE0 + u);
        s_active_unit = u;
    }
}

void rb_unit_bind(int u, GLenum target, uint32_t id)
{
    GLuint *slot = target == GL_TEXTURE_CUBE_MAP ? &s_unitcube[u] : &s_unit2d[u];
    if (*slot != id) {
        rb_active_unit(u);
        glBindTexture(target, id);
        *slot = id;
    }
}

void rb_bind_draw(uint32_t fbo)
{
    if (s_bound_fbo != fbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        s_bound_fbo = fbo;
    }
}


uint32_t rb_draw_bound(void) { return s_bound_fbo; }        /* (GLuint)-1: not known */
void rb_draw_bound_set(uint32_t fbo) { s_bound_fbo = fbo; }  /* bound by other means */
void rb_vbuf_lost(void) { s_bound_ab = (GLuint)-1; }
void rb_vbuf_bound_set(uint32_t b) { s_bound_ab = b; }

/* the viewport at the origin, w x h */
void rb_viewport(int w, int h)
{
    if (s_bound_vp[0] != w || s_bound_vp[1] != h) {
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        s_bound_vp[0] = w; s_bound_vp[1] = h;
    }
}

/* the scissor (GL rows: y from the bottom); rb_scissor_once: for one
 * operation, the next rb_scissor sends its own again */
void rb_scissor(int x, int y, int w, int h)
{
    if (s_bound_sc[0] != x || s_bound_sc[1] != y || s_bound_sc[2] != w || s_bound_sc[3] != h) {
        glScissor(x, y, w, h);
        s_bound_sc[0] = x; s_bound_sc[1] = y; s_bound_sc[2] = w; s_bound_sc[3] = h;
    }
}
void rb_scissor_get(int sc[4]) { memcpy(sc, s_bound_sc, sizeof s_bound_sc); }
void rb_scissor_once(int x, int y, int w, int h)
{
    glScissor(x, y, w, h);
    s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
}

/* something bound framebuffers, set the viewport or the scissor behind
 * these caches' back */
void rb_bindings_lost(void)
{
    s_bound_fbo = (GLuint)-1;
    s_bound_vp[0] = s_bound_vp[1] = -1;
    s_bound_sc[0] = s_bound_sc[1] = s_bound_sc[2] = s_bound_sc[3] = -1;
}
/* ── anti-aliasing and the MSAA upscale (moved whole from nv2a_gl.c) ─────── */

/* MSAA: draw a single-sampled image into a multisample framebuffer (ms_up_run) */
typedef struct { GLuint src_tex, dst_fbo; int w, h; } MsUpArgs;

/* v238: anti-aliasing on the way to the screen. v238c: RECOMP_GL_AA =
 * fxaa, smaa_low, smaa_medium, smaa_high or smaa_ultra (off when not set).
 * The finished frame (its texture, at the internal resolution) goes through
 * it into a texture of the same size; the presenting blits then read that
 * instead of the frame. Every GL state it touches is read first and put
 * back, so the renderer's caches stay true.
 *
 * SMAA (third_party/smaa, MIT; Jorge Jimenez et al.): edge detection, blend
 * weights and neighbourhood blending, the reference shader built as GLSL
 * 3.30. It is written the Direct3D way up (row 0 at the top), as are its
 * area and search textures: the frame is first copied upside down, every
 * pass works in that layout, and the last one writes the right way up. */
#include "../third_party/smaa/smaa_glsl.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

static const char *FXAA_VS =
"#version 330 core\n"
"out vec2 v_uv;\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  v_uv = p;\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";
static const char *FXAA_FS =
"#version 330 core\n"
"uniform sampler2D u_tex;\n"
"uniform vec2 u_rcp;\n"
"in vec2 v_uv;\n"
"out vec4 o_col;\n"
"float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
"void main() {\n"
"  vec4 m4 = texture(u_tex, v_uv);\n"
"  vec3 nw = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_rcp).rgb;\n"
"  vec3 ne = texture(u_tex, v_uv + vec2( 1.0, -1.0) * u_rcp).rgb;\n"
"  vec3 sw = texture(u_tex, v_uv + vec2(-1.0,  1.0) * u_rcp).rgb;\n"
"  vec3 se = texture(u_tex, v_uv + vec2( 1.0,  1.0) * u_rcp).rgb;\n"
"  float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se);\n"
"  float lm = luma(m4.rgb);\n"
"  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));\n"
"  float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
"  if (lmax - lmin < max(0.0312, lmax * 0.125)) { o_col = vec4(m4.rgb, 1.0); return; }\n"
"  vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
"  float red = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);\n"
"  float rcpmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);\n"
"  dir = clamp(dir * rcpmin, vec2(-8.0), vec2(8.0)) * u_rcp;\n"
"  vec3 a = 0.5 * (texture(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
"                  texture(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
"  vec3 b = a * 0.5 + 0.25 * (texture(u_tex, v_uv + dir * -0.5).rgb +\n"
"                             texture(u_tex, v_uv + dir *  0.5).rgb);\n"
"  float lb = luma(b);\n"
"  o_col = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);\n"
"}\n";

static const char *SMAA_EDGE_VS =
"out vec2 v_uv;\n"
"out vec4 v_off[3];\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"  v_uv = p;\n"
"  SMAAEdgeDetectionVS(v_uv, v_off);\n"
"}\n";
static const char *SMAA_EDGE_FS =
"uniform sampler2D u_color;\n"
"in vec2 v_uv;\n"
"in vec4 v_off[3];\n"
"out vec4 o_col;\n"
"void main() {\n"
"  o_col = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, u_color), 0.0, 0.0);\n"
"}\n";
static const char *SMAA_WGT_VS =
"out vec2 v_uv;\n"
"out vec2 v_pix;\n"
"out vec4 v_off[3];\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"  v_uv = p;\n"
"  SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);\n"
"}\n";
static const char *SMAA_WGT_FS =
"uniform sampler2D u_edges;\n"
"uniform sampler2D u_area;\n"
"uniform sampler2D u_search;\n"
"in vec2 v_uv;\n"
"in vec2 v_pix;\n"
"in vec4 v_off[3];\n"
"out vec4 o_col;\n"
"void main() {\n"
"  o_col = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, u_edges, u_area, u_search, vec4(0.0));\n"
"}\n";
static const char *SMAA_NB_VS =
"out vec2 v_uv;\n"
"out vec4 v_off;\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"  v_uv = vec2(p.x, 1.0 - p.y);\n"
"  SMAANeighborhoodBlendingVS(v_uv, v_off);\n"
"}\n";
static const char *SMAA_NB_FS =
"uniform sampler2D u_color;\n"
"uniform sampler2D u_blend;\n"
"in vec2 v_uv;\n"
"in vec4 v_off;\n"
"out vec4 o_col;\n"
"void main() {\n"
"  o_col = SMAANeighborhoodBlendingPS(v_uv, v_off, u_color, u_blend);\n"
"}\n";

static GLuint compile(GLenum type, const char *src);
static struct {
    int state;                 /* 0 not tried, 1 ready, -1 off or failed */
    int mode;                  /* 1 fxaa, 2..5 smaa low..ultra */
    GLuint prog, vao, samp, fbo, tex;
    GLint l_tex, l_rcp;
    int w, h;
    /* SMAA */
    GLuint p_edge, p_wgt, p_nb, t_col, t_edge, t_blend, t_area, t_search;
    GLuint f_col, f_edge, f_blend;
    GLint le_m, le_col, lw_m, lw_edge, lw_area, lw_search, ln_m, ln_col, ln_blend;
} s_fx;

/* a program from a vertex and fragment shader made of several pieces */
static GLuint aa_shader(GLenum type, const char *const *parts, int n)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, n, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[GL] anti-aliasing shader failed to compile:\n%s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}
static GLuint aa_link(GLuint vs, GLuint fs)
{
    GLuint p;
    GLint ok = 0;
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}
/* one SMAA pass's program: the defines, the reference code, the wrapper */
static GLuint smaa_program(const char *preset, const char *vs_main, const char *fs_main)
{
    static char head_vs[256], head_fs[256];
    const char *parts[SMAA_GLSL_LINES + 2];
    GLuint vs, fs;
    int i;
    snprintf(head_vs, sizeof head_vs, "#version 330 core\n#define SMAA_GLSL_3 1\n"
             "#define SMAA_RT_METRICS u_metrics\n#define SMAA_PRESET_%s 1\n"
             "#define SMAA_INCLUDE_VS 1\n#define SMAA_INCLUDE_PS 0\nuniform vec4 u_metrics;\n", preset);
    snprintf(head_fs, sizeof head_fs, "#version 330 core\n#define SMAA_GLSL_3 1\n"
             "#define SMAA_RT_METRICS u_metrics\n#define SMAA_PRESET_%s 1\n"
             "#define SMAA_INCLUDE_VS 0\n#define SMAA_INCLUDE_PS 1\nuniform vec4 u_metrics;\n", preset);
    for (i = 0; i < SMAA_GLSL_LINES; i++)
        parts[i + 1] = SMAA_GLSL_SRC[i];
    parts[0] = head_vs;
    parts[SMAA_GLSL_LINES + 1] = vs_main;
    vs = aa_shader(GL_VERTEX_SHADER, parts, SMAA_GLSL_LINES + 2);
    parts[0] = head_fs;
    parts[SMAA_GLSL_LINES + 1] = fs_main;
    fs = aa_shader(GL_FRAGMENT_SHADER, parts, SMAA_GLSL_LINES + 2);
    return aa_link(vs, fs);
}
static void aa_target(GLuint *tex, GLuint *fbo, GLenum fmt, GLenum cfmt, int w, int h)
{
    if (!*tex) glGenTextures(1, tex);
    if (!*fbo) glGenFramebuffers(1, fbo);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, cfmt, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
}
static int aa_setup(void)
{
    static const char *const names[] = { "", "fxaa", "smaa_low", "smaa_medium", "smaa_high",
                                         "smaa_ultra" };
    static const char *const presets[] = { "", "", "LOW", "MEDIUM", "HIGH", "ULTRA" };
    const char *e = getenv("RECOMP_GL_AA");
    int i;
    s_fx.state = -1;
    if (!e || !*e)
        return 0;
    for (i = 1; i < 6; i++)
        if (!_stricmp(e, names[i]))
            s_fx.mode = i;
    if (!_stricmp(e, "smaa"))
        s_fx.mode = 4;
    if (!s_fx.mode) {
        if (_stricmp(e, "off") && strcmp(e, "0"))
            fprintf(stderr, "[GL] RECOMP_GL_AA=%s: not known (fxaa, smaa_low, smaa_medium,"
                    " smaa_high, smaa_ultra): off\n", e);
        return 0;
    }
    glGenVertexArrays(1, &s_fx.vao);
    glGenSamplers(1, &s_fx.samp);
    glSamplerParameteri(s_fx.samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s_fx.samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s_fx.samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s_fx.samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_fx.fbo);
    glGenTextures(1, &s_fx.tex);
    if (s_fx.mode == 1) {
        s_fx.prog = aa_link(compile(GL_VERTEX_SHADER, FXAA_VS), compile(GL_FRAGMENT_SHADER, FXAA_FS));
        if (!s_fx.prog) {
            fprintf(stderr, "[GL] FXAA shader did not build: anti-aliasing off\n");
            return 0;
        }
        s_fx.l_tex = glGetUniformLocation(s_fx.prog, "u_tex");
        s_fx.l_rcp = glGetUniformLocation(s_fx.prog, "u_rcp");
    } else {
        const char *pr = presets[s_fx.mode];
        s_fx.p_edge = smaa_program(pr, SMAA_EDGE_VS, SMAA_EDGE_FS);
        s_fx.p_wgt = smaa_program(pr, SMAA_WGT_VS, SMAA_WGT_FS);
        s_fx.p_nb = smaa_program(pr, SMAA_NB_VS, SMAA_NB_FS);
        if (!s_fx.p_edge || !s_fx.p_wgt || !s_fx.p_nb) {
            fprintf(stderr, "[GL] SMAA shaders did not build: anti-aliasing off\n");
            return 0;
        }
        s_fx.le_m = glGetUniformLocation(s_fx.p_edge, "u_metrics");
        s_fx.le_col = glGetUniformLocation(s_fx.p_edge, "u_color");
        s_fx.lw_m = glGetUniformLocation(s_fx.p_wgt, "u_metrics");
        s_fx.lw_edge = glGetUniformLocation(s_fx.p_wgt, "u_edges");
        s_fx.lw_area = glGetUniformLocation(s_fx.p_wgt, "u_area");
        s_fx.lw_search = glGetUniformLocation(s_fx.p_wgt, "u_search");
        s_fx.ln_m = glGetUniformLocation(s_fx.p_nb, "u_metrics");
        s_fx.ln_col = glGetUniformLocation(s_fx.p_nb, "u_color");
        s_fx.ln_blend = glGetUniformLocation(s_fx.p_nb, "u_blend");
        /* the two lookup textures, uploaded as stored (row 0 first) */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &s_fx.t_area);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_area);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT, 0, GL_RG,
                     GL_UNSIGNED_BYTE, areaTexBytes);
        glGenTextures(1, &s_fx.t_search);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_search);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, GL_RED,
                     GL_UNSIGNED_BYTE, searchTexBytes);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    s_fx.state = 1;
    fprintf(stderr, "[GL] anti-aliasing: %s (RECOMP_GL_AA)\n", names[s_fx.mode]);
    return 1;
}

/* the frame in `src` / `src_fbo` (w x h) through the anti-aliasing; returns
 * the framebuffer to read the result from, or 0 (off, or not possible: read
 * the frame itself) */
uint32_t rb_present_aa(uint32_t src, uint32_t src_fbo, int w, int h)
{
    GLint prog, vao, unit, tex0, tex1, tex2, samp0, samp1, samp2, dfbo, rfbo, vp[4], align;
    GLfloat cc[4];
    GLboolean cm[4], dm, en[9];
    static const GLenum caps[9] = { GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST,
                                    GL_SCISSOR_TEST, GL_POLYGON_OFFSET_FILL,
                                    GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1, GL_CLIP_DISTANCE2 };
    int i;
    if (s_fx.state < 0 || !src || w <= 0 || h <= 0)
        return 0;
    /* what is bound now, to put back */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp2);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
    if (s_fx.state == 0 && !aa_setup()) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        glActiveTexture((GLenum)unit);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        return 0;
    }
    for (i = 0; i < 9; i++) {
        en[i] = glIsEnabled(caps[i]);
        if (en[i]) glDisable(caps[i]);
    }
    if (w != s_fx.w || h != s_fx.h) {
        aa_target(&s_fx.tex, &s_fx.fbo, GL_RGBA8, GL_RGBA, w, h);
        if (s_fx.mode >= 2) {
            aa_target(&s_fx.t_col, &s_fx.f_col, GL_RGBA8, GL_RGBA, w, h);
            aa_target(&s_fx.t_edge, &s_fx.f_edge, GL_RG8, GL_RG, w, h);
            aa_target(&s_fx.t_blend, &s_fx.f_blend, GL_RGBA8, GL_RGBA, w, h);
        }
        s_fx.w = w; s_fx.h = h;
    }
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glBindVertexArray(s_fx.vao);
    glBindSampler(0, s_fx.samp);
    glBindSampler(1, s_fx.samp);
    glBindSampler(2, s_fx.samp);
    if (s_fx.mode == 1) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.fbo);
        glUseProgram(s_fx.prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(s_fx.l_tex, 0);
        glUniform2f(s_fx.l_rcp, 1.0f / (float)w, 1.0f / (float)h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        float m0 = 1.0f / (float)w, m1 = 1.0f / (float)h;
        /* the frame, upside down (row 0 at the top); v277: with MSAA from the
         * resolved copy (the caller resolved it), as a flipping copy out of
         * a multisampled framebuffer is not allowed */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_msaa > 1 ? s_resolve_fbo : src_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.f_col);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        /* 1: edges */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.f_edge);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(s_fx.p_edge);
        glUniform4f(s_fx.le_m, m0, m1, (float)w, (float)h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_col);
        glUniform1i(s_fx.le_col, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        /* 2: blend weights */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.f_blend);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(s_fx.p_wgt);
        glUniform4f(s_fx.lw_m, m0, m1, (float)w, (float)h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_edge);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_area);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_search);
        glUniform1i(s_fx.lw_edge, 0);
        glUniform1i(s_fx.lw_area, 1);
        glUniform1i(s_fx.lw_search, 2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        /* 3: neighbourhood blending, written the right way up */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.fbo);
        glUseProgram(s_fx.p_nb);
        glUniform4f(s_fx.ln_m, m0, m1, (float)w, (float)h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_col);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_fx.t_blend);
        glUniform1i(s_fx.ln_col, 0);
        glUniform1i(s_fx.ln_blend, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    /* put it all back */
    glBindSampler(2, (GLuint)samp2);
    glBindSampler(1, (GLuint)samp1);
    glBindSampler(0, (GLuint)samp0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    glActiveTexture((GLenum)unit);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glDepthMask(dm);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    for (i = 0; i < 9; i++)
        if (en[i]) glEnable(caps[i]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
    return s_fx.fbo;
}

/* ── MSAA upsample: draw the single-sampled frame into a multisample target ──
 * A single->multi glBlitFramebuffer is INVALID_OPERATION (multi -> single is
 * the only direction the spec allows), so this runs the image as a texture
 * through a fullscreen triangle instead; the LINEAR sampler also does the
 * RECOMP_GL_SCALE upscale. Runs inside gt_call_sync, so the whole save /
 * draw / restore stays coherent against the executor's caches. */
static const char *MS_UP_FS =
"#version 330 core\n"
"uniform sampler2D u_tex;\n"
"in vec2 v_uv;\n"
"out vec4 o_col;\n"
"void main() {\n"
"  o_col = texture(u_tex, v_uv);\n"
"}\n";

static struct {
    int state;                 /* 0 not built, 1 ready, -1 failed */
    GLuint prog, vao, samp;
    GLint l_tex;
} s_ms_up;

static void ms_up_run(void *p)
{
    MsUpArgs *a = (MsUpArgs *)p;
    static const GLenum caps[9] = { GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST,
                                    GL_SCISSOR_TEST, GL_POLYGON_OFFSET_FILL,
                                    GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1, GL_CLIP_DISTANCE2 };
    GLint prog, vao, unit, tex0, samp0, dfbo, rfbo, vp[4], align;
    GLfloat cc[4];
    GLboolean cm[4], dm, en[9];
    int i;

    if (s_ms_up.state == 0) {
        glGenVertexArrays(1, &s_ms_up.vao);        /* no attributes enabled */
        glGenSamplers(1, &s_ms_up.samp);
        glSamplerParameteri(s_ms_up.samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_ms_up.samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(s_ms_up.samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_ms_up.samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_ms_up.prog = aa_link(compile(GL_VERTEX_SHADER, FXAA_VS),
                               compile(GL_FRAGMENT_SHADER, MS_UP_FS));
        if (!s_ms_up.prog) {
            fprintf(stderr, "[GL] MSAA upsample shader did not build: draws dropped\n");
            s_ms_up.state = -1;
            return;
        }
        s_ms_up.l_tex = glGetUniformLocation(s_ms_up.prog, "u_tex");
        s_ms_up.state = 1;
    }
    if (s_ms_up.state < 0)
        return;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
    for (i = 0; i < 9; i++) {
        en[i] = glIsEnabled(caps[i]);
        if (en[i]) glDisable(caps[i]);
    }

    glViewport(0, 0, a->w, a->h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    rb_bind_draw(a->dst_fbo);
    glBindVertexArray(s_ms_up.vao);
    glUseProgram(s_ms_up.prog);
    glBindSampler(0, s_ms_up.samp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->src_tex);
    glUniform1i(s_ms_up.l_tex, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    rb_draw_bound_set(a->dst_fbo);

    /* put it all back */
    glBindSampler(0, (GLuint)samp0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    glActiveTexture((GLenum)unit);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glDepthMask(dm);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    for (i = 0; i < 9; i++)
        if (en[i]) glEnable(caps[i]);
    if ((GLuint)dfbo != (GLuint)a->dst_fbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfbo);
        rb_draw_bound_set((GLuint)dfbo);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
    rb_fixed_lost();
}

/* a single-sampled image (texture src, w x h, any size: the sampler scales)
 * into a multisampled target: a single to multi blit is invalid, so a
 * fullscreen triangle, on the GL thread */
void rb_ms_upscale(uint32_t src_tex, uint32_t dst_target, int w, int h)
{
    MsUpArgs a;
    a.src_tex = src_tex; a.dst_fbo = dst_target; a.w = w; a.h = h;
    gt_call_sync(ms_up_run, &a);
}
