/* v250: the quit window. Clicking both sticks (L3 + R3) asks "Quit the
 * game?" over the picture; A quits, B goes back (input/xinput_device.c:
 * xbox_QuitDialogOpen). Drawn here, on the finished frame just before it is
 * shown, by the three window paths (Windows, Windows with the GL thread,
 * Linux), in the window's own pixels so it stays sharp at any size.
 *
 * Text: stb_easy_font (public domain), which makes each letter a few
 * rectangles -- no font file, no texture. One tiny shader draws coloured
 * triangles. The program, vertex array and buffer bound before are put back
 * (the renderer keeps them cached); blend and depth are re-applied by the
 * renderer after every present (s_ap_valid = 0). Included by nv2a_gl.c. */
#ifndef NV2A_GL_QUIT_H
#define NV2A_GL_QUIT_H

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../third_party/stb/stb_easy_font.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

extern int xbox_QuitDialogOpen(void);   /* input/xinput_device.c */

typedef struct { float x, y, r, g, b, a; } QdVert;
#define QD_MAX (6 * 6000)
static QdVert s_qd_v[QD_MAX];
static int s_qd_n;
static int s_qd_state;          /* 0 not set up, 1 ready, -1 failed */
static GLuint s_qd_prog, s_qd_vao, s_qd_vbo;
static GLint s_qd_l_vp;

static GLuint qd_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[QUIT] window shader: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int qd_setup(void)
{
    static const char *vs =
        "#version 330 core\n"
        "layout(location = 0) in vec2 p;\n"
        "layout(location = 1) in vec4 c;\n"
        "uniform vec3 vp;\n"   /* width, height, 1 or -1 (DXGI: upside down) */
        "out vec4 vc;\n"
        "void main() { gl_Position = vec4(p.x / vp.x * 2.0 - 1.0, (1.0 - p.y / vp.y * 2.0) * vp.z, 0.0, 1.0); vc = c; }\n";
    static const char *fs =
        "#version 330 core\n"
        "in vec4 vc;\n"
        "out vec4 o;\n"
        "void main() { o = vc; }\n";
    GLuint v = qd_shader(GL_VERTEX_SHADER, vs), f = qd_shader(GL_FRAGMENT_SHADER, fs);
    GLint ok = 0;
    if (!v || !f) return 0;
    s_qd_prog = glCreateProgram();
    glAttachShader(s_qd_prog, v);
    glAttachShader(s_qd_prog, f);
    glLinkProgram(s_qd_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_qd_prog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    s_qd_l_vp = glGetUniformLocation(s_qd_prog, "vp");
    glGenVertexArrays(1, &s_qd_vao);
    glGenBuffers(1, &s_qd_vbo);
    glBindVertexArray(s_qd_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_qd_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QdVert), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(QdVert), (void *)(2 * sizeof(float)));
    return 1;
}

static void qd_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    QdVert q[6] = {
        { x0, y0, r, g, b, a }, { x1, y0, r, g, b, a }, { x1, y1, r, g, b, a },
        { x0, y0, r, g, b, a }, { x1, y1, r, g, b, a }, { x0, y1, r, g, b, a },
    };
    if (s_qd_n + 6 > QD_MAX) return;
    memcpy(&s_qd_v[s_qd_n], q, sizeof q);
    s_qd_n += 6;
}

/* text at (x, y), letters k times stb_easy_font's size (about 7 px high) */
static float qd_text_w(const char *s, float k) { return stb_easy_font_width((char *)s) * k; }

static void qd_text(float x, float y, float k, const char *s, float r, float g, float b, float a)
{
    static char buf[64 * 1024];   /* 16 bytes a corner, 4 corners a rectangle */
    int n = stb_easy_font_print(0, 0, (char *)s, NULL, buf, sizeof buf);
    for (int i = 0; i < n; i++) {
        const float *c0 = (const float *)(buf + (i * 4 + 0) * 16);
        const float *c2 = (const float *)(buf + (i * 4 + 2) * 16);
        qd_rect(x + c0[0] * k, y + c0[1] * k, x + c2[0] * k, y + c2[1] * k, r, g, b, a);
    }
}

/* the window, over a frame of cw x ch pixels in the framebuffer bound for
 * drawing; flip: that framebuffer's row 0 is the top (DXGI) */
static void present_overlay_f(int cw, int ch, int flip)
{
    if (!xbox_QuitDialogOpen() || cw <= 0 || ch <= 0) return;
    if (!s_qd_state) {
        GLint p0 = 0, va0 = 0, ab0 = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &p0);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va0);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab0);
        s_qd_state = qd_setup() ? 1 : -1;
        if (s_qd_state < 0) fprintf(stderr, "[QUIT] the quit window cannot be drawn\n");
        glBindVertexArray((GLuint)va0);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab0);
        glUseProgram((GLuint)p0);
    }
    if (s_qd_state < 0) return;

    /* sizes from the window's height, so it looks the same at any size */
    float u = ch / 480.0f;
    float kt = 3.2f * u, kb = 2.2f * u;              /* title, buttons */
    const char *title = "QUIT THE GAME?";
    const char *yes = "A  YES", *no = "B  NO";
    float bw = qd_text_w(title, kt) + 80 * u, bh = 120 * u;
    float x0 = (cw - bw) / 2, y0 = (ch - bh) / 2, x1 = x0 + bw, y1 = y0 + bh;
    float bd = 3 * u;

    s_qd_n = 0;
    qd_rect(0, 0, (float)cw, (float)ch, 0, 0, 0, 0.55f);                     /* dim the game */
    qd_rect(x0 - bd, y0 - bd, x1 + bd, y1 + bd, 1.0f, 0.80f, 0.15f, 1.0f);   /* yellow border */
    qd_rect(x0, y0, x1, y1, 0.07f, 0.07f, 0.09f, 0.94f);                    /* the box */
    qd_text((cw - qd_text_w(title, kt)) / 2, y0 + 24 * u, kt, title, 1.0f, 0.80f, 0.15f, 1.0f);
    {
        float wy = qd_text_w(yes, kb), wn = qd_text_w(no, kb), gap = 60 * u;
        float tx = (cw - (wy + gap + wn)) / 2, ty = y0 + 78 * u;
        qd_text(tx, ty, kb, yes, 0.45f, 0.90f, 0.35f, 1.0f);                 /* A green */
        qd_text(tx + wy + gap, ty, kb, no, 0.95f, 0.40f, 0.35f, 1.0f);       /* B red */
    }

    GLint p0 = 0, va0 = 0, ab0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &p0);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va0);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    glUseProgram(s_qd_prog);
    glUniform3f(s_qd_l_vp, (float)cw, (float)ch, flip ? -1.0f : 1.0f);
    glBindVertexArray(s_qd_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_qd_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_qd_n * sizeof(QdVert)), s_qd_v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, s_qd_n);
    glDisable(GL_BLEND);
    glBindVertexArray((GLuint)va0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab0);
    glUseProgram((GLuint)p0);
}

static void present_overlay(int cw, int ch) { present_overlay_f(cw, ch, 0); }

#endif /* NV2A_GL_QUIT_H */
