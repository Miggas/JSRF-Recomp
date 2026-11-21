/* m26: the stage viewer (stage editor, level 1): a whole stage in 3D, drawn
 * in software on all the computer's cores (as the 3D view: the same on
 * Windows and the Steam Deck): its models and props with their textures,
 * its collision coloured by surface, its grind rails. Flown through with
 * the mouse and the keys; exported as .obj. */
#ifndef STAGE3D_H
#define STAGE3D_H
#include <SDL.h>
#include "jstage.h"

struct nk_context;

/* stage `name` ("Stg30") from dir (Media/Stage); 0 done, else err */
int  s3d_open(const char *dir, const char *name, char *err, size_t errn);
void s3d_close(void);
int  s3d_is_open(void);
const JStage *s3d_stage(void);
/* the controls (a Nuklear window at x, y, w, h) and the view beside them;
 * returns 1 when Back was pressed; msg gets news for the status line;
 * more (if any) adds its own rows at the end of the controls */
int  s3d_draw(struct nk_context *ctx, SDL_Renderer *ren, float x, float y, float w, float h, float row,
              char *msg, size_t msgn, const char *export_dir, void (*more)(struct nk_context *ctx, float row));
/* (tests) the camera: position, turn and tilt in degrees; layers as "vpcr" letters */
void s3d_set_camera(float x, float y, float z, float yaw_deg, float pitch_deg);
void s3d_set_layers(const char *letters);

#endif
