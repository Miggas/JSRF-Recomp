/* v250: the quit window. Clicking both sticks (L3 + R3) asks "Quit the
 * game?" over the picture, drawn after the game's own Yes / No box (v251,
 * idea by Miggas); left / right picks, A takes it, B goes back (input/xinput_device.c:
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

static void qd_quad(const float *xy, float r, float g, float b, float a)   /* 4 corners in order */
{
    QdVert q[6] = {
        { xy[0], xy[1], r, g, b, a }, { xy[2], xy[3], r, g, b, a }, { xy[4], xy[5], r, g, b, a },
        { xy[0], xy[1], r, g, b, a }, { xy[4], xy[5], r, g, b, a }, { xy[6], xy[7], r, g, b, a },
    };
    if (s_qd_n + 6 > QD_MAX) return;
    memcpy(&s_qd_v[s_qd_n], q, sizeof q);
    s_qd_n += 6;
}

/* The picture is laid out in the game's 640 x 480 units (its own dialog,
 * measured from Miggas's capture of "Which is chosen?": box 195..446 x
 * 206..275, bar 199..442 x 212..236, buttons 240..300 and 341..401 x
 * 243..268), then placed on the 4:3 middle of the window. */
static float s_qx, s_qy, s_qs;   /* window pixel = s_qx + unit * s_qs */
#define QX(u) (s_qx + (u) * s_qs)
#define QY(u) (s_qy + (u) * s_qs)

static void qd_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    float xy[8] = { QX(x0), QY(y0), QX(x1), QY(y0), QX(x1), QY(y1), QX(x0), QY(y1) };
    qd_quad(xy, r, g, b, a);
}

/* a box with its corners cut by c (the game's boxes have small bevels) */
static void qd_box(float x0, float y0, float x1, float y1, float c,
                   float r, float g, float b, float a)
{
    qd_rect(x0 + c, y0, x1 - c, y1, r, g, b, a);
    qd_rect(x0, y0 + c, x0 + c, y1 - c, r, g, b, a);
    qd_rect(x1 - c, y0 + c, x1, y1 - c, r, g, b, a);
    {
        float t[4][8] = {
            { QX(x0), QY(y0 + c), QX(x0 + c), QY(y0), QX(x0 + c), QY(y0 + c), QX(x0), QY(y0 + c) },
            { QX(x1 - c), QY(y0), QX(x1), QY(y0 + c), QX(x1), QY(y0 + c), QX(x1 - c), QY(y0 + c) },
            { QX(x0), QY(y1 - c), QX(x0 + c), QY(y1 - c), QX(x0 + c), QY(y1), QX(x0 + c), QY(y1) },
            { QX(x1 - c), QY(y1 - c), QX(x1), QY(y1 - c), QX(x1 - c), QY(y1), QX(x1 - c), QY(y1) },
        };
        for (int i = 0; i < 4; i++) qd_quad(t[i], r, g, b, a);
    }
}

/* a bordered box: border colour, then the fill inset by w */
static void qd_frame(float x0, float y0, float x1, float y1, float c, float w,
                     const float *edge, const float *fill)
{
    qd_box(x0, y0, x1, y1, c, edge[0], edge[1], edge[2], edge[3]);
    qd_box(x0 + w, y0 + w, x1 - w, y1 - w, c > w ? c - w : 0, fill[0], fill[1], fill[2], fill[3]);
}

/* stb_easy_font letters (thin bars on a 6-wide, 7-high cell) scaled kx by ky,
 * each bar made heavier by bold units and leaned right by slant (the game's
 * font is a heavy italic); text centred on (cx, cy), in game units */
static float qd_text_w(const char *s, float kx) { return stb_easy_font_width((char *)s) * kx; }

static void qd_text(float cx, float cy, float kx, float ky, float bold, float slant,
                    const char *s, const float *col)
{
    static char buf[64 * 1024];   /* 16 bytes a corner, 4 corners a bar */
    int n = stb_easy_font_print(0, 0, (char *)s, NULL, buf, sizeof buf);
    float x = cx - qd_text_w(s, kx) / 2 + bold, base = cy + 3.5f * ky, y = cy - 3.5f * ky;
    for (int i = 0; i < n; i++) {
        const float *c0 = (const float *)(buf + (i * 4 + 0) * 16);
        const float *c2 = (const float *)(buf + (i * 4 + 2) * 16);
        float x0 = x + c0[0] * kx - bold, x1 = x + c2[0] * kx + bold;
        float y0 = y + c0[1] * ky - bold, y1 = y + c2[1] * ky + bold;
        float l0 = (base - y0) * slant, l1 = (base - y1) * slant;
        float xy[8] = { QX(x0 + l0), QY(y0), QX(x1 + l0), QY(y0),
                        QX(x1 + l1), QY(y1), QX(x0 + l1), QY(y1) };
        qd_quad(xy, col[0], col[1], col[2], col[3]);
    }
}

extern int xbox_QuitDialogSel(void);   /* 0 YES, 1 NO */

/* the picked button pulses: red, fading to white over a second, then red
 * again (as the game's; Miggas's description); t = 0 red .. 1 white */
static int s_qd_last_sel = -1;   /* -1: closed, so opening starts red */

static void qd_mix(float *o, const float *a, const float *b, float t)
{
    for (int i = 0; i < 4; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}

static void qd_button(float x0, float x1, const char *label, int picked, float t)
{
    static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 }, black[4] = { 0.04f, 0.04f, 0.04f, 1 },
                       shade[4] = { 0.45f, 0.45f, 0.45f, 1 };
    static const float red[4] = { 0.85f, 0.29f, 0.28f, 1 }, redfill[4] = { 0.13f, 0.04f, 0.04f, 1 },
                       redtext[4] = { 1.0f, 0.33f, 0.33f, 1 };
    const float y0 = 243.3f, y1 = 268.0f, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    float edge[4], fill[4], text[4], sh[4];
    if (!picked) t = 1.0f;
    qd_mix(edge, red, white, t);
    qd_mix(fill, redfill, black, t);
    qd_mix(text, redtext, white, t);
    memcpy(sh, shade, sizeof sh);
    sh[3] = t;                                   /* the shadow comes in with the white */
    qd_frame(x0, y0, x1, y1, 1.6f, 0.8f, edge, fill);
    qd_text(cx + 0.7f, cy + 0.7f, 2.75f, 1.95f, 0.6f, 0.06f, label, sh);
    qd_text(cx, cy, 2.75f, 1.95f, 0.6f, 0.06f, label, text);
}

/* the window, over a frame of cw x ch pixels in the framebuffer bound for
 * drawing; flip: that framebuffer's row 0 is the top (DXGI) */
extern char g_gl_build_msg[64];   /* v253: nv2a_gl.c, while shaders are built */

static void present_overlay_f(int cw, int ch, int flip)
{
    int quit = xbox_QuitDialogOpen();
    if (!quit) s_qd_last_sel = -1;
    if ((!quit && !g_gl_build_msg[0]) || cw <= 0 || ch <= 0) return;
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

    /* the 4:3 middle of the window, as the game's own 2D */
    s_qs = (float)ch / 480.0f;
    if (640.0f * s_qs > cw) s_qs = (float)cw / 640.0f;
    s_qx = (cw - 640.0f * s_qs) / 2;
    s_qy = (ch - 480.0f * s_qs) / 2;

    {
        static const float edge[4] = { 0.78f, 0.78f, 0.77f, 1 }, box[4] = { 0.0f, 0.0f, 0.0f, 0.93f };
        static const float bar_edge[4] = { 0.93f, 0.94f, 0.94f, 1 }, teal[4] = { 0.0f, 0.60f, 0.61f, 1 };
        static const float ink[4] = { 0.03f, 0.04f, 0.08f, 1 };
        static unsigned long long t0;
        unsigned long long now = GetTickCount64();
        int sel = xbox_QuitDialogSel();
        if (sel != s_qd_last_sel) { s_qd_last_sel = sel; t0 = now; }   /* a new pick starts red */
        float t = (float)((now - t0) % 1000) / 1000.0f;
        s_qd_n = 0;
        if (g_gl_build_msg[0]) {
            /* v253: "Building Shaders (xx out of yyy built)", bottom middle */
            static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 };
            char m[64];
            memcpy(m, g_gl_build_msg, sizeof m);
            m[63] = 0;
            float w = qd_text_w(m, 1.45f) + 22.0f;
            qd_frame(320.0f - w / 2, 428.0f, 320.0f + w / 2, 452.0f, 1.6f, 0.9f, edge, box);
            qd_text(320.5f, 440.0f, 1.45f, 1.9f, 0.45f, 0.22f, m, white);
        }
        if (quit) {
            qd_frame(195.0f, 205.7f, 446.0f, 275.3f, 1.8f, 1.1f, edge, box);
            qd_frame(199.0f, 211.7f, 442.0f, 236.0f, 0.0f, 0.8f, bar_edge, teal);
            qd_text(320.5f, 223.4f, 2.05f, 2.55f, 0.6f, 0.24f, "Quit the game?", ink);
            qd_button(240.0f, 300.0f, "YES", sel == 0, t);
            qd_button(340.7f, 400.7f, "NO", sel == 1, t);
        }
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
