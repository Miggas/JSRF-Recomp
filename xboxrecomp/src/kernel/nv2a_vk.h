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

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when RECOMP_RENDERER=vulkan (or RECOMP_VK_PROBE=1) asks for Vulkan */
int nv2a_vk_wanted(void);
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
