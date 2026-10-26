SMAA (Enhanced Subpixel Morphological Antialiasing), from
https://github.com/iryoku/smaa -- MIT licence, see LICENSE.txt.

- SMAA.hlsl, AreaTex.h, SearchTex.h: as in the original repository.
- smaa_glsl.h: SMAA.hlsl with its comments removed, as C strings, for
  `RECOMP_GL_AA=smaa_*` (xboxrecomp/src/kernel/nv2a_gl.c).
