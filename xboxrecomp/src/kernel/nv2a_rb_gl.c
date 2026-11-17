/* nv2a_rb_gl.c: the OpenGL backend (nv2a_rb.h).
 *
 * While step 1 of the backends plan moves the GL code here piece by piece,
 * this file is compiled as part of nv2a_gl.c (included after the GL thread's
 * headers, so its calls go through the GL thread like the rest); when every
 * GL call is here, it becomes a file of its own and nv2a_gl.c has none. */

static RbFixed s_rbf;                /* what GL has now */
static int s_rbf_valid;

void rb_fixed_lost(void) { s_rbf_valid = 0; }

#define RBF_CH(a, b) (all || memcmp(&f->a, &s_rbf.a, sizeof f->a) || (b))
void rb_fixed(const RbFixed *f)
{
    int all = !s_rbf_valid;
    if (all || f->depth_test != s_rbf.depth_test || f->depth_func != s_rbf.depth_func
            || f->depth_mask != s_rbf.depth_mask) {
        if (f->depth_test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(f->depth_func);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(f->depth_mask ? GL_TRUE : GL_FALSE);
    }
    if (all || f->stencil_test != s_rbf.stencil_test || f->stencil_func != s_rbf.stencil_func
            || f->stencil_ref != s_rbf.stencil_ref || f->stencil_mask != s_rbf.stencil_mask
            || f->stencil_fail != s_rbf.stencil_fail || f->stencil_zfail != s_rbf.stencil_zfail
            || f->stencil_zpass != s_rbf.stencil_zpass || f->stencil_wmask != s_rbf.stencil_wmask) {
        if (f->stencil_test) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(f->stencil_func, (GLint)f->stencil_ref, f->stencil_mask);
            glStencilOp(f->stencil_fail, f->stencil_zfail, f->stencil_zpass);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        glStencilMask(f->stencil_wmask);
    }
    if (all || f->blend != s_rbf.blend || f->blend_src != s_rbf.blend_src
            || f->blend_dst != s_rbf.blend_dst || f->blend_eq != s_rbf.blend_eq
            || memcmp(f->blend_color, s_rbf.blend_color, sizeof f->blend_color)) {
        if (f->blend) {
            glEnable(GL_BLEND);
            glBlendFunc(f->blend_src, f->blend_dst);
            glBlendEquation(f->blend_eq);
            glBlendColor(f->blend_color[0], f->blend_color[1], f->blend_color[2], f->blend_color[3]);
        } else {
            glDisable(GL_BLEND);
        }
    }
    if (all || f->cmask != s_rbf.cmask)
        glColorMask((f->cmask & 1) != 0, (f->cmask & 2) != 0, (f->cmask & 4) != 0, (f->cmask & 8) != 0);
    if (all || f->poly_offset != s_rbf.poly_offset || f->poly_factor != s_rbf.poly_factor
            || f->poly_units != s_rbf.poly_units) {
        if (f->poly_offset) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(f->poly_factor, f->poly_units);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    if (all || f->cull != s_rbf.cull || f->cull_face != s_rbf.cull_face
            || f->front_face != s_rbf.front_face) {
        if (f->cull) {
            glEnable(GL_CULL_FACE);
            glCullFace(f->cull_face);
            glFrontFace(f->front_face);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
    s_rbf = *f;
    s_rbf_valid = 1;
}
#undef RBF_CH
