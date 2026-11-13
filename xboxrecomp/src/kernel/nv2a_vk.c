/* Vulkan renderer, step 1: the probe (see nv2a_vk.h).
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

#include <volk.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_LOG(...) fprintf(stderr, "[VK] " __VA_ARGS__)

int nv2a_vk_wanted(void)
{
    static int w = -1;
    if (w < 0) {
        const char *r = getenv("RECOMP_RENDERER"), *p = getenv("RECOMP_VK_PROBE");
        w = (r && (!strcmp(r, "vulkan") || !strcmp(r, "vk"))) || (p && p[0] == '1');
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
    if (!glslang_shader_preprocess(sh, in) || !glslang_shader_parse(sh, in)) {
        VK_LOG("GLSL (%s) did not compile:\n%s\n",
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
    if (!glslang_program_link(pg, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
        VK_LOG("GLSL did not link:\n%s\n", glslang_program_get_info_log(pg));
    else if (!glslang_program_map_io(pg))
        VK_LOG("GLSL inputs/outputs could not be mapped:\n%s\n", glslang_program_get_info_log(pg));
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

static int probe_thread(void *u)
{
    (void)u;
    nv2a_vk_probe();
    return 0;
}

void nv2a_vk_probe_async(void)
{
    static int started;
    SDL_Thread *t;
    if (started++)
        return;
    VK_LOG("RECOMP_RENDERER=vulkan: the Vulkan renderer is being built on the vulkan-test"
           " branch; for now the game draws with OpenGL and a probe tests Vulkan"
           " on this computer\n");
    t = SDL_CreateThread(probe_thread, "vk_probe", NULL);
    if (t)
        SDL_DetachThread(t);
}
