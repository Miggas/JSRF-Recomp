/* nv2a_rb_gl.c: the OpenGL backend (nv2a_rb.h).
 *
 * While step 1 of the backends plan moves the GL code here piece by piece,
 * this file is compiled as part of nv2a_gl.c (included after the GL thread's
 * headers, so its calls go through the GL thread like the rest); when every
 * GL call is here, it becomes a file of its own and nv2a_gl.c has none. */

/* the texture unit for binding a texture to work on it (uploads, parameters),
 * so the draw stages' bindings stay as they are */
#define NV_GL_SCRATCH_UNIT 15

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

/* ── the window: presenting (moved from nv2a_gl.c) ──────────────────────────
 * WGL (the game window's DC), DXGI flip model (RECOMP_GL_PRESENT=dxgi) and
 * SDL (Linux); on the GL thread when that is on; the GPU timing of frames. */

static void (*s_rb_overlay)(int cw, int ch, int flip);   /* our menus, over the frame */
static SDL_Window *s_rb_win;                               /* Linux: the GL context's window */
void xbox_PresentFit(int cw, int ch, int *dw, int *dh);    /* fb_present.c */
int xbox_PresentEdgeBars(void);
void rb_set_overlay(void (*fn)(int cw, int ch, int flip)) { s_rb_overlay = fn; }
void rb_set_window(void *sdl_window) { s_rb_win = (SDL_Window *)sdl_window; }

/* v213: the bars' colour: black, or (edge bars) the colour of the picture's
 * own left edge (one pixel of the read framebuffer, halfway down) */
static void rbg_bar_color(GLint sw, GLint sh)
{
    GLubyte px[4] = {0, 0, 0, 255};
    if (xbox_PresentEdgeBars() && sw > 4 && sh > 4) {
        GLint pb = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);   /* read to memory, not a PBO */
        if (pb) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(2, sh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (pb) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
        glClearColor(px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, 1.0f);
    } else
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

static void rbg_row_fill(double k, GLint sw, GLint sh, int dx, int y0, int dw, int y1)
{
    GLint el, in, ex;
    if (k <= 0.0 || sw < 16 || sh < 16 || dw < 16)
        return;
    el = (GLint)(sw * (1.0 - k) * 0.5 + 0.5);       /* source x of the 4:3 left edge */
    in = sw / 320 > 1 ? sw / 320 : 1;               /* a pixel or two inside it */
    if (el <= in || el + in >= sw / 2)
        return;
    ex = (GLint)((double)dw * (el + in) / sw + 0.5);  /* shown width to fill, each side */
    glBlitFramebuffer(el + in, 0, el + in + 1, sh, dx, y0, dx + ex, y1,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBlitFramebuffer(sw - el - in - 1, 0, sw - el - in, sh, dx + dw - ex, y0, dx + dw, y1,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowGLOwned(void);
#if defined(_WIN32)
typedef BOOL  (WINAPI *wgl_make_t)(HDC, HGLRC);
typedef HGLRC (WINAPI *wgl_getctx_t)(void);
typedef HDC   (WINAPI *wgl_getdc_t)(void);
typedef PROC  (WINAPI *wgl_proc_t)(LPCSTR);
typedef int   (WINAPI *gdi_getpf_t)(HDC);
typedef int   (WINAPI *gdi_descpf_t)(HDC, int, UINT, LPPIXELFORMATDESCRIPTOR);
typedef BOOL  (WINAPI *gdi_setpf_t)(HDC, int, const PIXELFORMATDESCRIPTOR *);
typedef BOOL  (WINAPI *gdi_swap_t)(HDC);
#endif /* _WIN32: v244, the timing below is shared with Linux */
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

int rb_gpu_frame_stats(char *buf, size_t n)
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

#if defined(_WIN32)
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

#endif /* _WIN32 (DXGI) */

#if defined(_WIN32)
/* Present s through DXGI; 0 = not available (caller falls back). */
static int dxp_present(const RbFrame *f, HWND hwnd, int cw, int ch, int dx, int dy, int dw, int dh)
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
    {
        GLuint fx = rb_present_aa(f->tex, f->fbo, f->w, f->h);   /* v238 */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_dx.fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx ? fx
                         : f->resolve ? f->resolve : f->fbo);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    rbg_bar_color(f->w, f->h);   /* v213 */
    glClear(GL_COLOR_BUFFER_BIT);
    /* D3D's row 0 is the top: flip vertically on the way in */
    glBlitFramebuffer(0, 0, f->w, f->h, dx, dy + dh, dx + dw, dy,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rbg_row_fill(f->rowk, f->w, f->h, dx, dy + dh, dw, dy);   /* v217b */
    if (s_rb_overlay) s_rb_overlay(cw, ch, 1);   /* v250: D3D's row 0 is the top */
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
    rb_bindings_lost();
    rb_fixed_lost();
    return 1;
}

static struct {
    int state;                 /* 0 untried, 1 working, -1 off */
    HWND hwnd; HDC hdc, own_hdc; HGLRC ctx;
    wgl_make_t make; gdi_swap_t swap;
} s_gw;
static int s_gw_tries, s_gw_wait;   /* v147: retries of the window setup */

static int gl_window_present_impl(const RbFrame *f)
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
            if (dxp_present(f, hw, cw, ch, dx, dy, dw, dh)) {
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
    {
        GLuint fx = rb_present_aa(f->tex, f->fbo, f->w, f->h);   /* v238 */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx ? fx
                         : f->resolve ? f->resolve : f->fbo);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    rbg_bar_color(f->w, f->h);   /* v213 */
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, f->w, f->h, dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rbg_row_fill(f->rowk, f->w, f->h, dx, dy, dw, dy + dh);   /* v217b */
    if (s_rb_overlay) s_rb_overlay(cw, ch, 0);   /* v250 */
    gq_mark();
    s_gw.swap(s_gw.hdc);
    gw_timing_swap();
    s_gw.make(s_gw.own_hdc, s_gw.ctx);
    /* everything cached about bindings and fixed state is stale now */
    glEnable(GL_SCISSOR_TEST);
    rb_bindings_lost();
    rb_fixed_lost();
    return 1;
}

/* v172: with the GL thread, a frame into the game window once that is set
 * up: the blit and SwapBuffers on the GL thread (queued); the caches of
 * what is bound are dropped here, where they are kept */
typedef struct { RbFrame fr; uint64_t queued; } GwFrame;
/* v172b: how long a finished frame waited in the queue before SwapBuffers --
 * the delay the GL thread adds to what the player sees */
static volatile uint64_t s_gt_lat_sum, s_gt_lat_max, s_gt_lat_n;
static void gw_present_gl(void *arg)
{
    const GwFrame *gf = (const GwFrame *)arg;
    const RbFrame *f = &gf->fr;
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
    {
        GLuint fx = rb_present_aa(f->tex, f->fbo, f->w, f->h);   /* v238 */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx ? fx
                         : f->resolve ? f->resolve : f->fbo);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    rbg_bar_color(f->w, f->h);   /* v213 */
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, f->w, f->h, dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rbg_row_fill(f->rowk, f->w, f->h, dx, dy, dw, dy + dh);   /* v224 */
    if (s_rb_overlay) s_rb_overlay(cw, ch, 0);   /* v250 */
    s_gw.swap(s_gw.hdc);
    {
        uint64_t d = TM_NOW() - gf->queued;
        s_gt_lat_sum += d;
        if (d > s_gt_lat_max) s_gt_lat_max = d;
        s_gt_lat_n++;
    }
    gw_timing_swap();
    s_gw.make(s_gw.own_hdc, s_gw.ctx);
    glEnable(GL_SCISSOR_TEST);
}
typedef struct { const RbFrame *f; int r; } GwSync;
static void gw_present_sync(void *arg)
{
    GwSync *a = (GwSync *)arg;
    a->r = gl_window_present_impl(a->f);
}
int rb_window_present(const RbFrame *fr)
{
    if (s_gt_on && t_gt_mode == 1) {
        if (s_gw.state == 1) {
            GwFrame f;
            f.fr = *fr;
            f.queued = TM_NOW();
            gt_call(gw_present_gl, &f, sizeof f);
            rb_bindings_lost();
            rb_fixed_lost();
            return 1;
        }
        {
            /* setting up (or not possible): all of it there, waited for */
            GwSync a;
            a.f = fr; a.r = 0;
            gt_call_sync(gw_present_sync, &a);
            return a.r;
        }
    }
    return gl_window_present_impl(fr);
}
#else
/* v244: Linux. The context's own SDL window is the game window (shown at
 * the first frame, fb_present.c handles its events); each finished frame
 * is blitted into it, fitted, and swapped -- on the GL thread when that is
 * on, as on Windows. */
typedef struct { RbFrame fr; uint64_t queued; } GwFrame;
static volatile uint64_t s_gt_lat_sum, s_gt_lat_max, s_gt_lat_n;
static int s_sdlw_state;               /* 0 not shown yet, 1 shown */
extern void xbox_FramebufferWindowAttach(void *sdl_window);   /* fb_present.c */
extern void xbox_FramebufferWindowPump(void);
static void gw_present_sdl(void *arg)
{
    const GwFrame *gf = (const GwFrame *)arg;
    const RbFrame *f = &gf->fr;
    int cw = 0, ch = 0, dw, dh, dx, dy;
    SDL_GL_GetDrawableSize(s_rb_win, &cw, &ch);
    if (cw <= 0 || ch <= 0)
        return;
    xbox_PresentFit(cw, ch, &dw, &dh);
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    {
        GLuint fx = rb_present_aa(f->tex, f->fbo, f->w, f->h);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        /* the default framebuffer takes no multisample blit (driver raises
         * INVALID_OPERATION), so under MSAA read the resolve target */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx ? fx
                         : f->resolve ? f->resolve : f->fbo);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    rbg_bar_color(f->w, f->h);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, f->w, f->h, dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rbg_row_fill(f->rowk, f->w, f->h, dx, dy, dw, dy + dh);
    if (s_rb_overlay) s_rb_overlay(cw, ch, 0);   /* v250 */
    gq_mark();
    SDL_GL_SwapWindow(s_rb_win);
    {
        uint64_t d = TM_NOW() - gf->queued;
        s_gt_lat_sum += d;
        if (d > s_gt_lat_max) s_gt_lat_max = d;
        s_gt_lat_n++;
    }
    gw_timing_swap();
    glEnable(GL_SCISSOR_TEST);
}
int rb_window_present(const RbFrame *fr)
{
    GwFrame f;
    if (!s_rb_win)
        return 0;
    if (!s_sdlw_state) {
        const char *v = getenv("RECOMP_GL_VSYNC");
        s_sdlw_state = 1;
        if (s_gt_on && t_gt_mode == 1)
            glFinish();                    /* the context is the GL thread's now */
        SDL_GL_SetSwapInterval(v && v[0] == '1' ? 1 : 0);
        xbox_FramebufferWindowAttach(s_rb_win);
        xbox_FramebufferWindowGLOwned();
        fprintf(stderr, "[GL] frames drawn into the game window (SDL)\n");
    }
    xbox_FramebufferWindowPump();
    f.fr = *fr;
    f.queued = TM_NOW();
    if (s_gt_on && t_gt_mode == 1)
        gt_call(gw_present_sdl, &f, sizeof f);
    else
        gw_present_sdl(&f);
    rb_bindings_lost();
    rb_fixed_lost();
    return 1;
}
#endif

/* ── render targets and depth buffers ──────────────────────────────────── */

typedef struct { GLuint tex; GLenum target; GLsizei samples; GLenum internalformat;
                 GLsizei w, h; GLboolean fixed; } MsTexCreateArgs;
static void ms_tex_create_cb(void *p)
{
    MsTexCreateArgs *a = (MsTexCreateArgs *)p;
    glBindTexture(a->target, a->tex);
    glTexImage2DMultisample(a->target, a->samples, a->internalformat, a->w, a->h, a->fixed);
}

/* (re)make a target for a w x h surface: its colour texture (RGBA8, or RGB8
 * without alpha), and with samples > 1 a multisampled colour image drawn
 * into instead (resolved into the texture); the depth buffer is attached
 * later (rb_target_depth). *fbo / *tex are made when 0, *ms_tex replaced. */
void rb_target_shape(uint32_t *fbo, uint32_t *tex, uint32_t *ms_tex, int w, int h, int alpha, int samples)
{
    if (!*fbo) {
        glGenFramebuffers(1, fbo);
        glGenTextures(1, tex);
    }
    /* clear any stale attachments */
    rb_bind_draw(*fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0, 0);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    if (*ms_tex)
        glDeleteTextures(1, ms_tex);    /* the previous shape's */
    *ms_tex = 0;
    rb_unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, *tex); rb_active_unit(NV_GL_SCRATCH_UNIT);
    glTexImage2D(GL_TEXTURE_2D, 0, alpha ? GL_RGBA8 : GL_RGB8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (samples > 1) {
        MsTexCreateArgs ma;
        glGenTextures(1, ms_tex);
        ma.tex = *ms_tex;
        ma.target = GL_TEXTURE_2D_MULTISAMPLE;
        ma.samples = samples;
        ma.internalformat = alpha ? GL_RGBA8 : GL_RGB8;
        ma.w = (GLsizei)w;
        ma.h = (GLsizei)h;
        ma.fixed = GL_TRUE;
        gt_call_sync(ms_tex_create_cb, &ma);
    }
    rb_bind_draw(*fbo);
    if (samples > 1) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D_MULTISAMPLE, *ms_tex, 0);
    } else {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, *tex, 0);
    }
}

/* (re)make a depth-stencil buffer, w x h (D24S8, or D32F S8 with
 * depth32f; with samples > 1 a multisampled image, *is_tex 1), cleared to
 * clear_depth and stencil 0; *buf made when 0, or of the other kind */
void rb_depth_make(uint32_t *buf, int *is_tex, int w, int h, int samples, int depth32f, double clear_depth)
{
    if (samples > 1) {
        /* evict: free a prior name of the other kind before (re)creating */
        if (*buf && !*is_tex) {
            glDeleteRenderbuffers(1, buf);
            *buf = 0;
        }
        if (!*buf)
            glGenTextures(1, buf);
        *is_tex = 1;
        {
            MsTexCreateArgs za;
            za.tex = *buf;
            za.target = GL_TEXTURE_2D_MULTISAMPLE;
            za.samples = samples;
            za.internalformat = depth32f ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
            za.w = (GLsizei)w;
            za.h = (GLsizei)h;
            za.fixed = GL_TRUE;
            gt_call_sync(ms_tex_create_cb, &za);
        }
    } else {
        if (!*buf)
            glGenRenderbuffers(1, buf);
        *is_tex = 0;
        glBindRenderbuffer(GL_RENDERBUFFER, *buf);
        glRenderbufferStorage(GL_RENDERBUFFER, depth32f ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8,
                              (GLsizei)w, (GLsizei)h);
    }
    /* A new depth buffer starts at "far" with stencil 0, as the software
     * one does. */
    {
        GLuint tmp;
        glGenFramebuffers(1, &tmp);
        rb_bind_draw(tmp);
        if (*is_tex)
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D_MULTISAMPLE, *buf, 0);
        else
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, *buf);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearDepth(clear_depth);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        rb_bind_draw(0);
        glDeleteFramebuffers(1, &tmp);
        rb_fixed_lost();             /* depth and stencil masks changed */
    }
}

/* a depth-stencil buffer onto a target (bound for drawing after): 0, or
 * OpenGL's status when the target can't be drawn into */
uint32_t rb_target_depth(uint32_t fbo, uint32_t buf, int is_tex)
{
    GLenum fs;
    rb_bind_draw(fbo);
    if (is_tex)
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D_MULTISAMPLE, buf, 0);
    else
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, buf);
    fs = s_gt_on ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    return fs == GL_FRAMEBUFFER_COMPLETE ? 0 : fs;
}

/* ── surfaces: uploads, read-backs, resolves ───────────────────────────── */

static GLuint s_dn_fbo, s_dn_rb, s_up_fbo, s_up_tex;
static uint32_t s_dn_w, s_dn_h, s_up_w, s_up_h;

/* guest pixels (gw x gh, BGRA, rows bottom-up) into a target drawn at sw x sh:
 * straight into its texture, or scaled up through a staging texture; with
 * samples > 1 into its multisampled image through a draw */
void rb_surface_upload(uint32_t fbo, uint32_t tex, int gw, int gh, int sw, int sh,
                       const uint32_t *px, int samples)
{
    if (sw == gw && sh == gh) {
        rb_unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, tex); rb_active_unit(NV_GL_SCRATCH_UNIT);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)gw, (GLsizei)gh,
                        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        if (samples > 1) {
            /* single -> multi goes through a draw: a single to multi
             * glBlitFramebuffer is invalid */
            rb_ms_upscale(tex, fbo, sw, sh);
        }
    } else {
        /* guest size into a staging texture, then scaled up into the surface */
        if (!s_up_fbo) {
            glGenFramebuffers(1, &s_up_fbo);
            glGenTextures(1, &s_up_tex);
        }
        rb_unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, s_up_tex); rb_active_unit(NV_GL_SCRATCH_UNIT);
        if (s_up_w != (uint32_t)gw || s_up_h != (uint32_t)gh) {
            GLuint prev = rb_draw_bound();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)gw, (GLsizei)gh, 0,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            rb_bind_draw(s_up_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_up_tex, 0);
            rb_bind_draw(prev == (GLuint)-1 ? 0 : prev);
            s_up_w = (uint32_t)gw; s_up_h = (uint32_t)gh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)gw, (GLsizei)gh,
                            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        }
        if (samples > 1) {
            /* the LINEAR sampler of the upsample draw does the upscale */
            rb_ms_upscale(s_up_tex, fbo, sw, sh);
        } else {
            rb_copy_target(s_up_fbo, gw, gh, fbo, sw, sh, GL_LINEAR);
        }
    }
}

/* a target's image (gw x gh, BGRA, rows bottom-up) into memory */
void rb_read_pixels(uint32_t fbo, int w, int h, uint32_t *px)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
}

/* Resolve a multisampled target's colour into its single-sampled texture
 * (w x h): the target that holds it, for reading */
uint32_t rb_resolve(uint32_t fbo, uint32_t tex, int w, int h)
{
    static GLuint s_resolve_fbo;
    GLuint prev_draw, prev_read;
    if (!s_resolve_fbo)
        glGenFramebuffers(1, &s_resolve_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&prev_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, (GLint *)&prev_read);
    rb_bind_draw(s_resolve_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    rb_fixed_lost();
    rb_bind_draw(prev_draw == (GLuint)-1 ? 0 : prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    return s_resolve_fbo;
}

/* A target holding `fbo`'s image (sw x sh) at gw x gh: a scaled-down copy,
 * for reading pixels back at the guest's size */
uint32_t rb_target_scaled(uint32_t fbo, int sw, int sh, int gw, int gh)
{
    uint32_t w = (uint32_t)gw, h = (uint32_t)gh;
    if (!s_dn_fbo) {
        glGenFramebuffers(1, &s_dn_fbo);
        glGenRenderbuffers(1, &s_dn_rb);
    }
    if (s_dn_w != w || s_dn_h != h) {
        GLuint prev = rb_draw_bound();
        glBindRenderbuffer(GL_RENDERBUFFER, s_dn_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        rb_bind_draw(s_dn_fbo);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, s_dn_rb);
        rb_bind_draw(prev == (GLuint)-1 ? 0 : prev);
        s_dn_w = w; s_dn_h = h;
    }
    rb_copy_target(fbo, sw, sh, s_dn_fbo, (int)w, (int)h, GL_LINEAR);
    return s_dn_fbo;
}

/* ── textures ──────────────────────────────────────────────────────────────
 * On the texture bound to the active unit's target (rb_unit_bind /
 * rb_active_unit), as OpenGL does; targets, formats and parameter names are
 * OpenGL's numbers, which the Vulkan backend maps too. */

void rb_tex_new(uint32_t *id) { glGenTextures(1, id); }
void rb_tex_delete(uint32_t *id) { glDeleteTextures(1, id); }
void rb_tex_image(uint32_t target, int level, int ifmt, int w, int h, int border,
                  uint32_t format, uint32_t type, const void *data)
{
    glTexImage2D(target, level, ifmt, w, h, border, format, type, data);
}
void rb_tex_image_dxt(uint32_t target, int level, uint32_t cfmt, int w, int h, int border,
                      int size, const void *data)
{
    glCompressedTexImage2D(target, level, cfmt, w, h, border, size, data);
}
void rb_tex_param(uint32_t target, uint32_t pname, int v) { glTexParameteri(target, pname, v); }
void rb_tex_mipmaps(uint32_t target) { glGenerateMipmap(target); }
void rb_errors_clear(void) { int q = 8; while (q-- && glGetError() != GL_NO_ERROR) {} }
int  rb_no_error(void) { return glGetError() == GL_NO_ERROR; }
int  rb_max_aniso(void) { GLint mx = 0; glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &mx); return mx; }

/* A surface's image into a texture, flipped (a surface's rows run
 * bottom-up, a texture's top-down): the bottom w x h of a target sh high,
 * into texture id, (re)made w x h first when `remake` (alpha read as 1 with
 * one_alpha). */
void rb_tex_from_target(uint32_t id, int remake, int one_alpha, int w, int h, uint32_t src_fbo, int sh)
{
    static GLuint s_copy_fbo;
    GLuint prev = rb_draw_bound();
    if (!s_copy_fbo)
        glGenFramebuffers(1, &s_copy_fbo);
    if (remake) {
        rb_unit_bind(NV_GL_SCRATCH_UNIT, GL_TEXTURE_2D, id); rb_active_unit(NV_GL_SCRATCH_UNIT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h,
                     0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        if (one_alpha)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    rb_bind_draw(s_copy_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glDisable(GL_SCISSOR_TEST);             /* v172: as blit_fb */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, sh - h, w, sh, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    rb_fixed_lost();
    rb_bind_draw(prev == (GLuint)-1 ? 0 : prev);
}
