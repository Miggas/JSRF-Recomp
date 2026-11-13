/* Vulkan renderer (vulkan-test branch), beside the OpenGL one (nv2a_gl.c).
 * Chosen with RECOMP_RENDERER=vulkan. Step 1: the probe -- the game still
 * draws through OpenGL while a thread of its own brings Vulkan up, turns
 * OpenGL-style GLSL into SPIR-V (glslang, relaxed Vulkan rules), builds a
 * pipeline, draws a triangle offscreen and checks it, timing each step. */
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

#ifdef __cplusplus
}
#endif
#endif
