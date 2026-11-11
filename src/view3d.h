/* The 3D view (m5, step b): the picked file's models wearing the picked
 * texture (the painting's copy while painting), drawn in software: the
 * same on Windows and the Steam Deck, nothing asked of the graphics card. */
#ifndef VIEW3D_H
#define VIEW3D_H
#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

struct nk_context;
struct nk_command_buffer;

void v3d_load(const uint8_t *buf, size_t size);   /* the file's models (after opening or saving it) */
void v3d_free(void);
/* the model list and the view in the current Nuklear window, h high:
 * texid the picked texture's, rgba (tw x th) its pixels, ver a number
 * that changes when they do */
void v3d_panel(struct nk_context *ctx, SDL_Renderer *ren, uint32_t texid, const uint8_t *rgba, int tw, int th,
               unsigned ver, float row, float h);

/* m5b: with Wires on, the shown model's triangles over the texture drawn
 * at x, y (w x h on the screen) */
void v3d_draw_uv(struct nk_command_buffer *cv, float x, float y, float w, float h);
void v3d_set_wire(int on);

#endif
