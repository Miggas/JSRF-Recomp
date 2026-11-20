/* nv2a_boost_fx.h -- v312e: the new boost effect (Idea by Miggas), included
 * by both backends (nv2a_rb_gl.c, nv2a_rb_vk.c: OpenGL, and the same calls
 * answered on Vulkan) after their MSAA upsample, whose helpers it uses
 * (FXAA_VS, aa_link, compile, gt_call_sync, rb_bind_draw, rb_fixed_lost).
 *
 * A radial blur, sharp in a circle around the player and blurrier towards
 * the edges, the samples taken along the line to the centre, and speed lines:
 * thin bright streaks pointing at the centre, in the outer part of the
 * picture, new ones every few frames. The frame's 3D picture (the target
 * `dst`) is copied to `tmp` and drawn back through it; the game draws its HUD
 * over it afterwards (jsrf_boost.c marks the place in the command stream). */

static const char *BOOST_FS =
"#version 330 core\n"
"uniform sampler2D u_tex;\n"
"uniform float u_s;\n"          /* strength 0..1 (fades in and out) */
"uniform float u_t;\n"          /* seconds, for the speed lines */
"uniform vec2 u_c;\n"           /* the clear centre (texture coordinates) */
"uniform float u_aspect;\n"     /* width / height as shown */
"in vec2 v_uv;\n"
"out vec4 o_col;\n"
"float hash(float n) { return fract(sin(n) * 43758.5453); }\n"
"void main() {\n"
"  vec2 d = v_uv - u_c;\n"
"  vec2 da = vec2(d.x * u_aspect, d.y);\n"
"  float r = length(da);\n"
"  float k = smoothstep(0.16, 0.72, r) * u_s;\n"
"  vec3 acc = vec3(0.0);\n"
"  for (int i = 0; i < 14; i++) {\n"
"    float f = float(i) / 13.0;\n"
"    acc += texture(u_tex, u_c + d * (1.0 - k * 0.14 * f)).rgb;\n"
"  }\n"
"  vec3 col = acc / 14.0;\n"
"  float a = atan(da.y, da.x) / 6.2831853 + 0.5;\n"
"  float n = a * 220.0;\n"
"  float seg = floor(n);\n"
"  float tick = floor(u_t * 24.0);\n"
"  float on = step(0.86, hash(seg * 1.731 + tick * 17.13));\n"
"  float thin = 1.0 - smoothstep(0.08, 0.38, abs(fract(n) - 0.5));\n"
"  float r0 = 0.30 + 0.30 * hash(seg * 3.117 + tick * 5.71);\n"
"  float along = smoothstep(r0, r0 + 0.18, r);\n"
"  float line = on * thin * along * u_s;\n"
"  col = mix(col, vec3(1.0), clamp(line * 0.55, 0.0, 1.0));\n"
"  o_col = vec4(col, texture(u_tex, v_uv).a);\n"
"}\n";

double xbox_PresentAspect(void);

typedef struct { GLuint dst_fbo, tmp_fbo, tmp_tex; int w, h; float s, t, cx, cy; } BoostArgs;

static struct {
    int state;                 /* 0 not built, 1 ready, -1 failed */
    GLuint prog, vao, samp;
    GLint l_tex, l_s, l_t, l_c, l_aspect;
} s_boost;

static void boost_run(void *p)
{
    BoostArgs *a = (BoostArgs *)p;
    static const GLenum caps[9] = { GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST,
                                    GL_SCISSOR_TEST, GL_POLYGON_OFFSET_FILL,
                                    GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1, GL_CLIP_DISTANCE2 };
    GLint prog, vao, unit, tex0, samp0, dfbo, rfbo, vp[4];
    GLboolean cm[4], dm, en[9];
    int i;

    if (s_boost.state == 0) {
        glGenVertexArrays(1, &s_boost.vao);        /* no attributes enabled */
        glGenSamplers(1, &s_boost.samp);
        glSamplerParameteri(s_boost.samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_boost.samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(s_boost.samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_boost.samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_boost.prog = aa_link(compile(GL_VERTEX_SHADER, FXAA_VS),
                               compile(GL_FRAGMENT_SHADER, BOOST_FS));
        if (!s_boost.prog) {
            fprintf(stderr, "[BOOST] the boost effect's shader did not build: effect off\n");
            s_boost.state = -1;
            return;
        }
        s_boost.l_tex = glGetUniformLocation(s_boost.prog, "u_tex");
        s_boost.l_s = glGetUniformLocation(s_boost.prog, "u_s");
        s_boost.l_t = glGetUniformLocation(s_boost.prog, "u_t");
        s_boost.l_c = glGetUniformLocation(s_boost.prog, "u_c");
        s_boost.l_aspect = glGetUniformLocation(s_boost.prog, "u_aspect");
        s_boost.state = 1;
    }
    if (s_boost.state < 0 || !a->dst_fbo || !a->tmp_fbo || a->w <= 0 || a->h <= 0)
        return;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
    for (i = 0; i < 9; i++) {
        en[i] = glIsEnabled(caps[i]);
        if (en[i]) glDisable(caps[i]);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    /* the picture so far, into tmp (a multisampled target is resolved by it) */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, a->dst_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, a->tmp_fbo);
    glBlitFramebuffer(0, 0, a->w, a->h, 0, 0, a->w, a->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* and back through the effect */
    glViewport(0, 0, a->w, a->h);
    rb_bind_draw(a->dst_fbo);
    glBindVertexArray(s_boost.vao);
    glUseProgram(s_boost.prog);
    glBindSampler(0, s_boost.samp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->tmp_tex);
    glUniform1i(s_boost.l_tex, 0);
    glUniform1f(s_boost.l_s, a->s);
    glUniform1f(s_boost.l_t, a->t);
    glUniform2f(s_boost.l_c, a->cx, a->cy);
    glUniform1f(s_boost.l_aspect, (float)xbox_PresentAspect());   /* as shown (fb_present.c) */
    glDrawArrays(GL_TRIANGLES, 0, 3);
    rb_draw_bound_set(a->dst_fbo);

    /* put it all back */
    glBindSampler(0, (GLuint)samp0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    glActiveTexture((GLenum)unit);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
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

void rb_boost_fx(uint32_t dst_target, uint32_t tmp_target, uint32_t tmp_tex, int w, int h,
                 float strength, float t, float cx, float cy)
{
    BoostArgs a;
    a.dst_fbo = dst_target; a.tmp_fbo = tmp_target; a.tmp_tex = tmp_tex;
    a.w = w; a.h = h; a.s = strength; a.t = t; a.cx = cx; a.cy = cy;
    gt_call_sync(boost_run, &a);
}
