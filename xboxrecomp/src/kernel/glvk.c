/* glvk: nv2a_gl.c's OpenGL calls on Vulkan (see glvk.h).
 *
 * How it is put together
 *  - One device (nv2a_vk.c's, shared with the presenter), one queue, all
 *    calls on one thread (the executor's: the GL thread is off with it).
 *  - Commands are recorded into a batch; a batch is submitted when the
 *    window is presented, or when the CPU has to wait for the GPU (a
 *    read-back, a query result, glFinish, glClientWaitSync). Three batches
 *    take turns; each has its own fence, staging ring (texture uploads),
 *    uniform ring (each draw's uniforms, constant attributes) and a list of
 *    objects to destroy once it has finished.
 *  - vk16: each image knows its layout and what last used it (img_use):
 *    attachments are in the attachment layouts while drawn into, textures
 *    in SHADER_READ_ONLY while sampled, copies in the transfer layouts, and
 *    a barrier for that one image is made only when its use changes (it
 *    was every image in GENERAL and a full memory barrier around every
 *    pass, upload and copy: 12-19 a frame in play). The window's image is
 *    left in GENERAL for the presenter.
 *  - Rendering is dynamic rendering (Vulkan 1.3), begun at the first draw
 *    or clear into a framebuffer and ended by anything else.
 *  - Programs: both stages' GLSL to SPIR-V through glslang and the SPIR-V
 *    cache; the vertex stage gets OpenGL's depth range mapped to Vulkan's.
 *    The loose uniforms are glslang's default uniform block (set 0,
 *    binding 0): read off the SPIR-V (names, offsets, strides), kept per
 *    program on the CPU as OpenGL keeps them, and copied into the uniform
 *    ring for each draw that changed them. Samplers are push descriptors.
 *  - Pipelines: one per program and the state OpenGL can't change
 *    dynamically in core Vulkan 1.3 (blending, colour mask, attachment
 *    formats, samples, vertex formats); the rest is dynamic state. A
 *    pipeline cache on disk, shader_cache/glvk_pipelines.bin. */
#include "glvk.h"
#include "nv2a_vk_core.h"

#include <glad/gl.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define GV_LOG(...) fprintf(stderr, "[VK] " __VA_ARGS__)
#define GV_ONCE(n, ...) do { static int said_; if (said_++ < (n)) GV_LOG(__VA_ARGS__); } while (0)
#define API GLAD_API_PTR

/* ── the device and the batches ─────────────────────────────────────────── */

#define NB 3                        /* batches in turn */
#define RING_UBO (8u << 20)         /* uniforms, constant attributes */
#define RING_STG (16u << 20)        /* texture uploads */
#define QPOOL 1024                  /* occlusion queries a batch */

typedef struct {
    VkBuffer b;
    VkDeviceMemory m;
    uint8_t *map;
    VkDeviceSize size, off;
} Ring;

typedef struct { int kind; uint64_t h; VkDeviceMemory m; } Junk;   /* kind: 1 image 2 view 3 buffer 4 pipeline 5 module 6 pl 7 dsl */

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    uint64_t serial;
    int open;                       /* being recorded */
    Ring ubo, stg;
    Junk *junk;
    int njunk, cjunk;
    VkQueryPool qp;
    uint32_t qn;                    /* queries used in it */
    VkQueryPool tq;                 /* vk12d: GPU time of the batch (start, end) */
    int tq_set;
} Batch;
/* vk12d: what the GPU did (for the Vulkan backend's log) */
static uint64_t s_gpu_ns, s_gpu_batches, s_n_passes, s_n_barriers;

static struct {
    int up;
    NvVkCore c;
    VkPhysicalDeviceMemoryProperties mp;
    VkCommandPool pool;
    Batch b[NB];
    int cur;
    uint64_t serial;                /* of the batch being recorded */
    uint64_t done;                  /* every batch up to this one has finished */
    VkPipelineCache pcache;
    VkFormat ds24, ds32;            /* the depth-stencil formats */
    VkDeviceSize ubo_align;
    float max_aniso;
    int bc_ok;
    char name[256];
} G;

static uint64_t s_n_draws, s_n_pipes, s_n_submits;

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < G.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (G.mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int alloc_mem(VkMemoryRequirements *mr, VkMemoryPropertyFlags want,
                     VkMemoryPropertyFlags fallback, VkDeviceMemory *m)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t = mem_type(mr->memoryTypeBits, want);
    if (t < 0) t = mem_type(mr->memoryTypeBits, fallback);
    if (t < 0) return 0;
    ai.allocationSize = mr->size;
    ai.memoryTypeIndex = (uint32_t)t;
    return vkAllocateMemory(G.c.dev, &ai, NULL, m) == VK_SUCCESS;
}

/* a host-visible, mapped buffer */
static int host_buffer(VkDeviceSize size, VkBufferUsageFlags use, int cached,
                       VkBuffer *b, VkDeviceMemory *m, uint8_t **map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    void *p = NULL;
    bi.size = size ? size : 16;
    bi.usage = use;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(G.c.dev, &bi, NULL, b) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(G.c.dev, *b, &mr);
    if (!alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        | (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0),
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m)) {
        vkDestroyBuffer(G.c.dev, *b, NULL);
        *b = VK_NULL_HANDLE;
        return 0;
    }
    if (vkBindBufferMemory(G.c.dev, *b, *m, 0) != VK_SUCCESS
            || vkMapMemory(G.c.dev, *m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        vkDestroyBuffer(G.c.dev, *b, NULL);
        vkFreeMemory(G.c.dev, *m, NULL);
        *b = VK_NULL_HANDLE;
        return 0;
    }
    *map = (uint8_t *)p;
    return 1;
}

static void batch_begin(void);

/* destroyed once the GPU is done with it: kept with the batch being
 * recorded (opened if none is), whose fence comes after every batch
 * submitted before -- never with a batch not yet begun, whose start would
 * free it at once while an earlier one may still be reading it */
static void junk(int kind, uint64_t h, VkDeviceMemory m)
{
    Batch *b;
    if (!h && !m)
        return;
    batch_begin();
    b = &G.b[G.cur];
    if (b->njunk == b->cjunk) {
        int n = b->cjunk ? b->cjunk * 2 : 64;
        Junk *j = (Junk *)realloc(b->junk, (size_t)n * sizeof *j);
        if (!j)
            return;                      /* leaked rather than freed too early */
        b->junk = j;
        b->cjunk = n;
    }
    b->junk[b->njunk].kind = kind;
    b->junk[b->njunk].h = h;
    b->junk[b->njunk].m = m;
    b->njunk++;
}

static void junk_free(Batch *b)
{
    int i;
    for (i = 0; i < b->njunk; i++) {
        Junk *j = &b->junk[i];
        switch (j->kind) {
        case 1: vkDestroyImage(G.c.dev, (VkImage)j->h, NULL); break;
        case 2: vkDestroyImageView(G.c.dev, (VkImageView)j->h, NULL); break;
        case 3: vkDestroyBuffer(G.c.dev, (VkBuffer)j->h, NULL); break;
        case 4: vkDestroyPipeline(G.c.dev, (VkPipeline)j->h, NULL); break;
        case 5: vkDestroyShaderModule(G.c.dev, (VkShaderModule)j->h, NULL); break;
        case 6: vkDestroyPipelineLayout(G.c.dev, (VkPipelineLayout)j->h, NULL); break;
        case 7: vkDestroyDescriptorSetLayout(G.c.dev, (VkDescriptorSetLayout)j->h, NULL); break;
        }
        if (j->m)
            vkFreeMemory(G.c.dev, j->m, NULL);
    }
    b->njunk = 0;
}

/* vk17: the last batch known finished, published for other threads (the
 * Vulkan backend's recording-thread queue asks rb_fence_signaled from the
 * renderer's thread without waiting for the recording thread) */
static void done_set(uint64_t serial)
{
    __atomic_store_n(&G.done, serial, __ATOMIC_RELEASE);
}
/* batches submitted before that have finished (not waiting): G.done
 * forward, so fences a frame or two old are seen as passed */
static void done_poll(void)
{
    uint64_t best = G.done;
    int i;
    for (i = 0; i < NB; i++) {
        Batch *b = &G.b[i];
        if (b->open || !b->serial || b->serial <= best)
            continue;
        if (vkGetFenceStatus(G.c.dev, b->fence) == VK_SUCCESS)
            best = b->serial;
    }
    if (best > G.done)
        done_set(best);
}

/* state that lives in the command buffer and has to be set again in a new one */
static void cmd_state_lost(void);
static void queries_collect(Batch *b);

static void batch_begin(void)
{
    Batch *b = &G.b[G.cur];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (b->open)
        return;
    if (b->serial) {
        vkWaitForFences(G.c.dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
        if (b->serial > G.done)
            done_set(b->serial);
        queries_collect(b);
        if (b->tq && b->tq_set) {
            uint64_t t[2];
            if (vkGetQueryPoolResults(G.c.dev, b->tq, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)
                    == VK_SUCCESS && t[1] > t[0]) {
                s_gpu_ns += (uint64_t)((double)(t[1] - t[0]) * G.c.props.limits.timestampPeriod);
                s_gpu_batches++;
            }
        }
        b->tq_set = 0;
    }
    junk_free(b);
    vkResetFences(G.c.dev, 1, &b->fence);
    b->ubo.off = 0;
    b->stg.off = 0;
    b->serial = ++G.serial;
    vkResetCommandBuffer(b->cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(b->cmd, &bi);
    if (b->qp)
        vkCmdResetQueryPool(b->cmd, b->qp, 0, QPOOL);
    if (b->tq) {
        vkCmdResetQueryPool(b->cmd, b->tq, 0, 2);
        vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b->tq, 0);
    }
    b->qn = 0;
    b->open = 1;
    cmd_state_lost();
}

static VkCommandBuffer cmd(void)
{
    batch_begin();
    return G.b[G.cur].cmd;
}

static void rendering_end(void);

/* the batch to the GPU; the next one is begun at the next command */
static void batch_submit(void)
{
    Batch *b = &G.b[G.cur];
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (!b->open)
        return;
    rendering_end();
    if (b->tq) {
        vkCmdWriteTimestamp(b->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b->tq, 1);
        b->tq_set = 1;
    }
    vkEndCommandBuffer(b->cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &b->cmd;
    nv2a_vk_qlock();
    if (vkQueueSubmit(G.c.q, 1, &si, b->fence) != VK_SUCCESS)
        GV_ONCE(4, "a batch could not be submitted (the device may be lost)\n");
    nv2a_vk_qunlock();
    b->open = 0;
    s_n_submits++;
    G.cur = (G.cur + 1) % NB;
    done_poll();
}

/* has the batch `serial` finished? wait: until it has */
static int serial_done(uint64_t serial, int wait)
{
    int i;
    if (serial <= G.done)
        return 1;
    for (i = 0; i < NB; i++) {
        Batch *b = &G.b[i];
        if (b->serial != serial)
            continue;
        if (b->open) {
            if (!wait)
                return 0;
            batch_submit();
        }
        if (wait)
            vkWaitForFences(G.c.dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(G.c.dev, b->fence) != VK_SUCCESS)
            return 0;
        if (serial > G.done)
            done_set(serial);
        return 1;
    }
    return 1;                             /* long gone */
}

static void wait_idle(void)
{
    batch_submit();
    nv2a_vk_qlock();
    vkQueueWaitIdle(G.c.q);
    nv2a_vk_qunlock();
    done_set(G.serial);
}

/* room in a ring of the current batch; when it is full the batch goes and
 * the next one's ring is used */
static VkDeviceSize ring_take(int which, VkDeviceSize n, VkDeviceSize align, Ring **out)
{
    Ring *r;
    VkDeviceSize o;
    batch_begin();
    r = which ? &G.b[G.cur].stg : &G.b[G.cur].ubo;
    o = (r->off + align - 1) & ~(align - 1);
    if (o + n > r->size) {
        batch_submit();
        batch_begin();
        r = which ? &G.b[G.cur].stg : &G.b[G.cur].ubo;
        o = 0;
        if (n > r->size) {
            /* bigger than the ring: the ring grows (the old one goes with this batch) */
            VkDeviceSize ns = r->size;
            while (ns < n) ns *= 2;
            junk(3, (uint64_t)r->b, r->m);
            if (!host_buffer(ns, which ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                       : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                         | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             0, &r->b, &r->m, &r->map)) {
                r->size = 0;
                *out = NULL;
                return 0;
            }
            r->size = ns;
        }
    }
    r->off = o + n;
    *out = r;
    return o;
}


/* ── GL objects ─────────────────────────────────────────────────────────── */

typedef struct {
    int used;
    VkBuffer b;
    VkDeviceMemory m;
    uint8_t *map;
    VkDeviceSize size;
    uint64_t gpu_write;             /* batch whose commands write it */
    int read_back;                  /* made for reading (STREAM_READ) */
} GBuf;

typedef struct {
    int used;
    GLenum target;                  /* 0 until defined */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;               /* sampled: every level and layer */
    VkImageView att;                /* level 0, as an attachment */
    VkFormat fmt;
    int w, h, levels, layers, samples;
    int ds, rgb, bc, bpp;           /* depth-stencil; RGB (alpha reads 1); block-compressed */
    uint32_t defined;               /* levels with contents */
    /* texture parameters */
    GLint minf, magf, wrap_s, wrap_t, wrap_r, maxlev, aniso, swz_a1;
    int view_swz;                   /* the swizzle `view` was made with */
    /* vk16: its layout and its last use (img_use) */
    VkImageLayout lay;
    VkPipelineStageFlags st;        /* the stages that last used it */
    VkAccessFlags acc;              /* and how */
    int wr;                         /* that use wrote it */
} GTex;

typedef struct { int used; GLuint col, ds; } GFbo;

typedef struct {
    int used;
    GLint minf, magf, wrap_s, wrap_t;
} GSamp;

typedef struct {
    int enabled;
    GLint size;
    GLenum type;
    GLboolean norm;
    GLsizei stride;
    GLuint buf;
    uintptr_t off;
} GAttr;

#define NATTR 16
typedef struct { int used; GAttr a[NATTR]; GLuint ebo; } GVao;

typedef struct {
    int used;
    int active;                     /* begun, not ended */
    uint64_t serial;                /* the batch its samples were counted in */
    uint32_t first, n;              /* its slots in that batch's pool */
    int have;
    uint64_t result;
} GQuery;

/* tables: index = GL name; grown as names are made */
#define TABLE(T, name) static T *name; static GLuint name##_n
TABLE(GBuf, t_buf);
TABLE(GTex, t_tex);                 /* textures and renderbuffers share names */
TABLE(GFbo, t_fbo);
TABLE(GSamp, t_samp);
TABLE(GVao, t_vao);
TABLE(GQuery, t_query);

static void *table_get(void **tab, GLuint *n, size_t sz, GLuint id)
{
    if (id >= *n) {
        GLuint nn = *n ? *n : 64;
        void *t;
        while (nn <= id) nn *= 2;
        t = realloc(*tab, (size_t)nn * sz);
        if (!t)
            return NULL;
        memset((char *)t + (size_t)*n * sz, 0, (size_t)(nn - *n) * sz);
        *tab = t;
        *n = nn;
    }
    return (char *)*tab + (size_t)id * sz;
}
#define GET(tab, id) ((__typeof__(tab))table_get((void **)&tab, &tab##_n, sizeof *tab, (id)))

static GLuint s_next_name = 1;      /* names for everything */
static void gen_names(GLsizei n, GLuint *ids, void **tab, GLuint *tn, size_t sz)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        int *u;
        ids[i] = s_next_name++;
        u = (int *)table_get(tab, tn, sz, ids[i]);
        if (u)
            *u = 1;
    }
}

/* ── OpenGL's state ─────────────────────────────────────────────────────── */

#define NUNIT 16
typedef struct GProg GProg;
static struct {
    GLuint ab, ebo_unbound, pack, unpack_buf;    /* buffer bindings (ebo: in the VAO) */
    GLuint vao;
    GLuint draw_fbo, read_fbo, rb;
    GLuint prog;
    int unit;
    GLuint tex2d[NUNIT], texcube[NUNIT], texms[NUNIT], samp[NUNIT];
    int depth_test, stencil_test, blend, cull, scissor, poff, dclamp;
    GLenum depth_func;
    GLboolean depth_mask;
    GLenum st_func, st_fail, st_zfail, st_zpass;
    GLint st_ref;
    GLuint st_mask, st_wmask;
    GLenum bl_src, bl_dst, bl_eq;
    float bl_col[4];
    GLboolean cmask[4];
    GLenum cull_face, front_face;
    float po_factor, po_units;
    GLint vp[4], sc[4];
    float clear_col[4];
    double clear_depth;
    GLint clear_stencil;
    GLint unpack_align, pack_align;
    float generic[NATTR][4];        /* glVertexAttrib4f: inputs with no array */
    GLuint query_active;
    /* the default framebuffer */
    GLuint def_tex;
    int def_w, def_h;
} S;

/* what the command buffer has now */
static struct {
    int rendering;
    GLuint col, ds;                 /* the textures it renders to */
    VkPipeline pipe;
    int vw, vh;                     /* its size */
    uint32_t qslot;                 /* the query counting in it, or ~0 */
} R;

static void cmd_state_lost(void)
{
    R.rendering = 0;
    R.pipe = VK_NULL_HANDLE;
    R.qslot = ~0u;
}

static GTex *tex_of(GLuint id) { return id && id < t_tex_n && t_tex[id].used ? &t_tex[id] : NULL; }
static GBuf *buf_of(GLuint id) { return id && id < t_buf_n && t_buf[id].used ? &t_buf[id] : NULL; }

/* vk16: t used from here on in layout `lay`, by these stages, reading or
 * writing: a barrier for this image when it is needed (a write before, a
 * write now, or another layout). Never inside a rendering pass: one is
 * ended first when a barrier is made. */
static uint64_t s_n_ibarriers;
static void img_use(GTex *t, VkImageLayout lay, VkPipelineStageFlags st, VkAccessFlags acc, int write)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    if (!t || !t->img)
        return;
    if (t->lay == lay && !t->wr && !write) {
        t->st |= st;                    /* another reader: a write waits for all */
        t->acc |= acc;
        return;
    }
    if (R.rendering)
        rendering_end();
    b.srcAccessMask = t->wr ? t->acc : 0;
    b.dstAccessMask = acc;
    b.oldLayout = t->lay;
    b.newLayout = lay;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange.aspectMask = t->ds ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                          : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = (uint32_t)t->levels;
    b.subresourceRange.layerCount = (uint32_t)t->layers;
    vkCmdPipelineBarrier(cmd(), t->st ? t->st : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, st,
                         0, 0, NULL, 0, NULL, 1, &b);
    s_n_ibarriers++;
    t->lay = lay;
    t->st = st;
    t->acc = acc;
    t->wr = write;
}
#define IMG_SAMPLED(t) img_use((t), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, \
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, \
        VK_ACCESS_SHADER_READ_BIT, 0)
#define IMG_XSRC(t) img_use((t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, \
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0)
#define IMG_XDST(t) img_use((t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, \
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 1)
/* for the presenter, which copies from it on its own (GENERAL) */
#define IMG_PRESENT(t) img_use((t), VK_IMAGE_LAYOUT_GENERAL, \
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0)
/* device writes into host-visible buffers, for the CPU */
static void host_barrier(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* ── buffers ────────────────────────────────────────────────────────────── */

static GLuint *bind_slot(GLenum target)
{
    static GLuint dummy;
    switch (target) {
    case GL_ARRAY_BUFFER: return &S.ab;
    case GL_ELEMENT_ARRAY_BUFFER: {
        GVao *v = GET(t_vao, S.vao);
        return v ? &v->ebo : &S.ebo_unbound;
    }
    case GL_PIXEL_PACK_BUFFER: return &S.pack;
    case GL_PIXEL_UNPACK_BUFFER: return &S.unpack_buf;
    default: return &dummy;
    }
}

static void API gv_GenBuffers(GLsizei n, GLuint *ids) { gen_names(n, ids, (void **)&t_buf, &t_buf_n, sizeof *t_buf); }
static void API gv_DeleteBuffers(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        GBuf *b = buf_of(ids[i]);
        if (!b) continue;
        junk(3, (uint64_t)b->b, b->m);
        memset(b, 0, sizeof *b);
    }
}
static void API gv_BindBuffer(GLenum target, GLuint id) { *bind_slot(target) = id; }

static void API gv_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    GBuf *b = buf_of(*bind_slot(target));
    if (!b) return;
    /* new storage, as OpenGL's orphaning gives: the old goes when the
     * commands using it have finished */
    if (b->b)
        junk(3, (uint64_t)b->b, b->m);
    b->b = VK_NULL_HANDLE;
    b->read_back = usage == GL_STREAM_READ || usage == GL_DYNAMIC_READ || usage == GL_STATIC_READ;
    if (!host_buffer((VkDeviceSize)size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                     | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT, b->read_back, &b->b, &b->m, &b->map)) {
        GV_ONCE(4, "a buffer of %lld bytes could not be made\n", (long long)size);
        b->size = 0;
        return;
    }
    b->size = (VkDeviceSize)size;
    b->gpu_write = 0;
    if (data)
        memcpy(b->map, data, (size_t)size);
}

static void API gv_BufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    GBuf *b = buf_of(*bind_slot(target));
    if (!b || !b->map || (VkDeviceSize)(off + size) > b->size) return;
    memcpy(b->map + off, data, (size_t)size);
}

static void *API gv_MapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    GBuf *b = buf_of(*bind_slot(target));
    (void)len;
    if (!b || !b->map) return NULL;
    if ((access & GL_MAP_READ_BIT) && b->gpu_write)
        serial_done(b->gpu_write, 1);   /* what the GPU copied into it */
    return b->map + off;
}
static GLboolean API gv_UnmapBuffer(GLenum target) { (void)target; return GL_TRUE; }

/* ── textures ───────────────────────────────────────────────────────────── */

static void API gv_GenTextures(GLsizei n, GLuint *ids)
{
    GLsizei i;
    gen_names(n, ids, (void **)&t_tex, &t_tex_n, sizeof *t_tex);
    for (i = 0; i < n; i++) {
        GTex *t = tex_of(ids[i]);
        if (!t) continue;
        t->minf = GL_NEAREST_MIPMAP_LINEAR; t->magf = GL_LINEAR;
        t->wrap_s = t->wrap_t = t->wrap_r = GL_REPEAT;
        t->maxlev = 1000; t->aniso = 1;
    }
}
static void API gv_GenRenderbuffers(GLsizei n, GLuint *ids) { gv_GenTextures(n, ids); }

static void tex_free_vk(GTex *t)
{
    if (t->view) junk(2, (uint64_t)t->view, VK_NULL_HANDLE);
    if (t->att) junk(2, (uint64_t)t->att, VK_NULL_HANDLE);
    if (t->img) junk(1, (uint64_t)t->img, t->mem);
    t->view = t->att = VK_NULL_HANDLE;
    t->img = VK_NULL_HANDLE;
    t->mem = VK_NULL_HANDLE;
    t->defined = 0;
    t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
    t->st = 0; t->acc = 0; t->wr = 0;
}

static void API gv_DeleteTextures(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        GTex *t = tex_of(ids[i]);
        int u;
        if (!t) continue;
        if (R.rendering && (R.col == ids[i] || R.ds == ids[i]))
            rendering_end();
        tex_free_vk(t);
        memset(t, 0, sizeof *t);
        for (u = 0; u < NUNIT; u++) {
            if (S.tex2d[u] == ids[i]) S.tex2d[u] = 0;
            if (S.texcube[u] == ids[i]) S.texcube[u] = 0;
            if (S.texms[u] == ids[i]) S.texms[u] = 0;
        }
    }
}
static void API gv_DeleteRenderbuffers(GLsizei n, const GLuint *ids) { gv_DeleteTextures(n, ids); }

static GLuint *unit_slot(GLenum target, int unit)
{
    switch (target) {
    case GL_TEXTURE_CUBE_MAP: return &S.texcube[unit];
    case GL_TEXTURE_2D_MULTISAMPLE: return &S.texms[unit];
    default: return &S.tex2d[unit];
    }
}
static void API gv_ActiveTexture(GLenum u) { S.unit = (int)(u - GL_TEXTURE0) & (NUNIT - 1); }
static void API gv_BindTexture(GLenum target, GLuint id) { *unit_slot(target, S.unit) = id; }

static GTex *bound_tex(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_POSITIVE_X + 5)
        target = GL_TEXTURE_CUBE_MAP;
    return tex_of(*unit_slot(target, S.unit));
}

/* GL internal format -> Vulkan */
static VkFormat vk_format(GLenum ifmt, int *rgb, int *bc, int *ds, int *bpp)
{
    *rgb = *bc = *ds = 0;
    *bpp = 4;
    switch (ifmt) {
    case GL_RGB8: case GL_RGB: *rgb = 1; return VK_FORMAT_B8G8R8A8_UNORM;
    case GL_RGBA8: case GL_RGBA: case GL_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
    case GL_RG8: case GL_RG: *bpp = 2; return VK_FORMAT_R8G8_UNORM;
    case GL_R8: case GL_RED: *bpp = 1; return VK_FORMAT_R8_UNORM;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: *bc = 8; return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: *bc = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: *bc = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    case GL_DEPTH24_STENCIL8: *ds = 1; return G.ds24;
    case GL_DEPTH32F_STENCIL8: *ds = 1; return G.ds32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static int full_levels(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

static VkImageView make_view(GTex *t, int all, int swz_a1)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkImageView v = VK_NULL_HANDLE;
    vi.image = t->img;
    vi.viewType = all && t->layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->fmt;
    vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.a = swz_a1 || (t->rgb && all) ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
    /* one- and two-channel images read as OpenGL's do: (r,0,0,1), (r,g,0,1) */
    vi.subresourceRange.aspectMask = t->ds ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                           : VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->ds && all)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = all ? (uint32_t)t->levels : 1;
    vi.subresourceRange.layerCount = all ? (uint32_t)t->layers : 1;
    if (vkCreateImageView(G.c.dev, &vi, NULL, &v) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return v;
}

/* (re)make the image for this shape; contents start as zero (colour
 * images: 0,0,0,1) */
static int tex_alloc(GTex *t, GLenum target, GLenum ifmt, int w, int h, int levels, int layers, int samples)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    int rgb, bc, ds, bpp;
    VkFormat f = vk_format(ifmt, &rgb, &bc, &ds, &bpp);
    if (f == VK_FORMAT_UNDEFINED || w <= 0 || h <= 0) {
        GV_ONCE(8, "texture format 0x%X (%dx%d) is not handled\n", ifmt, w, h);
        return 0;
    }
    if (bc && !G.bc_ok)
        return 0;
    if (t->img && t->fmt == f && t->w == w && t->h == h && t->levels == levels
            && t->layers == layers && t->samples == samples)
        return 1;                       /* the same shape: kept */
    if (R.rendering)
        rendering_end();
    tex_free_vk(t);
    t->target = target; t->fmt = f; t->w = w; t->h = h; t->levels = levels;
    t->layers = layers; t->samples = samples; t->rgb = rgb; t->bc = bc; t->ds = ds; t->bpp = bpp;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = f;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)levels;
    ii.arrayLayers = (uint32_t)layers;
    ii.samples = (VkSampleCountFlagBits)samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ds)
        ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (bc)
        ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    else
        ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (layers == 6)
        ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(G.c.dev, &ii, NULL, &t->img) != VK_SUCCESS) {
        GV_ONCE(8, "an image %dx%d (format %d) could not be made\n", w, h, (int)f);
        t->img = VK_NULL_HANDLE;
        return 0;
    }
    vkGetImageMemoryRequirements(G.c.dev, t->img, &mr);
    if (!alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)
            || vkBindImageMemory(G.c.dev, t->img, t->mem, 0) != VK_SUCCESS) {
        GV_ONCE(8, "no memory for an image %dx%d\n", w, h);
        vkDestroyImage(G.c.dev, t->img, NULL);
        if (t->mem) vkFreeMemory(G.c.dev, t->mem, NULL);
        t->img = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE;
        return 0;
    }
    t->view_swz = t->swz_a1;
    if (!ds && samples == 1)
        t->view = make_view(t, 1, t->swz_a1);
    if (!bc)
        t->att = make_view(t, 0, 0);
    {
        /* cleared, where the commands are now */
        VkImageSubresourceRange all = { 0, 0, (uint32_t)levels, 0, (uint32_t)layers };
        VkCommandBuffer c;
        all.aspectMask = ds ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                            : VK_IMAGE_ASPECT_COLOR_BIT;
        t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
        t->st = 0; t->acc = 0; t->wr = 0;
        IMG_XDST(t);
        c = cmd();
        if (ds) {
            VkClearDepthStencilValue z = { 1.0f, 0 };
            vkCmdClearDepthStencilImage(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        } else if (!bc) {
            VkClearColorValue k;
            memset(&k, 0, sizeof k);
            k.float32[3] = 1.0f;
            vkCmdClearColorImage(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &k, 1, &all);
        }
    }
    return 1;
}

/* rows of `data` (format/type as OpenGL takes them) into level/layer of t */
static void tex_upload(GTex *t, int level, int layer, int x, int y, int w, int h,
                       GLenum format, GLenum type, const void *data, size_t csize)
{
    Ring *r;
    VkDeviceSize o, n;
    VkBufferImageCopy cp;
    int row, bpp_in;
    size_t pitch_in;
    if (!data || !t->img || w <= 0 || h <= 0)
        return;
    if (R.rendering)
        rendering_end();
    if (t->bc) {
        n = (VkDeviceSize)csize;
    } else {
        n = (VkDeviceSize)w * h * t->bpp;
    }
    o = ring_take(1, n, 16, &r);
    if (!r)
        return;
    if (t->bc) {
        memcpy(r->map + o, data, (size_t)n);
    } else {
        int a = S.unpack_align > 0 ? S.unpack_align : 4;
        bpp_in = (format == GL_RG) ? 2 : (format == GL_RED) ? 1 : 4;
        pitch_in = ((size_t)w * bpp_in + a - 1) / a * a;
        for (row = 0; row < h; row++) {
            const uint8_t *src = (const uint8_t *)data + (size_t)row * pitch_in;
            uint8_t *dst = r->map + o + (size_t)row * w * t->bpp;
            if (bpp_in == t->bpp && !(format == GL_RGBA && type == GL_UNSIGNED_BYTE && t->bpp == 4)) {
                memcpy(dst, src, (size_t)w * t->bpp);
            } else if (bpp_in == 4 && t->bpp == 4) {
                int i;                      /* R,G,B,A -> B,G,R,A */
                for (i = 0; i < w; i++) {
                    dst[i * 4 + 0] = src[i * 4 + 2];
                    dst[i * 4 + 1] = src[i * 4 + 1];
                    dst[i * 4 + 2] = src[i * 4 + 0];
                    dst[i * 4 + 3] = src[i * 4 + 3];
                }
            } else {
                memset(dst, 0, (size_t)w * t->bpp);
            }
        }
    }
    IMG_XDST(t);
    memset(&cp, 0, sizeof cp);
    cp.bufferOffset = o;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.mipLevel = (uint32_t)level;
    cp.imageSubresource.baseArrayLayer = (uint32_t)layer;
    cp.imageSubresource.layerCount = 1;
    cp.imageOffset.x = x;
    cp.imageOffset.y = y;
    cp.imageExtent.width = (uint32_t)w;
    cp.imageExtent.height = (uint32_t)h;
    cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd(), r->b, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
}

static void tex_image(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                      GLenum format, GLenum type, const void *data, size_t csize)
{
    GTex *t = bound_tex(target);
    int layer = 0, layers = 1, lw, lh;
    if (!t || level < 0 || level > 15) return;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_POSITIVE_X + 5) {
        layer = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        layers = 6;
        target = GL_TEXTURE_CUBE_MAP;
    }
    if (level == 0) {
        int lv = full_levels(w, h);
        if (!tex_alloc(t, target, ifmt, w, h, lv, layers, 1))
            return;
        if (!data || layers == 1)
            t->defined = 0;
    } else {
        if (!t->img || level >= t->levels)
            return;
        lw = t->w >> level; lh = t->h >> level;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        if (lw != w || lh != h)
            return;                     /* a level that doesn't follow level 0 */
    }
    if (data || t->ds || !t->bc)
        t->defined |= 1u << level;
    tex_upload(t, level, layer, 0, 0, w, h, format, type, data, csize);
}

static void API gv_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                              GLint border, GLenum format, GLenum type, const void *data)
{
    (void)border;
    tex_image(target, level, (GLenum)ifmt, w, h, format, type, data, 0);
}
static void API gv_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w,
                                        GLsizei h, GLint border, GLsizei size, const void *data)
{
    (void)border;
    tex_image(target, level, ifmt, w, h, 0, 0, data, (size_t)size);
}
static void API gv_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                 GLsizei h, GLenum format, GLenum type, const void *data)
{
    GTex *t = bound_tex(target);
    int layer = 0;
    if (!t || !t->img) return;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_POSITIVE_X + 5)
        layer = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    tex_upload(t, level, layer, x, y, w, h, format, type, data, 0);
}
static void API gv_TexImage2DMultisample(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w,
                                         GLsizei h, GLboolean fixed)
{
    GTex *t = bound_tex(target);
    (void)fixed;
    if (!t) return;
    tex_alloc(t, target, ifmt, w, h, 1, 1, samples);
    t->defined = 1;
}

static void API gv_BindRenderbuffer(GLenum target, GLuint id) { (void)target; S.rb = id; }
static void API gv_RenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h)
{
    GTex *t = tex_of(S.rb);
    (void)target;
    if (!t) return;
    if (tex_alloc(t, GL_RENDERBUFFER, ifmt, w, h, 1, 1, 1))
        t->defined = 1;
}

static void API gv_TexParameteri(GLenum target, GLenum pname, GLint v)
{
    GTex *t = bound_tex(target);
    if (!t) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->minf = v; break;
    case GL_TEXTURE_MAG_FILTER: t->magf = v; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = v; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = v; break;
    case GL_TEXTURE_WRAP_R: t->wrap_r = v; break;
    case GL_TEXTURE_MAX_LEVEL: t->maxlev = v; break;
    case GL_TEXTURE_MAX_ANISOTROPY: t->aniso = v; break;
    case GL_TEXTURE_SWIZZLE_A: t->swz_a1 = v == GL_ONE; break;
    }
}

static void API gv_GenerateMipmap(GLenum target)
{
    GTex *t = bound_tex(target);
    int l, f;
    VkCommandBuffer c;
    if (!t || !t->img || t->bc || t->ds || t->levels < 2) return;
    if (R.rendering) rendering_end();
    /* level to level in the one image: GENERAL, a barrier between levels */
    img_use(t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    c = cmd();
    for (l = 1; l < t->levels; l++) {
        for (f = 0; f < t->layers; f++) {
            VkImageBlit b;
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.mipLevel = (uint32_t)l - 1;
            b.dstSubresource.mipLevel = (uint32_t)l;
            b.srcSubresource.baseArrayLayer = b.dstSubresource.baseArrayLayer = (uint32_t)f;
            b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
            b.srcOffsets[1].x = t->w >> (l - 1) ? t->w >> (l - 1) : 1;
            b.srcOffsets[1].y = t->h >> (l - 1) ? t->h >> (l - 1) : 1;
            b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = t->w >> l ? t->w >> l : 1;
            b.dstOffsets[1].y = t->h >> l ? t->h >> l : 1;
            b.dstOffsets[1].z = 1;
            vkCmdBlitImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, t->img, VK_IMAGE_LAYOUT_GENERAL,
                           1, &b, VK_FILTER_LINEAR);
        }
        {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &mb, 0, NULL, 0, NULL);
        }
    }
    t->defined = (t->levels >= 32) ? ~0u : ((1u << t->levels) - 1);
}

/* samplers */
static void API gv_GenSamplers(GLsizei n, GLuint *ids)
{
    GLsizei i;
    gen_names(n, ids, (void **)&t_samp, &t_samp_n, sizeof *t_samp);
    for (i = 0; i < n; i++) {
        GSamp *s = GET(t_samp, ids[i]);
        if (!s) continue;
        s->minf = GL_NEAREST_MIPMAP_LINEAR; s->magf = GL_LINEAR;
        s->wrap_s = s->wrap_t = GL_REPEAT;
    }
}
static void API gv_SamplerParameteri(GLuint id, GLenum pname, GLint v)
{
    GSamp *s = id < t_samp_n ? &t_samp[id] : NULL;
    if (!s || !s->used) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: s->minf = v; break;
    case GL_TEXTURE_MAG_FILTER: s->magf = v; break;
    case GL_TEXTURE_WRAP_S: s->wrap_s = v; break;
    case GL_TEXTURE_WRAP_T: s->wrap_t = v; break;
    }
}
static void API gv_BindSampler(GLuint unit, GLuint id) { if (unit < NUNIT) S.samp[unit] = id; }

/* ── framebuffers ───────────────────────────────────────────────────────── */

static void API gv_GenFramebuffers(GLsizei n, GLuint *ids) { gen_names(n, ids, (void **)&t_fbo, &t_fbo_n, sizeof *t_fbo); }
static void API gv_DeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        GFbo *f = ids[i] < t_fbo_n ? &t_fbo[ids[i]] : NULL;
        if (f) memset(f, 0, sizeof *f);
        if (S.draw_fbo == ids[i]) S.draw_fbo = 0;
        if (S.read_fbo == ids[i]) S.read_fbo = 0;
    }
}
static void API gv_BindFramebuffer(GLenum target, GLuint id)
{
    if (target == GL_DRAW_FRAMEBUFFER || target == GL_FRAMEBUFFER) S.draw_fbo = id;
    if (target == GL_READ_FRAMEBUFFER || target == GL_FRAMEBUFFER) S.read_fbo = id;
}
static void attach(GLenum target, GLenum att, GLuint tex)
{
    GFbo *f = GET(t_fbo, target == GL_READ_FRAMEBUFFER ? S.read_fbo : S.draw_fbo);
    if (!f) return;
    if (att == GL_COLOR_ATTACHMENT0) f->col = tex;
    else if (att == GL_DEPTH_STENCIL_ATTACHMENT || att == GL_DEPTH_ATTACHMENT) f->ds = tex;
    if (R.rendering)
        rendering_end();                /* it may be the framebuffer drawn into */
}
static void API gv_FramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)
{
    (void)textarget; (void)level;
    attach(target, att, tex);
}
static void API gv_FramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb)
{
    (void)rbt;
    attach(target, att, rb);
}
static GLenum API gv_CheckFramebufferStatus(GLenum target)
{
    GFbo *f = GET(t_fbo, target == GL_READ_FRAMEBUFFER ? S.read_fbo : S.draw_fbo);
    if (!f || (!tex_of(f->col) && !tex_of(f->ds)))
        return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    return GL_FRAMEBUFFER_COMPLETE;
}

/* the default framebuffer's colour image */
static GTex *def_tex(void)
{
    GTex *t;
    if (!S.def_tex) {
        gv_GenTextures(1, &S.def_tex);
    }
    t = tex_of(S.def_tex);
    if (t && S.def_w > 0 && S.def_h > 0
            && (!t->img || t->w != S.def_w || t->h != S.def_h)) {
        if (tex_alloc(t, GL_TEXTURE_2D, GL_RGBA8, S.def_w, S.def_h, 1, 1, 1))
            t->defined = 1;
    }
    return t && t->img ? t : NULL;
}

/* the colour and depth-stencil textures of a framebuffer name */
static void fbo_tex(GLuint fbo, GTex **col, GTex **ds, GLuint *col_id, GLuint *ds_id)
{
    *col = *ds = NULL;
    *col_id = *ds_id = 0;
    if (!fbo) {
        *col = def_tex();
        *col_id = S.def_tex;
        return;
    }
    if (fbo < t_fbo_n && t_fbo[fbo].used) {
        *col = tex_of(t_fbo[fbo].col);
        *ds = tex_of(t_fbo[fbo].ds);
        if (*col && !(*col)->img) *col = NULL;
        if (*ds && !(*ds)->img) *ds = NULL;
        if (*col) *col_id = t_fbo[fbo].col;
        if (*ds) *ds_id = t_fbo[fbo].ds;
    }
}

static void rendering_end(void)
{
    if (!R.rendering)
        return;
    if (R.qslot != ~0u) {
        vkCmdEndQuery(G.b[G.cur].cmd, G.b[G.cur].qp, R.qslot);
        R.qslot = ~0u;
    }
    vkCmdEndRendering(G.b[G.cur].cmd);
    R.rendering = 0;
    R.pipe = VK_NULL_HANDLE;
}

static void query_slot_begin(void);

/* rendering into the draw framebuffer, begun if it isn't */
static int rendering_begin(void)
{
    GTex *col, *ds;
    GLuint ci, di;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    int w, h;
    fbo_tex(S.draw_fbo, &col, &ds, &ci, &di);
    if (col && col->bc) col = NULL;
    if (!col && !ds)
        return 0;
    if (R.rendering && R.col == ci && R.ds == di)
        return 1;
    rendering_end();
    batch_begin();
    s_n_passes++;
    w = col ? col->w : ds->w;
    h = col ? col->h : ds->h;
    if (col && ds) {
        if (ds->w < w) w = ds->w;
        if (ds->h < h) h = ds->h;
        if (ds->samples != col->samples)
            ds = NULL, di = 0;          /* not drawable together: the colour only */
    }
    /* vk16: into the attachment layouts (a barrier when the last use was
     * another, or wrote: the previous pass into it) */
    if (col)
        img_use(col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 1);
    if (ds)
        img_use(ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
    ri.renderArea.extent.width = (uint32_t)w;
    ri.renderArea.extent.height = (uint32_t)h;
    ri.layerCount = 1;
    if (col) {
        ca.imageView = col->att;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (ds) {
        da.imageView = ds->att;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(G.b[G.cur].cmd, &ri);
    R.rendering = 1;
    R.col = ci;
    R.ds = di;
    R.vw = w;
    R.vh = h;
    R.pipe = VK_NULL_HANDLE;
    if (S.query_active)
        query_slot_begin();
    return 1;
}

/* ── shaders and programs ───────────────────────────────────────────────── */

typedef struct { int used; GLenum type; char *src; } GShader;
TABLE(GShader, t_sh);

/* a uniform of the default block, or a sampler */
typedef struct {
    char name[48];
    int loc;                        /* its first location; elements follow */
    int count;                      /* array elements (1 if not an array) */
    int sampler;                    /* 1: a sampler (binding, below) */
    /* block member */
    uint32_t off, astride, mstride;
    int base;                       /* 0 float, 1 int, 2 uint, 3 bool */
    int comps, cols;                /* vecN, matCxN (cols 1 for vectors) */
    /* sampler */
    uint32_t binding;
    int cube, ms;
    int unit[16];                   /* glUniform1i: the texture unit of each element */
} Uni;

#define NPIPE 64
typedef struct { uint64_t key; VkPipeline p; } PipeSlot;

struct GProg {
    int used;
    GLuint sh[2];
    int linked;
    char *log;
    VkShaderModule vm, fm;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    Uni *u;
    int nu, nloc;
    int *loc_u;                     /* location -> uniform */
    uint8_t *shadow;                /* the default block, as OpenGL keeps it */
    uint32_t ubo_size;
    int ubo_binding;                /* -1: no default block */
    uint32_t inputs;                /* vertex input locations it reads */
    int dirty;
    uint64_t ubo_serial;            /* the batch its uniforms were copied into */
    VkDeviceSize ubo_off;
    VkBuffer ubo_buf;
    PipeSlot *pipes;
    int npipes, cpipes;
};
TABLE(GProg, t_prog);

static GLuint API gv_CreateShader(GLenum type)
{
    GLuint id;
    GShader *s;
    gen_names(1, &id, (void **)&t_sh, &t_sh_n, sizeof *t_sh);
    s = GET(t_sh, id);
    if (s) s->type = type;
    return id;
}
static void API gv_ShaderSource(GLuint id, GLsizei n, const GLchar *const *str, const GLint *len)
{
    GShader *s = id < t_sh_n && t_sh[id].used ? &t_sh[id] : NULL;
    size_t total = 1, k = 0;
    GLsizei i;
    if (!s) return;
    for (i = 0; i < n; i++)
        total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(s->src);
    s->src = (char *)malloc(total);
    if (!s->src) return;
    for (i = 0; i < n; i++) {
        size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        memcpy(s->src + k, str[i], l);
        k += l;
    }
    s->src[k] = 0;
}
static void API gv_CompileShader(GLuint id) { (void)id; }   /* compiled with its program */
static void API gv_GetShaderiv(GLuint id, GLenum pname, GLint *v)
{
    (void)id;
    *v = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}
static void API gv_GetShaderInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
    (void)id;
    if (n > 0) log[0] = 0;
    if (len) *len = 0;
}
static void API gv_DeleteShader(GLuint id)
{
    GShader *s = id < t_sh_n && t_sh[id].used ? &t_sh[id] : NULL;
    if (!s) return;
    free(s->src);
    memset(s, 0, sizeof *s);
}

static GLuint API gv_CreateProgram(void)
{
    GLuint id;
    gen_names(1, &id, (void **)&t_prog, &t_prog_n, sizeof *t_prog);
    return id;
}
static GProg *prog_of(GLuint id) { return id && id < t_prog_n && t_prog[id].used ? &t_prog[id] : NULL; }
static void API gv_AttachShader(GLuint p, GLuint s)
{
    GProg *g = prog_of(p);
    GShader *sh = s < t_sh_n && t_sh[s].used ? &t_sh[s] : NULL;
    if (!g || !sh) return;
    g->sh[sh->type == GL_FRAGMENT_SHADER ? 1 : 0] = s;
}
static void API gv_ProgramParameteri(GLuint p, GLenum pname, GLint v) { (void)p; (void)pname; (void)v; }

static void prog_free_vk(GProg *g)
{
    int i;
    for (i = 0; i < g->npipes; i++)
        junk(4, (uint64_t)g->pipes[i].p, VK_NULL_HANDLE);
    free(g->pipes);
    g->pipes = NULL;
    g->npipes = g->cpipes = 0;
    if (g->vm) junk(5, (uint64_t)g->vm, VK_NULL_HANDLE);
    if (g->fm) junk(5, (uint64_t)g->fm, VK_NULL_HANDLE);
    if (g->pl) junk(6, (uint64_t)g->pl, VK_NULL_HANDLE);
    if (g->dsl) junk(7, (uint64_t)g->dsl, VK_NULL_HANDLE);
    g->vm = g->fm = VK_NULL_HANDLE;
    g->pl = VK_NULL_HANDLE;
    g->dsl = VK_NULL_HANDLE;
    free(g->u); g->u = NULL; g->nu = 0;
    free(g->loc_u); g->loc_u = NULL; g->nloc = 0;
    free(g->shadow); g->shadow = NULL;
    free(g->log); g->log = NULL;
}

static void API gv_DeleteProgram(GLuint id)
{
    GProg *g = prog_of(id);
    if (!g) return;
    prog_free_vk(g);
    memset(g, 0, sizeof *g);
    if (S.prog == id) S.prog = 0;
}

/* -- SPIR-V reflection: the default block's members, the samplers, the
 *    vertex inputs -- */
typedef struct {
    uint32_t bound;
    uint32_t *op;           /* defining opcode of each id */
    uint32_t *a, *b, *c;    /* its first operands (meaning by opcode) */
    const char **name;      /* OpName */
    uint32_t *binding, *set, *loc, *astride, *builtin, *block;
    uint32_t *cval;         /* OpConstant value */
} Refl;

typedef struct { uint32_t type, member; const char *name; uint32_t off, mstride; } RMember;

static int refl_scan(const uint32_t *w, size_t n, Refl *r, RMember **mem, int *nmem)
{
    size_t i;
    int cm = 0;
    memset(r, 0, sizeof *r);
    *mem = NULL;
    *nmem = 0;
    if (n < 5 || w[0] != 0x07230203u)
        return 0;
    r->bound = w[3];
    if (!r->bound || r->bound > (1u << 22))
        return 0;
#define RA(f) r->f = (uint32_t *)calloc(r->bound, 4)
    RA(op); RA(a); RA(b); RA(c); RA(binding); RA(set); RA(loc); RA(astride); RA(builtin); RA(block); RA(cval);
#undef RA
    r->name = (const char **)calloc(r->bound, sizeof(char *));
    if (!r->op || !r->a || !r->b || !r->c || !r->binding || !r->set || !r->loc || !r->astride
            || !r->builtin || !r->block || !r->cval || !r->name)
        return 0;
    for (i = 0; i < r->bound; i++) { r->binding[i] = r->loc[i] = r->builtin[i] = ~0u; }
    for (i = 5; i < n; ) {
        uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
        const uint32_t *o = w + i + 1;
        if (!len || i + len > n) break;
        switch (op) {
        case 5:  /* OpName */
            if (o[0] < r->bound) r->name[o[0]] = (const char *)(o + 1);
            break;
        case 6:  /* OpMemberName */
        case 72: /* OpMemberDecorate */
            if (o[0] >= r->bound) break;
            {
                int k;
                for (k = 0; k < *nmem; k++)
                    if ((*mem)[k].type == o[0] && (*mem)[k].member == o[1]) break;
                if (k == *nmem) {
                    if (*nmem == cm) {
                        RMember *nm;
                        cm = cm ? cm * 2 : 64;
                        nm = (RMember *)realloc(*mem, (size_t)cm * sizeof *nm);
                        if (!nm) return 0;
                        *mem = nm;
                    }
                    memset(&(*mem)[k], 0, sizeof **mem);
                    (*mem)[k].type = o[0];
                    (*mem)[k].member = o[1];
                    (*nmem)++;
                }
                if (op == 6) (*mem)[k].name = (const char *)(o + 2);
                else if (o[2] == 35) (*mem)[k].off = o[3];        /* Offset */
                else if (o[2] == 7) (*mem)[k].mstride = o[3];     /* MatrixStride */
            }
            break;
        case 71: /* OpDecorate */
            if (o[0] >= r->bound) break;
            switch (o[1]) {
            case 33: r->binding[o[0]] = o[2]; break;
            case 34: r->set[o[0]] = o[2]; break;
            case 30: r->loc[o[0]] = o[2]; break;
            case 6:  r->astride[o[0]] = o[2]; break;
            case 11: r->builtin[o[0]] = o[2]; break;
            case 2:  r->block[o[0]] = 1; break;
            }
            break;
        case 20: /* OpTypeBool */
        case 21: /* OpTypeInt: width, signed */
        case 22: /* OpTypeFloat */
        case 23: /* OpTypeVector: comp, n */
        case 24: /* OpTypeMatrix: col, n */
        case 25: /* OpTypeImage: sampled type, dim, depth, arrayed, ms */
        case 27: /* OpTypeSampledImage: image */
        case 28: /* OpTypeArray: elem, length id */
        case 30: /* OpTypeStruct */
        case 32: /* OpTypePointer: storage, type */
            if (o[0] < r->bound) {
                r->op[o[0]] = op;
                if (len > 2) r->a[o[0]] = o[1];
                if (len > 3) r->b[o[0]] = o[2];
                if (op == 25 && len > 5) r->c[o[0]] = o[4];   /* arrayed */
                if (op == 25 && len > 6) r->c[o[0]] |= o[5] << 1;   /* multisampled */
            }
            break;
        case 43: /* OpConstant: type, id, value */
            if (o[1] < r->bound) { r->op[o[1]] = op; r->cval[o[1]] = o[2]; }
            break;
        case 59: /* OpVariable: type, id, storage */
            if (o[1] < r->bound) { r->op[o[1]] = op; r->a[o[1]] = o[0]; r->b[o[1]] = o[2]; }
            break;
        }
        i += len;
    }
    return 1;
}

static void refl_free(Refl *r, RMember *m)
{
    free(r->op); free(r->a); free(r->b); free(r->c); free(r->binding); free(r->set);
    free(r->loc); free(r->astride); free(r->builtin); free(r->block); free(r->cval);
    free((void *)r->name);
    free(m);
}

/* a member type -> base/comps/cols/count/astride */
static void refl_type(const Refl *r, uint32_t t, Uni *u)
{
    u->count = 1;
    u->astride = 0;
    if (t < r->bound && r->op[t] == 28) {      /* array */
        uint32_t len = r->b[t];
        u->count = len < r->bound ? (int)r->cval[len] : 1;
        u->astride = r->astride[t];
        t = r->a[t];
    }
    u->cols = 1;
    if (t < r->bound && r->op[t] == 24) {      /* matrix */
        u->cols = (int)r->b[t];
        t = r->a[t];
    }
    u->comps = 1;
    if (t < r->bound && r->op[t] == 23) {      /* vector */
        u->comps = (int)r->b[t];
        t = r->a[t];
    }
    u->base = 0;
    if (t < r->bound) {
        if (r->op[t] == 21) u->base = r->b[t] ? 1 : 2;
        else if (r->op[t] == 20) u->base = 3;
    }
}

static Uni *uni_add(GProg *g, const char *name)
{
    int i;
    Uni *u;
    for (i = 0; i < g->nu; i++)
        if (!strcmp(g->u[i].name, name))
            return &g->u[i];
    u = (Uni *)realloc(g->u, (size_t)(g->nu + 1) * sizeof *u);
    if (!u) return NULL;
    g->u = u;
    u = &g->u[g->nu++];
    memset(u, 0, sizeof *u);
    snprintf(u->name, sizeof u->name, "%s", name);
    u->loc = -1;
    return u;
}

/* both stages' reflection into g: uniforms, samplers, inputs */
static int prog_reflect(GProg *g, const uint32_t *v, size_t vn, const uint32_t *f, size_t fn)
{
    int stage, i, k;
    g->ubo_binding = -1;
    g->ubo_size = 0;
    g->inputs = 0;
    for (stage = 0; stage < 2; stage++) {
        Refl r;
        RMember *m;
        int nm;
        uint32_t id;
        if (!refl_scan(stage ? f : v, stage ? fn : vn, &r, &m, &nm)) {
            refl_free(&r, m);
            return 0;
        }
        for (id = 1; id < r.bound; id++) {
            uint32_t pt, t, sc;
            if (r.op[id] != 59) continue;
            pt = r.a[id];
            sc = r.b[id];
            if (pt >= r.bound || r.op[pt] != 32) continue;
            t = r.b[pt];
            if (sc == 1 && !stage && r.loc[id] != ~0u && r.builtin[id] == ~0u && r.loc[id] < 32) {
                g->inputs |= 1u << r.loc[id];               /* a vertex input */
            } else if (sc == 2 && t < r.bound && r.op[t] == 30 && r.set[id] == 0
                       && r.binding[id] != ~0u) {           /* the default block */
                int k2;
                g->ubo_binding = (int)r.binding[id];
                for (k2 = 0; k2 < nm; k2++) {
                    RMember *mm = &m[k2];
                    Uni tmp, *u;
                    uint32_t mt = 0;
                    size_t ww;
                    if (mm->type != t || !mm->name) continue;
                    /* the member's type: the struct's operand list */
                    for (ww = 5; ww < (stage ? fn : vn); ) {
                        const uint32_t *code = stage ? f : v;
                        uint32_t op = code[ww] & 0xFFFF, len = code[ww] >> 16;
                        if (!len) break;
                        if (op == 30 && code[ww + 1] == t && 2 + mm->member < len) {
                            mt = code[ww + 2 + mm->member];
                            break;
                        }
                        ww += len;
                    }
                    memset(&tmp, 0, sizeof tmp);
                    refl_type(&r, mt, &tmp);
                    u = uni_add(g, mm->name);
                    if (!u) continue;
                    if (u->loc >= 0 && u->off != mm->off)
                        GV_ONCE(4, "uniform %s: the two stages put it at %u and %u\n",
                                mm->name, u->off, mm->off);
                    u->off = mm->off;
                    u->mstride = mm->mstride;
                    u->base = tmp.base; u->comps = tmp.comps; u->cols = tmp.cols;
                    u->count = tmp.count; u->astride = tmp.astride;
                    u->loc = 0;             /* numbered below */
                    {
                        uint32_t elem = (uint32_t)(u->cols > 1 ? u->mstride * u->cols : u->comps * 4);
                        uint32_t end = u->off + (u->count > 1 ? u->astride * (uint32_t)(u->count - 1) : 0) + elem;
                        if (end > g->ubo_size) g->ubo_size = end;
                    }
                }
            } else if (sc == 0 && r.binding[id] != ~0u && r.name[id]) {   /* a sampler */
                Uni *u;
                uint32_t st = t;
                int count = 1;
                if (st < r.bound && r.op[st] == 28) {
                    count = r.b[st] < r.bound ? (int)r.cval[r.b[st]] : 1;
                    st = r.a[st];
                }
                if (st >= r.bound || r.op[st] != 27) continue;
                st = r.a[st];               /* the image type */
                u = uni_add(g, r.name[id]);
                if (!u) continue;
                u->sampler = 1;
                u->binding = r.binding[id];
                u->count = count > 16 ? 16 : count;
                u->cube = st < r.bound && r.b[st] == 3;
                u->ms = st < r.bound && (r.c[st] >> 1);
                u->loc = 0;
            }
        }
        refl_free(&r, m);
    }
    /* locations: each element its own, as OpenGL numbers them */
    g->nloc = 0;
    for (i = 0; i < g->nu; i++) g->nloc += g->u[i].count;
    g->loc_u = (int *)malloc((size_t)(g->nloc ? g->nloc : 1) * sizeof(int));
    if (!g->loc_u) return 0;
    for (i = 0, k = 0; i < g->nu; i++) {
        int e;
        g->u[i].loc = k;
        for (e = 0; e < g->u[i].count; e++)
            g->loc_u[k++] = i;
    }
    g->shadow = (uint8_t *)calloc(g->ubo_size ? g->ubo_size : 16, 1);
    return g->shadow != NULL;
}

/* OpenGL's clip-space depth (-w..w) into Vulkan's (0..w), after main */
static char *patch_vs(const char *src)
{
    const char *m = strstr(src, "void main()");
    size_t n = strlen(src);
    char *o;
    static const char tail[] =
        "\nvoid main() { glvk_main(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n";
    if (!m)
        m = strstr(src, "void main(void)");
    if (!m)
        return strdup(src);
    o = (char *)malloc(n + sizeof tail + 16);
    if (!o) return NULL;
    memcpy(o, src, (size_t)(m - src));
    strcpy(o + (m - src), "void glvk_main()");
    strcat(o, m + (strncmp(m, "void main(void)", 15) ? 11 : 15));
    strcat(o, tail);
    return o;
}

static VkShaderModule module_of(const uint32_t *w, size_t n)
{
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    mi.codeSize = n * 4;
    mi.pCode = w;
    vkCreateShaderModule(G.c.dev, &mi, NULL, &m);
    return m;
}

static uint64_t s_n_links, s_n_links_cached;
static double s_link_ms;

static void API gv_LinkProgram(GLuint id)
{
    GProg *g = prog_of(id);
    GShader *vs, *fs;
    char *vsrc;
    uint32_t *vw = NULL, *fw = NULL;
    size_t vn = 0, fn = 0;
    int cached = 0, i;
    char *err = NULL;
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (!g) return;
    prog_free_vk(g);
    g->linked = 0;
    vs = g->sh[0] < t_sh_n && t_sh[g->sh[0]].used ? &t_sh[g->sh[0]] : NULL;
    fs = g->sh[1] < t_sh_n && t_sh[g->sh[1]].used ? &t_sh[g->sh[1]] : NULL;
    if (!vs || !fs || !vs->src || !fs->src) {
        g->log = strdup("glvk: a program needs a vertex and a fragment shader");
        return;
    }
    vsrc = patch_vs(vs->src);
    if (!vsrc || !nv2a_vk_spirv(vsrc, fs->src, &vw, &vn, &fw, &fn, &cached, &err)) {
        g->log = err ? err : strdup("glvk: GLSL -> SPIR-V failed");
        free(vsrc);
        return;
    }
    free(vsrc);
    if (!prog_reflect(g, vw, vn, fw, fn)) {
        g->log = strdup("glvk: the SPIR-V could not be read");
        free(vw); free(fw);
        return;
    }
    g->vm = module_of(vw, vn);
    g->fm = module_of(fw, fn);
    free(vw); free(fw);
    {
        VkDescriptorSetLayoutBinding lb[40];
        VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        uint32_t nb = 0;
        if (g->ubo_binding >= 0) {
            memset(&lb[nb], 0, sizeof lb[nb]);
            lb[nb].binding = (uint32_t)g->ubo_binding;
            lb[nb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            lb[nb].descriptorCount = 1;
            lb[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            nb++;
        }
        for (i = 0; i < g->nu && nb < 40; i++) {
            if (!g->u[i].sampler) continue;
            memset(&lb[nb], 0, sizeof lb[nb]);
            lb[nb].binding = g->u[i].binding;
            lb[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            lb[nb].descriptorCount = (uint32_t)g->u[i].count;
            lb[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            nb++;
        }
        di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        di.bindingCount = nb;
        di.pBindings = lb;
        if (vkCreateDescriptorSetLayout(G.c.dev, &di, NULL, &g->dsl) != VK_SUCCESS) {
            g->log = strdup("glvk: descriptor set layout failed");
            return;
        }
        li.setLayoutCount = 1;
        li.pSetLayouts = &g->dsl;
        if (vkCreatePipelineLayout(G.c.dev, &li, NULL, &g->pl) != VK_SUCCESS) {
            g->log = strdup("glvk: pipeline layout failed");
            return;
        }
    }
    if (!g->vm || !g->fm) {
        g->log = strdup("glvk: shader modules failed");
        return;
    }
    g->linked = 1;
    g->dirty = 1;
    s_n_links++;
    s_n_links_cached += cached;
    s_link_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0
                 / (double)SDL_GetPerformanceFrequency();
}

static void API gv_GetProgramiv(GLuint id, GLenum pname, GLint *v)
{
    GProg *g = prog_of(id);
    *v = 0;
    if (!g) return;
    switch (pname) {
    case GL_LINK_STATUS: *v = g->linked ? GL_TRUE : GL_FALSE; break;
    case GL_ACTIVE_UNIFORMS: *v = g->nu; break;
    case GL_INFO_LOG_LENGTH: *v = g->log ? (GLint)strlen(g->log) + 1 : 0; break;
    }
}
static void API gv_GetProgramInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
    GProg *g = prog_of(id);
    const char *s = g && g->log ? g->log : "";
    if (n > 0) {
        strncpy(log, s, (size_t)n - 1);
        log[n - 1] = 0;
    }
    if (len) *len = (GLsizei)strlen(s);
}

static void API gv_UseProgram(GLuint id) { S.prog = id; }

static GLint API gv_GetUniformLocation(GLuint id, const GLchar *name)
{
    GProg *g = prog_of(id);
    char base[64];
    const char *br;
    int idx = 0, i;
    if (!g || !g->linked) return -1;
    snprintf(base, sizeof base, "%s", name);
    br = strchr(base, '[');
    if (br) {
        idx = atoi(br + 1);
        base[br - base] = 0;
    }
    for (i = 0; i < g->nu; i++)
        if (!strcmp(g->u[i].name, base)) {
            if (idx < 0 || idx >= g->u[i].count) return -1;
            return g->u[i].loc + idx;
        }
    return -1;
}

static void API gv_GetActiveUniform(GLuint id, GLuint index, GLsizei n, GLsizei *len, GLint *size,
                                    GLenum *type, GLchar *name)
{
    GProg *g = prog_of(id);
    Uni *u;
    char nm[64];
    if (!g || (int)index >= g->nu) { if (n > 0) name[0] = 0; return; }
    u = &g->u[index];
    snprintf(nm, sizeof nm, u->count > 1 ? "%s[0]" : "%s", u->name);
    if (n > 0) { strncpy(name, nm, (size_t)n - 1); name[n - 1] = 0; }
    if (len) *len = (GLsizei)strlen(nm);
    *size = u->count;
    if (u->sampler) *type = u->cube ? GL_SAMPLER_CUBE : GL_SAMPLER_2D;
    else if (u->cols == 4) *type = GL_FLOAT_MAT4;
    else if (u->base == 0) *type = u->comps == 4 ? GL_FLOAT_VEC4 : u->comps == 3 ? GL_FLOAT_VEC3
                                 : u->comps == 2 ? GL_FLOAT_VEC2 : GL_FLOAT;
    else *type = u->base == 2 ? GL_UNSIGNED_INT : GL_INT;
}

/* glUniform*: `count` elements of `comps` values each (4-byte), from loc on */
static void uni_set(GLint loc, GLsizei count, int comps, int kind, const void *vals, int transpose)
{
    GProg *g = prog_of(S.prog);
    Uni *u;
    int e0, e;
    const uint32_t *src = (const uint32_t *)vals;
    if (!g || loc < 0 || loc >= g->nloc) return;
    u = &g->u[g->loc_u[loc]];
    e0 = loc - u->loc;
    if (u->sampler) {
        for (e = 0; e < count && e0 + e < u->count; e++)
            u->unit[e0 + e] = kind == 2 ? (int)((const float *)vals)[e] : ((const int *)vals)[e];
        return;
    }
    if (u->cols <= 1 && kind == 2 && u->base == 0 && comps == u->comps) {
        /* v311g-vk12: floats into floats (glUniform4fv and the like, most of
         * the calls): copied whole, element by element at most */
        int n = count;
        uint8_t *dst = g->shadow + u->off + (uint32_t)e0 * u->astride;
        if (n > u->count - e0) n = u->count - e0;
        if (n <= 0) return;
        if (u->astride == (uint32_t)comps * 4)
            memcpy(dst, src, (size_t)n * comps * 4);
        else
            for (e = 0; e < n; e++)
                memcpy(dst + (uint32_t)e * u->astride, src + (size_t)e * comps, (size_t)comps * 4);
        g->dirty = 1;
        return;
    }
    for (e = 0; e < count && e0 + e < u->count; e++) {
        uint8_t *dst = g->shadow + u->off + (uint32_t)(e0 + e) * u->astride;
        if (u->cols > 1) {
            int c, r2;
            for (c = 0; c < u->cols; c++)
                for (r2 = 0; r2 < u->comps; r2++) {
                    uint32_t v = transpose ? src[(size_t)e * comps + r2 * u->cols + c]
                                           : src[(size_t)e * comps + c * u->comps + r2];
                    memcpy(dst + c * u->mstride + r2 * 4, &v, 4);
                }
        } else {
            int c;
            for (c = 0; c < comps && c < u->comps; c++) {
                uint32_t v = src[(size_t)e * comps + c];
                if (u->base == 0 && kind == 1) {          /* an int into a float */
                    float fv = (float)(int32_t)v; memcpy(&v, &fv, 4);
                } else if (u->base != 0 && kind == 2) {   /* a float into an int */
                    float fv; memcpy(&fv, &v, 4); v = (uint32_t)(int32_t)fv;
                }
                if (u->base == 3) v = v != 0;
                memcpy(dst + c * 4, &v, 4);
            }
        }
    }
    g->dirty = 1;
}

static void API gv_Uniform1i(GLint l, GLint a) { uni_set(l, 1, 1, 1, &a, 0); }
static void API gv_Uniform1ui(GLint l, GLuint a) { uni_set(l, 1, 1, 0, &a, 0); }
static void API gv_Uniform1f(GLint l, GLfloat a) { uni_set(l, 1, 1, 2, &a, 0); }
static void API gv_Uniform2f(GLint l, GLfloat a, GLfloat b) { float v[2] = { a, b }; uni_set(l, 1, 2, 2, v, 0); }
static void API gv_Uniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { float v[3] = { a, b, c }; uni_set(l, 1, 3, 2, v, 0); }
static void API gv_Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { float v[4] = { a, b, c, d }; uni_set(l, 1, 4, 2, v, 0); }
static void API gv_Uniform1iv(GLint l, GLsizei n, const GLint *v) { uni_set(l, n, 1, 1, v, 0); }
static void API gv_Uniform1uiv(GLint l, GLsizei n, const GLuint *v) { uni_set(l, n, 1, 0, v, 0); }
static void API gv_Uniform1fv(GLint l, GLsizei n, const GLfloat *v) { uni_set(l, n, 1, 2, v, 0); }
static void API gv_Uniform2fv(GLint l, GLsizei n, const GLfloat *v) { uni_set(l, n, 2, 2, v, 0); }
static void API gv_Uniform3fv(GLint l, GLsizei n, const GLfloat *v) { uni_set(l, n, 3, 2, v, 0); }
static void API gv_Uniform4fv(GLint l, GLsizei n, const GLfloat *v) { uni_set(l, n, 4, 2, v, 0); }
static void API gv_UniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat *v) { uni_set(l, n, 16, 2, v, tr); }

/* ── fixed state ────────────────────────────────────────────────────────── */

static int *cap_of(GLenum cap)
{
    static int dummy;
    switch (cap) {
    case GL_DEPTH_TEST: return &S.depth_test;
    case GL_STENCIL_TEST: return &S.stencil_test;
    case GL_BLEND: return &S.blend;
    case GL_CULL_FACE: return &S.cull;
    case GL_SCISSOR_TEST: return &S.scissor;
    case GL_POLYGON_OFFSET_FILL: return &S.poff;
    case GL_DEPTH_CLAMP: return &S.dclamp;
    default: return &dummy;             /* clip distances: always on in Vulkan */
    }
}
static void API gv_Enable(GLenum cap) { *cap_of(cap) = 1; }
static void API gv_Disable(GLenum cap) { *cap_of(cap) = 0; }
static GLboolean API gv_IsEnabled(GLenum cap) { return *cap_of(cap) ? GL_TRUE : GL_FALSE; }
static void API gv_DepthFunc(GLenum f) { S.depth_func = f; }
static void API gv_DepthMask(GLboolean m) { S.depth_mask = m; }
static void API gv_StencilFunc(GLenum f, GLint ref, GLuint mask) { S.st_func = f; S.st_ref = ref; S.st_mask = mask; }
static void API gv_StencilOp(GLenum a, GLenum b, GLenum c) { S.st_fail = a; S.st_zfail = b; S.st_zpass = c; }
static void API gv_StencilMask(GLuint m) { S.st_wmask = m; }
static void API gv_BlendFunc(GLenum s, GLenum d) { S.bl_src = s; S.bl_dst = d; }
static void API gv_BlendEquation(GLenum e) { S.bl_eq = e; }
static void API gv_BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.bl_col[0] = r; S.bl_col[1] = g; S.bl_col[2] = b; S.bl_col[3] = a; }
static void API gv_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.cmask[0] = r; S.cmask[1] = g; S.cmask[2] = b; S.cmask[3] = a; }
static void API gv_CullFace(GLenum f) { S.cull_face = f; }
static void API gv_FrontFace(GLenum f) { S.front_face = f; }
static void API gv_PolygonOffset(GLfloat f, GLfloat u) { S.po_factor = f; S.po_units = u; }
static void API gv_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
static void API gv_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
static void API gv_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clear_col[0] = r; S.clear_col[1] = g; S.clear_col[2] = b; S.clear_col[3] = a; }
static void API gv_ClearDepth(GLdouble d) { S.clear_depth = d; }
static void API gv_ClearStencil(GLint s) { S.clear_stencil = s; }
static void API gv_PixelStorei(GLenum p, GLint v)
{
    if (p == GL_UNPACK_ALIGNMENT) S.unpack_align = v;
    else if (p == GL_PACK_ALIGNMENT) S.pack_align = v;
}

static VkCompareOp vk_cmp(GLenum f)
{
    switch (f) {
    case GL_NEVER: return VK_COMPARE_OP_NEVER;
    case GL_LESS: return VK_COMPARE_OP_LESS;
    case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp vk_sop(GLenum o)
{
    switch (o) {
    case GL_ZERO: return VK_STENCIL_OP_ZERO;
    case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
    case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case GL_INVERT: return VK_STENCIL_OP_INVERT;
    case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
static VkBlendFactor vk_bf(GLenum f)
{
    switch (f) {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_bo(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GL_MIN: return VK_BLEND_OP_MIN;
    case GL_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkSamplerAddressMode vk_wrap(GLint w)
{
    switch (w) {
    case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* ── samplers (Vulkan's), by their settings ─────────────────────────────── */

#define NSAMPLERS 256
static struct { uint32_t key; VkSampler s; } s_samplers[NSAMPLERS];
static int s_nsamplers;

static VkSampler sampler_for(GLint minf, GLint magf, GLint ws, GLint wt, GLint wr, int maxlod, int aniso)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    uint32_t key;
    int i, mip = 0;
    VkSampler s = VK_NULL_HANDLE;
    if (aniso < 1) aniso = 1;
    if (aniso > 16) aniso = 16;
    if (maxlod > 15) maxlod = 15;
    key = (uint32_t)(minf & 0xF) | (uint32_t)(magf & 1) << 4 | (uint32_t)vk_wrap(ws) << 5
        | (uint32_t)vk_wrap(wt) << 8 | (uint32_t)vk_wrap(wr) << 11 | (uint32_t)maxlod << 14
        | (uint32_t)aniso << 18;
    for (i = 0; i < s_nsamplers; i++)
        if (s_samplers[i].key == key)
            return s_samplers[i].s;
    si.magFilter = magf == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    switch (minf) {
    case GL_NEAREST: si.minFilter = VK_FILTER_NEAREST; break;
    case GL_LINEAR: si.minFilter = VK_FILTER_LINEAR; break;
    case GL_NEAREST_MIPMAP_NEAREST: si.minFilter = VK_FILTER_NEAREST; mip = 1; break;
    case GL_LINEAR_MIPMAP_NEAREST: si.minFilter = VK_FILTER_LINEAR; mip = 1; break;
    case GL_NEAREST_MIPMAP_LINEAR: si.minFilter = VK_FILTER_NEAREST; mip = 2; break;
    default: si.minFilter = VK_FILTER_LINEAR; mip = 2; break;
    }
    si.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.minLod = 0.0f;
    si.maxLod = mip ? (float)maxlod : 0.25f;     /* no mips: level 0 only, as OpenGL */
    si.addressModeU = vk_wrap(ws);
    si.addressModeV = vk_wrap(wt);
    si.addressModeW = vk_wrap(wr);
    if (aniso > 1 && G.max_aniso > 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = (float)aniso < G.max_aniso ? (float)aniso : G.max_aniso;
    }
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(G.c.dev, &si, NULL, &s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (s_nsamplers < NSAMPLERS) {
        s_samplers[s_nsamplers].key = key;
        s_samplers[s_nsamplers].s = s;
        s_nsamplers++;
    }
    return s;
}

/* ── pipelines ──────────────────────────────────────────────────────────── */

static VkFormat attr_format(const GAttr *a)
{
    static const VkFormat ff[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                    VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat ub[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat sh[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                    VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    int n = a->size >= 1 && a->size <= 4 ? a->size - 1 : 3;
    if (a->size == GL_BGRA) return VK_FORMAT_B8G8R8A8_UNORM;
    switch (a->type) {
    case GL_UNSIGNED_BYTE: return ub[n];
    case GL_SHORT: return sh[n];
    default: return ff[n];
    }
}
static uint32_t attr_bytes(const GAttr *a)
{
    int n = a->size == GL_BGRA ? 4 : a->size;
    return (uint32_t)n * (a->type == GL_UNSIGNED_BYTE ? 1 : a->type == GL_SHORT ? 2 : 4);
}

typedef struct {
    VkFormat vf[NATTR];
    uint32_t vstride[NATTR];
    uint32_t vrel[NATTR];           /* the attribute's offset in its vertex */
    VkFormat cf, df;
    int samples;
    int blend;
    GLenum bs, bd, be;
    int cmask;
} PKey;

static uint64_t pkey_hash(const PKey *k)
{
    const unsigned char *p = (const unsigned char *)k;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < sizeof *k; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h ? h : 1;
}

/* a pipeline from the program's modules, layout and inputs (copied, so the
 * Vulkan backend can build on a worker thread: nv2a_rb_vk.c) */
static VkPipeline pipe_make_x(VkShaderModule vm, VkShaderModule fm, VkPipelineLayout pl,
                              uint32_t inputs, const PKey *k, int dclamp)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    VkVertexInputBindingDescription vb[NATTR];
    VkVertexInputAttributeDescription va[NATTR];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p = VK_NULL_HANDLE;
    uint32_t i, n = 0;
    for (i = 0; i < 2; i++) {
        st[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        st[i].module = i ? fm : vm;
        st[i].pName = "main";
    }
    for (i = 0; i < NATTR; i++) {
        if (!(inputs & (1u << i))) continue;
        vb[n].binding = i;
        vb[n].stride = k->vstride[i];
        vb[n].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        va[n].location = i;
        va[n].binding = i;
        va[n].format = k->vf[i];
        va[n].offset = k->vrel[i];
        n++;
    }
    vi.vertexBindingDescriptionCount = n;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = n;
    vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vs.viewportCount = 1;
    vs.scissorCount = 1;
    rs.depthClampEnable = dclamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)k->samples;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = vk_bf(k->bs);
    cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = vk_bf(k->bd);
    cba.colorBlendOp = cba.alphaBlendOp = vk_bo(k->be);
    cba.colorWriteMask = (VkColorComponentFlags)k->cmask;
    cb.attachmentCount = k->cf ? 1 : 0;
    cb.pAttachments = &cba;
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
    dy.pDynamicStates = dyn;
    pr.colorAttachmentCount = k->cf ? 1 : 0;
    pr.pColorAttachmentFormats = &k->cf;
    pr.depthAttachmentFormat = k->df;
    pr.stencilAttachmentFormat = k->df;
    gi.pNext = &pr;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vs;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = pl;
    if (vkCreateGraphicsPipelines(G.c.dev, G.pcache, 1, &gi, NULL, &p) != VK_SUCCESS) {
        GV_ONCE(8, "a pipeline could not be made\n");
        return VK_NULL_HANDLE;
    }
    return p;
}

static VkPipeline pipe_make(GProg *g, const PKey *k)
{
    VkPipeline p = pipe_make_x(g->vm, g->fm, g->pl, g->inputs, k, S.dclamp);
    if (p)
        s_n_pipes++;
    return p;
}

static VkPipeline pipe_get(GProg *g, const PKey *k)
{
    uint64_t h = pkey_hash(k);
    int i;
    VkPipeline p;
    for (i = 0; i < g->npipes; i++)
        if (g->pipes[i].key == h)
            return g->pipes[i].p;
    p = pipe_make(g, k);
    if (!p)
        return VK_NULL_HANDLE;
    if (g->npipes == g->cpipes) {
        int nc = g->cpipes ? g->cpipes * 2 : 8;
        PipeSlot *np = (PipeSlot *)realloc(g->pipes, (size_t)nc * sizeof *np);
        if (!np) return p;
        g->pipes = np;
        g->cpipes = nc;
    }
    g->pipes[g->npipes].key = h;
    g->pipes[g->npipes].p = p;
    g->npipes++;
    return p;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static GTex *s_white2d, *s_whitecube;
static GLuint s_white2d_id, s_whitecube_id;

/* the texture a sampler element reads (a white one when it can't be:
 * missing, undefined, the wrong kind, or the target being drawn into) */
static GTex *sampled_tex(const Uni *u, int e, GTex *col, int note)
{
    int unit = u->unit[e] & (NUNIT - 1);
    GTex *t = tex_of(u->cube ? S.texcube[unit] : S.tex2d[unit]);
    if (!t || !t->img || !t->view || t->ds || !(t->defined & 1)
            || (u->cube ? t->layers != 6 : t->layers != 1))
        return u->cube ? s_whitecube : s_white2d;
    if (t == col) {
        if (note) GV_ONCE(4, "a texture is sampled while drawn into (left out)\n");
        return u->cube ? s_whitecube : s_white2d;
    }
    return t;
}

/* vk16: every texture the draw samples in SHADER_READ_ONLY, before its
 * pass begins (a barrier ends one, so this comes first) */
static void sampling_ready(GProg *g)
{
    GTex *col, *ds;
    GLuint ci, di;
    int i, e;
    fbo_tex(S.draw_fbo, &col, &ds, &ci, &di);
    if (col && col->bc) col = NULL;
    for (i = 0; i < g->nu; i++) {
        const Uni *u = &g->u[i];
        if (!u->sampler) continue;
        for (e = 0; e < u->count; e++) {
            GTex *t = sampled_tex(u, e, col, 0);
            if (t && (t->lay != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t->wr))
                IMG_SAMPLED(t);
        }
    }
}

/* everything a draw needs bound; 0: nothing can be drawn */
static int draw_setup(void)
{
    GProg *g = prog_of(S.prog);
    GVao *v = GET(t_vao, S.vao);
    GTex *col, *ds;
    GLuint ci, di;
    PKey k;
    VkPipeline p;
    VkCommandBuffer c;
    VkBuffer vbufs[NATTR];
    VkDeviceSize voffs[NATTR];
    int i;
    if (!g || !g->linked || !v)
        return 0;
    if (S.vp[2] <= 0 || S.vp[3] <= 0)
        return 0;
    sampling_ready(g);
    if (!rendering_begin())
        return 0;
    fbo_tex(S.draw_fbo, &col, &ds, &ci, &di);
    if (col && col->bc) col = NULL;
    if (R.ds == 0) ds = NULL;
    memset(&k, 0, sizeof k);
    k.cf = col ? col->fmt : VK_FORMAT_UNDEFINED;
    k.df = ds ? ds->fmt : VK_FORMAT_UNDEFINED;
    k.samples = col ? col->samples : ds ? ds->samples : 1;
    k.blend = S.blend;
    if (S.blend) { k.bs = S.bl_src; k.bd = S.bl_dst; k.be = S.bl_eq; }
    k.cmask = (S.cmask[0] ? 1 : 0) | (S.cmask[1] ? 2 : 0) | (S.cmask[2] ? 4 : 0)
            | (S.cmask[3] && !(col && col->rgb) ? 8 : 0);     /* RGB targets keep alpha 1 */
    /* vertex inputs: an array, or the generic value as a one-vertex buffer */
    {
        Ring *r = NULL;
        VkDeviceSize co = 0;
        int nconst = 0;
        for (i = 0; i < NATTR; i++)
            if ((g->inputs & (1u << i)) && !(v->a[i].enabled && buf_of(v->a[i].buf)))
                nconst++;
        if (nconst) {
            co = ring_take(0, (VkDeviceSize)nconst * 16, 16, &r);
            if (!r) return 0;
        }
        for (i = 0; i < NATTR; i++) {
            if (!(g->inputs & (1u << i))) continue;
            if (v->a[i].enabled && buf_of(v->a[i].buf)) {
                GBuf *b = buf_of(v->a[i].buf);
                VkDeviceSize off = (VkDeviceSize)v->a[i].off;
                k.vf[i] = attr_format(&v->a[i]);
                k.vstride[i] = v->a[i].stride ? (uint32_t)v->a[i].stride : attr_bytes(&v->a[i]);
                /* bound at the start of a vertex, the attribute's place in
                 * it in the pipeline: drivers (AMD's) count the vertices a
                 * binding holds from its offset, and an offset inside the
                 * vertex loses the buffer's last one */
                k.vrel[i] = (uint32_t)(off % k.vstride[i]);
                if (k.vrel[i] > G.c.props.limits.maxVertexInputAttributeOffset)
                    k.vrel[i] = 0;
                vbufs[i] = b->b;
                voffs[i] = off - k.vrel[i];
            } else {
                memcpy(r->map + co, S.generic[i], 16);
                k.vf[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
                k.vstride[i] = 0;
                vbufs[i] = r->b;
                voffs[i] = co;
                co += 16;
            }
        }
    }
    p = pipe_get(g, &k);
    if (!p)
        return 0;
    c = G.b[G.cur].cmd;
    if (R.pipe != p) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        R.pipe = p;
    }
    for (i = 0; i < NATTR; i++)
        if (g->inputs & (1u << i))
            vkCmdBindVertexBuffers(c, (uint32_t)i, 1, &vbufs[i], &voffs[i]);
    /* dynamic state */
    {
        VkViewport vp;
        VkRect2D sc;
        vp.x = (float)S.vp[0]; vp.y = (float)S.vp[1];
        vp.width = (float)S.vp[2]; vp.height = (float)S.vp[3];
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(c, 0, 1, &vp);
        if (S.scissor) {
            int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1];
            int x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
            if (x1 > R.vw) x1 = R.vw;
            if (y1 > R.vh) y1 = R.vh;
            sc.offset.x = x0; sc.offset.y = y0;
            sc.extent.width = (uint32_t)(x1 > x0 ? x1 - x0 : 0);
            sc.extent.height = (uint32_t)(y1 > y0 ? y1 - y0 : 0);
        } else {
            sc.offset.x = sc.offset.y = 0;
            sc.extent.width = (uint32_t)R.vw;
            sc.extent.height = (uint32_t)R.vh;
        }
        if (!sc.extent.width || !sc.extent.height)
            return 0;
        vkCmdSetScissor(c, 0, 1, &sc);
        vkCmdSetPrimitiveTopology(c, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdSetCullMode(c, !S.cull ? VK_CULL_MODE_NONE
                         : S.cull_face == GL_FRONT ? VK_CULL_MODE_FRONT_BIT
                         : S.cull_face == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK
                         : VK_CULL_MODE_BACK_BIT);
        /* OpenGL's area sign is the opposite of Vulkan's for the same rows */
        vkCmdSetFrontFace(c, S.front_face == GL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                   : VK_FRONT_FACE_CLOCKWISE);
        vkCmdSetDepthTestEnable(c, S.depth_test && ds ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(c, S.depth_test && S.depth_mask && ds ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(c, vk_cmp(S.depth_func));
        vkCmdSetStencilTestEnable(c, S.stencil_test && ds ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, vk_sop(S.st_fail), vk_sop(S.st_zpass),
                          vk_sop(S.st_zfail), vk_cmp(S.st_func));
        vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, S.st_mask & 0xFF);
        vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, S.st_wmask & 0xFF);
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)S.st_ref & 0xFF);
        vkCmdSetDepthBiasEnable(c, S.poff ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBias(c, S.po_units, 0.0f, S.po_factor);
        vkCmdSetBlendConstants(c, S.bl_col);
    }
    /* the uniforms, when they changed or this batch hasn't got them */
    {
        VkWriteDescriptorSet w[40];
        VkDescriptorBufferInfo bi;
        VkDescriptorImageInfo ii[40 * 4];
        uint32_t nw = 0, nii = 0;
        if (g->ubo_binding >= 0) {
            if (g->dirty || g->ubo_serial != G.b[G.cur].serial || !g->ubo_buf) {
                Ring *r;
                VkDeviceSize o = ring_take(0, g->ubo_size ? g->ubo_size : 16, G.ubo_align, &r);
                if (!r) return 0;
                memcpy(r->map + o, g->shadow, g->ubo_size);
                g->ubo_off = o;
                g->ubo_buf = r->b;
                g->ubo_serial = G.b[G.cur].serial;
                g->dirty = 0;
            }
            bi.buffer = g->ubo_buf;
            bi.offset = g->ubo_off;
            bi.range = g->ubo_size ? g->ubo_size : 16;
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstBinding = (uint32_t)g->ubo_binding;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[nw].pBufferInfo = &bi;
            nw++;
        }
        for (i = 0; i < g->nu && nw < 40; i++) {
            Uni *u = &g->u[i];
            int e;
            if (!u->sampler) continue;
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstBinding = u->binding;
            w[nw].descriptorCount = (uint32_t)u->count;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[nw].pImageInfo = &ii[nii];
            for (e = 0; e < u->count && nii < 160; e++) {
                int unit = u->unit[e] & (NUNIT - 1);
                GTex *t = sampled_tex(u, e, col, 1);
                GSamp *sm = S.samp[unit] && S.samp[unit] < t_samp_n && t_samp[S.samp[unit]].used
                            ? &t_samp[S.samp[unit]] : NULL;
                int maxlod = 0;
                while (maxlod + 1 < t->levels && (t->defined >> (maxlod + 1) & 1))
                    maxlod++;
                if (maxlod > t->maxlev) maxlod = t->maxlev;
                if (t->swz_a1 != t->view_swz) {          /* the alpha swizzle changed */
                    junk(2, (uint64_t)t->view, VK_NULL_HANDLE);
                    t->view = make_view(t, 1, t->swz_a1);
                    t->view_swz = t->swz_a1;
                }
                ii[nii].imageView = t->view;
                ii[nii].imageLayout = t->lay;
                ii[nii].sampler = sm ? sampler_for(sm->minf, sm->magf, sm->wrap_s, sm->wrap_t,
                                                   GL_CLAMP_TO_EDGE, maxlod, 1)
                                     : sampler_for(t->minf, t->magf, t->wrap_s, t->wrap_t, t->wrap_r,
                                                   maxlod, t->aniso);
                nii++;
            }
            nw++;
        }
        if (nw)
            vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, nw, w);
    }
    s_n_draws++;
    return 1;
}

static void API gv_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (mode != GL_TRIANGLES || count <= 0 || !draw_setup()) return;
    vkCmdDraw(G.b[G.cur].cmd, (uint32_t)count, 1, (uint32_t)first, 0);
}

static void API gv_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                          const void *off, GLint base)
{
    GVao *v = GET(t_vao, S.vao);
    GBuf *e = v ? buf_of(v->ebo) : NULL;
    if (mode != GL_TRIANGLES || count <= 0 || !e || !e->b) return;
    if (!draw_setup()) return;
    vkCmdBindIndexBuffer(G.b[G.cur].cmd, e->b, 0,
                         type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(G.b[G.cur].cmd, (uint32_t)count, 1,
                     (uint32_t)((uintptr_t)off / (type == GL_UNSIGNED_SHORT ? 2 : 4)), base, 0);
}

/* vertex arrays */
static void API gv_GenVertexArrays(GLsizei n, GLuint *ids) { gen_names(n, ids, (void **)&t_vao, &t_vao_n, sizeof *t_vao); }
static void API gv_BindVertexArray(GLuint id) { S.vao = id; }
static void API gv_EnableVertexAttribArray(GLuint i) { GVao *v = GET(t_vao, S.vao); if (v && i < NATTR) v->a[i].enabled = 1; }
static void API gv_DisableVertexAttribArray(GLuint i) { GVao *v = GET(t_vao, S.vao); if (v && i < NATTR) v->a[i].enabled = 0; }
static void API gv_VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm,
                                       GLsizei stride, const void *ptr)
{
    GVao *v = GET(t_vao, S.vao);
    if (!v || i >= NATTR) return;
    v->a[i].size = size;
    v->a[i].type = type;
    v->a[i].norm = norm;
    v->a[i].stride = stride;
    v->a[i].buf = S.ab;
    v->a[i].off = (uintptr_t)ptr;
}
static void API gv_VertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (i >= NATTR) return;
    S.generic[i][0] = x; S.generic[i][1] = y; S.generic[i][2] = z; S.generic[i][3] = w;
}
static void API gv_VertexAttrib4fv(GLuint i, const GLfloat *v) { if (i < NATTR) memcpy(S.generic[i], v, 16); }

/* ── clearing, blits, read-backs ────────────────────────────────────────── */

static void API gv_Clear(GLbitfield mask)
{
    VkClearAttachment a[2];
    VkClearRect r;
    uint32_t n = 0;
    GTex *col, *ds;
    GLuint ci, di;
    if (!rendering_begin()) return;
    fbo_tex(S.draw_fbo, &col, &ds, &ci, &di);
    if (R.ds == 0) ds = NULL;
    memset(a, 0, sizeof a);
    if ((mask & GL_COLOR_BUFFER_BIT) && col && !col->bc
            && (S.cmask[0] || S.cmask[1] || S.cmask[2] || S.cmask[3])) {
        a[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        memcpy(a[n].clearValue.color.float32, S.clear_col, 16);
        if (col->rgb) a[n].clearValue.color.float32[3] = 1.0f;
        n++;
    }
    if (ds && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) {
        if ((mask & GL_DEPTH_BUFFER_BIT) && S.depth_mask) a[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((mask & GL_STENCIL_BUFFER_BIT) && (S.st_wmask & 0xFF)) a[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        a[n].clearValue.depthStencil.depth = (float)S.clear_depth;
        a[n].clearValue.depthStencil.stencil = (uint32_t)S.clear_stencil & 0xFF;
        if (a[n].aspectMask) n++;
    }
    if (!n) return;
    if (S.scissor) {
        int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1];
        int x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
        if (x1 > R.vw) x1 = R.vw;
        if (y1 > R.vh) y1 = R.vh;
        if (x1 <= x0 || y1 <= y0) return;
        r.rect.offset.x = x0; r.rect.offset.y = y0;
        r.rect.extent.width = (uint32_t)(x1 - x0); r.rect.extent.height = (uint32_t)(y1 - y0);
    } else {
        r.rect.offset.x = r.rect.offset.y = 0;
        r.rect.extent.width = (uint32_t)R.vw; r.rect.extent.height = (uint32_t)R.vh;
    }
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    vkCmdClearAttachments(G.b[G.cur].cmd, n, a, 1, &r);
}

/* vk15: a copy from one image to another, scaled (a resolve when the source
 * is multisampled): glBlitFramebuffer's, and the Vulkan backend's own */
static void tex_blit(GTex *sc, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                     GTex *dc, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter)
{
    VkCommandBuffer c;
    VkImageLayout sl = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (!sc || !dc || !sc->img || !dc->img || sc->bc || dc->bc) return;
    rendering_end();
    if (sc == dc) {                     /* within one image */
        sl = dl = VK_IMAGE_LAYOUT_GENERAL;
        img_use(sc, sl, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    } else {
        IMG_XSRC(sc);
        IMG_XDST(dc);
    }
    c = cmd();
    if (sc->samples > 1) {
        /* a resolve: the same size, no flip, into a single-sample image */
        VkImageResolve rv;
        memset(&rv, 0, sizeof rv);
        rv.srcSubresource.aspectMask = rv.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rv.srcSubresource.layerCount = rv.dstSubresource.layerCount = 1;
        rv.srcOffset.x = sx0 < sx1 ? sx0 : sx1;
        rv.srcOffset.y = sy0 < sy1 ? sy0 : sy1;
        rv.dstOffset.x = dx0 < dx1 ? dx0 : dx1;
        rv.dstOffset.y = dy0 < dy1 ? dy0 : dy1;
        rv.extent.width = (uint32_t)abs(sx1 - sx0);
        rv.extent.height = (uint32_t)abs(sy1 - sy0);
        rv.extent.depth = 1;
        if (dc->samples == 1)
            vkCmdResolveImage(c, sc->img, sl, dc->img, dl, 1, &rv);
    } else {
        VkImageBlit bl;
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
        bl.srcOffsets[0].x = sx0; bl.srcOffsets[0].y = sy0;
        bl.srcOffsets[1].x = sx1; bl.srcOffsets[1].y = sy1; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[0].x = dx0; bl.dstOffsets[0].y = dy0;
        bl.dstOffsets[1].x = dx1; bl.dstOffsets[1].y = dy1; bl.dstOffsets[1].z = 1;
        /* clamped to the images (OpenGL clips; a blit outside is invalid here) */
        if (sx0 < 0 || sy0 < 0 || sx1 < 0 || sy1 < 0 || sx0 > sc->w || sx1 > sc->w
                || sy0 > sc->h || sy1 > sc->h || dx0 < 0 || dy0 < 0 || dx1 < 0 || dy1 < 0
                || dx0 > dc->w || dx1 > dc->w || dy0 > dc->h || dy1 > dc->h) {
            GV_ONCE(4, "a blit outside its images (%d,%d-%d,%d -> %d,%d-%d,%d) left out\n",
                    sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
            return;
        }
        if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return;
        vkCmdBlitImage(c, sc->img, sl, dc->img, dl, 1, &bl,
                       filter == GL_LINEAR && sc->fmt == dc->fmt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
}

static void API gv_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                   GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                   GLbitfield mask, GLenum filter)
{
    GTex *sc, *sd, *dc, *dd;
    GLuint a, b;
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    fbo_tex(S.read_fbo, &sc, &sd, &a, &b);
    fbo_tex(S.draw_fbo, &dc, &dd, &a, &b);
    tex_blit(sc, sx0, sy0, sx1, sy1, dc, dx0, dy0, dx1, dy1, filter);
}

/* vk15: pixels of an image into memory (now, waited for) or into a
 * pixel-pack buffer (pb: data is the offset); glReadPixels's, and the
 * Vulkan backend's own */
static void tex_read(GTex *t, GBuf *pb, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum format, GLenum type, void *data)
{
    VkBufferImageCopy cp;
    VkBuffer dst;
    VkDeviceSize doff;
    int depth = format == GL_DEPTH_COMPONENT;
    uint32_t bpp = 4;
    VkBuffer tmp = VK_NULL_HANDLE;
    VkDeviceMemory tmpm = VK_NULL_HANDLE;
    uint8_t *tmap = NULL;
    if (!t || !t->img || t->samples > 1 || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
    rendering_end();
    IMG_XSRC(t);
    memset(&cp, 0, sizeof cp);
    cp.imageSubresource.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageOffset.x = x;
    cp.imageOffset.y = y;
    cp.imageExtent.width = (uint32_t)w;
    cp.imageExtent.height = (uint32_t)h;
    cp.imageExtent.depth = 1;
    if (pb) {
        /* into a pixel-pack buffer: read when it is mapped */
        if (!pb->b || (VkDeviceSize)((uintptr_t)data + (size_t)w * h * bpp) > pb->size) return;
        dst = pb->b;
        doff = (VkDeviceSize)(uintptr_t)data;
        cp.bufferOffset = doff;
        vkCmdCopyImageToBuffer(cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &cp);
        pb->gpu_write = G.b[G.cur].serial;
        host_barrier();
        return;
    }
    /* into memory: done now, waited for */
    if (!host_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &tmp, &tmpm, &tmap))
        return;
    vkCmdCopyImageToBuffer(cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, 1, &cp);
    host_barrier();
    wait_idle();
    {
        size_t i, n = (size_t)w * h;
        if (depth) {
            float *o = (float *)data;
            for (i = 0; i < n; i++) {
                uint32_t v;
                memcpy(&v, tmap + i * 4, 4);
                if (t->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(&o[i], &v, 4);
                else o[i] = (float)(v & 0xFFFFFF) / 16777215.0f;
            }
        } else if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
            uint8_t *o = (uint8_t *)data;
            for (i = 0; i < n; i++) {
                o[i * 4 + 0] = tmap[i * 4 + 2];
                o[i * 4 + 1] = tmap[i * 4 + 1];
                o[i * 4 + 2] = tmap[i * 4 + 0];
                o[i * 4 + 3] = t->rgb ? 0xFF : tmap[i * 4 + 3];
            }
        } else {
            memcpy(data, tmap, n * 4);
            if (t->rgb)
                for (i = 0; i < n; i++) ((uint8_t *)data)[i * 4 + 3] = 0xFF;
        }
    }
    vkDestroyBuffer(G.c.dev, tmp, NULL);
    vkFreeMemory(G.c.dev, tmpm, NULL);
}

static void API gv_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)
{
    GTex *col, *ds;
    GLuint a, b;
    fbo_tex(S.read_fbo, &col, &ds, &a, &b);
    tex_read(format == GL_DEPTH_COMPONENT ? ds : col, buf_of(S.pack), x, y, w, h, format, type, data);
}

/* ── queries (samples passed) and sync objects ──────────────────────────── */

static void API gv_GenQueries(GLsizei n, GLuint *ids) { gen_names(n, ids, (void **)&t_query, &t_query_n, sizeof *t_query); }

static void query_slot_begin(void)
{
    Batch *b = &G.b[G.cur];
    GQuery *q = S.query_active < t_query_n ? &t_query[S.query_active] : NULL;
    if (!q || !b->qp || b->qn >= QPOOL)
        return;
    if (q->serial != b->serial) {
        /* counted in a new batch: what the old one counted is kept */
        if (q->serial && q->n)
            GV_ONCE(2, "a query spans two batches; part of its count is lost\n");
        q->serial = b->serial;
        q->first = b->qn;
        q->n = 0;
    }
    if (q->first + q->n != b->qn) {
        GV_ONCE(2, "a query's slots are not together; part of its count is lost\n");
        return;
    }
    R.qslot = b->qn++;
    q->n++;
    vkCmdBeginQuery(b->cmd, b->qp, R.qslot, 0);
}

static void API gv_BeginQuery(GLenum target, GLuint id)
{
    GQuery *q = id < t_query_n && t_query[id].used ? &t_query[id] : NULL;
    (void)target;
    if (!q) return;
    q->active = 1;
    q->have = 0;
    q->result = 0;
    q->serial = 0;
    q->n = 0;
    S.query_active = id;
    if (R.rendering)
        query_slot_begin();
}
static void API gv_EndQuery(GLenum target)
{
    GQuery *q = S.query_active < t_query_n ? &t_query[S.query_active] : NULL;
    (void)target;
    if (R.rendering && R.qslot != ~0u) {
        vkCmdEndQuery(G.b[G.cur].cmd, G.b[G.cur].qp, R.qslot);
        R.qslot = ~0u;
    }
    if (q) {
        q->active = 0;
        if (!q->n) q->have = 1;              /* nothing drawn while it was on */
    }
    S.query_active = 0;
}

/* a finished batch: the queries counted in it */
static void queries_collect(Batch *b)
{
    GLuint i;
    if (!b->qp || !b->qn)
        return;
    for (i = 1; i < t_query_n; i++) {
        GQuery *q = &t_query[i];
        uint64_t r[QPOOL];
        uint32_t k;
        if (!q->used || q->have || q->serial != b->serial || !q->n)
            continue;
        if (vkGetQueryPoolResults(G.c.dev, b->qp, q->first, q->n, sizeof r, r, 8,
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        q->result = 0;
        for (k = 0; k < q->n; k++) q->result += r[k];
        q->have = 1;
    }
}

static int query_ready(GQuery *q, int wait)
{
    int i;
    if (q->have || !q->serial) return 1;
    if (!serial_done(q->serial, wait)) return 0;
    for (i = 0; i < NB; i++)
        if (G.b[i].serial == q->serial)
            queries_collect(&G.b[i]);
    q->have = 1;
    return 1;
}

static void API gv_GetQueryObjectuiv(GLuint id, GLenum pname, GLuint *v)
{
    GQuery *q = id < t_query_n && t_query[id].used ? &t_query[id] : NULL;
    if (!q) { *v = 0; return; }
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        *v = query_ready(q, 0) ? GL_TRUE : GL_FALSE;
        return;
    }
    query_ready(q, 1);
    *v = (GLuint)(q->result > 0xFFFFFFFFu ? 0xFFFFFFFFu : q->result);
}
static void API gv_GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *v)
{
    GLuint r = 0;
    gv_GetQueryObjectuiv(id, pname, &r);
    *v = r;
}

static GLsync API gv_FenceSync(GLenum cond, GLbitfield flags)
{
    (void)cond; (void)flags;
    batch_begin();
    return (GLsync)(uintptr_t)G.b[G.cur].serial;
}
static GLenum API gv_ClientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout)
{
    uint64_t serial = (uint64_t)(uintptr_t)s;
    int i;
    (void)flags;
    if (serial_done(serial, 0)) return GL_ALREADY_SIGNALED;
    if (!timeout) return GL_TIMEOUT_EXPIRED;
    for (i = 0; i < NB; i++)
        if (G.b[i].serial == serial) {
            if (G.b[i].open) batch_submit();
            if (vkWaitForFences(G.c.dev, 1, &G.b[i].fence, VK_TRUE, timeout) != VK_SUCCESS)
                return GL_TIMEOUT_EXPIRED;
            if (serial > G.done) done_set(serial);
            return GL_CONDITION_SATISFIED;
        }
    return GL_ALREADY_SIGNALED;
}
static void API gv_DeleteSync(GLsync s) { (void)s; }
static void API gv_Finish(void) { wait_idle(); }
static void API gv_Flush(void) { batch_submit(); }
static GLenum API gv_GetError(void) { return GL_NO_ERROR; }

/* ── getting state ──────────────────────────────────────────────────────── */

static const char *const s_exts[] = {
    "GL_EXT_texture_compression_s3tc",
    "GL_ARB_texture_filter_anisotropic",
    "GL_EXT_texture_filter_anisotropic",
};
#define NEXTS (int)(sizeof s_exts / sizeof s_exts[0])

static void API gv_GetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case GL_MAJOR_VERSION: *v = 3; break;
    case GL_MINOR_VERSION: *v = 3; break;
    case GL_NUM_EXTENSIONS: *v = G.bc_ok ? NEXTS : NEXTS - 1; break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = (GLint)S.pack; break;
    case GL_NUM_PROGRAM_BINARY_FORMATS: *v = 0; break;
    case GL_MAX_RENDERBUFFER_SIZE: *v = (GLint)G.c.props.limits.maxImageDimension2D; break;
    case GL_MAX_SAMPLES: {
        VkSampleCountFlags f = G.c.props.limits.framebufferColorSampleCounts
                             & G.c.props.limits.framebufferDepthSampleCounts;
        *v = f & VK_SAMPLE_COUNT_8_BIT ? 8 : f & VK_SAMPLE_COUNT_4_BIT ? 4 : f & VK_SAMPLE_COUNT_2_BIT ? 2 : 1;
        break;
    }
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: *v = (GLint)(G.c.props.limits.maxUniformBufferRange / 4 < 16384
                                                        ? G.c.props.limits.maxUniformBufferRange / 4 : 16384); break;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY: *v = (GLint)G.max_aniso; break;
    case GL_CURRENT_PROGRAM: *v = (GLint)S.prog; break;
    case GL_VERTEX_ARRAY_BINDING: *v = (GLint)S.vao; break;
    case GL_ACTIVE_TEXTURE: *v = (GLint)(GL_TEXTURE0 + S.unit); break;
    case GL_UNPACK_ALIGNMENT: *v = S.unpack_align; break;
    case GL_PACK_ALIGNMENT: *v = S.pack_align; break;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)S.tex2d[S.unit]; break;
    case GL_SAMPLER_BINDING: *v = (GLint)S.samp[S.unit]; break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = (GLint)S.draw_fbo; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)S.read_fbo; break;
    case GL_VIEWPORT: memcpy(v, S.vp, sizeof S.vp); break;
    default: *v = 0; break;
    }
}
static void API gv_GetInteger64v(GLenum p, GLint64 *v) { GLint i = 0; gv_GetIntegerv(p, &i); *v = i; }
static void API gv_GetFloatv(GLenum p, GLfloat *v)
{
    if (p == GL_COLOR_CLEAR_VALUE) memcpy(v, S.clear_col, 16);
    else if (p == GL_MAX_TEXTURE_MAX_ANISOTROPY) *v = G.max_aniso;
    else *v = 0.0f;
}
static void API gv_GetBooleanv(GLenum p, GLboolean *v)
{
    if (p == GL_COLOR_WRITEMASK) memcpy(v, S.cmask, 4);
    else if (p == GL_DEPTH_WRITEMASK) *v = S.depth_mask;
    else *v = GL_FALSE;
}
static const GLubyte *API gv_GetString(GLenum p)
{
    static char ver[64];
    switch (p) {
    case GL_VERSION:
        snprintf(ver, sizeof ver, "3.3.0 glvk (Vulkan %u.%u)", VK_API_VERSION_MAJOR(G.c.props.apiVersion),
                 VK_API_VERSION_MINOR(G.c.props.apiVersion));
        return (const GLubyte *)ver;
    case GL_RENDERER: return (const GLubyte *)G.name;
    case GL_VENDOR: return (const GLubyte *)"glvk";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"3.30";
    default: return (const GLubyte *)"";
    }
}
static const GLubyte *API gv_GetStringi(GLenum p, GLuint i)
{
    if (p != GL_EXTENSIONS) return NULL;
    if (!G.bc_ok) i++;                       /* no S3TC: left out */
    return i < (GLuint)NEXTS ? (const GLubyte *)s_exts[i] : NULL;
}

/* ── the function table glad loads ──────────────────────────────────────── */

typedef struct { const char *name; void *fn; } Proc;
#define P(n) { "gl" #n, (void *)gv_##n }
static const Proc s_procs[] = {
    P(ActiveTexture), P(AttachShader), P(BeginQuery), P(BindBuffer), P(BindFramebuffer),
    P(BindRenderbuffer), P(BindSampler), P(BindTexture), P(BindVertexArray), P(BlendColor),
    P(BlendEquation), P(BlendFunc), P(BlitFramebuffer), P(BufferData), P(BufferSubData),
    P(CheckFramebufferStatus), P(Clear), P(ClearColor), P(ClearDepth), P(ClearStencil),
    P(ClientWaitSync), P(ColorMask), P(CompileShader), P(CompressedTexImage2D), P(CreateProgram),
    P(CreateShader), P(CullFace), P(DeleteBuffers), P(DeleteFramebuffers), P(DeleteProgram),
    P(DeleteRenderbuffers), P(DeleteShader), P(DeleteSync), P(DeleteTextures), P(DepthFunc),
    P(DepthMask), P(Disable), P(DisableVertexAttribArray), P(DrawArrays), P(DrawElementsBaseVertex),
    P(Enable), P(EnableVertexAttribArray), P(EndQuery), P(FenceSync), P(Finish), P(Flush),
    P(FramebufferRenderbuffer), P(FramebufferTexture2D), P(FrontFace), P(GenBuffers),
    P(GenFramebuffers), P(GenQueries), P(GenRenderbuffers), P(GenSamplers), P(GenTextures),
    P(GenVertexArrays), P(GenerateMipmap), P(GetActiveUniform), P(GetBooleanv), P(GetError),
    P(GetFloatv), P(GetInteger64v), P(GetIntegerv), P(GetProgramInfoLog), P(GetProgramiv),
    P(GetQueryObjectui64v), P(GetQueryObjectuiv), P(GetShaderInfoLog), P(GetShaderiv),
    P(GetString), P(GetStringi), P(GetUniformLocation), P(IsEnabled), P(LinkProgram),
    P(MapBufferRange), P(PixelStorei), P(PolygonOffset), P(ProgramParameteri), P(ReadPixels),
    P(RenderbufferStorage), P(SamplerParameteri), P(Scissor), P(ShaderSource), P(StencilFunc),
    P(StencilMask), P(StencilOp), P(TexImage2D), P(TexImage2DMultisample), P(TexParameteri),
    P(TexSubImage2D), P(Uniform1f), P(Uniform1fv), P(Uniform1i), P(Uniform1iv), P(Uniform1ui),
    P(Uniform1uiv), P(Uniform2f), P(Uniform2fv), P(Uniform3f), P(Uniform3fv), P(Uniform4f), P(Uniform4fv),
    P(UniformMatrix4fv), P(UnmapBuffer), P(UseProgram), P(VertexAttrib4f), P(VertexAttrib4fv),
    P(VertexAttribPointer), P(Viewport),
};
#undef P

void *glvk_proc(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof s_procs / sizeof s_procs[0]; i++)
        if (!strcmp(s_procs[i].name, name))
            return s_procs[i].fn;
    return NULL;
}

/* ── start, the window, the cache ───────────────────────────────────────── */

static char s_pc_path[64];

static void pcache_load(void)
{
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    void *data = NULL;
    long len = 0;
    FILE *f;
#if defined(_WIN32)
    CreateDirectoryA("shader_cache", NULL);
    snprintf(s_pc_path, sizeof s_pc_path, "shader_cache\\glvk_pipelines.bin");
#else
    mkdir("shader_cache", 0755);
    snprintf(s_pc_path, sizeof s_pc_path, "shader_cache/glvk_pipelines.bin");
#endif
    f = fopen(s_pc_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 32 && len < (256L << 20) && (data = malloc((size_t)len))
                && fread(data, 1, (size_t)len, f) == (size_t)len) {
            ci.initialDataSize = (size_t)len;
            ci.pInitialData = data;
        }
        fclose(f);
    }
    if (vkCreatePipelineCache(G.c.dev, &ci, NULL, &G.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(G.c.dev, &ci, NULL, &G.pcache);
    }
    free(data);
}

static void pcache_save(void)
{
    size_t len = 0;
    void *data;
    FILE *f;
    if (!G.pcache || vkGetPipelineCacheData(G.c.dev, G.pcache, &len, NULL) != VK_SUCCESS || !len)
        return;
    if (!(data = malloc(len)))
        return;
    if (vkGetPipelineCacheData(G.c.dev, G.pcache, &len, data) == VK_SUCCESS) {
        char tmp[80];
        snprintf(tmp, sizeof tmp, "%s.tmp", s_pc_path);
        if ((f = fopen(tmp, "wb"))) {
            int ok = fwrite(data, 1, len, f) == len;
            ok = !fclose(f) && ok;
            remove(s_pc_path);
            if (!ok || rename(tmp, s_pc_path))
                remove(tmp);
        }
    }
    free(data);
}

static VkFormat ds_format(VkFormat want, VkFormat alt)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(G.c.pd, want, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        return want;
    return alt;
}

int glvk_init(void *win)
{
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBuffer cbs[NB];
    int i;
    if (G.up)
        return G.up > 0;
    G.up = -1;
    if (!nv2a_vk_core(win, &G.c))
        return 0;
    vkGetPhysicalDeviceMemoryProperties(G.c.pd, &G.mp);
    snprintf(G.name, sizeof G.name, "%s (Vulkan %u.%u.%u, glvk)", G.c.props.deviceName,
             VK_API_VERSION_MAJOR(G.c.props.apiVersion), VK_API_VERSION_MINOR(G.c.props.apiVersion),
             VK_API_VERSION_PATCH(G.c.props.apiVersion));
    {
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(G.c.pd, &f);
        G.bc_ok = f.textureCompressionBC;
        G.max_aniso = f.samplerAnisotropy ? G.c.props.limits.maxSamplerAnisotropy : 1.0f;
    }
    G.ubo_align = G.c.props.limits.minUniformBufferOffsetAlignment;
    if (G.ubo_align < 16) G.ubo_align = 16;
    G.ds24 = ds_format(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT);
    G.ds32 = ds_format(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT);
    cpi.queueFamilyIndex = G.c.qfam;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(G.c.dev, &cpi, NULL, &G.pool) != VK_SUCCESS)
        return 0;
    cai.commandPool = G.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = NB;
    if (vkAllocateCommandBuffers(G.c.dev, &cai, cbs) != VK_SUCCESS)
        return 0;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < NB; i++) {
        Batch *b = &G.b[i];
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        b->cmd = cbs[i];
        if (vkCreateFence(G.c.dev, &fci, NULL, &b->fence) != VK_SUCCESS)
            return 0;
        if (!host_buffer(RING_UBO, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         0, &b->ubo.b, &b->ubo.m, &b->ubo.map)
                || !host_buffer(RING_STG, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0,
                                &b->stg.b, &b->stg.m, &b->stg.map))
            return 0;
        b->ubo.size = RING_UBO;
        b->stg.size = RING_STG;
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = QPOOL;
        if (vkCreateQueryPool(G.c.dev, &qi, NULL, &b->qp) != VK_SUCCESS)
            b->qp = VK_NULL_HANDLE;
        if (G.c.props.limits.timestampComputeAndGraphics) {
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(G.c.dev, &qi, NULL, &b->tq) != VK_SUCCESS)
                b->tq = VK_NULL_HANDLE;
        }
    }
    pcache_load();
    /* OpenGL's defaults */
    S.depth_func = GL_LESS;
    S.depth_mask = GL_TRUE;
    S.st_func = GL_ALWAYS;
    S.st_mask = S.st_wmask = 0xFFFFFFFFu;
    S.st_fail = S.st_zfail = S.st_zpass = GL_KEEP;
    S.bl_src = GL_ONE; S.bl_dst = GL_ZERO; S.bl_eq = GL_FUNC_ADD;
    S.cmask[0] = S.cmask[1] = S.cmask[2] = S.cmask[3] = GL_TRUE;
    S.cull_face = GL_BACK;
    S.front_face = GL_CCW;
    S.clear_depth = 1.0;
    S.unpack_align = S.pack_align = 4;
    for (i = 0; i < NATTR; i++) S.generic[i][3] = 1.0f;
    G.up = 1;
    {
        /* what an unbound or unfinished texture reads: 0,0,0,1 as OpenGL */
        static const uint32_t black[6] = { 0xFF000000u, 0xFF000000u, 0xFF000000u,
                                           0xFF000000u, 0xFF000000u, 0xFF000000u };
        int u0 = S.unit;
        GLuint id;
        S.unit = NUNIT - 1;
        gv_GenTextures(1, &id); s_white2d_id = id;
        gv_BindTexture(GL_TEXTURE_2D, id);
        gv_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, black);
        gv_GenTextures(1, &id); s_whitecube_id = id;
        gv_BindTexture(GL_TEXTURE_CUBE_MAP, id);
        for (i = 0; i < 6; i++)
            gv_TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                          GL_UNSIGNED_INT_8_8_8_8_REV, &black[i]);
        gv_BindTexture(GL_TEXTURE_2D, 0);
        gv_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
        S.unit = u0;
        s_white2d = tex_of(s_white2d_id);
        s_whitecube = tex_of(s_whitecube_id);
        if (s_white2d) s_white2d->minf = GL_NEAREST;
        if (s_whitecube) s_whitecube->minf = GL_NEAREST;
    }
#ifndef GLVK_AS_CORE                    /* nv2a_rb_vk.c says its own */
    GV_LOG("renderer: OpenGL calls on Vulkan (glvk) -- %s\n", G.name);
#endif
    return 1;
}

void glvk_default_size(int w, int h)
{
    S.def_w = w;
    S.def_h = h;
}

const char *glvk_device_name(void) { return G.name; }

int glvk_swap(void *win)
{
    GTex *t = def_tex();
    static uint64_t frames;
    static int saved;
    int ok;
    if (!t)
        return 0;
    rendering_end();
    IMG_PRESENT(t);
    batch_submit();
    ok = nv2a_vk_present_image(win, t->img, (uint32_t)t->w, (uint32_t)t->h);
    frames++;
    if (frames == 1)
        GV_LOG("first frame in the window (%dx%d)\n", t->w, t->h);
    if (frames % 600 == 0) {
        GV_LOG("%llu frames: %llu draws, %llu submits, %llu pipelines, %llu programs"
               " (%llu from the SPIR-V cache, %.0f ms)\n",
               (unsigned long long)frames, (unsigned long long)s_n_draws,
               (unsigned long long)s_n_submits, (unsigned long long)s_n_pipes,
               (unsigned long long)s_n_links, (unsigned long long)s_n_links_cached, s_link_ms);
    }
    if ((frames == 120 && !saved) || frames % 3600 == 0) {
        pcache_save();                        /* the pipelines kept */
        saved = 1;
    }
    return ok;
}
