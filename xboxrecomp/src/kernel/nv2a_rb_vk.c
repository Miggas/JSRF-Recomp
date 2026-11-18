/* nv2a_rb_vk.c: the Vulkan backend (nv2a_rb.h, rbvk_ops), chosen with
 * RECOMP_RENDERER=vulkan. Renderer backends plan, step 2.
 *
 * Step 2b, the start: no OpenGL anywhere -- no driver, no glad loader, no GL
 * thread. The backend is built on glvk's Vulkan core (glvk.c is compiled
 * into this file, its device, batches, rings, images, pipelines and SPIR-V
 * cache used directly), and its calls start as the OpenGL backend's code
 * pointed at that core. Slice by slice they become Vulkan's own (state
 * straight into pipelines, targets as images, passes instead of bound
 * framebuffers), each checked against RECOMP_RENDERER=glvk and OpenGL. */
#include "nv2a_gl.h"
#define RB_BACKEND_PREFIX(n) rbvk_##n
#include "nv2a_rb_names.h"
#include "nv2a_rb.h"
#include "nv2a_rb_ops.h"
#include "nv2a_vk.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#if !defined(_WIN32)
#include <SDL_vulkan.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "nv2a_tm.h"             /* TM_NOW */

/* glvk's core, compiled in (its public names kept apart from glvk.c's own) */
#define GLVK_AS_CORE 1
#define glvk_init         rbvk_core_init
#define glvk_proc         rbvk_core_proc
#define glvk_default_size rbvk_core_default_size
#define glvk_swap         rbvk_core_swap
#define glvk_device_name  rbvk_core_device_name
#include "glvk.c"

/* what was OpenGL in the code below goes straight to the core */
#undef glActiveTexture
#define glActiveTexture gv_ActiveTexture
#undef glAttachShader
#define glAttachShader gv_AttachShader
#undef glBeginQuery
#define glBeginQuery gv_BeginQuery
#undef glBindBuffer
#define glBindBuffer gv_BindBuffer
#undef glBindFramebuffer
#define glBindFramebuffer gv_BindFramebuffer
#undef glBindRenderbuffer
#define glBindRenderbuffer gv_BindRenderbuffer
#undef glBindSampler
#define glBindSampler gv_BindSampler
#undef glBindTexture
#define glBindTexture gv_BindTexture
#undef glBindVertexArray
#define glBindVertexArray gv_BindVertexArray
#undef glBindVertexBuffers
#define glBindVertexBuffers gvx_BindVertexBuffers
#undef glBlendColor
#define glBlendColor gv_BlendColor
#undef glBlendEquation
#define glBlendEquation gv_BlendEquation
#undef glBlendFunc
#define glBlendFunc gv_BlendFunc
#undef glBlitFramebuffer
#define glBlitFramebuffer gv_BlitFramebuffer
#undef glBufferData
#define glBufferData gv_BufferData
#undef glBufferStorage
#define glBufferStorage vkx_BufferStorage
#undef glBufferSubData
#define glBufferSubData gv_BufferSubData
#undef glCheckFramebufferStatus
#define glCheckFramebufferStatus gv_CheckFramebufferStatus
#undef glClear
#define glClear gv_Clear
#undef glClearColor
#define glClearColor gv_ClearColor
#undef glClearDepth
#define glClearDepth gv_ClearDepth
#undef glClearStencil
#define glClearStencil gv_ClearStencil
#undef glClientWaitSync
#define glClientWaitSync gv_ClientWaitSync
#undef glColorMask
#define glColorMask gv_ColorMask
#undef glCompileShader
#define glCompileShader gv_CompileShader
#undef glCompressedTexImage2D
#define glCompressedTexImage2D gv_CompressedTexImage2D
#undef glCreateProgram
#define glCreateProgram gv_CreateProgram
#undef glCreateShader
#define glCreateShader gv_CreateShader
#undef glCullFace
#define glCullFace gv_CullFace
#undef glDeleteBuffers
#define glDeleteBuffers gv_DeleteBuffers
#undef glDeleteFramebuffers
#define glDeleteFramebuffers gv_DeleteFramebuffers
#undef glDeleteProgram
#define glDeleteProgram gv_DeleteProgram
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers gv_DeleteRenderbuffers
#undef glDeleteShader
#define glDeleteShader gv_DeleteShader
#undef glDeleteSync
#define glDeleteSync gv_DeleteSync
#undef glDeleteTextures
#define glDeleteTextures gv_DeleteTextures
#undef glDepthFunc
#define glDepthFunc gv_DepthFunc
#undef glDepthMask
#define glDepthMask gv_DepthMask
#undef glDisable
#define glDisable gv_Disable
#undef glDisableVertexAttribArray
#define glDisableVertexAttribArray gv_DisableVertexAttribArray
#undef glDrawArrays
#define glDrawArrays vkd_DrawArrays
#undef glDrawElementsBaseVertex
#define glDrawElementsBaseVertex vkd_DrawElementsBaseVertex
#undef glEnable
#define glEnable gv_Enable
#undef glEnableVertexAttribArray
#define glEnableVertexAttribArray gv_EnableVertexAttribArray
#undef glEndQuery
#define glEndQuery gv_EndQuery
#undef glFenceSync
#define glFenceSync gv_FenceSync
#undef glFinish
#define glFinish gv_Finish
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer gv_FramebufferRenderbuffer
#undef glFramebufferTexture2D
#define glFramebufferTexture2D gv_FramebufferTexture2D
#undef glFrontFace
#define glFrontFace gv_FrontFace
#undef glGenBuffers
#define glGenBuffers gv_GenBuffers
#undef glGenFramebuffers
#define glGenFramebuffers gv_GenFramebuffers
#undef glGenQueries
#define glGenQueries gv_GenQueries
#undef glGenRenderbuffers
#define glGenRenderbuffers gv_GenRenderbuffers
#undef glGenSamplers
#define glGenSamplers gv_GenSamplers
#undef glGenTextures
#define glGenTextures gv_GenTextures
#undef glGenVertexArrays
#define glGenVertexArrays gv_GenVertexArrays
#undef glGenerateMipmap
#define glGenerateMipmap gv_GenerateMipmap
#undef glGetActiveUniform
#define glGetActiveUniform gv_GetActiveUniform
#undef glGetBooleanv
#define glGetBooleanv gv_GetBooleanv
#undef glGetError
#define glGetError gv_GetError
#undef glGetFloatv
#define glGetFloatv gv_GetFloatv
#undef glGetIntegerv
#define glGetIntegerv gv_GetIntegerv
#undef glGetProgramBinary
#define glGetProgramBinary gvx_GetProgramBinary
#undef glGetProgramInfoLog
#define glGetProgramInfoLog gv_GetProgramInfoLog
#undef glGetProgramiv
#define glGetProgramiv gv_GetProgramiv
#undef glGetQueryObjectuiv
#define glGetQueryObjectuiv gv_GetQueryObjectuiv
#undef glGetShaderInfoLog
#define glGetShaderInfoLog gv_GetShaderInfoLog
#undef glGetShaderiv
#define glGetShaderiv gv_GetShaderiv
#undef glGetString
#define glGetString gv_GetString
#undef glGetUniformLocation
#define glGetUniformLocation gv_GetUniformLocation
#undef glIsEnabled
#define glIsEnabled gv_IsEnabled
#undef glLinkProgram
#define glLinkProgram gv_LinkProgram
#undef glMapBufferRange
#define glMapBufferRange gv_MapBufferRange
#undef glPixelStorei
#define glPixelStorei gv_PixelStorei
#undef glPolygonOffset
#define glPolygonOffset gv_PolygonOffset
#undef glProgramBinary
#define glProgramBinary gvx_ProgramBinary
#undef glProgramParameteri
#define glProgramParameteri gv_ProgramParameteri
#undef glReadPixels
#define glReadPixels gv_ReadPixels
#undef glRenderbufferStorage
#define glRenderbufferStorage gv_RenderbufferStorage
#undef glSamplerParameteri
#define glSamplerParameteri gv_SamplerParameteri
#undef glScissor
#define glScissor gv_Scissor
#undef glShaderSource
#define glShaderSource gv_ShaderSource
#undef glStencilFunc
#define glStencilFunc gv_StencilFunc
#undef glStencilMask
#define glStencilMask gv_StencilMask
#undef glStencilOp
#define glStencilOp gv_StencilOp
#undef glTexImage2D
#define glTexImage2D gv_TexImage2D
#undef glTexImage2DMultisample
#define glTexImage2DMultisample gv_TexImage2DMultisample
#undef glTexParameteri
#define glTexParameteri gv_TexParameteri
#undef glTexSubImage2D
#define glTexSubImage2D gv_TexSubImage2D
#undef glUniform1f
#define glUniform1f gv_Uniform1f
#undef glUniform1fv
#define glUniform1fv gv_Uniform1fv
#undef glUniform1i
#define glUniform1i gv_Uniform1i
#undef glUniform1iv
#define glUniform1iv gv_Uniform1iv
#undef glUniform1ui
#define glUniform1ui gv_Uniform1ui
#undef glUniform1uiv
#define glUniform1uiv gv_Uniform1uiv
#undef glUniform2f
#define glUniform2f gv_Uniform2f
#undef glUniform2fv
#define glUniform2fv gv_Uniform2fv
#undef glUniform3f
#define glUniform3f gv_Uniform3f
#undef glUniform3fv
#define glUniform3fv gv_Uniform3fv
#undef glUniform4f
#define glUniform4f gv_Uniform4f
#undef glUniform4fv
#define glUniform4fv gv_Uniform4fv
#undef glUniformMatrix4fv
#define glUniformMatrix4fv gv_UniformMatrix4fv
#undef glUnmapBuffer
#define glUnmapBuffer gv_UnmapBuffer
#undef glUseProgram
#define glUseProgram gv_UseProgram
#undef glVertexAttrib4f
#define glVertexAttrib4f gv_VertexAttrib4f
#undef glVertexAttrib4fv
#define glVertexAttrib4fv gv_VertexAttrib4fv
#undef glVertexAttribBinding
#define glVertexAttribBinding gvx_VertexAttribBinding
#undef glVertexAttribFormat
#define glVertexAttribFormat gvx_VertexAttribFormat
#undef glVertexAttribPointer
#define glVertexAttribPointer gv_VertexAttribPointer
#undef glViewport
#define glViewport gv_Viewport


/* ── draws (step 2b, slice 1: the backend's own) ──────────────────────────
 * The core's draw_setup records everything a draw needs on every draw: the
 * pipeline looked up by a hash of its whole key, each vertex buffer, sixteen
 * pieces of dynamic state, the index buffer and a push of every descriptor.
 * Here the command buffer's state is remembered and only what changed is
 * recorded: the pipeline key is compared with the last draw's before any
 * hashing, vertex buffers go in one call when they moved, dynamic state is
 * set when it differs, and the descriptors are pushed only when the layout,
 * the uniforms' place or a texture or sampler changed. All of it is
 * forgotten with a new command buffer. The pictures are the same. */

int rb_prof_enabled(void);
#define rb_prof_enabled_fwd rb_prof_enabled
#define VKD_NII 160
static struct {
    VkCommandBuffer cmd; uint64_t serial;      /* the command buffer this is about */
    /* pipeline */
    GProg *g; PKey k; VkPipeline kp;          /* the last key looked up, and its pipeline */
    /* vertex and index buffers */
    VkBuffer vb[NATTR]; VkDeviceSize vo[NATTR]; uint32_t vmask;
    VkBuffer ib; VkIndexType it;
    /* dynamic state */
    int dyn_ok;
    VkViewport vp; VkRect2D sc;
    VkCullModeFlags cull; VkFrontFace ff;
    VkBool32 dt, dw, st, be;
    VkCompareOp dc, sf;
    VkStencilOp so[3];
    uint32_t scm, swm, sref;
    float bias[2], bc[4];
    /* descriptors */
    VkPipelineLayout pl;
    VkDescriptorBufferInfo bi; int ubo;
    VkDescriptorImageInfo ii[VKD_NII]; uint32_t nii;
    uint32_t wb[40], wc[40], nw;
} D;
static uint64_t s_vkd_draws, s_vkd_pipe_hash, s_vkd_vb, s_vkd_dyn, s_vkd_push;
/* RECOMP_RB_PROFILE=1: where a draw's time goes (ticks): 0 rendering begun,
 * 1 vertex inputs and key, 2 pipeline, 3 vertex buffers, 4 dynamic state,
 * 5 uniforms copied, 6 descriptors, 7 the draw command; and uniform bytes */
static uint64_t s_vkd_t[8], s_vkd_ubytes;
static int s_vkd_prof = -1;
#define VKD_T(i) do { if (s_vkd_prof) { uint64_t t_ = TM_NOW(); s_vkd_t[i] += t_ - tp_; tp_ = t_; } } while (0)

static VkPipeline vkp_get(GProg *g, GLuint id, const PKey *k);

static int vkd_same_cmd(void)
{
    VkCommandBuffer c = G.b[G.cur].cmd;
    if (D.cmd == c && D.serial == G.b[G.cur].serial)
        return 1;
    memset(&D, 0, sizeof D);
    D.cmd = c;
    D.serial = G.b[G.cur].serial;
    return 0;
}

static int vkd_setup(void)
{
    GProg *g = prog_of(S.prog);
    GVao *v = GET(t_vao, S.vao);
    GTex *col, *ds;
    GLuint ci, di;
    PKey k;
    VkPipeline p;
    VkCommandBuffer c;
    VkBuffer vbufs[NATTR];
    VkDeviceSize voffs[NATTR];
    int i;
    uint64_t tp_ = 0;
    if (s_vkd_prof < 0) s_vkd_prof = rb_prof_enabled_fwd();
    if (s_vkd_prof) tp_ = TM_NOW();
    if (!g || !g->linked || !v)
        return 0;
    if (S.vp[2] <= 0 || S.vp[3] <= 0)
        return 0;
    if (!rendering_begin())
        return 0;
    vkd_same_cmd();
    VKD_T(0);
    fbo_tex(S.draw_fbo, &col, &ds, &ci, &di);
    if (col && col->bc) col = NULL;
    if (R.ds == 0) ds = NULL;
    memset(&k, 0, sizeof k);
    k.cf = col ? col->fmt : VK_FORMAT_UNDEFINED;
    k.df = ds ? ds->fmt : VK_FORMAT_UNDEFINED;
    k.samples = col ? col->samples : ds ? ds->samples : 1;
    k.blend = S.blend;
    if (S.blend) { k.bs = S.bl_src; k.bd = S.bl_dst; k.be = S.bl_eq; }
    k.cmask = (S.cmask[0] ? 1 : 0) | (S.cmask[1] ? 2 : 0) | (S.cmask[2] ? 4 : 0)
            | (S.cmask[3] && !(col && col->rgb) ? 8 : 0);     /* RGB targets keep alpha 1 */
    /* vertex inputs: an array, or the generic value as a one-vertex buffer */
    {
        Ring *r = NULL;
        VkDeviceSize co = 0;
        int nconst = 0;
        for (i = 0; i < NATTR; i++)
            if ((g->inputs & (1u << i)) && !(v->a[i].enabled && buf_of(v->a[i].buf)))
                nconst++;
        if (nconst) {
            co = ring_take(0, (VkDeviceSize)nconst * 16, 16, &r);
            if (!r) return 0;
        }
        for (i = 0; i < NATTR; i++) {
            if (!(g->inputs & (1u << i))) continue;
            if (v->a[i].enabled && buf_of(v->a[i].buf)) {
                GBuf *b = buf_of(v->a[i].buf);
                VkDeviceSize off = (VkDeviceSize)v->a[i].off;
                k.vf[i] = attr_format(&v->a[i]);
                k.vstride[i] = v->a[i].stride ? (uint32_t)v->a[i].stride : attr_bytes(&v->a[i]);
                /* bound at the start of a vertex, the attribute's place in it
                 * in the pipeline (AMD counts a binding's vertices from its
                 * offset: v311g-vk5d) */
                k.vrel[i] = (uint32_t)(off % k.vstride[i]);
                if (k.vrel[i] > G.c.props.limits.maxVertexInputAttributeOffset)
                    k.vrel[i] = 0;
                vbufs[i] = b->b;
                voffs[i] = off - k.vrel[i];
            } else {
                memcpy(r->map + co, S.generic[i], 16);
                k.vf[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
                k.vstride[i] = 0;
                vbufs[i] = r->b;
                voffs[i] = co;
                co += 16;
            }
        }
    }
    VKD_T(1);
    if (D.kp && D.g == g && !memcmp(&D.k, &k, sizeof k))
        p = D.kp;                             /* the same pipeline as the last draw */
    else {
        p = vkp_get(g, S.prog, &k);
        s_vkd_pipe_hash++;
        if (!p)
            return 0;
        D.g = g; D.k = k; D.kp = p;
    }
    c = G.b[G.cur].cmd;
    if (R.pipe != p) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        R.pipe = p;
    }
    VKD_T(2);
    /* vertex buffers: the inputs' bindings, in runs, where they moved */
    {
        uint32_t m = g->inputs & ((1u << NATTR) - 1);
        int changed = m != D.vmask;
        for (i = 0; i < NATTR && !changed; i++)
            if ((m >> i & 1) && (D.vb[i] != vbufs[i] || D.vo[i] != voffs[i]))
                changed = 1;
        if (changed) {
            for (i = 0; i < NATTR; ) {
                int j;
                if (!(m >> i & 1)) { i++; continue; }
                for (j = i; j < NATTR && (m >> j & 1); j++) {}
                vkCmdBindVertexBuffers(c, (uint32_t)i, (uint32_t)(j - i), &vbufs[i], &voffs[i]);
                i = j;
            }
            for (i = 0; i < NATTR; i++) { D.vb[i] = vbufs[i]; D.vo[i] = voffs[i]; }
            D.vmask = m;
            s_vkd_vb++;
        }
    }
    VKD_T(3);
    /* dynamic state, where it differs */
    {
        VkViewport vp;
        VkRect2D sc;
        VkCullModeFlags cull;
        VkFrontFace ff;
        VkBool32 dt, dwr, st, be;
        VkCompareOp dc, sf;
        VkStencilOp so[3];
        int all = !D.dyn_ok, n = 0;
        vp.x = (float)S.vp[0]; vp.y = (float)S.vp[1];
        vp.width = (float)S.vp[2]; vp.height = (float)S.vp[3];
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        if (S.scissor) {
            int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1];
            int x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
            if (x1 > R.vw) x1 = R.vw;
            if (y1 > R.vh) y1 = R.vh;
            sc.offset.x = x0; sc.offset.y = y0;
            sc.extent.width = (uint32_t)(x1 > x0 ? x1 - x0 : 0);
            sc.extent.height = (uint32_t)(y1 > y0 ? y1 - y0 : 0);
        } else {
            sc.offset.x = sc.offset.y = 0;
            sc.extent.width = (uint32_t)R.vw;
            sc.extent.height = (uint32_t)R.vh;
        }
        if (!sc.extent.width || !sc.extent.height)
            return 0;
        if (all || memcmp(&vp, &D.vp, sizeof vp)) { vkCmdSetViewport(c, 0, 1, &vp); D.vp = vp; n++; }
        if (all || memcmp(&sc, &D.sc, sizeof sc)) { vkCmdSetScissor(c, 0, 1, &sc); D.sc = sc; n++; }
        if (all) vkCmdSetPrimitiveTopology(c, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cull = !S.cull ? VK_CULL_MODE_NONE
             : S.cull_face == GL_FRONT ? VK_CULL_MODE_FRONT_BIT
             : S.cull_face == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK
             : VK_CULL_MODE_BACK_BIT;
        if (all || cull != D.cull) { vkCmdSetCullMode(c, cull); D.cull = cull; n++; }
        /* OpenGL's area sign is the opposite of Vulkan's for the same rows */
        ff = S.front_face == GL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        if (all || ff != D.ff) { vkCmdSetFrontFace(c, ff); D.ff = ff; n++; }
        dt = S.depth_test && ds ? VK_TRUE : VK_FALSE;
        dwr = S.depth_test && S.depth_mask && ds ? VK_TRUE : VK_FALSE;
        dc = vk_cmp(S.depth_func);
        if (all || dt != D.dt) { vkCmdSetDepthTestEnable(c, dt); D.dt = dt; n++; }
        if (all || dwr != D.dw) { vkCmdSetDepthWriteEnable(c, dwr); D.dw = dwr; n++; }
        if (all || dc != D.dc) { vkCmdSetDepthCompareOp(c, dc); D.dc = dc; n++; }
        st = S.stencil_test && ds ? VK_TRUE : VK_FALSE;
        if (all || st != D.st) { vkCmdSetStencilTestEnable(c, st); D.st = st; n++; }
        so[0] = vk_sop(S.st_fail); so[1] = vk_sop(S.st_zpass); so[2] = vk_sop(S.st_zfail);
        sf = vk_cmp(S.st_func);
        if (all || memcmp(so, D.so, sizeof so) || sf != D.sf) {
            vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, so[0], so[1], so[2], sf);
            memcpy(D.so, so, sizeof so); D.sf = sf; n++;
        }
        if (all || (S.st_mask & 0xFF) != D.scm) {
            D.scm = S.st_mask & 0xFF;
            vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, D.scm); n++;
        }
        if (all || (S.st_wmask & 0xFF) != D.swm) {
            D.swm = S.st_wmask & 0xFF;
            vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, D.swm); n++;
        }
        if (all || ((uint32_t)S.st_ref & 0xFF) != D.sref) {
            D.sref = (uint32_t)S.st_ref & 0xFF;
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, D.sref); n++;
        }
        be = S.poff ? VK_TRUE : VK_FALSE;
        if (all || be != D.be) { vkCmdSetDepthBiasEnable(c, be); D.be = be; n++; }
        if (all || S.po_units != D.bias[0] || S.po_factor != D.bias[1]) {
            vkCmdSetDepthBias(c, S.po_units, 0.0f, S.po_factor);
            D.bias[0] = S.po_units; D.bias[1] = S.po_factor; n++;
        }
        if (all || memcmp(S.bl_col, D.bc, sizeof D.bc)) {
            vkCmdSetBlendConstants(c, S.bl_col);
            memcpy(D.bc, S.bl_col, sizeof D.bc); n++;
        }
        D.dyn_ok = 1;
        s_vkd_dyn += (uint64_t)n;
    }
    VKD_T(4);
    /* the uniforms and textures, pushed when something in them changed */
    {
        VkWriteDescriptorSet w[40];
        VkDescriptorBufferInfo bi;
        VkDescriptorImageInfo ii[VKD_NII];
        uint32_t nw = 0, nii = 0, wb[40], wc[40];
        int ubo = 0, same;
        memset(&bi, 0, sizeof bi);
        if (g->ubo_binding >= 0) {
            if (g->dirty || g->ubo_serial != G.b[G.cur].serial || !g->ubo_buf) {
                Ring *r;
                VkDeviceSize o = ring_take(0, g->ubo_size ? g->ubo_size : 16, G.ubo_align, &r);
                if (!r) return 0;
                memcpy(r->map + o, g->shadow, g->ubo_size);
                g->ubo_off = o;
                g->ubo_buf = r->b;
                g->ubo_serial = G.b[G.cur].serial;
                g->dirty = 0;
                s_vkd_ubytes += g->ubo_size;
            }
            VKD_T(5);
            bi.buffer = g->ubo_buf;
            bi.offset = g->ubo_off;
            bi.range = g->ubo_size ? g->ubo_size : 16;
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstBinding = (uint32_t)g->ubo_binding;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[nw].pBufferInfo = &bi;
            wb[nw] = w[nw].dstBinding; wc[nw] = 1;
            nw++;
            ubo = 1;
        }
        for (i = 0; i < g->nu && nw < 40; i++) {
            Uni *u = &g->u[i];
            int e;
            if (!u->sampler) continue;
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstBinding = u->binding;
            w[nw].descriptorCount = (uint32_t)u->count;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[nw].pImageInfo = &ii[nii];
            for (e = 0; e < u->count && nii < VKD_NII; e++) {
                int unit = u->unit[e] & (NUNIT - 1);
                GTex *t = tex_of(u->cube ? S.texcube[unit] : S.tex2d[unit]);
                GSamp *sm = S.samp[unit] && S.samp[unit] < t_samp_n && t_samp[S.samp[unit]].used
                            ? &t_samp[S.samp[unit]] : NULL;
                int maxlod = 0;
                if (!t || !t->img || !t->view || t->ds || !(t->defined & 1)
                        || (u->cube ? t->layers != 6 : t->layers != 1)) {
                    t = u->cube ? s_whitecube : s_white2d;
                } else if (t == col) {
                    GV_ONCE(4, "a texture is sampled while drawn into (left out)\n");
                    t = u->cube ? s_whitecube : s_white2d;
                }
                while (maxlod + 1 < t->levels && (t->defined >> (maxlod + 1) & 1))
                    maxlod++;
                if (maxlod > t->maxlev) maxlod = t->maxlev;
                if (t->swz_a1 != t->view_swz) {          /* the alpha swizzle changed */
                    junk(2, (uint64_t)t->view, VK_NULL_HANDLE);
                    t->view = make_view(t, 1, t->swz_a1);
                    t->view_swz = t->swz_a1;
                }
                ii[nii].imageView = t->view;
                ii[nii].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ii[nii].sampler = sm ? sampler_for(sm->minf, sm->magf, sm->wrap_s, sm->wrap_t,
                                                   GL_CLAMP_TO_EDGE, maxlod, 1)
                                     : sampler_for(t->minf, t->magf, t->wrap_s, t->wrap_t, t->wrap_r,
                                                   maxlod, t->aniso);
                nii++;
            }
            wb[nw] = w[nw].dstBinding; wc[nw] = w[nw].descriptorCount;
            nw++;
        }
        same = D.pl == g->pl && D.nw == nw && D.ubo == ubo && D.nii == nii
               && (!ubo || !memcmp(&D.bi, &bi, sizeof bi))
               && !memcmp(D.wb, wb, nw * sizeof wb[0]) && !memcmp(D.wc, wc, nw * sizeof wc[0])
               && !memcmp(D.ii, ii, nii * sizeof ii[0]);
        if (nw && !same) {
            vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, nw, w);
            D.pl = g->pl; D.nw = nw; D.ubo = ubo; D.bi = bi; D.nii = nii;
            memcpy(D.wb, wb, nw * sizeof wb[0]); memcpy(D.wc, wc, nw * sizeof wc[0]);
            memcpy(D.ii, ii, nii * sizeof ii[0]);
            s_vkd_push++;
        }
    }
    VKD_T(6);
    s_n_draws++;
    s_vkd_draws++;
    return 1;
}

/* ── pipelines built ahead (v311g-vk9) ─────────────────────────────────────
 * A pipeline is the program with everything fixed around it (vertex
 * formats, the target's formats and samples, blending); the driver compiles
 * it when it is made, and making it on the first draw that needs it is a
 * hitch (Miggas's vk8d: 58 -> 99 pipelines made during a session, 2-10 us a
 * draw averaged over the ten seconds they fell in). So each one made is
 * written down -- the program's sources once, and the pipeline's key --
 * in shader_cache/vk_pipes.bin, and the next start makes them all before
 * they are needed: the programs at once (from the SPIR-V cache), the
 * pipelines on worker threads while the game boots. rb_program_build hands
 * the renderer the program already made; the first draw takes its pipeline
 * from the workers (waiting only if that one is still being made). */

typedef struct VkpJob {
    int kind;                          /* 0 a pipeline, 1 a program's SPIR-V (vk10) */
    int state;                         /* 0 to do, 1 being done, 2 done, 3 taken */
    int ahead;                         /* from the list, at start-up */
    /* a pipeline */
    GLuint prog;                       /* the program it is for */
    uint64_t kh;                       /* pkey_hash of k */
    PKey k;
    VkShaderModule vm, fm;             /* copied: the program table may move */
    VkPipelineLayout pl;
    uint32_t inputs;
    int dclamp;
    VkPipeline p;
    /* a program's SPIR-V (into the SPIR-V cache, where linking finds it):
     * vs/fs the sources as given, cvs/cfs as linking compiles them (the
     * renderer's defines after #version, the vertex shader patched) */
    char *vs, *fs, *cvs, *cfs;
    int ok;
} VkpJob;
typedef struct { uint64_t h; GLuint id; int handed; } VkpReady;
static GLuint vkp_link(const char *vsrc, const char *fsrc);

static SDL_mutex *s_vkp_mu;
static SDL_cond *s_vkp_cv;             /* a job done */
static SDL_cond *s_vkp_work;           /* a job to do */
static VkpJob **s_vkp_job;
static int s_vkp_njob, s_vkp_cjob, s_vkp_next, s_vkp_threads;
static int s_vkp_nahead, s_vkp_made, s_vkp_taken, s_vkp_waited, s_vkp_logged;
static VkpReady *s_vkp_ready;
static int s_vkp_nready;
static uint64_t s_vkp_t0, s_vkp_t1;
static uint64_t *s_vkp_idh;            /* program id -> its sources' hash (0: unknown) */
static uint32_t s_vkp_nidh;
static uint64_t s_vkp_known[8192];     /* hashes written down: programs, and program x key */
static FILE *s_vkp_out;
static int s_vkp_off = -1;
/* each program's pipeline keys so far (what a program built in the
 * background in its place will need: vk10) */
typedef struct { GLuint prog; PKey k; } VkpKey;
static VkpKey *s_vkp_pk;
static int s_vkp_npk, s_vkp_cpk;

static int vkp_on(void)
{
    if (s_vkp_off < 0) {
        const char *e = getenv("RECOMP_VK_PIPES_AHEAD");
        s_vkp_off = e && e[0] == '0';
    }
    return !s_vkp_off;
}
static uint64_t vkp_src_hash(const char *v, const char *f)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const unsigned char *p;
    for (p = (const unsigned char *)v; *p; p++) h = (h ^ *p) * 0x100000001B3ull;
    h = (h ^ 0xFF) * 0x100000001B3ull;
    for (p = (const unsigned char *)f; *p; p++) h = (h ^ *p) * 0x100000001B3ull;
    return h ? h : 1;
}
/* 1 when h was new (and is now known) */
static int vkp_mark(uint64_t h)
{
    unsigned i, at = (unsigned)(h ^ (h >> 31)) & 8191u;
    if (!h) h = 1;
    for (i = 0; i < 8192; i++, at = (at + 1) & 8191u) {
        if (s_vkp_known[at] == h) return 0;
        if (!s_vkp_known[at]) { s_vkp_known[at] = h; return 1; }
    }
    return 0;
}
static void vkp_set_id(GLuint id, uint64_t h)
{
    if (id >= s_vkp_nidh) {
        uint32_t n = s_vkp_nidh ? s_vkp_nidh : 256;
        uint64_t *nh;
        while (n <= id) n *= 2;
        nh = (uint64_t *)realloc(s_vkp_idh, n * sizeof *nh);
        if (!nh) return;
        memset(nh + s_vkp_nidh, 0, (n - s_vkp_nidh) * sizeof *nh);
        s_vkp_idh = nh;
        s_vkp_nidh = n;
    }
    s_vkp_idh[id] = h;
}
static uint64_t vkp_id_hash(GLuint id) { return id < s_vkp_nidh ? s_vkp_idh[id] : 0; }

static const char *vkp_path(void)
{
#if defined(_WIN32)
    return "shader_cache\\vk_pipes.bin";
#else
    return "shader_cache/vk_pipes.bin";
#endif
}
static FILE *vkp_writer(void)
{
    if (!s_vkp_out && vkp_on()) {
        s_vkp_out = fopen(vkp_path(), "ab");
        if (s_vkp_out && ftell(s_vkp_out) == 0) {
            uint32_t ks = (uint32_t)sizeof(PKey);
            fwrite("JSRFVKP1", 1, 8, s_vkp_out);
            fwrite(&ks, 4, 1, s_vkp_out);
        }
        if (!s_vkp_out) s_vkp_off = 1;   /* no shader_cache folder: nothing kept */
    }
    return s_vkp_out;
}
static void vkp_write_prog(uint64_t h, const char *v, const char *f)
{
    FILE *o;
    uint32_t vl = (uint32_t)strlen(v), fl = (uint32_t)strlen(f);
    if (!vkp_mark(h) || !(o = vkp_writer())) return;
    fputc('P', o);
    fwrite(&h, 8, 1, o); fwrite(&vl, 4, 1, o); fwrite(&fl, 4, 1, o);
    fwrite(v, 1, vl, o); fwrite(f, 1, fl, o);
    fflush(o);
}
static void vkp_write_key(uint64_t h, uint64_t kh, const PKey *k)
{
    FILE *o;
    if (!h || !vkp_mark(h ^ (kh * 0x9E3779B97F4A7C15ull)) || !(o = vkp_writer())) return;
    fputc('K', o);
    fwrite(&h, 8, 1, o);
    fwrite(k, sizeof *k, 1, o);
    fflush(o);
}

static void vkp_pipes_add(GProg *g, uint64_t kh, VkPipeline p)
{
    if (g->npipes == g->cpipes) {
        int nc = g->cpipes ? g->cpipes * 2 : 8;
        PipeSlot *np = (PipeSlot *)realloc(g->pipes, (size_t)nc * sizeof *np);
        if (!np) return;
        g->pipes = np;
        g->cpipes = nc;
    }
    g->pipes[g->npipes].key = kh;
    g->pipes[g->npipes].p = p;
    g->npipes++;
}

static void vkp_key_note(GLuint prog, const PKey *k)
{
    if (s_vkp_npk == s_vkp_cpk) {
        int nc = s_vkp_cpk ? s_vkp_cpk * 2 : 256;
        VkpKey *n = (VkpKey *)realloc(s_vkp_pk, (size_t)nc * sizeof *n);
        if (!n) return;
        s_vkp_pk = n;
        s_vkp_cpk = nc;
    }
    s_vkp_pk[s_vkp_npk].prog = prog;
    s_vkp_pk[s_vkp_npk].k = *k;
    s_vkp_npk++;
}

/* a job for the workers (the lock held) */
static int vkp_push_locked(VkpJob *j)
{
    if (s_vkp_njob == s_vkp_cjob) {
        int nc = s_vkp_cjob ? s_vkp_cjob * 2 : 256;
        VkpJob **n = (VkpJob **)realloc(s_vkp_job, (size_t)nc * sizeof *n);
        if (!n) return 0;
        s_vkp_job = n;
        s_vkp_cjob = nc;
    }
    s_vkp_job[s_vkp_njob++] = j;
    SDL_CondSignal(s_vkp_work);
    return 1;
}
static VkpJob *vkp_pipe_job(GLuint prog, GProg *g, const PKey *k)
{
    VkpJob *j = (VkpJob *)calloc(1, sizeof *j);
    if (!j) return NULL;
    j->kind = 0;
    j->prog = prog;
    j->k = *k;
    j->kh = pkey_hash(k);
    j->vm = g->vm; j->fm = g->fm; j->pl = g->pl;
    j->inputs = g->inputs;
    j->dclamp = S.dclamp;
    return j;
}

static void vkp_do(VkpJob *j)
{
    if (j->kind == 0) {
        j->p = pipe_make_x(j->vm, j->fm, j->pl, j->inputs, &j->k, j->dclamp);
    } else {
        uint32_t *vw = NULL, *fw = NULL;
        size_t vn = 0, fn = 0;
        int cached = 0;
        char *err = NULL;
        j->ok = j->cvs && j->cfs && nv2a_vk_spirv(j->cvs, j->cfs, &vw, &vn, &fw, &fn, &cached, &err);
        free(vw); free(fw); free(err);
    }
}

static int SDLCALL vkp_worker(void *arg)
{
    (void)arg;
    SDL_LockMutex(s_vkp_mu);
    for (;;) {
        VkpJob *j;
        while (s_vkp_next < s_vkp_njob && s_vkp_job[s_vkp_next]->state != 0)
            s_vkp_next++;
        if (s_vkp_next >= s_vkp_njob) {
            SDL_CondWait(s_vkp_work, s_vkp_mu);
            continue;
        }
        j = s_vkp_job[s_vkp_next++];
        j->state = 1;
        SDL_UnlockMutex(s_vkp_mu);
        vkp_do(j);
        SDL_LockMutex(s_vkp_mu);
        j->state = 2;
        if (j->ahead && ++s_vkp_made == s_vkp_nahead) s_vkp_t1 = TM_NOW();
        SDL_CondBroadcast(s_vkp_cv);
    }
    return 0;                          /* not reached: the workers live as long as the game */
}

/* the job, done: here if no worker has begun it, else waited for (the lock held) */
static void vkp_finish_locked(VkpJob *j)
{
    if (j->state == 0) {
        j->state = 1;
        SDL_UnlockMutex(s_vkp_mu);
        vkp_do(j);
        SDL_LockMutex(s_vkp_mu);
        j->state = 2;
        if (j->ahead && ++s_vkp_made == s_vkp_nahead) s_vkp_t1 = TM_NOW();
        SDL_CondBroadcast(s_vkp_cv);
    }
    while (j->state == 1)
        SDL_CondWait(s_vkp_cv, s_vkp_mu);
}

/* the pipeline for program `id` (g) and key k: already made, made ahead, or now */
static VkPipeline vkp_get(GProg *g, GLuint id, const PKey *k)
{
    uint64_t kh = pkey_hash(k);
    VkPipeline p;
    int i;
    for (i = 0; i < g->npipes; i++)
        if (g->pipes[i].key == kh)
            return g->pipes[i].p;
    if (s_vkp_njob) {
        VkpJob *j = NULL;
        SDL_LockMutex(s_vkp_mu);
        for (i = 0; i < s_vkp_njob; i++) {
            VkpJob *c = s_vkp_job[i];
            if (c->kind == 0 && c->prog == id && c->kh == kh && c->state != 3
                    && !memcmp(&c->k, k, sizeof *k)) {
                j = c;
                break;
            }
        }
        if (j) {
            if (j->state == 1) s_vkp_waited++;
            vkp_finish_locked(j);
            p = j->p;
            j->state = 3;
            s_vkp_taken++;
            SDL_UnlockMutex(s_vkp_mu);
            if (p) {
                vkp_pipes_add(g, kh, p);
                vkp_key_note(id, k);
                s_n_pipes++;
                return p;
            }
        } else
            SDL_UnlockMutex(s_vkp_mu);
    }
    p = pipe_make(g, k);
    if (!p)
        return VK_NULL_HANDLE;
    vkp_pipes_add(g, kh, p);
    vkp_key_note(id, k);
    vkp_write_key(vkp_id_hash(id), kh, k);
    return p;
}

/* ── programs built in the background (v311g-vk10) ────────────────────────
 * The first time a shader is needed it was compiled there and then (glslang,
 * then the driver for its pipelines): tens of milliseconds the game waited.
 * Where the renderer has something to draw with meanwhile (the general
 * fragment shader for a specialised one, the CPU for a vertex program), it
 * asks for the program in the background: a worker makes its SPIR-V, the
 * main thread links it (from the SPIR-V cache, a fraction of a millisecond),
 * the workers make the pipelines the stand-in has been drawn with, and only
 * then is it ready. Nothing waits. RECOMP_VK_ASYNC=0: built at once. */
#define VKA_HANDLE 0x40000000u
typedef struct {
    uint32_t handle;
    int state;                         /* 0 compiling, 1 pipelines, -1 failed */
    int cancel;
    GLuint id, like;
    uint64_t h;
    VkpJob *spv;
    VkpJob **pj;
    int npj;
} VkpAsync;
static VkpAsync **s_vka;
static int s_nvka, s_cvka;
static uint32_t s_vka_count;
static unsigned s_vka_started, s_vka_ready, s_vka_failed, s_vka_pipes, s_vka_hit, s_vkp_guessed;
static int s_vka_off = -1;

static int vka_on(void)
{
    if (s_vka_off < 0) {
        const char *e = getenv("RECOMP_VK_ASYNC");
        s_vka_off = e && e[0] == '0';
    }
    return !s_vka_off && s_vkp_threads > 0;
}
static int vka_find(uint32_t handle)
{
    int i;
    for (i = 0; i < s_nvka; i++)
        if (s_vka[i]->handle == handle) return i;
    return -1;
}
static void vka_free(int i)
{
    VkpAsync *a = s_vka[i];
    free(a->pj);
    free(a);
    s_vka[i] = s_vka[--s_nvka];
}
/* a SPIR-V job no longer wanted: dropped if not begun (the lock held);
 * 1 when it can be freed now */
static int vkp_spv_drop_locked(VkpJob *j)
{
    if (j->state == 0) j->state = 3;
    if (j->state == 1) return 0;
    j->state = 3;
    return 1;
}

/* a source as rbg_compile hands it to the core: the renderer's defines
 * after the #version line */
static const char *(*s_rb_defs)(void);
static char *vka_with_defs(const char *src)
{
    const char *nl = strchr(src, '\n');
    const char *defs = s_rb_defs ? s_rb_defs() : NULL;
    size_t a, b, c;
    char *o;
    if (!(defs && nl && !strncmp(src, "#version", 8)))
        return strdup(src);
    a = (size_t)(nl - src) + 1;
    if (a > 63) a = 63;                  /* rbg_compile keeps 63 bytes of it */
    b = strlen(defs);
    c = strlen(nl + 1);
    o = (char *)malloc(a + b + c + 1);
    if (!o) return NULL;
    memcpy(o, src, a);
    memcpy(o + a, defs, b);
    memcpy(o + a + b, nl + 1, c + 1);
    return o;
}

/* ── likely pipelines (v311g-vk10c) ─────────────────────────────────────────
 * A pipeline for a new combination was still made on the draw that first
 * needed it (Miggas's vk10b: 59 us a draw over the first 120 frames). The
 * parts of a key other than the program -- vertex formats, target formats and
 * samples, blending -- repeat across programs, so a new program gets, in the
 * background, the keys other programs with the same vertex inputs have been
 * drawn with most (up to 16). A program with a stand-in also gets its
 * stand-in's keys, and it waits for those only. */
#define VKP_GUESS 16
static uint32_t vkp_key_mask(const PKey *k)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < NATTR; i++)
        if (k->vf[i] != VK_FORMAT_UNDEFINED) m |= 1u << i;
    return m;
}
/* a job for program id (g) and key k, unless it has the pipeline or a job
 * for it already; added to *list when list is given */
static void vkp_queue(GLuint id, GProg *g, const PKey *k, VkpJob ***list, int *n)
{
    uint64_t kh = pkey_hash(k);
    VkpJob *pj;
    int i;
    for (i = 0; i < g->npipes; i++)
        if (g->pipes[i].key == kh) return;
    SDL_LockMutex(s_vkp_mu);
    for (i = 0; i < s_vkp_njob; i++) {
        VkpJob *c = s_vkp_job[i];
        if (c->kind == 0 && c->prog == id && c->kh == kh && c->state != 3) {
            SDL_UnlockMutex(s_vkp_mu);
            return;
        }
    }
    SDL_UnlockMutex(s_vkp_mu);
    pj = vkp_pipe_job(id, g, k);
    if (!pj) return;
    if (list) {
        VkpJob **nl = (VkpJob **)realloc(*list, (size_t)(*n + 1) * sizeof *nl);
        if (!nl) { free(pj); return; }
        *list = nl;
    }
    SDL_LockMutex(s_vkp_mu);
    if (vkp_push_locked(pj)) {
        if (list) (*list)[(*n)++] = pj;
    } else
        free(pj);
    SDL_UnlockMutex(s_vkp_mu);
}
static void vkp_queue_like(GLuint id, GProg *g, GLuint like, VkpJob ***list, int *n)
{
    int k;
    for (k = 0; k < s_vkp_npk; k++)
        if (s_vkp_pk[k].prog == like)
            vkp_queue(id, g, &s_vkp_pk[k].k, list, n);
}
static void vkp_queue_guess(GLuint id, GProg *g, VkpJob ***list, int *n)
{
    uint64_t kh[VKP_GUESS];
    int cnt[VKP_GUESS], at[VKP_GUESS], nc = 0, k, i;
    uint32_t m = g->inputs & ((1u << NATTR) - 1);
    if (!s_vkp_threads) return;
    for (k = 0; k < s_vkp_npk; k++) {
        uint64_t h;
        if (vkp_key_mask(&s_vkp_pk[k].k) != m) continue;
        h = pkey_hash(&s_vkp_pk[k].k);
        for (i = 0; i < nc && kh[i] != h; i++) {}
        if (i < nc) { cnt[i]++; continue; }
        if (nc < VKP_GUESS) { kh[nc] = h; cnt[nc] = 1; at[nc] = k; nc++; continue; }
        /* full: replace the least used, if it is used once (keeps the common ones) */
        {
            int lo = 0;
            for (i = 1; i < nc; i++) if (cnt[i] < cnt[lo]) lo = i;
            if (cnt[lo] <= 1) { kh[lo] = h; cnt[lo] = 1; at[lo] = k; }
        }
    }
    for (i = 0; i < nc; i++)
        vkp_queue(id, g, &s_vkp_pk[at[i]].k, list, n);
    s_vkp_guessed += (unsigned)nc;
}

uint32_t rb_program_build_async(const char *vsrc, const char *fsrc, uint32_t like)
{
    uint64_t h;
    VkpAsync *a;
    VkpJob *j;
    int i;
    if (!vka_on())
        return 0;
    h = vkp_src_hash(vsrc, fsrc);
    for (i = 0; i < s_vkp_nready; i++)
        if (s_vkp_ready[i].h == h && !s_vkp_ready[i].handed) {
            s_vkp_ready[i].handed = 1;       /* made at start-up: ready now */
            return s_vkp_ready[i].id;
        }
    a = (VkpAsync *)calloc(1, sizeof *a);
    j = (VkpJob *)calloc(1, sizeof *j);
    if (!a || !j || s_nvka == (1 << 20)) { free(a); free(j); return 0; }
    j->kind = 1;
    j->vs = strdup(vsrc);
    j->fs = strdup(fsrc);
    {
        char *d = vka_with_defs(vsrc);
        j->cvs = d ? patch_vs(d) : NULL;      /* as gv_LinkProgram compiles it */
        free(d);
        j->cfs = vka_with_defs(fsrc);
    }
    if (!j->vs || !j->fs || !j->cvs || !j->cfs) {
        free(j->vs); free(j->fs); free(j->cvs); free(j->cfs); free(j); free(a);
        return 0;
    }
    if (s_nvka == s_cvka) {
        int nc = s_cvka ? s_cvka * 2 : 32;
        VkpAsync **n = (VkpAsync **)realloc(s_vka, (size_t)nc * sizeof *n);
        if (!n) { free(j->vs); free(j->fs); free(j->cvs); free(j->cfs); free(j); free(a); return 0; }
        s_vka = n;
        s_cvka = nc;
    }
    a->handle = VKA_HANDLE | (++s_vka_count & (VKA_HANDLE - 1));
    a->like = like;
    a->h = h;
    a->spv = j;
    SDL_LockMutex(s_vkp_mu);
    if (!vkp_push_locked(j)) {
        SDL_UnlockMutex(s_vkp_mu);
        free(j->vs); free(j->fs); free(j->cvs); free(j->cfs); free(j); free(a);
        return 0;
    }
    SDL_UnlockMutex(s_vkp_mu);
    s_vka[s_nvka++] = a;
    s_vka_started++;
    return a->handle;
}

int rb_program_ready(uint32_t *id)
{
    int i, k, done;
    VkpAsync *a;
    if (!(*id & VKA_HANDLE))
        return 1;
    i = vka_find(*id);
    if (i < 0)
        return -1;
    a = s_vka[i];
    if (a->state == 0) {
        VkpJob *j = a->spv;
        GProg *g;
        SDL_LockMutex(s_vkp_mu);
        done = j->state == 2;
        SDL_UnlockMutex(s_vkp_mu);
        if (!done)
            return 0;
        /* linked here, from the SPIR-V the worker left in the cache */
        {
            uint64_t c0 = s_n_links_cached;
            a->id = j->ok ? vkp_link(j->vs, j->fs) : 0;
            if (s_n_links_cached != c0) s_vka_hit++;   /* the worker's SPIR-V, as meant */
        }
        if (a->id) {
            vkp_set_id(a->id, a->h);
            if (vkp_on()) vkp_write_prog(a->h, j->vs, j->fs);
        }
        SDL_LockMutex(s_vkp_mu);
        free(j->vs); free(j->fs); free(j->cvs); free(j->cfs);   /* taken (the job stays listed) */
        j->vs = j->fs = j->cvs = j->cfs = NULL;
        j->state = 3;
        SDL_UnlockMutex(s_vkp_mu);
        a->spv = NULL;
        g = a->id ? prog_of(a->id) : NULL;
        if (!g || !g->linked) {
            if (a->id) rb_program_delete(a->id);
            s_vka_failed++;
            vka_free(i);
            return -1;
        }
        /* the pipelines the stand-in was drawn with; with no stand-in (a
         * vertex program: the CPU meanwhile), the likeliest for its inputs */
        if (a->like) {
            vkp_queue_like(a->id, g, a->like, &a->pj, &a->npj);
            vkp_queue_guess(a->id, g, NULL, NULL);   /* not waited for */
        } else
            vkp_queue_guess(a->id, g, &a->pj, &a->npj);
        a->state = 1;
    }
    /* state 1: ready when its pipelines are */
    SDL_LockMutex(s_vkp_mu);
    for (k = 0; k < a->npj; k++)
        if (a->pj[k]->state < 2) {
            SDL_UnlockMutex(s_vkp_mu);
            return 0;
        }
    SDL_UnlockMutex(s_vkp_mu);
    {
        GProg *g = prog_of(a->id);
        for (k = 0; k < a->npj; k++) {
            VkpJob *pj = a->pj[k];
            SDL_LockMutex(s_vkp_mu);
            pj->state = 3;                   /* taken (left in the list, as the made-ahead ones) */
            SDL_UnlockMutex(s_vkp_mu);
            if (pj->p && g) {
                int dup = 0, m;
                for (m = 0; m < g->npipes; m++) if (g->pipes[m].key == pj->kh) dup = 1;
                if (dup) { vkDestroyPipeline(G.c.dev, pj->p, NULL); continue; }
                vkp_pipes_add(g, pj->kh, pj->p);
                vkp_key_note(a->id, &pj->k);
                vkp_write_key(a->h, pj->kh, &pj->k);
                s_n_pipes++;
                s_vka_pipes++;
            }
        }
    }
    *id = a->id;
    s_vka_ready++;
    vka_free(i);
    return 1;
}

GLuint rb_program_build(const char *vsrc, const char *fsrc)
{
    uint64_t h = vkp_src_hash(vsrc, fsrc);
    GLuint id;
    int i;
    for (i = 0; i < s_vkp_nready; i++)
        if (s_vkp_ready[i].h == h && !s_vkp_ready[i].handed) {
            s_vkp_ready[i].handed = 1;       /* made at start-up */
            return s_vkp_ready[i].id;
        }
    id = vkp_link(vsrc, fsrc);
    if (id && vkp_on()) {
        vkp_set_id(id, h);
        vkp_write_prog(h, vsrc, fsrc);
    }
    if (id && vka_on()) {                    /* vk10c: the likeliest pipelines, ahead */
        GProg *g = prog_of(id);
        if (g && g->linked)
            vkp_queue_guess(id, g, NULL, NULL);
    }
    return id;
}

void rb_program_delete(uint32_t id)
{
    int i;
    if (id & VKA_HANDLE) {                   /* one still being built: let go of it */
        i = vka_find(id);
        if (i < 0) return;
        if (s_vka[i]->state == 0) {
            VkpJob *j = s_vka[i]->spv;
            int now;
            SDL_LockMutex(s_vkp_mu);
            now = vkp_spv_drop_locked(j);
            SDL_UnlockMutex(s_vkp_mu);
            if (!now) { s_vka[i]->cancel = 1; return; }   /* freed by vka_reap */
            SDL_LockMutex(s_vkp_mu);
            free(j->vs); free(j->fs); free(j->cvs); free(j->cfs);
            j->vs = j->fs = j->cvs = j->cfs = NULL;
            SDL_UnlockMutex(s_vkp_mu);
            vka_free(i);
            return;
        }
        id = s_vka[i]->id;                   /* linked: its pipelines are dropped below */
        vka_free(i);
    }
    if (s_vkp_njob) {
        SDL_LockMutex(s_vkp_mu);
        for (i = 0; i < s_vkp_njob; i++) {
            VkpJob *j = s_vkp_job[i];
            if (j->kind != 0 || j->prog != id || j->state == 3) continue;
            if (j->state == 0) j->state = 3;
            while (j->state == 1)
                SDL_CondWait(s_vkp_cv, s_vkp_mu);
            if (j->state == 2 && j->p)
                vkDestroyPipeline(G.c.dev, j->p, NULL);   /* never used */
            j->p = VK_NULL_HANDLE;
            j->state = 3;
        }
        SDL_UnlockMutex(s_vkp_mu);
    }
    for (i = 0; i < s_vkp_nready; i++)
        if (s_vkp_ready[i].id == id) s_vkp_ready[i].id = 0, s_vkp_ready[i].handed = 1;
    for (i = 0; i < s_vkp_npk; )
        if (s_vkp_pk[i].prog == id) s_vkp_pk[i] = s_vkp_pk[--s_vkp_npk];
        else i++;
    vkp_set_id(id, 0);
    glDeleteProgram(id);
}

/* the ones let go of while a worker had them, freed once it is done */
static void vka_reap(void)
{
    int i;
    for (i = 0; i < s_nvka; ) {
        VkpAsync *a = s_vka[i];
        int done;
        if (!a->cancel) { i++; continue; }
        SDL_LockMutex(s_vkp_mu);
        done = a->spv->state != 1;
        SDL_UnlockMutex(s_vkp_mu);
        if (!done) { i++; continue; }
        SDL_LockMutex(s_vkp_mu);
        free(a->spv->vs); free(a->spv->fs); free(a->spv->cvs); free(a->spv->cfs);
        a->spv->vs = a->spv->fs = a->spv->cvs = a->spv->cfs = NULL;
        a->spv->state = 3;
        SDL_UnlockMutex(s_vkp_mu);
        vka_free(i);
    }
}

/* what the last runs wrote down: read at rb_init, before any program is
 * built (so nothing is written twice) */
typedef struct { uint64_t h; char *v, *fr; GLuint id; } VkpProg;
static VkpProg *s_vkp_rp;
static int s_vkp_nrp;
static PKey *s_vkp_rk;
static uint64_t *s_vkp_rkh;
static int s_vkp_nrk;

static void vkp_read(void)
{
    FILE *f;
    char magic[8];
    uint32_t ks = 0;
    int cp = 0, ck = 0;
    if (!vkp_on())
        return;
    f = fopen(vkp_path(), "rb");
    if (!f)
        return;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "JSRFVKP1", 8) || fread(&ks, 4, 1, f) != 1
            || ks != (uint32_t)sizeof(PKey)) {
        fclose(f);
        f = fopen(vkp_path(), "wb");     /* another build's: started again */
        if (f) fclose(f);
        return;
    }
    for (;;) {
        int c = fgetc(f);
        uint64_t h;
        if (c == EOF || fread(&h, 8, 1, f) != 1) break;
        if (c == 'P') {
            uint32_t vl, fl;
            char *v, *fr;
            if (fread(&vl, 4, 1, f) != 1 || fread(&fl, 4, 1, f) != 1 || vl > (1u << 20) || fl > (1u << 20))
                break;
            v = (char *)malloc(vl + 1); fr = (char *)malloc(fl + 1);
            if (!v || !fr || fread(v, 1, vl, f) != vl || fread(fr, 1, fl, f) != fl) { free(v); free(fr); break; }
            v[vl] = 0; fr[fl] = 0;
            if (!vkp_mark(h)) { free(v); free(fr); continue; }
            if (s_vkp_nrp == cp) {
                VkpProg *n;
                cp = cp ? cp * 2 : 64;
                n = (VkpProg *)realloc(s_vkp_rp, (size_t)cp * sizeof *n);
                if (!n) { free(v); free(fr); break; }
                s_vkp_rp = n;
            }
            s_vkp_rp[s_vkp_nrp].h = h; s_vkp_rp[s_vkp_nrp].v = v;
            s_vkp_rp[s_vkp_nrp].fr = fr; s_vkp_rp[s_vkp_nrp].id = 0;
            s_vkp_nrp++;
        } else if (c == 'K') {
            PKey k;
            if (fread(&k, sizeof k, 1, f) != 1) break;
            if (!vkp_mark(h ^ (pkey_hash(&k) * 0x9E3779B97F4A7C15ull))) continue;
            if (s_vkp_nrk == ck) {
                PKey *nk; uint64_t *nh;
                ck = ck ? ck * 2 : 128;
                nk = (PKey *)realloc(s_vkp_rk, (size_t)ck * sizeof *nk);
                if (nk) s_vkp_rk = nk;
                nh = (uint64_t *)realloc(s_vkp_rkh, (size_t)ck * sizeof *nh);
                if (nh) s_vkp_rkh = nh;
                if (!nk || !nh) break;
            }
            s_vkp_rk[s_vkp_nrk] = k; s_vkp_rkh[s_vkp_nrk] = h; s_vkp_nrk++;
        } else
            break;
    }
    fclose(f);
}

/* at start-up (rb_init_state, once depth clamp is set): the programs made
 * (or found among those the renderer made already) and their pipelines
 * given to the workers */
static void vkp_start(void)
{
    int i, a, nthreads;
    s_vkp_t0 = TM_NOW();
    if (s_vkp_nrk)
        s_vkp_ready = (VkpReady *)calloc((size_t)(s_vkp_nrp ? s_vkp_nrp : 1), sizeof *s_vkp_ready);
    for (i = 0; i < s_vkp_nrk && s_vkp_ready; i++) {
        GProg *g;
        VkpJob *j;
        for (a = 0; a < s_vkp_nrp && s_vkp_rp[a].h != s_vkp_rkh[i]; a++) {}
        if (a == s_vkp_nrp) continue;          /* its program was not written down */
        if (!s_vkp_rp[a].id) {
            uint32_t id;
            for (id = 1; id < s_vkp_nidh && s_vkp_idh[id] != s_vkp_rp[a].h; id++) {}
            if (id < s_vkp_nidh)
                s_vkp_rp[a].id = id;           /* the renderer has it already */
            else {
                s_vkp_rp[a].id = vkp_link(s_vkp_rp[a].v, s_vkp_rp[a].fr);
                if (!s_vkp_rp[a].id) { s_vkp_rp[a].h = 0; continue; }
                vkp_set_id(s_vkp_rp[a].id, s_vkp_rp[a].h);
                s_vkp_ready[s_vkp_nready].h = s_vkp_rp[a].h;
                s_vkp_ready[s_vkp_nready].id = s_vkp_rp[a].id;
                s_vkp_nready++;
            }
        }
        g = prog_of(s_vkp_rp[a].id);
        if (!g || !g->linked) continue;
        j = vkp_pipe_job(s_vkp_rp[a].id, g, &s_vkp_rk[i]);
        if (!j) continue;
        j->ahead = 1;
        if (s_vkp_njob == s_vkp_cjob) {
            int nc = s_vkp_cjob ? s_vkp_cjob * 2 : 256;
            VkpJob **n = (VkpJob **)realloc(s_vkp_job, (size_t)nc * sizeof *n);
            if (!n) { free(j); continue; }
            s_vkp_job = n;
            s_vkp_cjob = nc;
        }
        s_vkp_job[s_vkp_njob++] = j;
        s_vkp_nahead++;
    }
    for (i = 0; i < s_vkp_nrp; i++) { free(s_vkp_rp[i].v); free(s_vkp_rp[i].fr); }
    free(s_vkp_rp); free(s_vkp_rk); free(s_vkp_rkh);
    s_vkp_rp = NULL; s_vkp_rk = NULL; s_vkp_rkh = NULL;
    {
        const char *e = getenv("RECOMP_VK_ASYNC");
        if (!s_vkp_njob && e && e[0] == '0')
            return;                            /* nothing for workers to do */
    }
    nv2a_vk_spirv_init();                      /* before any worker compiles */
    s_vkp_mu = SDL_CreateMutex();
    s_vkp_cv = SDL_CreateCond();
    s_vkp_work = SDL_CreateCond();
    if (!s_vkp_mu || !s_vkp_cv || !s_vkp_work) {
        s_vkp_njob = s_vkp_nahead = 0;
        return;
    }
    nthreads = SDL_GetCPUCount() - 2;
    if (nthreads > 4) nthreads = 4;
    if (nthreads < 1) nthreads = 1;
    for (i = 0; i < nthreads; i++) {
        SDL_Thread *t = SDL_CreateThread(vkp_worker, "vk builder", NULL);
        if (t) { SDL_DetachThread(t); s_vkp_threads++; }
    }
    if (s_vkp_nahead)
        fprintf(stderr, "[VK] pipelines ahead: %d programs made (%.0f ms), %d pipelines being made"
                " on %d threads (RECOMP_VK_PIPES_AHEAD=0 for none)\n", s_vkp_nready,
                (double)(TM_NOW() - s_vkp_t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(),
                s_vkp_nahead, s_vkp_threads);
    if (vka_on())
        fprintf(stderr, "[VK] new shaders are built in the background on %d threads, drawn with"
                " the general ones meanwhile (RECOMP_VK_ASYNC=0: built at once)\n", s_vkp_threads);
}

/* once they are all made: one line */
static void vkp_report(void)
{
    vka_reap();
    if (s_vkp_logged || !s_vkp_nahead) return;
    SDL_LockMutex(s_vkp_mu);
    if (s_vkp_made == s_vkp_nahead) {
        s_vkp_logged = 1;
        fprintf(stderr, "[VK] pipelines ahead: all %d made in %.0f ms; %d used so far, %d draws"
                " waited for one\n", s_vkp_nahead,
                (double)(s_vkp_t1 - s_vkp_t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(),
                s_vkp_taken, s_vkp_waited);
    }
    SDL_UnlockMutex(s_vkp_mu);
}

static void API vkd_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    uint64_t tp_;
    if (mode != GL_TRIANGLES || count <= 0 || !vkd_setup()) return;
    tp_ = s_vkd_prof ? TM_NOW() : 0;
    vkCmdDraw(G.b[G.cur].cmd, (uint32_t)count, 1, (uint32_t)first, 0);
    VKD_T(7);
}

static void API vkd_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                           const void *off, GLint base)
{
    GVao *v = GET(t_vao, S.vao);
    GBuf *e = v ? buf_of(v->ebo) : NULL;
    VkIndexType it = type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (mode != GL_TRIANGLES || count <= 0 || !e || !e->b) return;
    uint64_t tp_;
    if (!vkd_setup()) return;
    tp_ = s_vkd_prof ? TM_NOW() : 0;
    if (D.ib != e->b || D.it != it) {
        vkCmdBindIndexBuffer(G.b[G.cur].cmd, e->b, 0, it);
        D.ib = e->b; D.it = it;
    }
    vkCmdDrawIndexed(G.b[G.cur].cmd, (uint32_t)count, 1,
                     (uint32_t)((uintptr_t)off / (type == GL_UNSIGNED_SHORT ? 2 : 4)), base, 0);
    VKD_T(7);
}


/* persistent rings (v311g-vk8d): the core's buffers are host memory mapped
 * for good, so immutable storage is a buffer like any other and the renderer
 * writes its vertices and indices straight into it -- no copy through
 * glBufferSubData, and no new buffer each time a ring comes round (each one
 * a vkAllocateMemory: 2.4 ms on Windows, Miggas's vk8c profile) */
static void vkx_BufferStorage(GLenum t, GLsizeiptr n, const void *d, GLbitfield fl)
{
    (void)fl;
    gv_BufferData(t, n, d, GL_STREAM_DRAW);
}

/* not in the core: the paths that use them are off here (rb_caps) */
static void gvx_BindVertexBuffers(GLuint f, GLsizei n, const GLuint *b, const GLintptr *o, const GLsizei *st) { (void)f; (void)n; (void)b; (void)o; (void)st; }
static void gvx_GetProgramBinary(GLuint p, GLsizei n, GLsizei *len, GLenum *fmt, void *b) { (void)p; (void)n; (void)fmt; (void)b; if (len) *len = 0; }
static void gvx_ProgramBinary(GLuint p, GLenum fmt, const void *b, GLsizei n) { (void)p; (void)fmt; (void)b; (void)n; }
static void gvx_VertexAttribBinding(GLuint a, GLuint b) { (void)a; (void)b; }
static void gvx_VertexAttribFormat(GLuint a, GLint n, GLenum t, GLboolean no, GLuint r) { (void)a; (void)n; (void)t; (void)no; (void)r; }

/* no GL thread here: everything runs on the caller's thread */
#define gt_call(fn, arg, n) ((void)(n), fn((void *)(arg)))
#define gt_call_sync(fn, arg) fn(arg)
static const int s_gt_on = 0;

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

/* ── programs (moved from the renderer: compile, the program-binary cache, link) ── */

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

/* the program itself (rb_program_build, below, may have it ready) */
static GLuint vkp_link(const char *vsrc, const char *fsrc)
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
/* rb_program_delete: below, with the pipelines built ahead */
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

/* ── bindings: what GL has bound, cached (moved from the renderer) ──────────── */

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
/* ── anti-aliasing and the MSAA upscale (moved whole from the renderer) ─────── */

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

static GLuint compile(GLenum type, const char *src) { return rb_shader_compile(type == GL_VERTEX_SHADER, src); }
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
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
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


/* ── the window: presenting (moved from the renderer) ──────────────────────────
 * WGL (the game window's DC), DXGI flip model (RECOMP_GL_PRESENT=dxgi) and
 * SDL (Linux); on the GL thread when that is on; the GPU timing of frames. */

static void (*s_rb_overlay)(int cw, int ch, int flip);   /* our menus, over the frame */
static SDL_Window *s_rb_win;                               /* Linux: the GL context's window */
void xbox_PresentFit(int cw, int ch, int *dw, int *dh);    /* fb_present.c */
int xbox_PresentEdgeBars(void);
void rb_set_overlay(void (*fn)(int cw, int ch, int flip)) { s_rb_overlay = fn; }

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


extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowGLOwned(void);
extern void xbox_FramebufferWindowAttach(void *sdl_window);   /* fb_present.c */
extern void xbox_FramebufferWindowPump(void);

/* the frame into the window: the default framebuffer (sized to the window)
 * through the core's presenter. Windows: the game's own window (fb_present.c's
 * HWND, which stops drawing with GDI); Linux: the SDL Vulkan window rb_init made. */
static int s_vkw_state;
static uint64_t s_win_frames;
int rb_window_present(const RbFrame *f)
{
    int cw = 0, ch = 0, dw, dh, dx, dy;
    void *win;
    GLuint fx;
#if defined(_WIN32)
    RECT cr;
    win = xbox_FramebufferWindowHwnd();
    if (!win || !GetClientRect((HWND)win, &cr))
        return 0;
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    if (!s_vkw_state) {
        s_vkw_state = 1;
        xbox_FramebufferWindowGLOwned();
        fprintf(stderr, "[VK] frames drawn into the game window\n");
    }
#else
    win = s_rb_win;
    if (!win)
        return 0;
    if (!s_vkw_state) {
        s_vkw_state = 1;
        xbox_FramebufferWindowAttach(s_rb_win);
        xbox_FramebufferWindowGLOwned();
        fprintf(stderr, "[VK] frames drawn into the game window (SDL)\n");
    }
    xbox_FramebufferWindowPump();
    SDL_Vulkan_GetDrawableSize(s_rb_win, &cw, &ch);
#endif
    if (cw <= 0 || ch <= 0)
        return 1;                           /* minimised: nothing to show */
    xbox_PresentFit(cw, ch, &dw, &dh);
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;
    rbvk_core_default_size(cw, ch);
    fx = rb_present_aa(f->tex, f->resolve ? f->resolve : f->fbo, f->w, f->h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fx ? fx : f->resolve ? f->resolve : f->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, cw, ch);
    rbg_bar_color(f->w, f->h);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, f->w, f->h, dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rbg_row_fill(f->rowk, f->w, f->h, dx, dy, dw, dy + dh);
    if (s_rb_overlay) s_rb_overlay(cw, ch, 0);
    rbvk_core_swap(win);
    s_win_frames++;
    vkp_report();
    {
        static uint64_t frames;
        if ((++frames % 600 == 0 || frames == 120) && s_vkd_draws) {
            fprintf(stderr, "[VK] draws %llu: pipeline looked up %.1f%%, vertex buffers bound %.1f%%,"
                    " %.2f dynamic states set a draw (of 16), descriptors pushed %.1f%%\n",
                    (unsigned long long)s_vkd_draws, 100.0 * (double)s_vkd_pipe_hash / (double)s_vkd_draws,
                    100.0 * (double)s_vkd_vb / (double)s_vkd_draws, (double)s_vkd_dyn / (double)s_vkd_draws,
                    100.0 * (double)s_vkd_push / (double)s_vkd_draws);
            if (s_vkd_prof > 0) {
                double us = 1e6 / (double)SDL_GetPerformanceFrequency() / (double)s_vkd_draws;
                fprintf(stderr, "[VK]   a draw's time (us): begin %.2f, inputs %.2f, pipeline %.2f,"
                        " vertex buffers %.2f, dynamic %.2f, uniforms %.2f (%.0f bytes), descriptors %.2f,"
                        " draw %.2f\n", s_vkd_t[0] * us, s_vkd_t[1] * us, s_vkd_t[2] * us, s_vkd_t[3] * us,
                        s_vkd_t[4] * us, s_vkd_t[5] * us, (double)s_vkd_ubytes / (double)s_vkd_draws,
                        s_vkd_t[6] * us, s_vkd_t[7] * us);
                memset(s_vkd_t, 0, sizeof s_vkd_t);
                s_vkd_ubytes = 0;
            }
            if (s_vka_started || s_vkp_guessed)
                fprintf(stderr, "[VK]   built in the background so far: %u shaders asked for, %u ready"
                        " (%u linked from the workers' SPIR-V, %u pipelines with them), %u failed,"
                        " %d being built; %u likely pipelines asked for ahead\n", s_vka_started,
                        s_vka_ready, s_vka_hit, s_vka_pipes, s_vka_failed, s_nvka, s_vkp_guessed);
            s_vkd_draws = s_vkd_pipe_hash = s_vkd_vb = s_vkd_dyn = s_vkd_push = 0;
        }
    }
    glEnable(GL_SCISSOR_TEST);
    rb_bindings_lost();
    rb_fixed_lost();
    return 1;
}

int rb_gpu_frame_stats(char *buf, size_t n) { (void)buf; (void)n; return 0; }   /* no GPU timing yet */
void rb_window_stats(uint64_t *frames, double *behind_avg, uint64_t *behind_max,
                     uint64_t *behind2, double *iv_max_ms, uint64_t *iv20, uint64_t *iv34)
{
    *frames = s_win_frames; s_win_frames = 0;
    *behind_avg = 0.0; *behind_max = *behind2 = 0;
    *iv_max_ms = 0.0; *iv20 = *iv34 = 0;
}

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

/* ── vertex data, fences and draws (OpenGL's meaning; the Vulkan backend
 *    keeps vertex layouts itself) ───────────────────────────────────────── */

void rb_buf_bind(uint32_t target, uint32_t b) { glBindBuffer(target, b); }
void rb_buf_data(uint32_t target, intptr_t size, const void *data, uint32_t usage) { glBufferData(target, (GLsizeiptr)size, data, usage); }
void rb_buf_sub(uint32_t target, intptr_t off, intptr_t size, const void *data) { glBufferSubData(target, (GLintptr)off, (GLsizeiptr)size, data); }
void rb_buf_storage(uint32_t target, intptr_t size, const void *data, uint32_t flags) { glBufferStorage(target, (GLsizeiptr)size, data, flags); }
void *rb_buf_map(uint32_t target, intptr_t off, intptr_t len, uint32_t access) { return glMapBufferRange(target, (GLintptr)off, (GLsizeiptr)len, access); }
int rb_buf_unmap(uint32_t target) { return glUnmapBuffer(target); }
void rb_buf_new(int n, uint32_t *ids) { glGenBuffers(n, ids); }
void rb_buf_delete(int n, const uint32_t *ids) { glDeleteBuffers(n, ids); }
void *rb_fence(uint32_t cond, uint32_t flags) { return (void *)glFenceSync(cond, flags); }
uint32_t rb_fence_wait(void *f, uint32_t flags, uint64_t timeout_ns) { return glClientWaitSync((GLsync)f, flags, timeout_ns); }
void rb_fence_delete(void *f) { glDeleteSync((GLsync)f); }
void rb_finish(void) { glFinish(); }
void rb_va_new(int n, uint32_t *ids) { glGenVertexArrays(n, ids); }
void rb_va_bind(uint32_t va) { glBindVertexArray(va); }
void rb_attr_on(uint32_t loc) { glEnableVertexAttribArray(loc); }
void rb_attr_off(uint32_t loc) { glDisableVertexAttribArray(loc); }
void rb_attr_pointer(uint32_t loc, int size, uint32_t type, int norm, int stride, const void *ptr) { glVertexAttribPointer(loc, size, type, norm ? GL_TRUE : GL_FALSE, stride, ptr); }
void rb_attr_format(uint32_t loc, int size, uint32_t type, int norm, uint32_t rel) { glVertexAttribFormat(loc, size, type, norm ? GL_TRUE : GL_FALSE, rel); }
void rb_attr_binding(uint32_t loc, uint32_t stream) { glVertexAttribBinding(loc, stream); }
void rb_vbufs(uint32_t first, int n, const uint32_t *bufs, const intptr_t *offs, const int *strides) { glBindVertexBuffers(first, n, bufs, (const GLintptr *)offs, (const GLsizei *)strides); }
void rb_attr_const4fv(uint32_t loc, const float *v) { glVertexAttrib4fv(loc, v); }
void rb_attr_const4f(uint32_t loc, float x, float y, float z, float w) { glVertexAttrib4f(loc, x, y, z, w); }
void rb_draw_elements(uint32_t mode, int count, uint32_t type, const void *off, int base) { glDrawElementsBaseVertex(mode, count, type, off, base); }
void rb_draw_arrays(uint32_t mode, int first, int count) { glDrawArrays(mode, first, count); }

/* ── clearing ──────────────────────────────────────────────────────────── */
/* The bound target, within the scissor: colour (every channel; alpha only
 * when `alpha`), depth, stencil, each when asked. Leaves the masks changed
 * (rb_fixed_lost). */
void rb_clear(int color, const float rgba[4], int alpha, int depth, double zval, int stencil, int sval)
{
    GLbitfield bits = 0;
    if (color) {
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, alpha ? GL_TRUE : GL_FALSE);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
        glDepthMask(GL_TRUE);
        glClearDepth(zval);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (stencil) {
        glStencilMask(0xFF);
        glClearStencil((GLint)sval);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits)
        glClear(bits);
    rb_fixed_lost();                 /* masks changed above */
}

/* ── occlusion queries (samples passed) ───────────────────────────────── */
void rb_query_new(int n, uint32_t *ids) { glGenQueries(n, ids); }
void rb_query_begin(uint32_t id) { glBeginQuery(GL_SAMPLES_PASSED, id); }
void rb_query_end(void) { glEndQuery(GL_SAMPLES_PASSED); }
void rb_query_result(uint32_t id, uint32_t *v) { glGetQueryObjectuiv(id, GL_QUERY_RESULT, v); }   /* waits */

/* ── reading pixels ────────────────────────────────────────────────────── */
/* a target's image (w x h, BGRA, rows bottom-up) into a buffer, read later
 * through rb_buf_map (asynchronous) */
void rb_read_pixels_buf(uint32_t fbo, int w, int h, uint32_t buf)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    rb_buf_bind(GL_PIXEL_PACK_BUFFER, buf);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (void *)0);
    rb_buf_bind(GL_PIXEL_PACK_BUFFER, 0);
}
/* one pixel of a target (GL rows), BGRA (debug probes) */
void rb_read_pixel(uint32_t fbo, int x, int y, uint32_t *c)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(x, y, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, c);
}
/* its depth there (0..1): 0 when it can't be read */
int rb_read_depth(uint32_t fbo, int x, int y, float *z)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, z);
    return glGetError() == GL_NO_ERROR;
}


/* ── start-up ─────────────────────────────────────────────────────────── */

static RbCaps s_rb_caps;

int rb_init(void)
{
    void *vwin = NULL;
    const char *sw = getenv("RECOMP_VK_SOFTWARE");
    GLint v;
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[VK] SDL video init failed: %s -- software renderer\n", SDL_GetError());
        return 0;
    }
    /* the window: on Windows the game's own (fb_present.c's HWND), on Linux
     * an SDL Vulkan window made here */
#if !defined(_WIN32)
    s_rb_win = SDL_CreateWindow("Jet Set Radio Future", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, 640, 480,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
    vwin = s_rb_win;
#endif
    if ((sw && sw[0] == '1') || !rbvk_core_init(vwin)) {
        /* the software rasteriser, shown through Vulkan */
        extern int (*xbox_fb_vk_frame)(void *, const uint32_t *, uint32_t, uint32_t,
                                       int, int, int, int, int, int);
#if !defined(_WIN32)
        if (s_rb_win) { SDL_DestroyWindow(s_rb_win); s_rb_win = NULL; }
#endif
        xbox_fb_vk_frame = nv2a_vk_present_frame;
        fprintf(stderr, "[VK] RECOMP_RENDERER=vulkan: %s; the software rasteriser draws"
                " the frames, shown through Vulkan (slow)\n",
                sw && sw[0] == '1' ? "RECOMP_VK_SOFTWARE=1"
                                   : "the Vulkan renderer can't run here (above)");
        return 0;
    }
    fprintf(stderr, "[VK] renderer: the Vulkan backend -- %s (RECOMP_RENDERER=glvk: OpenGL's calls on Vulkan)\n", G.name);
    s_rb_caps.version  = (const char *)gv_GetString(GL_VERSION);
    s_rb_caps.renderer = (const char *)gv_GetString(GL_RENDERER);
    s_rb_caps.vendor   = (const char *)gv_GetString(GL_VENDOR);
    v = 0; gv_GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &v);          s_rb_caps.max_target = v;
    v = 0; gv_GetIntegerv(GL_MAX_SAMPLES, &v);                    s_rb_caps.max_samples = v;
    v = 0; gv_GetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &v);  s_rb_caps.vs_uniform_comps = v;
    s_rb_caps.persistent_buffers = 1;     /* vk8d: the core maps its buffers for good */
    s_rb_caps.dxt = G.bc_ok;
    s_rb_caps.aniso = G.max_aniso > 1.0f;
    s_rb_caps.attrib_binding = 0;
    vkp_read();                        /* vk9: the pipelines the last runs used */
    return 1;
}

const RbCaps *rb_caps(void) { return &s_rb_caps; }

void rb_init_state(int clip_planes)
{
    int ci;
    glEnable(GL_DEPTH_CLAMP);
    for (ci = 0; ci < clip_planes; ci++)
        glEnable(GL_CLIP_DISTANCE0 + ci);
    glEnable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    vkp_start();                       /* vk9: the pipelines the last runs used */
}

void rb_thread_start(void) {}          /* everything on the caller's thread */
void rb_pump(void) { if (s_rb_win) SDL_PumpEvents(); }
void rb_tex_bind_raw(uint32_t target, uint32_t id) { glBindTexture(target, id); }
void rb_errors_drain(void) {}
uint32_t rb_error(void) { return 0; }

/* ── our menus over the frame (moved from nv2a_gl_quit.h) ────────────────
 * One tiny shader draws coloured or font-textured triangles; the font is
 * on unit 15. The program, vertex array and buffer bound before are put
 * back (the renderer keeps them cached), and so is unit 15. */

#define RBG_OV_UNIT 15
static GLuint s_ov_prog, s_ov_vao, s_ov_vbo, s_ov_tex;
static GLint s_ov_l_vp, s_ov_l_tex;

static GLuint ov_shader(GLenum type, const char *src)
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

static int ov_setup(const unsigned char *atlas, int aw, int ah)
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
    GLuint v = ov_shader(GL_VERTEX_SHADER, vs), f = ov_shader(GL_FRAGMENT_SHADER, fs);
    GLint ok = 0;
    if (!v || !f) return 0;
    s_ov_prog = glCreateProgram();
    glAttachShader(s_ov_prog, v);
    glAttachShader(s_ov_prog, f);
    glLinkProgram(s_ov_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_ov_prog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    s_ov_l_vp = glGetUniformLocation(s_ov_prog, "vp");
    s_ov_l_tex = glGetUniformLocation(s_ov_prog, "tex");
    glGenVertexArrays(1, &s_ov_vao);
    glGenBuffers(1, &s_ov_vbo);
    glBindVertexArray(s_ov_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_ov_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(RbOvVert), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RbOvVert), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RbOvVert), (void *)(2 * sizeof(float)));
    if (atlas) {   /* v266: the font into a texture, on unit 15, whatever was there put back */
        GLint unit0 = 0, tex0 = 0, smp0 = 0, align0 = 4;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
        glActiveTexture(GL_TEXTURE0 + RBG_OV_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_SAMPLER_BINDING, &smp0);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align0);
        glBindSampler(RBG_OV_UNIT, 0);
        glGenTextures(1, &s_ov_tex);
        glBindTexture(GL_TEXTURE_2D, s_ov_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align0);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        glBindSampler(RBG_OV_UNIT, (GLuint)smp0);
        glActiveTexture((GLenum)unit0);
    }
    return 1;
}

int rb_overlay_init(const unsigned char *atlas, int w, int h)
{
    GLint p0 = 0, va0 = 0, ab0 = 0;
    int ok;
    glGetIntegerv(GL_CURRENT_PROGRAM, &p0);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va0);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab0);
    ok = ov_setup(atlas, w, h);
    glBindVertexArray((GLuint)va0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab0);
    glUseProgram((GLuint)p0);
    return ok;
}

void rb_overlay_draw(const RbOvVert *v, int n, int cw, int ch, int flip, int font)
{
    GLint p0 = 0, va0 = 0, ab0 = 0;
    GLint unit0 = 0, tex0 = 0, smp0 = 0;
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
    glUseProgram(s_ov_prog);
    glUniform3f(s_ov_l_vp, (float)cw, (float)ch, flip ? -1.0f : 1.0f);
    if (font) {            /* v266: the font on unit 15; what was there put back after */
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit0);
        glActiveTexture(GL_TEXTURE0 + RBG_OV_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_SAMPLER_BINDING, &smp0);
        glBindTexture(GL_TEXTURE_2D, s_ov_tex);
        glBindSampler(RBG_OV_UNIT, 0);
        glUniform1i(s_ov_l_tex, RBG_OV_UNIT);
    }
    glBindVertexArray(s_ov_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_ov_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * sizeof(RbOvVert)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n);
    if (font) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        glBindSampler(RBG_OV_UNIT, (GLuint)smp0);
        glActiveTexture((GLenum)unit0);
    }
    glDisable(GL_BLEND);
    glBindVertexArray((GLuint)va0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab0);
    glUseProgram((GLuint)p0);
}


/* ── the thread and the counters ──────────────────────────────────────── */

int  rb_threaded(void) { return 0; }
void rb_frame_end(void) {}
void rb_gpu_frame_mark(void) {}
int  rb_shader_cache_on(void) { return shader_cache_on(); }
void rb_shader_cache_path(char *b, size_t n, const char *vsrc, const char *fsrc) { cache_path(b, n, vsrc, fsrc); }
int  rb_call_stats(char *buf, size_t n, uint64_t draws, uint64_t layouts_new)
{
    (void)buf; (void)n; (void)draws; (void)layouts_new;
    return 0;
}

/* ── the table (nv2a_rb_ops.h) ─────────────────────────────────────────── */
const RbOps rbvk_ops = RB_OPS_TABLE("Vulkan");
