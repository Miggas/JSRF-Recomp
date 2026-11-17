/* vulkan-test: what nv2a_vk.c (the device, the presenter, the SPIR-V cache)
 * shares with glvk.c (the OpenGL calls of nv2a_gl.c, on Vulkan). */
#ifndef NV2A_VK_CORE_H
#define NV2A_VK_CORE_H

#include <volk.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qfam;
    VkPhysicalDeviceProperties props;
} NvVkCore;

/* the shared device (made on the first call): 0 when there is none, or it
 * lacks what the renderer needs */
int nv2a_vk_core(void *win, NvVkCore *c);
/* the renderer's frame into the window: img (w x h, layout GENERAL, rows
 * bottom-up as OpenGL keeps them), drawn by commands already submitted to
 * the same queue; the swapchain is made / rebuilt for w x h */
int nv2a_vk_present_image(void *win, VkImage img, uint32_t w, uint32_t h);
/* GLSL to SPIR-V through the cache in shader_cache/spirv (step 3); both
 * stages linked together. *cached: 1 when it came from the cache. The
 * words are malloc'd. 0 when the GLSL doesn't compile (log in *err, which
 * is malloc'd too, or NULL). */
int nv2a_vk_spirv(const char *vs, const char *fs, uint32_t **v, size_t *vn,
                  uint32_t **f, size_t *fn, int *cached, char **err);

#endif
