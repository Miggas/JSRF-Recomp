/**
 * NV2A vertex program interpreter, for the software pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) could only draw vertices that arrive already
 * in screen space. A title running vertex programs -- every 3D scene on the
 * Xbox -- sends model-space positions and lets the GPU transform them, so
 * those batches were counted and skipped. This runs the uploaded program on
 * the CPU, one vertex at a time, and hands back the outputs the rasteriser
 * needs.
 *
 * Programs and constants arrive through the pushbuffer exactly as the GPU
 * would take them (SET_TRANSFORM_PROGRAM_LOAD / _PROGRAM, _CONSTANT_LOAD /
 * _CONSTANT), so the executor forwards those methods here and nothing about
 * a title's D3D state has to be known.
 *
 * Instruction encoding: the field table at the top of nv2a_vsh_interp.c.
 * Portable C; no
 * graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_INTERP_H
#define XBOXRECOMP_NV2A_VSH_INTERP_H

#include <stdint.h>

#define NV2A_VSH_SLOTS      136     /* program memory, 128-bit instructions */
#define NV2A_VSH_CONSTANTS  192     /* c[0..191]; D3D's c0 is c[96]          */
#define NV2A_VSH_INPUTS     16      /* v0..v15                                */

/* What one vertex comes out as. Position is whatever the program wrote to
 * oPos: on Xbox D3D that is already screen space -- x, y in pixels, z in the
 * depth buffer's range -- because the runtime appends the viewport transform
 * and the divide by w to every shader, with w left as the clip-space w. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aVshOutput;

void nv2a_vsh_set_load_slot(uint32_t slot);           /* _PROGRAM_LOAD      */
void nv2a_vsh_program_word(uint32_t word);            /* _PROGRAM(i)        */
void nv2a_vsh_set_start_slot(uint32_t slot);          /* _PROGRAM_START     */
void nv2a_vsh_set_constant_load(uint32_t index);      /* _CONSTANT_LOAD     */
void nv2a_vsh_constant_word(uint32_t word);           /* _CONSTANT(i)       */
void nv2a_vsh_set_cxt_write(uint32_t enable);         /* _CXT_WRITE_EN      */

/* Run the program from the start slot on one vertex. Inputs are the ten
 * attribute values as floats. Returns 0 if there is no program to run (no
 * FINAL flag within program memory), which the caller treats as "cannot
 * transform this batch" rather than drawing garbage. */
int  nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out);

/* Decode the current program if it changed. nv2a_vsh_run does this itself
 * when needed; a caller that runs vertices on several threads calls it first,
 * since after it nv2a_vsh_run only reads shared state -- unless the program
 * writes constants (CXT_WRITE_EN), which has to stay on one thread. */
void nv2a_vsh_prepare(void);
int  nv2a_vsh_writes_constants(void);

/* Direct access, for the self-test. */
void nv2a_vsh_set_constant(uint32_t index, const float v[4]);
const float *nv2a_vsh_constant(uint32_t index);   /* for traces */
void nv2a_vsh_trace_dump(void);    /* v95: program words + constants into a frame trace */
void nv2a_vsh_trace_reset(void);
int  nv2a_vsh_is_passthrough(void);   /* v97: D3D's XYZRHW pass-through program */
/* One component of a constant, raw bits: the viewport methods write c[58]
 * (scale) and c[59] (offset) a word at a time rather than via _CONSTANT. */
void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word);
void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4]);

/* v77, for the GPU vertex shader: the decoded program packed four words an
 * instruction (layout in nv2a_gl.c's VS), its generation, the inputs it
 * reads, and the constant array with a generation that moves on any change. */
int nv2a_vsh_gpu_program(uint32_t out[NV2A_VSH_SLOTS][4], uint32_t *gen);
uint32_t nv2a_vsh_input_mask(void);
const float *nv2a_vsh_constants(uint32_t *gen);
int nv2a_vsh_constants_changed(uint32_t since, uint32_t *lo, uint32_t *hi);

#endif
