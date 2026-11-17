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
