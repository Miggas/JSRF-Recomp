/* glvk: the OpenGL 3.3 calls nv2a_gl.c makes, answered with Vulkan
 * (vulkan-test, step 5). RECOMP_RENDERER=vulkan loads these into glad's
 * function pointers instead of a GL driver's, so all of nv2a_gl.c -- the
 * surfaces kept in step with guest memory, the texture cache, widescreen,
 * the stand-ins, the presentation -- runs unchanged on Vulkan.
 *
 * Only what nv2a_gl.c uses is there: about 115 functions. The extensions it
 * looks for are answered so that it takes its simplest paths (no buffer
 * storage, no attribute bindings, no program binaries, no GPU timing).
 * Images stay in OpenGL's row order (row 0 at the bottom), so viewports,
 * scissors, blits, read-backs and texture coordinates need no flipping;
 * only the window's final blit turns the picture the right way up, and
 * OpenGL's counter-clockwise front face is Vulkan's clockwise. */
#ifndef GLVK_H
#define GLVK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan up for rendering, the shared device of nv2a_vk.c (win: the HWND on
 * Windows -- may be NULL there -- or the SDL Vulkan window on Linux).
 * 0 when this computer's Vulkan can't do it; the log says why. */
int  glvk_init(void *win);
/* glad's loader: the function called `name`, or NULL */
void *glvk_proc(const char *name);
/* the default framebuffer (0) is this size from now on */
void glvk_default_size(int w, int h);
/* the default framebuffer into the window, and a new frame */
int  glvk_swap(void *win);
/* one line for the log: the device */
const char *glvk_device_name(void);

#ifdef __cplusplus
}
#endif
#endif
