/* v250: the quit window. Clicking both sticks (L3 + R3) asks "Quit the
 * game?" over the picture, drawn after the game's own Yes / No box (v251,
 * idea by Miggas); left / right picks, A takes it, B goes back (input/xinput_device.c:
 * xbox_QuitDialogOpen). Drawn here, on the finished frame just before it is
 * shown, by the three window paths (Windows, Windows with the GL thread,
 * Linux), in the window's own pixels so it stays sharp at any size.
 *
 * Text: v266, the NiseJSRF font (Yoyo's, dafont.com, "100% Free";
 * third_party/fonts, Miggas's pick), baked once by stb_truetype (public
 * domain) into a texture on unit 15 (bindings there put back); before,
 * and if that fails, stb_easy_font, which makes each letter a few
 * rectangles. One tiny shader draws coloured or font-textured triangles. The program, vertex array and buffer bound before are put back
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
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"
#include "../third_party/fonts/nisejsrf_ttf.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

extern int xbox_QuitDialogOpen(void);   /* input/xinput_device.c */
/* v278: the list menu (mission select) and the note line, same file */
extern int xbox_ListMenuOpen(void), xbox_ListMenuCount(void), xbox_ListMenuSel(void);
extern const char *xbox_ListMenuTitle(void), *xbox_ListMenuItem(int i);
extern int xbox_ListMenuPickable(int i);
extern char g_xbox_note[96];
extern char g_xbox_timer[24];   /* v300 */
extern char g_xbox_side_title[32], g_xbox_side_rows[5][48];   /* v301: a panel beside the list menu */

typedef struct { float x, y, u, v, r, g, b, a; } QdVert;   /* u < 0: no texture */
#define QD_MAX (6 * 6000)
static QdVert s_qd_v[QD_MAX];
static int s_qd_n;
static int s_qd_state;          /* 0 not set up, 1 ready, -1 failed */
static GLuint s_qd_prog, s_qd_vao, s_qd_vbo;
static GLint s_qd_l_vp, s_qd_l_tex;
/* v266: the font, baked at 64 pixels (ASCII 32..126) */
#define QD_FONT_PX 64.0f
#define QD_ATLAS_W 1024
#define QD_ATLAS_H 512
#define QD_UNIT 15
static stbtt_bakedchar s_qd_cd[95];
static GLuint s_qd_tex;
static int s_qd_font;           /* 1: the font is there */
static float s_qd_cap;          /* the capitals' height, in baked pixels */

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
        "layout(location = 2) in vec2 t;\n"
        "uniform vec3 vp;\n"   /* width, height, 1 or -1 (DXGI: upside down) */
        "out vec4 vc;\n"
        "out vec2 vt;\n"
        "void main() { gl_Position = vec4(p.x / vp.x * 2.0 - 1.0, (1.0 - p.y / vp.y * 2.0) * vp.z, 0.0, 1.0); vc = c; vt = t; }\n";
    static const char *fs =
        "#version 330 core\n"
        "in vec4 vc;\n"
        "in vec2 vt;\n"
        "uniform sampler2D tex;\n"
        "out vec4 o;\n"
        "void main() { o = vt.x < 0.0 ? vc : vec4(vc.rgb, vc.a * texture(tex, vt).r); }\n";
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
    s_qd_l_tex = glGetUniformLocation(s_qd_prog, "tex");
    glGenVertexArrays(1, &s_qd_vao);
    glGenBuffers(1, &s_qd_vbo);
    glBindVertexArray(s_qd_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_qd_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QdVert), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(QdVert), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(QdVert), (void *)(2 * sizeof(float)));
    {   /* v266: the font into a texture, on unit 15, whatever was there put back */
        static unsigned char atlas[QD_ATLAS_W * QD_ATLAS_H];
        int r = stbtt_BakeFontBitmap(k_nisejsrf_ttf, 0, QD_FONT_PX, atlas, QD_ATLAS_W, QD_ATLAS_H,
                                     32, 95, s_qd_cd);
        if (r > 0) {
            GLint unit0 = 0, tex0 = 0, smp0 = 0, align0 = 4;
            float x = 0, y = 0;
            stbtt_aligned_quad q;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
            glActiveTexture(GL_TEXTURE0 + QD_UNIT);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
            glGetIntegerv(GL_SAMPLER_BINDING, &smp0);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &align0);
            glBindSampler(QD_UNIT, 0);
            glGenTextures(1, &s_qd_tex);
            glBindTexture(GL_TEXTURE_2D, s_qd_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, QD_ATLAS_W, QD_ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
            glPixelStorei(GL_UNPACK_ALIGNMENT, align0);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
            glBindSampler(QD_UNIT, (GLuint)smp0);
            glActiveTexture((GLenum)unit0);
            stbtt_GetBakedQuad(s_qd_cd, QD_ATLAS_W, QD_ATLAS_H, 'H' - 32, &x, &y, &q, 1);
            s_qd_cap = -q.y0;
            s_qd_font = s_qd_cap > 1.0f;
        }
        if (!s_qd_font) fprintf(stderr, "[QUIT] the font could not be baked: the plain letters\n");
    }
    return 1;
}

static void qd_quad(const float *xy, float r, float g, float b, float a)   /* 4 corners in order */
{
    QdVert q[6] = {
        { xy[0], xy[1], -1, 0, r, g, b, a }, { xy[2], xy[3], -1, 0, r, g, b, a }, { xy[4], xy[5], -1, 0, r, g, b, a },
        { xy[0], xy[1], -1, 0, r, g, b, a }, { xy[4], xy[5], -1, 0, r, g, b, a }, { xy[6], xy[7], -1, 0, r, g, b, a },
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

/* v266: text in the font: capitals cap units high (shrunk to fit maxw),
 * centred on (cx, cy) by the capitals, in game units */
static float qd_ttf_w(const char *s)    /* in baked pixels */
{
    float x = 0, y = 0;
    stbtt_aligned_quad q;
    for (; *s; s++)
        if (*s >= 32 && *s < 127) stbtt_GetBakedQuad(s_qd_cd, QD_ATLAS_W, QD_ATLAS_H, *s - 32, &x, &y, &q, 1);
    return x;
}

static float qd_ttf_scale(const char *s, float cap, float maxw)
{
    float k = cap / s_qd_cap, w = qd_ttf_w(s) * k;
    if (maxw > 0 && w > maxw) k *= maxw / w;
    return k;
}

static void qd_ttf_text(float cx, float cy, float k, const char *s, const float *col)
{
    float x = 0, y = 0, x0 = cx - qd_ttf_w(s) * k / 2, base = cy + s_qd_cap * k / 2;
    stbtt_aligned_quad q;
    for (; *s; s++) {
        if (*s < 32 || *s >= 127) continue;
        stbtt_GetBakedQuad(s_qd_cd, QD_ATLAS_W, QD_ATLAS_H, *s - 32, &x, &y, &q, 1);
        if (q.x1 <= q.x0) continue;
        {
            float X0 = QX(x0 + q.x0 * k), X1 = QX(x0 + q.x1 * k), Y0 = QY(base + q.y0 * k), Y1 = QY(base + q.y1 * k);
            QdVert v[6] = {
                { X0, Y0, q.s0, q.t0, col[0], col[1], col[2], col[3] },
                { X1, Y0, q.s1, q.t0, col[0], col[1], col[2], col[3] },
                { X1, Y1, q.s1, q.t1, col[0], col[1], col[2], col[3] },
                { X0, Y0, q.s0, q.t0, col[0], col[1], col[2], col[3] },
                { X1, Y1, q.s1, q.t1, col[0], col[1], col[2], col[3] },
                { X0, Y1, q.s0, q.t1, col[0], col[1], col[2], col[3] },
            };
            if (s_qd_n + 6 > QD_MAX) return;
            memcpy(&s_qd_v[s_qd_n], v, sizeof v);
            s_qd_n += 6;
        }
    }
}

extern int xbox_QuitDialogSel(void);   /* v278b: 0 Quit Game, 1 Quit to Title, 2 Return */
extern int xbox_QuitTitleOK(void);

/* the picked button pulses: red, fading to white over a second, then red
 * again (as the game's; Miggas's description); t = 0 red .. 1 white */
static int s_qd_last_sel = -1;   /* -1: closed, so opening starts red */

static void qd_mix(float *o, const float *a, const float *b, float t)
{
    for (int i = 0; i < 4; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}

static void qd_button_f(float x0, float x1, const char *label, int picked, float t, int off);
static void qd_button(float x0, float x1, const char *label, int picked, float t)
{
    qd_button_f(x0, x1, label, picked, t, 0);
}

/* v278b: a button the width of its box (long labels shrink to fit); off:
 * greyed, not pickable */
static void qd_button_f(float x0, float x1, const char *label, int picked, float t, int off)
{
    static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 }, black[4] = { 0.04f, 0.04f, 0.04f, 1 },
                       shade[4] = { 0.45f, 0.45f, 0.45f, 1 };
    static const float red[4] = { 0.85f, 0.29f, 0.28f, 1 }, redfill[4] = { 0.13f, 0.04f, 0.04f, 1 },
                       redtext[4] = { 1.0f, 0.33f, 0.33f, 1 };
    const float y0 = 243.3f, y1 = 268.0f, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    float edge[4], fill[4], text[4], sh[4];
    if (!picked) t = 1.0f;
    if (off) {
        static const float grey[4] = { 0.40f, 0.40f, 0.40f, 1 }, dark[4] = { 0.03f, 0.03f, 0.03f, 1 };
        qd_frame(x0, y0, x1, y1, 1.6f, 0.8f, grey, dark);
        if (s_qd_font) qd_ttf_text(cx, cy, qd_ttf_scale(label, 11.0f, x1 - x0 - 8.0f), label, grey);
        else qd_text(cx, cy, 1.5f, 1.95f, 0.4f, 0.06f, label, grey);
        return;
    }
    qd_mix(edge, red, white, t);
    qd_mix(fill, redfill, black, t);
    qd_mix(text, redtext, white, t);
    memcpy(sh, shade, sizeof sh);
    sh[3] = t;                                   /* the shadow comes in with the white */
    qd_frame(x0, y0, x1, y1, 1.6f, 0.8f, edge, fill);
    if (s_qd_font) {
        float k = qd_ttf_scale(label, 11.0f, x1 - x0 - 8.0f);
        qd_ttf_text(cx + 0.7f, cy + 0.7f, k, label, sh);
        qd_ttf_text(cx, cy, k, label, text);
    } else {
        float kx = 2.75f, w = qd_text_w(label, 1.0f);
        if (w * kx > x1 - x0 - 8.0f) kx = (x1 - x0 - 8.0f) / w;
        qd_text(cx + 0.7f, cy + 0.7f, kx, 1.95f, 0.6f, 0.06f, label, sh);
        qd_text(cx, cy, kx, 1.95f, 0.6f, 0.06f, label, text);
    }
}

/* the window, over a frame of cw x ch pixels in the framebuffer bound for
 * drawing; flip: that framebuffer's row 0 is the top (DXGI) */
extern char g_gl_build_msg[64];   /* v253: nv2a_gl.c, while shaders are built */

static void present_overlay_f(int cw, int ch, int flip)
{
    int quit = xbox_QuitDialogOpen(), menu = xbox_ListMenuOpen();
    if (!quit) s_qd_last_sel = -1;
    if ((!quit && !menu && !g_gl_build_msg[0] && !g_xbox_note[0] && !g_xbox_timer[0]) || cw <= 0 || ch <= 0) return;
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
            if (s_qd_font) {
                float k = qd_ttf_scale(m, 9.0f, 560.0f), w = qd_ttf_w(m) * k + 22.0f;
                qd_frame(320.0f - w / 2, 428.0f, 320.0f + w / 2, 452.0f, 1.6f, 0.9f, edge, box);
                qd_ttf_text(320.0f, 440.0f, k, m, white);
            } else {
                float w = qd_text_w(m, 1.45f) + 22.0f;
                qd_frame(320.0f - w / 2, 428.0f, 320.0f + w / 2, 452.0f, 1.6f, 0.9f, edge, box);
                qd_text(320.5f, 440.0f, 1.45f, 1.9f, 0.45f, 0.22f, m, white);
            }
        }
        if (g_xbox_timer[0] && !menu && !quit) {
            /* v300: the mission timer (mission select), the window's top-left */
            static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 };
            char m[24];
            const float x0 = 10.0f - s_qx / s_qs, y0 = 10.0f - s_qy / s_qs;
            memcpy(m, g_xbox_timer, sizeof m);
            m[23] = 0;
            if (s_qd_font) {
                float k = qd_ttf_scale(m, 10.0f, 0.0f), w = qd_ttf_w("00:00.00") * k + 18.0f;
                qd_frame(x0, y0, x0 + w, y0 + 24.0f, 1.6f, 0.9f, edge, box);
                qd_ttf_text(x0 + 9.0f + qd_ttf_w(m) * k / 2, y0 + 12.0f, k, m, white);   /* from the left */
            } else {
                float w = qd_text_w("00:00.00", 1.6f) + 18.0f;
                qd_frame(x0, y0, x0 + w, y0 + 24.0f, 1.6f, 0.9f, edge, box);
                qd_text(x0 + w / 2 + 0.5f, y0 + 12.0f, 1.6f, 1.9f, 0.45f, 0.22f, m, white);
            }
        }
        if (g_xbox_note[0] && !g_gl_build_msg[0] && !menu) {
            /* v278: a note (mission select: "... ready: choose NEW GAME"), bottom middle */
            static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 };
            char m[96];
            memcpy(m, g_xbox_note, sizeof m);
            m[95] = 0;
            if (s_qd_font) {
                float k = qd_ttf_scale(m, 9.0f, 560.0f), w = qd_ttf_w(m) * k + 22.0f;
                qd_frame(320.0f - w / 2, 428.0f, 320.0f + w / 2, 452.0f, 1.6f, 0.9f, edge, box);
                qd_ttf_text(320.0f, 440.0f, k, m, white);
            } else {
                float w = qd_text_w(m, 1.45f) + 22.0f;
                qd_frame(320.0f - w / 2, 428.0f, 320.0f + w / 2, 452.0f, 1.6f, 0.9f, edge, box);
                qd_text(320.5f, 440.0f, 1.45f, 1.9f, 0.45f, 0.22f, m, white);
            }
        }
        if (menu) {
            /* v278: the list menu: a title bar, 11 rows around the pick, a hint line */
            static const float white[4] = { 0.94f, 0.95f, 0.94f, 1 }, dim[4] = { 0.62f, 0.63f, 0.62f, 1 };
            static const float red[4] = { 0.85f, 0.29f, 0.28f, 1 }, redfill[4] = { 0.13f, 0.04f, 0.04f, 1 },
                               redtext[4] = { 1.0f, 0.33f, 0.33f, 1 }, black[4] = { 0.04f, 0.04f, 0.04f, 1 };
            enum { ROWS = 11 };
            int n = xbox_ListMenuCount(), sel = xbox_ListMenuSel(), first, i;
            /* v292c: a short list (the "Start with" menu, Miggas) gets a box
             * of its rows only, narrower, in the middle of the screen */
            const int rows = n < ROWS ? (n > 0 ? n : 1) : ROWS;
            const float rh = 22.0f;
            float wide = n <= 6 ? 300.0f : 460.0f;   /* v279: wide, for the objectives */
            /* v301: a side panel (the personal bests, Miggas) right of the
             * menu: the two side by side in the middle of the window, the
             * menu narrower when the window is too (4:3) */
            const int side = g_xbox_side_title[0] && n > 6;
            const float sw = 214.0f, gap = 10.0f;
            float mx = 320.0f;
            if (side) {
                const float ww = cw / s_qs, wl = -s_qx / s_qs;
                if (wide + 12.0f + gap + sw + 20.0f > ww) wide = ww - 12.0f - gap - sw - 20.0f;
                mx = wl + ww / 2 - (gap + sw) / 2;
            }
            const float x0 = mx - wide / 2, x1 = mx + wide / 2;
            const float bt = 240.0f - (52.0f + rows * rh + 34.0f) / 2;  /* the box's top (76 when full) */
            const float top = bt + 52.0f;
            const char *ttl = xbox_ListMenuTitle();
            static int last_sel = -1;
            static unsigned long long m0;
            if (sel != last_sel) { last_sel = sel; m0 = now; }
            float mt = (float)((now - m0) % 1000) / 1000.0f;
            first = sel - ROWS / 2;
            if (first > n - ROWS) first = n - ROWS;
            if (first < 0) first = 0;
            qd_frame(x0 - 6.0f, bt, x1 + 6.0f, top + rows * rh + 34.0f, 1.8f, 1.1f, edge, box);
            qd_frame(x0 - 2.0f, bt + 6.0f, x1 + 2.0f, bt + 30.0f, 0.0f, 0.8f, bar_edge, teal);
            if (s_qd_font) qd_ttf_text(mx, bt + 18.0f, qd_ttf_scale(ttl, 12.0f, x1 - x0 - 20.0f), ttl, ink);
            else qd_text(mx + 0.5f, bt + 17.5f, 2.05f, 2.55f, 0.6f, 0.24f, ttl, ink);
            for (i = 0; i < rows && first + i < n; i++) {
                const char *it = xbox_ListMenuItem(first + i);
                float y0 = top + i * rh, cy = y0 + rh / 2 - 1.0f;
                float e4[4], f4[4], t4[4];
                if (!it[0]) continue;                       /* v288: a gap */
                if (it[0] == '\x01') {
                    /* v288: a heading (Chapter 1, ...): teal, not pickable */
                    static const float head[4] = { 0.25f, 0.82f, 0.82f, 1 };
                    it++;
                    if (s_qd_font) qd_ttf_text(mx, cy, qd_ttf_scale(it, 9.5f, x1 - x0 - 16.0f), it, head);
                    else qd_text(mx + 0.5f, cy, 1.6f, 2.0f, 0.45f, 0.18f, it, head);
                    continue;
                }
                if (first + i == sel) {
                    qd_mix(e4, red, white, mt);
                    qd_mix(f4, redfill, black, mt);
                    qd_mix(t4, redtext, white, mt);
                    qd_frame(x0, y0, x1, y0 + rh - 3.0f, 1.4f, 0.6f, e4, f4);
                } else {
                    memcpy(t4, dim, sizeof t4);
                }
                if (s_qd_font) qd_ttf_text(mx, cy, qd_ttf_scale(it, 8.5f, x1 - x0 - 16.0f), it, t4);
                else qd_text(mx + 0.5f, cy, 1.4f, 1.8f, 0.4f, 0.15f, it, t4);
            }
            {
                char h[96];
                int at = 0, all = 0, heads = 0, k;
                for (k = 0; k < n; k++) {                   /* v288: counted on the pickable rows */
                    if (xbox_ListMenuPickable(k)) { all++; if (k <= sel) at++; }
                    else heads += xbox_ListMenuItem(k)[0] == '\x01';
                }
                if (heads || n > ROWS)                      /* v288b: a heading's section a step */
                    snprintf(h, sizeof h, "%d / %d    A = choose    B = back    left / right = %s", at, all,
                             heads ? "chapter" : "page");
                else                                        /* v292c: nothing to page through */
                    snprintf(h, sizeof h, "%d / %d    A = choose    B = back", at, all);
                if (s_qd_font) qd_ttf_text(mx, top + rows * rh + 16.0f, qd_ttf_scale(h, 7.0f, x1 - x0), h, white);
                else qd_text(mx + 0.5f, top + rows * rh + 16.0f, 1.2f, 1.6f, 0.3f, 0.1f, h, white);
            }
            if (side) {
                /* v301: the panel: its title bar, then a row each, from the left */
                const float px0 = x1 + 6.0f + gap, px1 = px0 + sw, pr = 26.0f;
                int k;
                char pt[32];
                memcpy(pt, g_xbox_side_title, sizeof pt);
                pt[31] = 0;
                qd_frame(px0, bt, px1, bt + 40.0f + 5 * pr + 8.0f, 1.8f, 1.1f, edge, box);
                qd_frame(px0 + 4.0f, bt + 6.0f, px1 - 4.0f, bt + 30.0f, 0.0f, 0.8f, bar_edge, teal);
                if (s_qd_font) qd_ttf_text((px0 + px1) / 2, bt + 18.0f, qd_ttf_scale(pt, 12.0f, sw - 24.0f), pt, ink);
                else qd_text((px0 + px1) / 2 + 0.5f, bt + 17.5f, 2.05f, 2.55f, 0.6f, 0.24f, pt, ink);
                for (k = 0; k < 5; k++) {
                    char r[48];
                    float cy = bt + 40.0f + k * pr + pr / 2;
                    memcpy(r, g_xbox_side_rows[k], sizeof r);
                    r[47] = 0;
                    if (!r[0]) continue;
                    if (s_qd_font) {
                        float kk = qd_ttf_scale(r, 9.0f, sw - 24.0f);
                        qd_ttf_text(px0 + 12.0f + qd_ttf_w(r) * kk / 2, cy, kk, r, white);
                    } else {
                        float kx = 1.5f, w = qd_text_w(r, kx);
                        if (w > sw - 24.0f) kx *= (sw - 24.0f) / w, w = sw - 24.0f;
                        qd_text(px0 + 12.0f + w / 2, cy, kx, 1.9f, 0.45f, 0.18f, r, white);
                    }
                }
            }
        }
        if (quit) {
            /* v278b: Quit Game, Quit to Title (greyed when the game can't go
             * back now), Return (Miggas) */
            qd_frame(165.0f, 205.7f, 476.0f, 275.3f, 1.8f, 1.1f, edge, box);
            qd_frame(169.0f, 211.7f, 472.0f, 236.0f, 0.0f, 0.8f, bar_edge, teal);
            if (s_qd_font)
                qd_ttf_text(320.0f, 223.9f, qd_ttf_scale("Quit the game?", 12.0f, 228.0f), "Quit the game?", ink);
            else
                qd_text(320.5f, 223.4f, 2.05f, 2.55f, 0.6f, 0.24f, "Quit the game?", ink);
            qd_button(176.0f, 266.0f, "Quit Game", sel == 0, t);
            qd_button_f(275.0f, 365.0f, "Quit to Title", sel == 1, t, !xbox_QuitTitleOK());
            qd_button(374.0f, 464.0f, "Return", sel == 2, t);
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
    GLint unit0 = 0, tex0 = 0, smp0 = 0;
    if (s_qd_font) {            /* v266: the font on unit 15; what was there put back after */
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
        glActiveTexture(GL_TEXTURE0 + QD_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_SAMPLER_BINDING, &smp0);
        glBindTexture(GL_TEXTURE_2D, s_qd_tex);
        glBindSampler(QD_UNIT, 0);
        glUniform1i(s_qd_l_tex, QD_UNIT);
    }
    glBindVertexArray(s_qd_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_qd_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_qd_n * sizeof(QdVert)), s_qd_v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, s_qd_n);
    if (s_qd_font) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        glBindSampler(QD_UNIT, (GLuint)smp0);
        glActiveTexture((GLenum)unit0);
    }
    glDisable(GL_BLEND);
    glBindVertexArray((GLuint)va0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab0);
    glUseProgram((GLuint)p0);
}

static void present_overlay(int cw, int ch) { present_overlay_f(cw, ch, 0); }

#endif /* NV2A_GL_QUIT_H */
