/* Vulkan renderer (vulkan-test branch), beside the OpenGL one (nv2a_gl.c).
 * Chosen with RECOMP_RENDERER=vulkan. Step 1: the probe -- the game still
 * draws through OpenGL while a thread of its own brings Vulkan up, turns
 * OpenGL-style GLSL into SPIR-V (glslang, relaxed Vulkan rules), builds a
 * pipeline, draws a triangle offscreen and checks it, timing each step.
 * Step 2: the shader mirror -- every shader program the OpenGL renderer
 * builds is built on that thread as a Vulkan pipeline too, from the same
 * GLSL, and both are timed side by side ([VK] lines), with a pipeline cache
 * kept on disk (shader_cache/vk_pipelines.bin) as the renderer will. */
#ifndef NV2A_VK_H
#define NV2A_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when RECOMP_RENDERER=vkmirror (or RECOMP_VK_PROBE=1) asks for steps
 * 1-3: OpenGL draws, the probe and the shader mirror run beside it */
int nv2a_vk_wanted(void);
/* step 4: 1 when RECOMP_RENDERER=vulkan, vk or glvk -- no OpenGL; the window
 * is presented through a Vulkan swapchain. Until the draws are ported the
 * software rasteriser draws the frames. */
int nv2a_vk_renderer(void);
/* step 2b: 1 for RECOMP_RENDERER=vulkan (or vk): the Vulkan backend
 * (nv2a_rb_vk.c); RECOMP_RENDERER=glvk is the OpenGL backend on glvk.c,
 * kept as the reference (nv2a_vk_renderer is 1 for both) */
int nv2a_vk_native(void);
/* a frame into the window: px (w x h, 32-bit B,G,R,X bytes, rows top-down)
 * fitted into dw x dh at dx,dy of the cw x ch window, the rest black. win:
 * the HWND (Windows) or the SDL_Window made with SDL_WINDOW_VULKAN (Linux).
 * Brings Vulkan up on the first call. Returns 1 when shown (or nothing to
 * show: minimised), 0 when Vulkan can't present here -- for good. Always
 * called from the same thread. */
int nv2a_vk_present_frame(void *win, const uint32_t *px, uint32_t w, uint32_t h,
                          int cw, int ch, int dx, int dy, int dw, int dh);
/* the probe on a thread of its own; its results go to the log as [VK] */
void nv2a_vk_probe_async(void);
/* the probe on the calling thread: 0 when the triangle came out right */
int nv2a_vk_probe(void);
/* step 2: the program OpenGL just built (vs, fs as given to it, defs the
 * #defines added after their #version line), and how long OpenGL took
 * (gl_cached: loaded from its own shader cache). Copied; queued for the
 * Vulkan thread. */
void nv2a_vk_mirror(const char *vs, const char *fs, const char *defs,
                    double gl_ms, int gl_cached);

#ifdef __cplusplus
}
#endif
#endif
