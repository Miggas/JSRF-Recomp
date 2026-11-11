/* The texture painter (m4): brush, eraser, colour picker and fill on a
 * full-quality copy of a texture, with undo and redo, zoom and pan. The
 * copy goes back into the game file only on Save, compressed once. */
#ifndef PAINT_H
#define PAINT_H
#include <stdint.h>
#include <SDL.h>

struct nk_context;

enum { PAINT_NONE, PAINT_SAVE, PAINT_CLOSE };

/* starts painting a copy of rgba (w x h); 0 done */
int paint_start(SDL_Renderer *ren, const uint8_t *rgba, int w, int h);
void paint_stop(void);
int paint_on(void);
int paint_dirty(void);          /* changed since the start or the last save */
void paint_saved(void);
const uint8_t *paint_image(void);
void paint_size(int *w, int *h);
unsigned paint_serial(void);   /* m5: changes whenever the pixels do */
/* m4c: the largest size Resize may make (0: none, as for graffiti) */
void paint_allow_resize(int max);
/* m4c: k times bigger (2, 4, 8), integer scaling; 0 done */
int paint_resize(int k);
/* the tools and the canvas, in the current Nuklear window, the canvas
 * canvas_h high; PAINT_SAVE or PAINT_CLOSE when asked */
int paint_panel(struct nk_context *ctx, float row, float canvas_h);

#endif
