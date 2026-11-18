/* Vulkan renderer, step 1: the probe; step 2: the shader mirror (see
 * nv2a_vk.h).
 *
 * Why Vulkan (Miggas, a player's log): on an RX 9060 XT with AMD's newest
 * driver, OpenGL compiles each of the renderer's shaders in 0.3-0.4 s, and
 * loading a saved one costs as much (RECOMP_GL_NO_SHADER_CACHE=1 changed
 * nothing), so the title stays black for ~9 s while its 19 shaders come in
 * one by one, on every start. The probe times the same work on Vulkan, on
 * the player's own card, before the renderer itself is ported:
 *   [VK] device, driver and API version
 *   [VK] GLSL -> SPIR-V (glslang), the pipeline (vkCreateGraphicsPipelines),
 *        each timed, and the same pipeline again through the pipeline cache
 *   [VK] a triangle drawn offscreen and its centre read back: right or not
 * The shaders are written the way nv2a_gl.c writes its own (#version 330
 * core, loose uniforms, in/out without locations): glslang's relaxed Vulkan
 * rules put the loose uniforms in one uniform block (set 0, binding 0) and
 * give the inputs and outputs their locations, so the existing GLSL can be
 * kept for the port. RECOMP_VK_VALIDATE=1 turns on the validation layer when
 * it is installed. */
#include "nv2a_vk.h"
#include "nv2a_vk_core.h"

#include <volk.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define VK_LOG(...) fprintf(stderr, "[VK] " __VA_ARGS__)

int nv2a_vk_wanted(void)
{
    static int w = -1;
    if (w < 0) {
        const char *r = getenv("RECOMP_RENDERER"), *p = getenv("RECOMP_VK_PROBE");
        w = (r && !strcmp(r, "vkmirror")) || (p && p[0] == '1');
    }
    return w;
}

static double ms_since(Uint64 t0)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0
           / (double)SDL_GetPerformanceFrequency();
}

/* ── GLSL to SPIR-V ──────────────────────────────────────────────────── */

static const char *const k_vs =
    "#version 330 core\n"
    "in vec4 a_pos;\n"
    "in vec4 a_col;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_wsk;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.xy, 0.0, 1.0);\n"
    "  gl_Position.x *= u_wsk;\n"
    "  v_col = a_col * u_tint;\n"
    "}\n";
static const char *const k_fs =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_wsk;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  o_col = v_col;\n"
    "}\n";

typedef struct { uint32_t *code; size_t words; } Spirv;

/* the last compile or link log; per thread (vk10: the Vulkan backend
 * compiles on worker threads) */
static _Thread_local char s_glsl_err[4096];

static glslang_shader_t *gl_shader(glslang_stage_t stage, const char *src, glslang_input_t *in)
{
    glslang_shader_t *sh;
    memset(in, 0, sizeof *in);
    in->language = GLSLANG_SOURCE_GLSL;
    in->stage = stage;
    in->client = GLSLANG_CLIENT_VULKAN;
    in->client_version = GLSLANG_TARGET_VULKAN_1_1;
    in->target_language = GLSLANG_TARGET_SPV;
    in->target_language_version = GLSLANG_TARGET_SPV_1_3;
    in->code = src;
    in->default_version = 330;
    in->default_profile = GLSLANG_CORE_PROFILE;
    in->messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    in->resource = glslang_default_resource();
    sh = glslang_shader_create(in);
    if (!sh)
        return NULL;
    glslang_shader_set_options(sh, GLSLANG_SHADER_AUTO_MAP_BINDINGS
                               | GLSLANG_SHADER_AUTO_MAP_LOCATIONS
                               | GLSLANG_SHADER_VULKAN_RULES_RELAXED);
    glslang_shader_set_default_uniform_block_set_and_binding(sh, 0, 0);
    /* textures from binding 1 on: a stage without loose uniforms would
     * otherwise give its first texture binding 0, the other stage's block */
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_TEXTURE, 1);
    glslang_shader_shift_binding(sh, GLSLANG_RESOURCE_TYPE_SAMPLER, 1);
    if (!glslang_shader_preprocess(sh, in) || !glslang_shader_parse(sh, in)) {
        snprintf(s_glsl_err, sizeof s_glsl_err, "%s shader: %s",
                 stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
                 glslang_shader_get_info_log(sh));
        VK_LOG("GLSL (%s) did not compile:\n%.1500s\n",
               stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
               glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        return NULL;
    }
    return sh;
}

static int spirv_take(glslang_program_t *pg, glslang_stage_t stage, Spirv *out)
{
    glslang_program_SPIRV_generate(pg, stage);
    out->words = glslang_program_SPIRV_get_size(pg);
    out->code = (uint32_t *)malloc(out->words * 4);
    if (!out->code)
        return 0;
    glslang_program_SPIRV_get(pg, out->code);
    return out->words > 5;
}

/* both stages linked together, so the varyings and the uniform block agree */
static int glsl_to_spirv(const char *vs, const char *fs, Spirv *v, Spirv *f)
{
    glslang_input_t iv, ifr;
    glslang_shader_t *sv = gl_shader(GLSLANG_STAGE_VERTEX, vs, &iv);
    glslang_shader_t *sf = sv ? gl_shader(GLSLANG_STAGE_FRAGMENT, fs, &ifr) : NULL;
    glslang_program_t *pg;
    int ok = 0;
    memset(v, 0, sizeof *v);
    memset(f, 0, sizeof *f);
    if (!sf) {
        if (sv) glslang_shader_delete(sv);
        return 0;
    }
    pg = glslang_program_create();
    glslang_program_add_shader(pg, sv);
    glslang_program_add_shader(pg, sf);
    if (!glslang_program_link(pg, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        snprintf(s_glsl_err, sizeof s_glsl_err, "link: %s", glslang_program_get_info_log(pg));
        VK_LOG("GLSL did not link:\n%.1500s\n", glslang_program_get_info_log(pg));
    }
    else if (!glslang_program_map_io(pg))
        VK_LOG("GLSL inputs/outputs could not be mapped:\n%.1500s\n", glslang_program_get_info_log(pg));
    else
        ok = spirv_take(pg, GLSLANG_STAGE_VERTEX, v) && spirv_take(pg, GLSLANG_STAGE_FRAGMENT, f);
    glslang_program_delete(pg);
    glslang_shader_delete(sv);
    glslang_shader_delete(sf);
    return ok;
}

/* ── the probe ───────────────────────────────────────────────────────── */

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qfam;
    VkCommandPool pool;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageView view;
    VkBuffer vbuf, ubuf, rbuf;
    VkDeviceMemory vmem, umem, rmem;
    VkRenderPass rp;
    VkFramebuffer fb;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkPipelineCache cache;
    VkPipeline pipe, pipe2;
    VkShaderModule vsm, fsm;
    int feedback;           /* VK_EXT_pipeline_creation_feedback (or 1.3) */
} Probe;

#define PW 64
#define PH 64

static int mem_type(Probe *p, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(p->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int make_buffer(Probe *p, VkDeviceSize size, VkBufferUsageFlags use,
                       VkBuffer *b, VkDeviceMemory *m, const void *data)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;
    bi.size = size;
    bi.usage = use;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(p->dev, &bi, NULL, b) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(p->dev, *b, &mr);
    t = mem_type(p, mr.memoryTypeBits,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0)
        return 0;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(p->dev, &ai, NULL, m) != VK_SUCCESS
            || vkBindBufferMemory(p->dev, *b, *m, 0) != VK_SUCCESS)
        return 0;
    if (data) {
        void *q;
        if (vkMapMemory(p->dev, *m, 0, size, 0, &q) != VK_SUCCESS)
            return 0;
        memcpy(q, data, (size_t)size);
        vkUnmapMemory(p->dev, *m);
    }
    return 1;
}

static const char *vendor_name(uint32_t id)
{
    switch (id) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x10005: return "Mesa (software)";
    default: return "other";
    }
}

static int probe_device(Probe *p)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *val = getenv("RECOMP_VK_VALIDATE");
    uint32_t n = 0, i, best = 0, nq = 0;
    VkPhysicalDevice pds[16];
    VkQueueFamilyProperties qf[16];
    VkPhysicalDeviceProperties pp;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };

    app.pApplicationName = "Jet Set Radio Future";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    if (val && val[0] == '1') {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ici, NULL, &p->inst) != VK_SUCCESS && val) {
        ici.enabledLayerCount = 0;       /* no validation layer installed */
        if (vkCreateInstance(&ici, NULL, &p->inst) != VK_SUCCESS)
            p->inst = VK_NULL_HANDLE;
    }
    if (!p->inst) {
        VK_LOG("no Vulkan instance (the driver has no Vulkan, or it is too old)\n");
        return 0;
    }
    volkLoadInstance(p->inst);
    n = 16;
    if (vkEnumeratePhysicalDevices(p->inst, &n, pds) < 0 || !n) {
        VK_LOG("no Vulkan device\n");
        return 0;
    }
    /* a discrete card first, then an integrated one, then anything */
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties a, b;
        vkGetPhysicalDeviceProperties(pds[i], &a);
        vkGetPhysicalDeviceProperties(pds[best], &b);
        if ((a.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
             && b.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            || (a.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                && b.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && b.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU))
            best = i;
    }
    p->pd = pds[best];
    vkGetPhysicalDeviceProperties(p->pd, &pp);
    {
        VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &drv;
        if (vkGetPhysicalDeviceProperties2 && VK_API_VERSION_MINOR(pp.apiVersion) >= 2)
            vkGetPhysicalDeviceProperties2(p->pd, &p2);
        else
            drv.driverName[0] = 0;
        VK_LOG("device: %s (%s), Vulkan %u.%u.%u, driver %s%s%s (%u of %u device(s))\n",
               pp.deviceName, vendor_name(pp.vendorID), VK_API_VERSION_MAJOR(pp.apiVersion),
               VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion),
               drv.driverName[0] ? drv.driverName : "?", drv.driverInfo[0] ? " " : "",
               drv.driverInfo, best + 1, n);
    }
    nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p->pd, &nq, qf);
    for (i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (i == nq) {
        VK_LOG("no graphics queue\n");
        return 0;
    }
    p->qfam = i;
    qi.queueFamilyIndex = i;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    {   /* step 2: what the renderer's shaders need (gl_ClipDistance), and
         * the driver telling whether a pipeline came from the cache */
        static VkPhysicalDeviceFeatures want;
        static const char *ext[1];
        VkPhysicalDeviceFeatures have;
        VkExtensionProperties ep[512];
        uint32_t ne = 512, k;
        vkGetPhysicalDeviceFeatures(p->pd, &have);
        memset(&want, 0, sizeof want);
        want.shaderClipDistance = have.shaderClipDistance;
        dci.pEnabledFeatures = &want;
        p->feedback = 0;
        if (vkEnumerateDeviceExtensionProperties(p->pd, NULL, &ne, ep) >= 0)
            for (k = 0; k < ne; k++)
                if (!strcmp(ep[k].extensionName, "VK_EXT_pipeline_creation_feedback")) {
                    ext[0] = "VK_EXT_pipeline_creation_feedback";
                    dci.enabledExtensionCount = 1;
                    dci.ppEnabledExtensionNames = ext;
                    p->feedback = 1;
                }
        if (!have.shaderClipDistance)
            VK_LOG("the device has no shaderClipDistance: the renderer's vertex shaders"
                   " will not build\n");
    }
    if (vkCreateDevice(p->pd, &dci, NULL, &p->dev) != VK_SUCCESS) {
        VK_LOG("the device could not be opened\n");
        return 0;
    }
    volkLoadDevice(p->dev);
    vkGetDeviceQueue(p->dev, p->qfam, 0, &p->q);
    cpi.queueFamilyIndex = p->qfam;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return vkCreateCommandPool(p->dev, &cpi, NULL, &p->pool) == VK_SUCCESS;
}

static int probe_target(Probe *p)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkAttachmentDescription at = { 0 };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    int t;

    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = PW; ii.extent.height = PH; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(p->dev, &ii, NULL, &p->img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(p->dev, p->img, &mr);
    if ((t = mem_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0)
        t = mem_type(p, mr.memoryTypeBits, 0);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(p->dev, &ai, NULL, &p->img_mem) != VK_SUCCESS
            || vkBindImageMemory(p->dev, p->img, p->img_mem, 0) != VK_SUCCESS)
        return 0;
    vi.image = p->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(p->dev, &vi, NULL, &p->view) != VK_SUCCESS)
        return 0;
    at.format = ii.format;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ar;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    if (vkCreateRenderPass(p->dev, &rpi, NULL, &p->rp) != VK_SUCCESS)
        return 0;
    fi.renderPass = p->rp;
    fi.attachmentCount = 1;
    fi.pAttachments = &p->view;
    fi.width = PW; fi.height = PH; fi.layers = 1;
    return vkCreateFramebuffer(p->dev, &fi, NULL, &p->fb) == VK_SUCCESS;
}

static VkShaderModule module_of(Probe *p, const Spirv *s)
{
    VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    si.codeSize = s->words * 4;
    si.pCode = s->code;
    vkCreateShaderModule(p->dev, &si, NULL, &m);
    return m;
}

static int probe_pipeline(Probe *p, VkPipeline *out, double *ms)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    VkVertexInputBindingDescription vb = { 0, 32, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[2] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkViewport vp = { 0, 0, PW, PH, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { PW, PH } };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    Uint64 t0;
    VkResult r;

    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = p->vsm; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = p->fsm; st[1].pName = "main";
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &sc;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vs; gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms_; gi.pColorBlendState = &cb;
    gi.layout = p->pl; gi.renderPass = p->rp;
    t0 = SDL_GetPerformanceCounter();
    r = vkCreateGraphicsPipelines(p->dev, p->cache, 1, &gi, NULL, out);
    *ms = ms_since(t0);
    return r == VK_SUCCESS;
}

static void probe_free(Probe *p)
{
    if (p->dev) {
        vkDeviceWaitIdle(p->dev);
        if (p->pipe2) vkDestroyPipeline(p->dev, p->pipe2, NULL);
        if (p->pipe) vkDestroyPipeline(p->dev, p->pipe, NULL);
        if (p->cache) vkDestroyPipelineCache(p->dev, p->cache, NULL);
        if (p->vsm) vkDestroyShaderModule(p->dev, p->vsm, NULL);
        if (p->fsm) vkDestroyShaderModule(p->dev, p->fsm, NULL);
        if (p->dpool) vkDestroyDescriptorPool(p->dev, p->dpool, NULL);
        if (p->pl) vkDestroyPipelineLayout(p->dev, p->pl, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(p->dev, p->dsl, NULL);
        if (p->fb) vkDestroyFramebuffer(p->dev, p->fb, NULL);
        if (p->rp) vkDestroyRenderPass(p->dev, p->rp, NULL);
        if (p->view) vkDestroyImageView(p->dev, p->view, NULL);
        if (p->img) vkDestroyImage(p->dev, p->img, NULL);
        if (p->img_mem) vkFreeMemory(p->dev, p->img_mem, NULL);
        if (p->vbuf) vkDestroyBuffer(p->dev, p->vbuf, NULL);
        if (p->ubuf) vkDestroyBuffer(p->dev, p->ubuf, NULL);
        if (p->rbuf) vkDestroyBuffer(p->dev, p->rbuf, NULL);
        if (p->vmem) vkFreeMemory(p->dev, p->vmem, NULL);
        if (p->umem) vkFreeMemory(p->dev, p->umem, NULL);
        if (p->rmem) vkFreeMemory(p->dev, p->rmem, NULL);
        if (p->pool) vkDestroyCommandPool(p->dev, p->pool, NULL);
        vkDestroyDevice(p->dev, NULL);
    }
    if (p->inst)
        vkDestroyInstance(p->inst, NULL);
    memset(p, 0, sizeof *p);
}

int nv2a_vk_probe(void)
{
    static Probe P;
    Probe *p = &P;
    Spirv v, f;
    Uint64 t0;
    double t_glsl, t_pipe = 0, t_pipe2 = 0;
    int ok = 0;
    /* a triangle across the middle: white, tinted by the uniform to a known
     * colour, so the uniform block and the varyings are both checked */
    static const float tri[3][8] = {
        { -0.8f, -0.8f, 0, 1,  1, 1, 1, 1 },
        {  0.8f, -0.8f, 0, 1,  1, 1, 1, 1 },
        {  0.0f,  0.9f, 0, 1,  1, 1, 1, 1 } };
    /* the default uniform block, std140: u_tint at 0, u_wsk at 16 */
    static const float ub[8] = { 0.25f, 0.5f, 1.0f, 1.0f,  1.0f, 0, 0, 0 };

    memset(p, 0, sizeof *p);
    if (volkInitialize() != VK_SUCCESS) {
        VK_LOG("no Vulkan loader (vulkan-1.dll / libvulkan.so.1): Vulkan can't be used here\n");
        return -1;
    }
    if (!probe_device(p))
        goto out;

    t0 = SDL_GetPerformanceCounter();
    glslang_initialize_process();
    ok = glsl_to_spirv(k_vs, k_fs, &v, &f);
    t_glsl = ms_since(t0);
    if (!ok) {
        VK_LOG("probe: GLSL -> SPIR-V failed\n");
        goto out;
    }
    ok = 0;
    VK_LOG("probe: GLSL -> SPIR-V %.1f ms (glslang, relaxed Vulkan rules; %zu + %zu words)\n",
           t_glsl, v.words, f.words);
    p->vsm = module_of(p, &v);
    p->fsm = module_of(p, &f);
    free(v.code);
    free(f.code);
    if (!p->vsm || !p->fsm || !probe_target(p))
        goto fail;
    {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        di.bindingCount = 1; di.pBindings = &b;
        if (vkCreateDescriptorSetLayout(p->dev, &di, NULL, &p->dsl) != VK_SUCCESS)
            goto fail;
        li.setLayoutCount = 1; li.pSetLayouts = &p->dsl;
        if (vkCreatePipelineLayout(p->dev, &li, NULL, &p->pl) != VK_SUCCESS
                || vkCreatePipelineCache(p->dev, &ci, NULL, &p->cache) != VK_SUCCESS)
            goto fail;
    }
    if (!probe_pipeline(p, &p->pipe, &t_pipe))
        goto fail;
    /* the same pipeline again: what a saved pipeline cache gives later runs */
    if (!probe_pipeline(p, &p->pipe2, &t_pipe2))
        goto fail;
    VK_LOG("probe: pipeline %.1f ms; the same again through the pipeline cache %.1f ms\n",
           t_pipe, t_pipe2);

    if (!make_buffer(p, sizeof tri, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &p->vbuf, &p->vmem, tri)
            || !make_buffer(p, sizeof ub, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &p->ubuf, &p->umem, ub)
            || !make_buffer(p, PW * PH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p->rbuf, &p->rmem, NULL))
        goto fail;
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorSet ds;
        VkDescriptorBufferInfo dbi = { 0, 0, sizeof ub };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue clear = { { { 0, 0, 0, 1 } } };
        VkBufferImageCopy cp = { 0 };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkCommandBuffer cmd;
        VkDeviceSize off = 0;
        unsigned char *px;
        unsigned c, e;

        dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(p->dev, &dpi, NULL, &p->dpool) != VK_SUCCESS)
            goto fail;
        dai.descriptorPool = p->dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &p->dsl;
        if (vkAllocateDescriptorSets(p->dev, &dai, &ds) != VK_SUCCESS)
            goto fail;
        dbi.buffer = p->ubuf;
        w.dstSet = ds; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(p->dev, 1, &w, 0, NULL);

        cai.commandPool = p->pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(p->dev, &cai, &cmd) != VK_SUCCESS)
            goto fail;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        rbi.renderPass = p->rp; rbi.framebuffer = p->fb;
        rbi.renderArea.extent.width = PW; rbi.renderArea.extent.height = PH;
        rbi.clearValueCount = 1; rbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, 0, 1, &ds, 0, NULL);
        vkCmdBindVertexBuffers(cmd, 0, 1, &p->vbuf, &off);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.imageSubresource.layerCount = 1;
        cp.imageExtent.width = PW; cp.imageExtent.height = PH; cp.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, p->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->rbuf, 1, &cp);
        vkEndCommandBuffer(cmd);
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        t0 = SDL_GetPerformanceCounter();
        if (vkQueueSubmit(p->q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS
                || vkQueueWaitIdle(p->q) != VK_SUCCESS)
            goto fail;
        if (vkMapMemory(p->dev, p->rmem, 0, PW * PH * 4, 0, (void **)&px) != VK_SUCCESS)
            goto fail;
        /* the centre: white x tint = (64, 128, 255); a corner stays black */
        c = (unsigned)((PH / 2) * PW + PW / 2) * 4;
        e = 0;
        ok = abs(px[c] - 64) <= 2 && abs(px[c + 1] - 128) <= 2 && px[c + 2] >= 253
             && px[e] == 0 && px[e + 1] == 0 && px[e + 2] == 0;
        VK_LOG("probe: triangle drawn and read back in %.1f ms: centre %u,%u,%u (want 64,128,255),"
               " corner %u,%u,%u -> %s\n", ms_since(t0), px[c], px[c + 1], px[c + 2],
               px[e], px[e + 1], px[e + 2], ok ? "right" : "WRONG");
        vkUnmapMemory(p->dev, p->rmem);
    }
    goto out;
fail:
    VK_LOG("probe: a Vulkan step failed (see above)\n");
out:
    probe_free(p);
    return ok ? 0 : 1;
}

/* ── step 2: the shader mirror ───────────────────────────────────────── */

typedef struct MJob {
    char *vs, *fs;
    double gl_ms;
    int gl_cached;
    struct MJob *next;
} MJob;

static SDL_SpinLock s_mq_lock;
static MJob *s_mq_head, *s_mq_tail;
static SDL_sem *s_mq_sem;
static int s_mq_n;              /* programs queued so far */

/* src with defs after its #version line, as nv2a_gl.c's compile() does */
static char *with_defs(const char *src, const char *defs)
{
    const char *nl = strchr(src, '\n');
    size_t n = strlen(src), d = defs ? strlen(defs) : 0;
    char *o = (char *)malloc(n + d + 1);
    if (!o)
        return NULL;
    if (d && nl && !strncmp(src, "#version", 8)) {
        size_t h = (size_t)(nl - src) + 1;
        memcpy(o, src, h);
        memcpy(o + h, defs, d);
        memcpy(o + h + d, nl + 1, n - h + 1);
    } else
        memcpy(o, src, n + 1);
    return o;
}

void nv2a_vk_mirror(const char *vs, const char *fs, const char *defs, double gl_ms, int gl_cached)
{
    MJob *j = (MJob *)calloc(1, sizeof *j);
    if (!j)
        return;
    j->vs = with_defs(vs, defs);
    j->fs = with_defs(fs, defs);
    j->gl_ms = gl_ms;
    j->gl_cached = gl_cached;
    if (!j->vs || !j->fs) {
        free(j->vs); free(j->fs); free(j);
        return;
    }
    SDL_AtomicLock(&s_mq_lock);
    if (!s_mq_sem)
        s_mq_sem = SDL_CreateSemaphore(0);
    if (s_mq_tail) s_mq_tail->next = j; else s_mq_head = j;
    s_mq_tail = j;
    s_mq_n++;
    SDL_AtomicUnlock(&s_mq_lock);
    if (s_mq_sem)
        SDL_SemPost(s_mq_sem);
}

static MJob *mq_take(void)
{
    MJob *j;
    SDL_AtomicLock(&s_mq_lock);
    j = s_mq_head;
    if (j) {
        s_mq_head = j->next;
        if (!s_mq_head) s_mq_tail = NULL;
    }
    SDL_AtomicUnlock(&s_mq_lock);
    return j;
}

/* The descriptors a SPIR-V module uses: set 0 bindings and their type,
 * read off its decorations (glslang's automatic bindings). */
#define MX_BIND 32
typedef struct { int used; VkDescriptorType type; } MBind;

static int spirv_bindings(const Spirv *s, MBind *b)
{
    uint32_t bound, i, w;
    uint32_t *bind, *set, *ptr_type, *ptr_sc, *kind;
    const uint32_t *c = s->code;
    if (s->words < 5 || c[0] != 0x07230203u)
        return 0;
    bound = c[3];
    if (!bound || bound > (1u << 22))
        return 0;
    bind = (uint32_t *)calloc(bound, 4 * 5);
    if (!bind)
        return 0;
    set = bind + bound; ptr_type = set + bound; ptr_sc = ptr_type + bound; kind = ptr_sc + bound;
    for (i = 0; i < bound; i++) { bind[i] = ~0u; set[i] = 0; }
    /* first pass: decorations and types */
    for (w = 5; w < s->words; ) {
        uint32_t op = c[w] & 0xFFFF, n = c[w] >> 16;
        if (!n || w + n > s->words) break;
        if (op == 71 && n >= 4 && c[w + 1] < bound) {          /* OpDecorate */
            if (c[w + 2] == 33) bind[c[w + 1]] = c[w + 3];     /* Binding */
            else if (c[w + 2] == 34) set[c[w + 1]] = c[w + 3]; /* DescriptorSet */
        } else if (op == 32 && n >= 4 && c[w + 1] < bound) {   /* OpTypePointer */
            ptr_sc[c[w + 1]] = c[w + 2];
            ptr_type[c[w + 1]] = c[w + 3];
        } else if ((op == 27 || op == 25 || op == 26 || op == 30 || op == 28) && n >= 2 && c[w + 1] < bound) {
            kind[c[w + 1]] = op;
            if (op == 28 && n >= 3 && c[w + 2] < bound)        /* array: its element */
                kind[c[w + 1]] = 0x10000u | c[w + 2];
        }
        w += n;
    }
    /* second pass: the variables */
    for (w = 5; w < s->words; ) {
        uint32_t op = c[w] & 0xFFFF, n = c[w] >> 16;
        if (!n || w + n > s->words) break;
        if (op == 59 && n >= 4 && c[w + 1] < bound && c[w + 2] < bound) {   /* OpVariable */
            uint32_t id = c[w + 2], pt = c[w + 1], sc = c[w + 3], t = ptr_type[pt], k;
            if ((sc == 0 || sc == 2 || sc == 12) && bind[id] != ~0u && set[id] == 0 && bind[id] < MX_BIND) {
                k = t < bound ? kind[t] : 0;
                while ((k & 0x10000u) && (k & 0xFFFFu) < bound) k = kind[k & 0xFFFFu];
                b[bind[id]].used = 1;
                b[bind[id]].type = sc == 12 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                 : k == 30 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                 : k == 27 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                 : k == 25 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                 : VK_DESCRIPTOR_TYPE_SAMPLER;
            }
        }
        w += n;
    }
    free(bind);
    return 1;
}

typedef struct {
    Probe d;                    /* the device */
    VkRenderPass rp;
    VkFormat zfmt;
    char cache_path[64];
    int n, fails, hits;
    double gl_ms, glsl_ms, pipe_ms, gl_cached_ms, pipe_cached_ms;
    int gl_cached_n, pipe_cached_n;
    int spv_cached_n;           /* step 3 */
    double spv_cached_ms;
} Mirror;

static int mirror_pass(Mirror *m)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference zr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFormatProperties fp;
    int i;
    /* depth as the renderer will keep it: float with stencil, else 24-bit */
    m->zfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    vkGetPhysicalDeviceFormatProperties(m->d.pd, m->zfmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        m->zfmt = VK_FORMAT_D24_UNORM_S8_UINT;
    memset(at, 0, sizeof at);
    for (i = 0; i < 2; i++) {
        at[i].samples = VK_SAMPLE_COUNT_1_BIT;
        at[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    at[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    at[0].initialLayout = at[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[1].format = m->zfmt;
    at[1].initialLayout = at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cr;
    sp.pDepthStencilAttachment = &zr;
    rpi.attachmentCount = 2;
    rpi.pAttachments = at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    return vkCreateRenderPass(m->d.dev, &rpi, NULL, &m->rp) == VK_SUCCESS;
}

static void mirror_cache_load(Mirror *m)
{
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    FILE *f;
    void *data = NULL;
    long len = 0;
#if defined(_WIN32)
    snprintf(m->cache_path, sizeof m->cache_path, "shader_cache\\vk_pipelines.bin");
#else
    snprintf(m->cache_path, sizeof m->cache_path, "shader_cache/vk_pipelines.bin");
#endif
    f = fopen(m->cache_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 32 && len < (256L << 20) && (data = malloc((size_t)len))
                && fread(data, 1, (size_t)len, f) == (size_t)len) {
            ci.initialDataSize = (size_t)len;   /* the driver checks it is its own */
            ci.pInitialData = data;
        }
        fclose(f);
    }
    if (vkCreatePipelineCache(m->d.dev, &ci, NULL, &m->d.cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(m->d.dev, &ci, NULL, &m->d.cache);
    }
    VK_LOG("mirror: pipeline cache %s (%s)\n", m->cache_path,
           ci.initialDataSize ? "loaded" : "new");
    free(data);
}

static void mirror_cache_save(Mirror *m)
{
    size_t len = 0;
    void *data;
    FILE *f;
    if (!m->d.cache || vkGetPipelineCacheData(m->d.dev, m->d.cache, &len, NULL) != VK_SUCCESS || !len)
        return;
    if (!(data = malloc(len)))
        return;
    if (vkGetPipelineCacheData(m->d.dev, m->d.cache, &len, data) == VK_SUCCESS) {
        char tmp[80];
        snprintf(tmp, sizeof tmp, "%s.tmp", m->cache_path);
        f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(data, 1, len, f) == len;
            ok = !fclose(f) && ok;
            remove(m->cache_path);
            if (!ok || rename(tmp, m->cache_path))
                remove(tmp);
        }
    }
    free(data);
}

/* ── step 3: the SPIR-V cache ─────────────────────────────────────────
 * The 9060 XT's log (v310k-vk2): the pipelines come from the pipeline
 * cache in 0.1 ms, but glslang takes 7-17 ms a program, every start. So
 * each program's SPIR-V is kept too: shader_cache/spirv/<key>.spv, the key
 * a 64-bit hash of both stages' GLSL (the #defines in it) and of the
 * compiler's settings below, so a change to either just makes new files.
 * The file: "XRSV", the version, the two lengths in words, the words. */
#define SPV_TAG "glslang-vk1.0-spv1.0-relaxed-2"

static uint64_t spv_hash(const char *s, uint64_t h)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    h ^= 0xFF; h *= 0x100000001b3ULL;          /* the end of this string */
    return h;
}

static void spv_path(char *out, size_t n, const char *vs, const char *fs)
{
    uint64_t h = spv_hash(fs, spv_hash(vs, spv_hash(SPV_TAG, 0xcbf29ce484222325ULL)));
#if defined(_WIN32)
    snprintf(out, n, "shader_cache\\spirv\\%016llx.spv", (unsigned long long)h);
#else
    snprintf(out, n, "shader_cache/spirv/%016llx.spv", (unsigned long long)h);
#endif
}

static int spv_load(const char *vs, const char *fs, Spirv *v, Spirv *f)
{
    char p[96];
    uint32_t hd[4];
    FILE *fp;
    int ok = 0;
    spv_path(p, sizeof p, vs, fs);
    if (!(fp = fopen(p, "rb")))
        return 0;
    if (fread(hd, 4, 4, fp) == 4 && hd[0] == 0x56535258u /* "XRSV" */ && hd[1] == 1
            && hd[2] > 5 && hd[2] < (1u << 22) && hd[3] > 5 && hd[3] < (1u << 22)) {
        v->words = hd[2];
        f->words = hd[3];
        v->code = (uint32_t *)malloc(v->words * 4);
        f->code = (uint32_t *)malloc(f->words * 4);
        ok = v->code && f->code && fread(v->code, 4, v->words, fp) == v->words
             && fread(f->code, 4, f->words, fp) == f->words
             && v->code[0] == 0x07230203u && f->code[0] == 0x07230203u;   /* SPIR-V's magic */
        if (!ok) { free(v->code); free(f->code); v->code = f->code = NULL; }
    }
    fclose(fp);
    return ok;
}

static void spv_save(const char *vs, const char *fs, const Spirv *v, const Spirv *f)
{
    char p[96], tmp[100];
    uint32_t hd[4] = { 0x56535258u, 1, (uint32_t)v->words, (uint32_t)f->words };
    FILE *fp;
    int ok;
    spv_path(p, sizeof p, vs, fs);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(fp = fopen(tmp, "wb")))
        return;
    ok = fwrite(hd, 4, 4, fp) == 4 && fwrite(v->code, 4, v->words, fp) == v->words
         && fwrite(f->code, 4, f->words, fp) == f->words;
    ok = !fclose(fp) && ok;
    remove(p);
    if (!ok || rename(tmp, p))
        remove(tmp);
}

/* the folders and glslang, once; called on the main thread before any
 * worker compiles (nv2a_rb_vk.c), and again harmlessly from below */
static volatile int s_spv_up;
void nv2a_vk_spirv_init(void)
{
    if (s_spv_up)
        return;
#if defined(_WIN32)
    CreateDirectoryA("shader_cache", NULL);
    CreateDirectoryA("shader_cache\\spirv", NULL);
#else
    mkdir("shader_cache", 0755);
    mkdir("shader_cache/spirv", 0755);
#endif
    glslang_initialize_process();
    s_spv_up = 1;
}

int nv2a_vk_spirv(const char *vs, const char *fs, uint32_t **vw, size_t *vn,
                  uint32_t **fw, size_t *fn, int *cached, char **err)
{
    Spirv v, f;
    if (err) *err = NULL;
    *cached = 0;
    if (spv_load(vs, fs, &v, &f)) {
        *cached = 1;
    } else {
        nv2a_vk_spirv_init();
        s_glsl_err[0] = 0;
        if (!glsl_to_spirv(vs, fs, &v, &f)) {
            free(v.code);
            free(f.code);
            if (err) *err = strdup(s_glsl_err[0] ? s_glsl_err : "SPIR-V generation failed");
            return 0;
        }
        spv_save(vs, fs, &v, &f);
    }
    *vw = v.code; *vn = v.words;
    *fw = f.code; *fn = f.words;
    return 1;
}

/* one program: GLSL -> SPIR-V, then the pipeline, as the renderer will
 * build it (16 vec4 inputs, colour and depth-stencil, blending and depth
 * on, the rest dynamic) */
static void mirror_one(Mirror *m, MJob *j)
{
    Spirv v, f;
    MBind b[MX_BIND];
    VkDescriptorSetLayoutBinding lb[MX_BIND];
    VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    VkVertexInputBindingDescription vb = { 0, 256, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[16];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineCreationFeedback fb_all = { 0 }, fb_st[2];
    VkPipelineCreationFeedbackCreateInfo fbi = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO };
    Uint64 t0;
    double t_glsl, t_pipe;
    uint32_t i, nb = 0;
    int no = ++m->n, hit = -1, spv_hit;
    const char *what = "failed";

    t0 = SDL_GetPerformanceCounter();
    spv_hit = spv_load(j->vs, j->fs, &v, &f);          /* step 3 */
    if (!spv_hit) {
        if (!glsl_to_spirv(j->vs, j->fs, &v, &f)) {
            m->fails++;
            VK_LOG("mirror %d: the GLSL did not become SPIR-V (above)\n", no);
            return;
        }
        spv_save(j->vs, j->fs, &v, &f);
    }
    t_glsl = ms_since(t0);
    if (spv_hit) { m->spv_cached_n++; m->spv_cached_ms += t_glsl; }
    memset(b, 0, sizeof b);
    spirv_bindings(&v, b);
    spirv_bindings(&f, b);
    for (i = 0; i < MX_BIND; i++)
        if (b[i].used) {
            lb[nb].binding = i;
            lb[nb].descriptorType = b[i].type;
            lb[nb].descriptorCount = 1;
            lb[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            lb[nb].pImmutableSamplers = NULL;
            nb++;
        }
    di.bindingCount = nb;
    di.pBindings = lb;
    vsm = module_of(&m->d, &v);
    fsm = module_of(&m->d, &f);
    free(v.code);
    free(f.code);
    if (!vsm || !fsm || vkCreateDescriptorSetLayout(m->d.dev, &di, NULL, &dsl) != VK_SUCCESS)
        goto done;
    li.setLayoutCount = 1;
    li.pSetLayouts = &dsl;
    if (vkCreatePipelineLayout(m->d.dev, &li, NULL, &pl) != VK_SUCCESS)
        goto done;

    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    for (i = 0; i < 16; i++) {
        va[i].location = i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = i * 16;
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 16; vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vs.viewportCount = 1; vs.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    ms_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vs; gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms_; gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
    gi.layout = pl; gi.renderPass = m->rp;
    if (m->d.feedback) {
        memset(fb_st, 0, sizeof fb_st);
        fbi.pPipelineCreationFeedback = &fb_all;
        fbi.pipelineStageCreationFeedbackCount = 2;
        fbi.pPipelineStageCreationFeedbacks = fb_st;
        gi.pNext = &fbi;
    }
    t0 = SDL_GetPerformanceCounter();
    if (vkCreateGraphicsPipelines(m->d.dev, m->d.cache, 1, &gi, NULL, &pipe) != VK_SUCCESS)
        goto done;
    t_pipe = ms_since(t0);
    if (m->d.feedback && (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT))
        hit = (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    m->gl_ms += j->gl_ms;
    m->glsl_ms += t_glsl;
    m->pipe_ms += t_pipe;
    if (j->gl_cached) { m->gl_cached_n++; m->gl_cached_ms += j->gl_ms; }
    if (hit == 1) { m->hits++; m->pipe_cached_n++; m->pipe_cached_ms += t_pipe; }
    what = hit == 1 ? "from the pipeline cache" : hit == 0 ? "compiled" : "built";
    VK_LOG("mirror %d: OpenGL %.1f ms (%s) | Vulkan: SPIR-V %.1f ms (%s), pipeline %.1f ms (%s)\n",
           no, j->gl_ms, j->gl_cached ? "its shader cache" : "compiled", t_glsl,
           spv_hit ? "its cache" : "from the GLSL", t_pipe, what);
done:
    if (!pipe) {
        m->fails++;
        VK_LOG("mirror %d: the Vulkan pipeline could not be built\n", no);
    }
    if (pipe) vkDestroyPipeline(m->d.dev, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(m->d.dev, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(m->d.dev, dsl, NULL);
    if (vsm) vkDestroyShaderModule(m->d.dev, vsm, NULL);
    if (fsm) vkDestroyShaderModule(m->d.dev, fsm, NULL);
}

static void mirror_run(void)
{
    static Mirror M;
    Mirror *m = &M;
    int since_save = 0;
    memset(m, 0, sizeof *m);
    if (!probe_device(&m->d) || !mirror_pass(m)) {
        VK_LOG("mirror: no Vulkan device for it, so no comparison\n");
        return;
    }
#if defined(_WIN32)
    CreateDirectoryA("shader_cache", NULL);
    CreateDirectoryA("shader_cache\\spirv", NULL);
#else
    mkdir("shader_cache", 0755);
    mkdir("shader_cache/spirv", 0755);
#endif
    mirror_cache_load(m);
    glslang_initialize_process();
    SDL_AtomicLock(&s_mq_lock);
    if (!s_mq_sem)
        s_mq_sem = SDL_CreateSemaphore(0);
    SDL_AtomicUnlock(&s_mq_lock);
    VK_LOG("mirror: every shader program OpenGL builds is built on Vulkan too, timed"
           " (depth %s)\n", m->zfmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32F+S8" : "D24S8");
    for (;;) {
        MJob *j = mq_take();
        if (!j) {
            if (since_save) {
                /* caught up: keep what was built, and the totals so far */
                mirror_cache_save(m);
                since_save = 0;
                VK_LOG("mirror so far: %d program(s) -- OpenGL %.0f ms in all (%d from its cache,"
                       " %.0f ms); Vulkan %.0f ms in all: SPIR-V %.0f ms (%d from its cache, %.0f ms),"
                       " pipelines %.0f ms (%d from the pipeline cache, %.0f ms)%s\n",
                       m->n, m->gl_ms, m->gl_cached_n, m->gl_cached_ms,
                       m->glsl_ms + m->pipe_ms, m->glsl_ms, m->spv_cached_n, m->spv_cached_ms,
                       m->pipe_ms, m->pipe_cached_n,
                       m->pipe_cached_ms, m->fails ? " -- some failed, see above" : "");
            }
            if (s_mq_sem)
                SDL_SemWait(s_mq_sem);
            else
                SDL_Delay(50);
            continue;
        }
        mirror_one(m, j);
        since_save++;
        free(j->vs);
        free(j->fs);
        free(j);
    }
}

static int probe_thread(void *u)
{
    (void)u;
    nv2a_vk_probe();
    mirror_run();
    return 0;
}

void nv2a_vk_probe_async(void)
{
    static int started;
    SDL_Thread *t;
    if (started++)
        return;
    VK_LOG("RECOMP_RENDERER=vkmirror: the Vulkan renderer is being built on the vulkan-test"
           " branch; for now the game draws with OpenGL, a probe tests Vulkan on this"
           " computer and every shader OpenGL builds is built on Vulkan too, timed\n");
    t = SDL_CreateThread(probe_thread, "vk_probe", NULL);
    if (t)
        SDL_DetachThread(t);
}

/* ── step 4: the window, through a Vulkan swapchain ──────────────────────
 *
 * RECOMP_RENDERER=vulkan: no OpenGL context at all. For now the frames are
 * the software rasteriser's (guest memory), handed over by fb_present.c the
 * way it hands them to GDI; each one is copied into an image and blitted
 * into the swapchain, fitted to the window (4:3 or widescreen, black bars).
 * The renderer's own GPU frames will come through the same blit. Called on
 * one thread only: the window's (Windows) or the executor's (Linux). */

#if !defined(_WIN32)
#include <SDL_vulkan.h>
#endif

#define VP_FRAMES 2          /* frames in flight */
#define VP_MAXIMG 8

typedef struct {
    int state;                       /* 0 not tried, 1 up, -1 failed */
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qfam;
    VkCommandPool pool;
    VkCommandBuffer cmd[VP_FRAMES];
    VkFence fence[VP_FRAMES];
    VkSemaphore acq[VP_FRAMES];
    VkBuffer stg[VP_FRAMES];
    VkDeviceMemory stg_mem[VP_FRAMES];
    void *stg_map[VP_FRAMES];
    VkDeviceSize stg_size[VP_FRAMES];
    uint32_t f;                      /* frame slot */
    /* the swapchain */
    VkSwapchainKHR sc;
    VkFormat fmt;
    VkPresentModeKHR mode;
    VkExtent2D ext;
    uint32_t nimg;
    VkImage img[VP_MAXIMG];
    VkSemaphore done[VP_MAXIMG];
    int stale;                       /* out of date: build it again */
    /* the frame's image */
    VkImage src;
    VkDeviceMemory src_mem;
    uint32_t src_w, src_h;
    VkFormat src_fmt;
    int req_w, req_h;                /* the window size it was made for */
    int pend_w, pend_h, pend_n;      /* vk12: a new size, and for how many frames */
    int subopt_n;                    /* vk12b: presents in a row that were suboptimal */
    uint64_t frames;
    int logged_sc;
} VkPresenter;

static VkPresenter s_vp;

static const char *vp_mode_name(VkPresentModeKHR m)
{
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo (vsync)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
    default: return "other";
    }
}

static int vp_mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s_vp.pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* The instance and the device, shared by the presenter and (step 5) the
 * renderer (glvk.c). Asks for Vulkan 1.3 and what the renderer uses
 * (dynamic rendering, push descriptors, clip distances, depth clamp, BC
 * textures, anisotropy); a device without them still presents, and the
 * renderer then stays off (vp_core_full). win: the HWND (Windows) or the
 * SDL Vulkan window (Linux, whose instance extensions SDL names). */
static int s_core_state;             /* 0 not tried, 1 up, -1 failed */
static int s_core_full;              /* the renderer's needs are all there */
static VkPhysicalDeviceProperties s_core_props;

static int vp_core(void *win)
{
    VkPresenter *p = &s_vp;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[8];
    uint32_t niext = 0, n, i, k, best = (uint32_t)-1, best_q = 0;
    int best_rank = -1;
    VkPhysicalDevice pds[16];
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *val = getenv("RECOMP_VK_VALIDATE");
    static int volk_up;

    if (s_core_state)
        return s_core_state > 0;
    s_core_state = -1;
    if (!volk_up) {
        if (volkInitialize() != VK_SUCCESS) {
            VK_LOG("no Vulkan loader (vulkan-1.dll / libvulkan.so.1): Vulkan can't be"
                   " used here\n");
            return 0;
        }
        volk_up = 1;
    }
#if defined(_WIN32)
    (void)win;
    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
    iext[niext++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
    {
        unsigned int ne = 0;
        if (!win || !SDL_Vulkan_GetInstanceExtensions((SDL_Window *)win, &ne, NULL) || ne > 8
                || !SDL_Vulkan_GetInstanceExtensions((SDL_Window *)win, &ne, iext)) {
            VK_LOG("the window can't take a Vulkan surface: %s\n", SDL_GetError());
            return 0;
        }
        niext = ne;
    }
#endif
    app.pApplicationName = "Jet Set Radio Future";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    if (val && val[0] == '1') {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ici, NULL, &p->inst) != VK_SUCCESS && val) {
        ici.enabledLayerCount = 0;
        if (vkCreateInstance(&ici, NULL, &p->inst) != VK_SUCCESS)
            p->inst = VK_NULL_HANDLE;
    }
    if (!p->inst) {
        VK_LOG("no Vulkan instance with presenting (the driver has no Vulkan, or it is"
               " too old)\n");
        return 0;
    }
    volkLoadInstance(p->inst);
    n = 16;
    if (vkEnumeratePhysicalDevices(p->inst, &n, pds) < 0 || !n) {
        VK_LOG("no Vulkan device\n");
        return 0;
    }
    /* Vulkan 1.3 first, then a discrete card, then an integrated one */
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pp;
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        int rank;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        rank = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
             : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (VK_API_VERSION_MINOR(pp.apiVersion) >= 3 || VK_API_VERSION_MAJOR(pp.apiVersion) > 1)
            rank += 4;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qf);
        for (k = 0; k < nq; k++) {
            if (!(qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            if (rank > best_rank) { best_rank = rank; best = i; best_q = k; }
            break;
        }
    }
    if (best == (uint32_t)-1) {
        VK_LOG("no Vulkan device with a graphics queue\n");
        return 0;
    }
    p->pd = pds[best];
    p->qfam = best_q;
    vkGetPhysicalDeviceProperties(p->pd, &s_core_props);
    VK_LOG("device: %s (%s), Vulkan %u.%u.%u\n", s_core_props.deviceName,
           vendor_name(s_core_props.vendorID), VK_API_VERSION_MAJOR(s_core_props.apiVersion),
           VK_API_VERSION_MINOR(s_core_props.apiVersion),
           VK_API_VERSION_PATCH(s_core_props.apiVersion));
    {
        float prio = 1.0f;
        const char *dext[4];
        uint32_t nd = 0;
        VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkPhysicalDeviceFeatures have, want;
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkExtensionProperties ep[512];
        uint32_t ne = 512;
        int v13 = VK_API_VERSION_MINOR(s_core_props.apiVersion) >= 3
                  || VK_API_VERSION_MAJOR(s_core_props.apiVersion) > 1;
        int push = 0;
        dext[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (vkEnumerateDeviceExtensionProperties(p->pd, NULL, &ne, ep) >= 0)
            for (k = 0; k < ne; k++)
                if (!strcmp(ep[k].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                    push = 1;
        if (push)
            dext[nd++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        vkGetPhysicalDeviceFeatures(p->pd, &have);
        memset(&want, 0, sizeof want);
        want.shaderClipDistance = have.shaderClipDistance;
        want.depthClamp = have.depthClamp;
        want.samplerAnisotropy = have.samplerAnisotropy;
        want.textureCompressionBC = have.textureCompressionBC;
        want.independentBlend = have.independentBlend;
        if (v13) {
            VkPhysicalDeviceVulkan13Features h13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceFeatures2 h2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            h2.pNext = &h13;
            vkGetPhysicalDeviceFeatures2(p->pd, &h2);
            f13.dynamicRendering = h13.dynamicRendering;
            f13.synchronization2 = h13.synchronization2;
            f2.features = want;
            f2.pNext = &f13;
            dci.pNext = &f2;
        } else {
            dci.pEnabledFeatures = &want;
        }
        s_core_full = v13 && f13.dynamicRendering && push && have.shaderClipDistance
                      && have.depthClamp;
        if (!s_core_full)
            VK_LOG("this device lacks what the Vulkan renderer needs (Vulkan 1.3 with dynamic"
                   " rendering, push descriptors, clip distances, depth clamp): %s%s%s%s%s\n",
                   v13 ? "" : "no 1.3 ", v13 && !f13.dynamicRendering ? "no dynamic rendering " : "",
                   push ? "" : "no push descriptors ", have.shaderClipDistance ? "" : "no clip distances ",
                   have.depthClamp ? "" : "no depth clamp");
        qi.queueFamilyIndex = p->qfam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = nd;
        dci.ppEnabledExtensionNames = dext;
        if (vkCreateDevice(p->pd, &dci, NULL, &p->dev) != VK_SUCCESS) {
            VK_LOG("the device could not be opened\n");
            return 0;
        }
        volkLoadDevice(p->dev);
        vkGetDeviceQueue(p->dev, p->qfam, 0, &p->q);
        cpi.queueFamilyIndex = p->qfam;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(p->dev, &cpi, NULL, &p->pool) != VK_SUCCESS)
            return 0;
        cai.commandPool = p->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = VP_FRAMES;
        if (vkAllocateCommandBuffers(p->dev, &cai, p->cmd) != VK_SUCCESS)
            return 0;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (i = 0; i < VP_FRAMES; i++)
            if (vkCreateFence(p->dev, &fci, NULL, &p->fence[i]) != VK_SUCCESS
                    || vkCreateSemaphore(p->dev, &sci, NULL, &p->acq[i]) != VK_SUCCESS)
                return 0;
    }
    s_core_state = 1;
    return 1;
}

int nv2a_vk_core(void *win, NvVkCore *c)
{
    if (!vp_core(win) || !s_core_full)
        return 0;
    c->inst = s_vp.inst;
    c->pd = s_vp.pd;
    c->dev = s_vp.dev;
    c->q = s_vp.q;
    c->qfam = s_vp.qfam;
    c->props = s_core_props;
    return 1;
}

/* the window's surface, once; the queue has to present to it */
static int vp_surface(void *win)
{
    VkPresenter *p = &s_vp;
    VkBool32 ok = VK_FALSE;
    if (p->surf)
        return 1;
#if defined(_WIN32)
    {
        VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        si.hinstance = GetModuleHandleA(NULL);
        si.hwnd = (HWND)win;
        if (vkCreateWin32SurfaceKHR(p->inst, &si, NULL, &p->surf) != VK_SUCCESS) {
            VK_LOG("no Vulkan surface for the window\n");
            return 0;
        }
    }
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)win, p->inst, &p->surf)) {
        VK_LOG("no Vulkan surface for the window: %s\n", SDL_GetError());
        return 0;
    }
#endif
    if (vkGetPhysicalDeviceSurfaceSupportKHR(p->pd, p->qfam, p->surf, &ok) != VK_SUCCESS || !ok) {
        VK_LOG("the device's graphics queue can't present to the window\n");
        return 0;
    }
    VK_LOG("presenting with %s\n", s_core_props.deviceName);
    return 1;
}

static void vp_swapchain_free(VkSwapchainKHR keep_old)
{
    VkPresenter *p = &s_vp;
    uint32_t i;
    for (i = 0; i < p->nimg; i++)
        if (p->done[i]) { vkDestroySemaphore(p->dev, p->done[i], NULL); p->done[i] = VK_NULL_HANDLE; }
    p->nimg = 0;
    if (p->sc && p->sc != keep_old)
        vkDestroySwapchainKHR(p->dev, p->sc, NULL);
    p->sc = VK_NULL_HANDLE;
}

/* (re)built for a w x h window; 0 when it can't be, -1 when the window has
 * no area (minimised: nothing to show) */
static int vp_swapchain(int w, int h)
{
    VkPresenter *p = &s_vp;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fm[64];
    VkPresentModeKHR pm[16];
    uint32_t nf = 64, npm = 16, i;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = p->sc;
    const char *vs = getenv("RECOMP_GL_VSYNC");
    int vsync = vs && vs[0] == '1';
    VkFormatProperties fp;

    vkDeviceWaitIdle(p->dev);
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->pd, p->surf, &caps) != VK_SUCCESS)
        return 0;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        w = (int)caps.currentExtent.width;
        h = (int)caps.currentExtent.height;
    }
    if (w < (int)caps.minImageExtent.width) w = (int)caps.minImageExtent.width;
    if (h < (int)caps.minImageExtent.height) h = (int)caps.minImageExtent.height;
    if (caps.maxImageExtent.width && w > (int)caps.maxImageExtent.width) w = (int)caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && h > (int)caps.maxImageExtent.height) h = (int)caps.maxImageExtent.height;
    if (w <= 0 || h <= 0)
        return -1;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(p->pd, p->surf, &nf, fm) < 0 || !nf)
        return 0;
    /* 8-bit BGRA or RGBA, plain first: the game's pixels are already
     * gamma-encoded. With only sRGB on offer, the frame's image is sRGB too,
     * so the blit's decode and encode cancel out. */
    p->fmt = VK_FORMAT_UNDEFINED;
    {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_FORMAT_A8B8G8R8_UNORM_PACK32,
                                         VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };
        uint32_t k;
        for (k = 0; k < sizeof pref / sizeof pref[0] && p->fmt == VK_FORMAT_UNDEFINED; k++)
            for (i = 0; i < nf; i++)
                if (fm[i].format == pref[k]) { p->fmt = pref[k]; ci.imageColorSpace = fm[i].colorSpace; break; }
        if (p->fmt == VK_FORMAT_UNDEFINED && nf == 1 && fm[0].format == VK_FORMAT_UNDEFINED) {
            p->fmt = VK_FORMAT_B8G8R8A8_UNORM;
            ci.imageColorSpace = fm[0].colorSpace;
        }
        if (p->fmt == VK_FORMAT_UNDEFINED) {
            VK_LOG("the window offers no 8-bit colour format to present in (%u formats)\n", nf);
            return 0;
        }
    }
    p->src_fmt = (p->fmt == VK_FORMAT_B8G8R8A8_SRGB || p->fmt == VK_FORMAT_R8G8B8A8_SRGB)
                 ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    vkGetPhysicalDeviceFormatProperties(p->pd, p->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("the window's format can't be blitted into\n");
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("the window's images can't be copied into\n");
        return 0;
    }
    /* RECOMP_GL_VSYNC=1: wait for the screen (fifo); else mailbox (no
     * tearing, no waiting), else immediate, else fifo, which is always there */
    p->mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync && vkGetPhysicalDeviceSurfacePresentModesKHR(p->pd, p->surf, &npm, pm) >= 0) {
        int mb = 0, im = 0;
        for (i = 0; i < npm; i++) {
            if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mb = 1;
            if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) im = 1;
        }
        p->mode = mb ? VK_PRESENT_MODE_MAILBOX_KHR : im ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                        : VK_PRESENT_MODE_FIFO_KHR;
    }
    ci.surface = p->surf;
    ci.minImageCount = caps.minImageCount + 1;
    if (p->mode == VK_PRESENT_MODE_MAILBOX_KHR && ci.minImageCount < 3)
        ci.minImageCount = 3;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = p->fmt;
    ci.imageExtent.width = (uint32_t)w;
    ci.imageExtent.height = (uint32_t)h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        static const VkCompositeAlphaFlagBitsKHR ca[] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
        for (i = 0; i < 3; i++)
            if (caps.supportedCompositeAlpha & ca[i]) { ci.compositeAlpha = ca[i]; break; }
    }
    ci.presentMode = p->mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    p->sc = VK_NULL_HANDLE;
    vp_swapchain_free(old);          /* the semaphores; the old swapchain stays for now */
    if (vkCreateSwapchainKHR(p->dev, &ci, NULL, &p->sc) != VK_SUCCESS) {
        if (old) vkDestroySwapchainKHR(p->dev, old, NULL);
        p->sc = VK_NULL_HANDLE;
        VK_LOG("the swapchain could not be made (%dx%d)\n", w, h);
        return 0;
    }
    if (old)
        vkDestroySwapchainKHR(p->dev, old, NULL);
    p->nimg = VP_MAXIMG;
    if (vkGetSwapchainImagesKHR(p->dev, p->sc, &p->nimg, p->img) < 0) {
        p->nimg = 0;
        return 0;
    }
    for (i = 0; i < p->nimg; i++) {
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(p->dev, &sci, NULL, &p->done[i]) != VK_SUCCESS)
            return 0;
    }
    p->ext = ci.imageExtent;
    p->stale = 0;
    if (p->logged_sc++ < 8)
        VK_LOG("swapchain %ux%u, %u images, %s, %s%s\n", p->ext.width, p->ext.height, p->nimg,
               p->fmt == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : p->fmt == VK_FORMAT_R8G8B8A8_UNORM
               ? "RGBA8" : "8-bit sRGB", vp_mode_name(p->mode),
               vsync ? " (RECOMP_GL_VSYNC=1)" : "");
    return 1;
}

/* the frame's image, w x h */
static int vp_src(uint32_t w, uint32_t h)
{
    VkPresenter *p = &s_vp;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkFormatProperties fp;
    int t;
    if (p->src && p->src_w == w && p->src_h == h)
        return 1;
    vkDeviceWaitIdle(p->dev);
    if (p->src) { vkDestroyImage(p->dev, p->src, NULL); p->src = VK_NULL_HANDLE; }
    if (p->src_mem) { vkFreeMemory(p->dev, p->src_mem, NULL); p->src_mem = VK_NULL_HANDLE; }
    vkGetPhysicalDeviceFormatProperties(p->pd, p->src_fmt, &fp);
    if ((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT
                                     | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            != (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        VK_LOG("BGRA8 images can't be blitted with filtering on this device\n");
        return 0;
    }
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = p->src_fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(p->dev, &ii, NULL, &p->src) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(p->dev, p->src, &mr);
    t = vp_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0)
        t = vp_mem_type(mr.memoryTypeBits, 0);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(p->dev, &ai, NULL, &p->src_mem) != VK_SUCCESS
            || vkBindImageMemory(p->dev, p->src, p->src_mem, 0) != VK_SUCCESS)
        return 0;
    p->src_w = w;
    p->src_h = h;
    return 1;
}

/* the slot's staging buffer, at least `size` bytes, mapped */
static int vp_staging(uint32_t f, VkDeviceSize size)
{
    VkPresenter *p = &s_vp;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;
    if (p->stg[f] && p->stg_size[f] >= size)
        return 1;
    if (p->stg[f]) {
        vkUnmapMemory(p->dev, p->stg_mem[f]);
        vkDestroyBuffer(p->dev, p->stg[f], NULL);
        vkFreeMemory(p->dev, p->stg_mem[f], NULL);
        p->stg[f] = VK_NULL_HANDLE;
    }
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(p->dev, &bi, NULL, &p->stg[f]) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(p->dev, p->stg[f], &mr);
    t = vp_mem_type(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(p->dev, &ai, NULL, &p->stg_mem[f]) != VK_SUCCESS
            || vkBindBufferMemory(p->dev, p->stg[f], p->stg_mem[f], 0) != VK_SUCCESS
            || vkMapMemory(p->dev, p->stg_mem[f], 0, size, 0, &p->stg_map[f]) != VK_SUCCESS)
        return 0;
    p->stg_size[f] = size;
    return 1;
}

static void vp_barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(c, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* v311g-vk11: the queue is used from two threads (the Vulkan backend's
 * frames are shown from a thread of their own), and Vulkan wants the queue
 * -- and the whole device, for vkDeviceWaitIdle -- used by one at a time */
static SDL_mutex *s_qmu;
void nv2a_vk_qlock(void)
{
    if (!s_qmu) s_qmu = SDL_CreateMutex();   /* first use is on the game thread */
    if (s_qmu) SDL_LockMutex(s_qmu);
}
void nv2a_vk_qunlock(void)
{
    if (s_qmu) SDL_UnlockMutex(s_qmu);
}

static int vp_present(void *win, const uint32_t *px, VkImage src_img, uint32_t w, uint32_t h,
                      int cw, int ch, int dx, int dy, int dw, int dh)
{
    VkPresenter *p = &s_vp;
    uint32_t f, img = 0;
    VkCommandBuffer c;
    VkResult r;
    int tries;

    if (p->state < 0)
        return 0;
    if (p->state == 0) {
        p->state = -1;
        if (!vp_core(win) || !vp_surface(win))
            return 0;
        p->state = 1;
    }
    if (!src_img && (!px || !w || !h))
        return 1;
    for (tries = 0; tries < 2; tries++) {
        /* v311g-vk12: while the window is being resized the frames go on into
         * the swapchain there is, fitted to it; a new one is made once the
         * size has held for 6 frames (each one made waits for the device:
         * dragging the window's edge made dozens, 3.2 s of waits in Miggas's
         * vk11 log). Out of date (the driver says it can't be used): now. */
        int remake = !p->sc || p->stale;
        if (!remake && (p->req_w != cw || p->req_h != ch)) {
            static int defer = -1;
            if (defer < 0) {
                const char *e = getenv("RECOMP_VK_RESIZE_SETTLE");
                defer = e ? atoi(e) : 6;
            }
            if (p->pend_w == cw && p->pend_h == ch) p->pend_n++;
            else { p->pend_w = cw; p->pend_h = ch; p->pend_n = 1; }
            remake = p->pend_n >= defer;
        }
        if (remake) {
            int s;
            nv2a_vk_qlock();
            s = vp_swapchain(cw, ch);
            nv2a_vk_qunlock();
            if (s < 0)
                return 1;            /* minimised */
            if (!s) { p->state = -1; return 0; }
            p->req_w = cw;
            p->req_h = ch;
            p->subopt_n = 0;
        }
        f = p->f;
        if (vkWaitForFences(p->dev, 1, &p->fence[f], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            VK_LOG("presenting: the device was lost\n");
            p->state = -1;
            return 0;
        }
        r = vkAcquireNextImageKHR(p->dev, p->sc, UINT64_MAX, p->acq[f], VK_NULL_HANDLE, &img);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { p->stale = 1; continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            VK_LOG("presenting: no image from the swapchain (%d)\n", (int)r);
            p->state = -1;
            return 0;
        }
        break;
    }
    if (tries == 2)
        return 1;                    /* the window keeps changing: next frame */
    if ((int)p->ext.width != cw || (int)p->ext.height != ch) {
        /* the surface's own size wins: the picture is fitted to it */
        double kx = (double)p->ext.width / (cw > 0 ? cw : 1);
        double ky = (double)p->ext.height / (ch > 0 ? ch : 1);
        dx = (int)(dx * kx); dw = (int)(dw * kx);
        dy = (int)(dy * ky); dh = (int)(dh * ky);
        cw = (int)p->ext.width; ch = (int)p->ext.height;
    }
    if (!src_img) {
        int ok;
        nv2a_vk_qlock();
        ok = vp_src(w, h) && vp_staging(f, (VkDeviceSize)w * h * 4);
        nv2a_vk_qunlock();
        if (!ok) src_img = VK_NULL_HANDLE, w = 0;
    }
    if (!src_img && !w) {
        p->state = -1;
        return 0;
    }
    vkResetFences(p->dev, 1, &p->fence[f]);
    if (!src_img)
        memcpy(p->stg_map[f], px, (size_t)w * h * 4);

    c = p->cmd[f];
    vkResetCommandBuffer(c, 0);
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkBufferImageCopy cp;
        VkClearColorValue black;
        VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageBlit bl;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(c, &bi);
        if (src_img) {
            /* step 5: the renderer's image, drawn by commands submitted
             * before this on the same queue (layout GENERAL, rows bottom-up) */
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        } else {
        /* the frame into its image (the last frame's blit read it: wait) */
        vp_barrier(c, p->src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&cp, 0, sizeof cp);
        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.imageSubresource.layerCount = 1;
        cp.imageExtent.width = w;
        cp.imageExtent.height = h;
        cp.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, p->stg[f], p->src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        vp_barrier(c, p->src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        /* the window: black, then the picture fitted into it */
        vp_barrier(c, p->img[img], VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (dx > 0 || dy > 0 || dw < cw || dh < ch) {
            memset(&black, 0, sizeof black);
            black.float32[3] = 1.0f;
            vkCmdClearColorImage(c, p->img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &all);
            vp_barrier(c, p->img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        if (dx + dw > cw) dw = cw - dx;
        if (dy + dh > ch) dh = ch - dy;
        if (dw > 0 && dh > 0) {
            memset(&bl, 0, sizeof bl);
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.srcSubresource.layerCount = 1;
            bl.srcOffsets[1].x = (int32_t)w;
            bl.srcOffsets[1].y = (int32_t)h;
            bl.srcOffsets[1].z = 1;
            if (src_img) {                   /* rows bottom-up: flipped */
                bl.srcOffsets[0].y = (int32_t)h;
                bl.srcOffsets[1].y = 0;
            }
            bl.dstSubresource = bl.srcSubresource;
            bl.dstOffsets[0].x = dx;
            bl.dstOffsets[0].y = dy;
            bl.dstOffsets[1].x = dx + dw;
            bl.dstOffsets[1].y = dy + dh;
            bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(c, src_img ? src_img : p->src,
                           src_img ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           p->img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                           (uint32_t)dw == w && (uint32_t)dh == h ? VK_FILTER_NEAREST
                                                                  : VK_FILTER_LINEAR);
        }
        vp_barrier(c, p->img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(c);
    }
    {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &p->acq[f];
        si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &p->done[img];
        nv2a_vk_qlock();
        if (vkQueueSubmit(p->q, 1, &si, p->fence[f]) != VK_SUCCESS) {
            nv2a_vk_qunlock();
            VK_LOG("presenting: the frame could not be submitted\n");
            p->state = -1;
            return 0;
        }
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &p->done[img];
        pi.swapchainCount = 1;
        pi.pSwapchains = &p->sc;
        pi.pImageIndices = &img;
        r = vkQueuePresentKHR(p->q, &pi);
        nv2a_vk_qunlock();
        if (r == VK_SUCCESS)
            p->subopt_n = 0;
        else if (r == VK_ERROR_OUT_OF_DATE_KHR)
            p->stale = 1;
        else if (r == VK_SUBOPTIMAL_KHR) {
            /* vk12b: shown, though not as well as it could be (a size on its
             * way): a new swapchain if it goes on for 6 frames. vk12 took it
             * for a failure and stopped showing frames (Miggas: F11 and
             * resizing left the last frame in the window) */
            if (++p->subopt_n >= 6) { p->stale = 1; p->subopt_n = 0; }
        } else if (r != VK_SUCCESS) {
            VK_LOG("presenting failed (%d)\n", (int)r);
            p->state = -1;
            return 0;
        }
    }
    p->f = (f + 1) % VP_FRAMES;
    if (p->frames++ == 0)
        VK_LOG("first frame shown (%ux%u into %dx%d of a %dx%d window)\n",
               w, h, dw, dh, cw, ch);
    return 1;
}

int nv2a_vk_present_frame(void *win, const uint32_t *px, uint32_t w, uint32_t h,
                          int cw, int ch, int dx, int dy, int dw, int dh)
{
    return vp_present(win, px, VK_NULL_HANDLE, w, h, cw, ch, dx, dy, dw, dh);
}

int nv2a_vk_present_image(void *win, VkImage img, uint32_t w, uint32_t h)
{
    return vp_present(win, NULL, img, w, h, (int)w, (int)h, 0, 0, (int)w, (int)h);
}

int nv2a_vk_renderer(void)
{
    static int v = -1;
    if (v < 0) {
        const char *r = getenv("RECOMP_RENDERER");
        v = r && (!strcmp(r, "vulkan") || !strcmp(r, "vk") || !strcmp(r, "glvk"));
    }
    return v;
}

int nv2a_vk_native(void)
{
    static int v = -1;
    if (v < 0) {
        const char *r = getenv("RECOMP_RENDERER");
        v = r && (!strcmp(r, "vulkan") || !strcmp(r, "vk"));
    }
    return v;
}
