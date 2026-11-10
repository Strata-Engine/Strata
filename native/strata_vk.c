/* strata_vk.c -- Strata's Vulkan shim: window, input, and the packed-quad
 * chunk renderer. One translation unit, one auditable unsafe surface.
 *
 * Shape (after Project Ascendant's voxel renderer):
 *   - every chunk's quads live in ONE large buffer (the "gigabuffer"),
 *     sub-allocated by a first-fit free list, 8 bytes per quad;
 *   - a quad is drawn as a 4-vertex triangle strip, instanced: the two packed
 *     words are per-instance attributes, the corner comes from gl_VertexIndex,
 *     so there is no per-chunk vertex or index buffer at all;
 *   - a chunk is never updated in place: an upload allocates a new range and
 *     frees the old one.
 * This first slice culls chunks on the CPU and issues one draw per visible
 * chunk layer. Compute culling and indirect draws replace that loop later
 * without changing the data layout.
 *
 * March values: a NativeArray arrives as its heap pointer; the element count
 * is at +16 and the data at +32.
 */
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "strata_shaders.h"

#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

#define SV_LAYERS 3            /* opaque, cutout, translucent */
#define SV_MAX_SLOTS 16384
#define SV_GIGA_QUADS (8u * 1024u * 1024u)   /* 64 MB of 8-byte quads */
#define SV_MAX_FREE 8192
#define SV_MAX_IMAGES 8
#define SV_MAX_KEYS 512

typedef struct { uint32_t first, count; } Range;
typedef struct {
    Range layer[SV_LAYERS];
    float origin[3];           /* chunk's low corner, in voxels */
    int   used;
} Slot;

static struct {
    GLFWwindow *win;
    VkInstance inst;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkSwapchainKHR swap;
    VkFormat format;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[SV_MAX_IMAGES];
    VkImageView views[SV_MAX_IMAGES];
    VkFramebuffer fbs[SV_MAX_IMAGES];
    VkImage depth; VkDeviceMemory depth_mem; VkImageView depth_view;
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipe_opaque, pipe_blend;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkCommandPool cpool;
    VkCommandBuffer cmd;
    VkSemaphore sem_image, sem_done;
    VkFence fence;
    VkBuffer giga; VkDeviceMemory giga_mem; uint32_t *giga_map;
    VkBuffer ubo;  VkDeviceMemory ubo_mem;  float *ubo_map;
    int ok, resized;
    /* gigabuffer free list, in quads, sorted by first */
    Range free_list[SV_MAX_FREE]; int nfree;
    uint32_t quads_live;
    Slot *slots;
    /* stats of the last frame */
    int64_t drawn_chunks, drawn_quads;
    /* input */
    unsigned char keys[SV_MAX_KEYS];
    double mx, my, mdx, mdy; int have_mouse;
    int shot_pending; char shot_path[1024];
} g;

/* The uniform block: view-projection, sun, sky, fog, voxel scale, then 256
 * palette colours indexed by a quad's texture id. Must match the shaders. */
#define UBO_FLOATS (16 + 4 + 4 + 4 + 4 + 256 * 4)

static void die(const char *what, int code) {
    fprintf(stderr, "strata_vk: %s failed (%d)\n", what, code);
}
#define VKOK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { die(what, (int)r_); return 0; } } while (0)

/* ── Gigabuffer allocator ─────────────────────────────────────────────────── */
static int giga_alloc(uint32_t n, uint32_t *first) {
    for (int i = 0; i < g.nfree; i++) {
        if (g.free_list[i].count >= n) {
            *first = g.free_list[i].first;
            g.free_list[i].first += n; g.free_list[i].count -= n;
            if (g.free_list[i].count == 0) {
                memmove(&g.free_list[i], &g.free_list[i + 1], (size_t)(g.nfree - i - 1) * sizeof(Range));
                g.nfree--;
            }
            g.quads_live += n;
            return 1;
        }
    }
    return 0;
}
static void giga_free(Range r) {
    if (r.count == 0) return;
    g.quads_live -= r.count;
    int i = 0;
    while (i < g.nfree && g.free_list[i].first < r.first) i++;
    /* merge with the range before and after where they touch */
    int merged_prev = (i > 0 && g.free_list[i - 1].first + g.free_list[i - 1].count == r.first);
    int merged_next = (i < g.nfree && r.first + r.count == g.free_list[i].first);
    if (merged_prev && merged_next) {
        g.free_list[i - 1].count += r.count + g.free_list[i].count;
        memmove(&g.free_list[i], &g.free_list[i + 1], (size_t)(g.nfree - i - 1) * sizeof(Range));
        g.nfree--;
    } else if (merged_prev) {
        g.free_list[i - 1].count += r.count;
    } else if (merged_next) {
        g.free_list[i].first = r.first; g.free_list[i].count += r.count;
    } else if (g.nfree < SV_MAX_FREE) {
        memmove(&g.free_list[i + 1], &g.free_list[i], (size_t)(g.nfree - i) * sizeof(Range));
        g.free_list[i] = r; g.nfree++;
    } /* else: the range leaks until the list has room; never corrupts */
}

/* ── Vulkan helpers ───────────────────────────────────────────────────────── */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VKOK(vkCreateBuffer(g.dev, &bi, NULL, buf), "vkCreateBuffer");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g.dev, *buf, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, mem), "vkAllocateMemory");
    VKOK(vkBindBufferMemory(g.dev, *buf, *mem, 0), "vkBindBufferMemory");
    VKOK(vkMapMemory(g.dev, *mem, 0, size, 0, map), "vkMapMemory");
    return 1;
}
static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &m) != VK_SUCCESS) die("vkCreateShaderModule", 0);
    return m;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < g.nimages; i++) {
        if (g.fbs[i]) vkDestroyFramebuffer(g.dev, g.fbs[i], NULL);
        if (g.views[i]) vkDestroyImageView(g.dev, g.views[i], NULL);
        g.fbs[i] = VK_NULL_HANDLE; g.views[i] = VK_NULL_HANDLE;
    }
    if (g.depth_view) vkDestroyImageView(g.dev, g.depth_view, NULL);
    if (g.depth) vkDestroyImage(g.dev, g.depth, NULL);
    if (g.depth_mem) vkFreeMemory(g.dev, g.depth_mem, NULL);
    g.depth_view = VK_NULL_HANDLE; g.depth = VK_NULL_HANDLE; g.depth_mem = VK_NULL_HANDLE;
    g.nimages = 0;
}

static int make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps), "surface caps");
    int fw, fh; glfwGetFramebufferSize(g.win, &fw, &fh);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = (uint32_t)fw; ext.height = (uint32_t)fh; }
    if (ext.width == 0 || ext.height == 0) return 0;
    uint32_t nfmt = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[64]; if (nfmt > 64) nfmt = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) pick = fmts[i];
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainKHR old = g.swap;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g.surface,
        .minImageCount = want, .imageFormat = pick.format, .imageColorSpace = pick.colorSpace, .imageExtent = ext,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
        .oldSwapchain = old };
    VKOK(vkCreateSwapchainKHR(g.dev, &sci, NULL, &g.swap), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(g.dev, old, NULL);
    g.format = pick.format; g.extent = ext;
    g.nimages = SV_MAX_IMAGES;
    VkResult r = vkGetSwapchainImagesKHR(g.dev, g.swap, &g.nimages, g.images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { die("swapchain images", (int)r); return 0; }

    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
        .extent = { ext.width, ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT };
    VKOK(vkCreateImage(g.dev, &ici, NULL, &g.depth), "depth image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, g.depth, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, &g.depth_mem), "depth memory");
    VKOK(vkBindImageMemory(g.dev, g.depth, g.depth_mem, 0), "depth bind");
    VkImageViewCreateInfo dvi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.depth, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    VKOK(vkCreateImageView(g.dev, &dvi, NULL, &g.depth_view), "depth view");
    return 1;
}

static int make_framebuffers(void) {
    for (uint32_t i = 0; i < g.nimages; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = g.format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VKOK(vkCreateImageView(g.dev, &vi, NULL, &g.views[i]), "image view");
        VkImageView att[2] = { g.views[i], g.depth_view };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass, .attachmentCount = 2,
            .pAttachments = att, .width = g.extent.width, .height = g.extent.height, .layers = 1 };
        VKOK(vkCreateFramebuffer(g.dev, &fi, NULL, &g.fbs[i]), "framebuffer");
    }
    return 1;
}

static int make_render_pass(void) {
    VkAttachmentDescription att[2] = {
        { .format = g.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, &g.pass), "vkCreateRenderPass");
    return 1;
}

static int make_pipeline(int blend, VkPipeline *out) {
    VkShaderModule vs = shader(strata_quad_vert, sizeof(strata_quad_vert));
    VkShaderModule fs = shader(strata_quad_frag, sizeof(strata_quad_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    /* one binding, stepped per INSTANCE: the quad's two packed words */
    VkVertexInputBindingDescription bind = { .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attrs[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 4 } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
        .depthWriteEnable = blend ? VK_FALSE : VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = g.layout, .renderPass = g.pass };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, NULL, out);
    vkDestroyShaderModule(g.dev, vs, NULL); vkDestroyShaderModule(g.dev, fs, NULL);
    if (r != VK_SUCCESS) { die("vkCreateGraphicsPipelines", (int)r); return 0; }
    return 1;
}

/* ── Input callbacks ──────────────────────────────────────────────────────── */
static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (key >= 0 && key < SV_MAX_KEYS) g.keys[key] = (action != GLFW_RELEASE);
}
static void on_cursor(GLFWwindow *w, double x, double y) {
    (void)w;
    if (g.have_mouse) { g.mdx += x - g.mx; g.mdy += y - g.my; }
    g.mx = x; g.my = y; g.have_mouse = 1;
}
static void on_resize(GLFWwindow *w, int width, int height) { (void)w; (void)width; (void)height; g.resized = 1; }

/* ── Open / close ─────────────────────────────────────────────────────────── */
int64_t sv_open(int64_t w, int64_t h, int64_t capture_mouse) {
    memset(&g, 0, sizeof g);
    if (!glfwInit()) { die("glfwInit", 0); return 0; }
    if (!glfwVulkanSupported()) { die("glfwVulkanSupported (is the Vulkan loader installed?)", 0); return 0; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g.win = glfwCreateWindow((int)w, (int)h, "Strata", NULL, NULL);
    if (!g.win) { die("glfwCreateWindow", 0); return 0; }
    glfwSetKeyCallback(g.win, on_key);
    glfwSetCursorPosCallback(g.win, on_cursor);
    glfwSetFramebufferSizeCallback(g.win, on_resize);
    if (capture_mouse) glfwSetInputMode(g.win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    uint32_t next = 0; const char **gext = glfwGetRequiredInstanceExtensions(&next);
    const char *exts[16]; uint32_t n = 0;
    for (uint32_t i = 0; i < next && n < 14; i++) exts[n++] = gext[i];
    exts[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;   /* MoltenVK is a portability driver */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "strata", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo = &app, .enabledExtensionCount = n, .ppEnabledExtensionNames = exts };
    VkResult ir = vkCreateInstance(&ici, NULL, &g.inst);
    if (ir != VK_SUCCESS) {          /* a non-portability loader rejects the flag: retry without */
        ici.flags = 0; ici.enabledExtensionCount = n - 1;
        VKOK(vkCreateInstance(&ici, NULL, &g.inst), "vkCreateInstance");
    }
    VKOK(glfwCreateWindowSurface(g.inst, g.win, NULL, &g.surface), "glfwCreateWindowSurface");

    uint32_t np = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(g.inst, &np, pds);
    if (np == 0) { die("no Vulkan device", 0); return 0; }
    g.phys = pds[0];
    uint32_t nq = 16; VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, qf);
    int found = 0;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, i, g.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { g.qfam = i; found = 1; break; }
    }
    if (!found) { die("no graphics+present queue", 0); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g.qfam, .queueCount = 1, .pQueuePriorities = &prio };
    /* enable the portability subset extension when the device advertises it, as the spec requires */
    const char *dext[4]; uint32_t nd = 0;
    dext[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(g.phys, NULL, &nde, NULL);
    VkExtensionProperties *dprops = calloc(nde ? nde : 1, sizeof *dprops);
    if (dprops) {
        vkEnumerateDeviceExtensionProperties(g.phys, NULL, &nde, dprops);
        for (uint32_t i = 0; i < nde; i++) if (strcmp(dprops[i].extensionName, "VK_KHR_portability_subset") == 0) dext[nd++] = "VK_KHR_portability_subset";
        free(dprops);
    }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext };
    VKOK(vkCreateDevice(g.phys, &dci, NULL, &g.dev), "vkCreateDevice");
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

    if (!make_swapchain()) return 0;
    if (!make_render_pass()) return 0;
    if (!make_framebuffers()) return 0;

    VkDescriptorSetLayoutBinding dslb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dslb };
    VKOK(vkCreateDescriptorSetLayout(g.dev, &dsli, NULL, &g.dsl), "descriptor set layout");
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VKOK(vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout), "pipeline layout");
    if (!make_pipeline(0, &g.pipe_opaque)) return 0;
    if (!make_pipeline(1, &g.pipe_blend)) return 0;

    void *map = NULL;
    if (!make_buffer((VkDeviceSize)SV_GIGA_QUADS * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.giga, &g.giga_mem, &map)) return 0;
    g.giga_map = map;
    if (!make_buffer(UBO_FLOATS * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g.ubo, &g.ubo_mem, &map)) return 0;
    g.ubo_map = map;
    memset(g.ubo_map, 0, UBO_FLOATS * 4);
    g.free_list[0] = (Range){ 0, SV_GIGA_QUADS }; g.nfree = 1;
    g.slots = calloc(SV_MAX_SLOTS, sizeof(Slot));
    if (!g.slots) { die("slot table", 0); return 0; }

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VKOK(vkCreateDescriptorPool(g.dev, &dpi, NULL, &g.dpool), "descriptor pool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g.dpool, .descriptorSetCount = 1, .pSetLayouts = &g.dsl };
    VKOK(vkAllocateDescriptorSets(g.dev, &dai, &g.dset), "descriptor set");
    VkDescriptorBufferInfo dbi = { g.ubo, 0, UBO_FLOATS * 4 };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(g.dev, 1, &wds, 0, NULL);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g.qfam };
    VKOK(vkCreateCommandPool(g.dev, &cpi, NULL, &g.cpool), "command pool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VKOK(vkAllocateCommandBuffers(g.dev, &cai, &g.cmd), "command buffer");
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VKOK(vkCreateSemaphore(g.dev, &si, NULL, &g.sem_image), "semaphore");
    VKOK(vkCreateSemaphore(g.dev, &si, NULL, &g.sem_done), "semaphore");
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VKOK(vkCreateFence(g.dev, &fi, NULL, &g.fence), "fence");
    g.ok = 1;
    return 1;
}

void sv_close(void) {
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        destroy_swapchain();
        if (g.swap) vkDestroySwapchainKHR(g.dev, g.swap, NULL);
        if (g.pipe_opaque) vkDestroyPipeline(g.dev, g.pipe_opaque, NULL);
        if (g.pipe_blend) vkDestroyPipeline(g.dev, g.pipe_blend, NULL);
        if (g.layout) vkDestroyPipelineLayout(g.dev, g.layout, NULL);
        if (g.dpool) vkDestroyDescriptorPool(g.dev, g.dpool, NULL);
        if (g.dsl) vkDestroyDescriptorSetLayout(g.dev, g.dsl, NULL);
        if (g.pass) vkDestroyRenderPass(g.dev, g.pass, NULL);
        if (g.giga) vkDestroyBuffer(g.dev, g.giga, NULL);
        if (g.giga_mem) vkFreeMemory(g.dev, g.giga_mem, NULL);
        if (g.ubo) vkDestroyBuffer(g.dev, g.ubo, NULL);
        if (g.ubo_mem) vkFreeMemory(g.dev, g.ubo_mem, NULL);
        if (g.sem_image) vkDestroySemaphore(g.dev, g.sem_image, NULL);
        if (g.sem_done) vkDestroySemaphore(g.dev, g.sem_done, NULL);
        if (g.fence) vkDestroyFence(g.dev, g.fence, NULL);
        if (g.cpool) vkDestroyCommandPool(g.dev, g.cpool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.surface) vkDestroySurfaceKHR(g.inst, g.surface, NULL);
    if (g.inst) vkDestroyInstance(g.inst, NULL);
    if (g.win) glfwDestroyWindow(g.win);
    glfwTerminate();
    free(g.slots);
    memset(&g, 0, sizeof g);
}

/* ── Window and input queries ─────────────────────────────────────────────── */
int64_t sv_should_close(void) { return (!g.win || glfwWindowShouldClose(g.win)) ? 1 : 0; }
void sv_poll(void) { g.mdx = 0; g.mdy = 0; if (g.win) glfwPollEvents(); }
int64_t sv_key(int64_t k) { return (k >= 0 && k < SV_MAX_KEYS) ? g.keys[k] : 0; }
double sv_mouse_dx(void) { return g.mdx; }
double sv_mouse_dy(void) { return g.mdy; }
double sv_time(void) { return glfwGetTime(); }
int64_t sv_fb_w(void) { return (int64_t)g.extent.width; }
int64_t sv_fb_h(void) { return (int64_t)g.extent.height; }
int64_t sv_drawn_chunks(void) { return g.drawn_chunks; }
int64_t sv_drawn_quads(void) { return g.drawn_quads; }
int64_t sv_live_quads(void) { return (int64_t)g.quads_live; }

/* ── Chunk uploads ────────────────────────────────────────────────────────── */
/* Replace one layer of a slot with [nquads] quads from a NativeI32Arr (two
 * words per quad). The new range is allocated before the old one is freed, so
 * a chunk is never updated in place. Returns 1, or 0 if the slot is out of
 * range, the array is short, or the gigabuffer is full (the old mesh stays). */
int64_t sv_chunk_upload(int64_t slot, int64_t layer, void *quads, int64_t nquads, double ox, double oy, double oz) {
    if (!g.ok || slot < 0 || slot >= SV_MAX_SLOTS || layer < 0 || layer >= SV_LAYERS || nquads < 0) return 0;
    if (nquads > 0 && (!quads || narr_len(quads) < nquads * 2)) return 0;
    Slot *s = &g.slots[slot];
    Range fresh = { 0, (uint32_t)nquads };
    if (nquads > 0) {
        if (!giga_alloc((uint32_t)nquads, &fresh.first)) return 0;
        memcpy(g.giga_map + (size_t)fresh.first * 2, narr_data(quads), (size_t)nquads * 8);
    }
    /* the GPU may still be reading the old range for the frame in flight */
    if (s->layer[layer].count) { vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX); giga_free(s->layer[layer]); }
    s->layer[layer] = fresh;
    s->origin[0] = (float)ox; s->origin[1] = (float)oy; s->origin[2] = (float)oz;
    s->used = 1;
    return 1;
}

void sv_chunk_free(int64_t slot) {
    if (!g.ok || slot < 0 || slot >= SV_MAX_SLOTS) return;
    Slot *s = &g.slots[slot];
    if (!s->used) return;
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    for (int l = 0; l < SV_LAYERS; l++) { giga_free(s->layer[l]); s->layer[l] = (Range){ 0, 0 }; }
    s->used = 0;
}

/* ── Frame parameters ─────────────────────────────────────────────────────── */
/* [vp] is a column-major view-projection in OpenGL clip conventions (y up,
 * z in -1..1); the vertex shader converts to Vulkan's. */
void sv_set_camera(void *vp16) {
    if (!g.ok || !vp16 || narr_len(vp16) < 16) return;
    memcpy(g.ubo_map, narr_data(vp16), 16 * 4);
}
void sv_set_sun(double x, double y, double z, double ambient) {
    if (!g.ok) return;
    float l = (float)sqrt(x * x + y * y + z * z); if (l < 1e-6f) l = 1.0f;
    g.ubo_map[16] = (float)x / l; g.ubo_map[17] = (float)y / l; g.ubo_map[18] = (float)z / l; g.ubo_map[19] = (float)ambient;
}
void sv_set_sky(double r, double gr, double b) {
    if (!g.ok) return;
    g.ubo_map[20] = (float)r; g.ubo_map[21] = (float)gr; g.ubo_map[22] = (float)b; g.ubo_map[23] = 1.0f;
}
void sv_set_fog(double start, double end, double cam_x, double cam_y) {
    (void)cam_x; (void)cam_y;
    if (!g.ok) return;
    g.ubo_map[24] = (float)start; g.ubo_map[25] = (float)end;
}
void sv_set_eye(double x, double y, double z, double scale) {
    if (!g.ok) return;
    g.ubo_map[28] = (float)x; g.ubo_map[29] = (float)y; g.ubo_map[30] = (float)z; g.ubo_map[31] = (float)scale;
}
/* Colour of texture id [i] (0..255): linear rgb plus alpha. */
void sv_set_palette(int64_t i, double r, double gr, double b, double a) {
    if (!g.ok || i < 0 || i >= 256) return;
    float *p = g.ubo_map + 32 + i * 4;
    p[0] = (float)r; p[1] = (float)gr; p[2] = (float)b; p[3] = (float)a;
}

/* ── Culling ──────────────────────────────────────────────────────────────── */
/* Is the chunk's box (side 32 voxels at [o], scaled) outside the clip volume
 * of the GL-convention view-projection in the uniform block? */
static int culled(const float *m, const float *o, float scale) {
    float c[8][4];
    for (int i = 0; i < 8; i++) {
        float x = (o[0] + ((i & 1) ? 32.0f : 0.0f)) * scale;
        float y = (o[1] + ((i & 2) ? 32.0f : 0.0f)) * scale;
        float z = (o[2] + ((i & 4) ? 32.0f : 0.0f)) * scale;
        for (int r = 0; r < 4; r++) c[i][r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
    }
    for (int p = 0; p < 6; p++) {
        int out = 0;
        for (int i = 0; i < 8; i++) {
            float w = c[i][3], v = c[i][p >> 1];
            if ((p & 1) ? (v > w) : (v < -w)) out++;
        }
        if (out == 8) return 1;
    }
    return 0;
}

static void write_shot(uint32_t image_index);

/* ── Draw ─────────────────────────────────────────────────────────────────── */
static int rebuild_swapchain(void) {
    int w = 0, h = 0; glfwGetFramebufferSize(g.win, &w, &h);
    if (w == 0 || h == 0) return 0;
    vkDeviceWaitIdle(g.dev);
    destroy_swapchain();
    if (!make_swapchain()) return 0;
    return make_framebuffers();
}

/* Draw every used slot that survives the frustum test: opaque and cutout
 * first, then translucent with blending. Returns 1 when a frame was
 * presented, 0 when it was skipped (minimised, swapchain out of date). */
int64_t sv_frame(void) {
    if (!g.ok) return 0;
    if (g.resized) { g.resized = 0; if (!rebuild_swapchain()) return 0; }
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(g.dev, g.swap, UINT64_MAX, g.sem_image, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { rebuild_swapchain(); return 0; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { die("vkAcquireNextImageKHR", (int)ar); return 0; }
    vkResetFences(g.dev, 1, &g.fence);
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(g.cmd, &bi);
    VkClearValue clear[2];
    clear[0].color.float32[0] = g.ubo_map[20]; clear[0].color.float32[1] = g.ubo_map[21];
    clear[0].color.float32[2] = g.ubo_map[22]; clear[0].color.float32[3] = 1.0f;
    clear[1].depthStencil.depth = 1.0f; clear[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass, .framebuffer = g.fbs[idx],
        .renderArea = { { 0, 0 }, g.extent }, .clearValueCount = 2, .pClearValues = clear };
    vkCmdBeginRenderPass(g.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt = { 0, 0, (float)g.extent.width, (float)g.extent.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, g.extent };
    vkCmdSetViewport(g.cmd, 0, 1, &vpt); vkCmdSetScissor(g.cmd, 0, 1, &sc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g.cmd, 0, 1, &g.giga, &zero);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.dset, 0, NULL);

    float scale = g.ubo_map[31] > 0.0f ? g.ubo_map[31] : 1.0f;
    g.drawn_chunks = 0; g.drawn_quads = 0;
    static unsigned char *vis = NULL;
    if (!vis) vis = malloc(SV_MAX_SLOTS);
    for (int pass = 0; pass < 2; pass++) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? g.pipe_opaque : g.pipe_blend);
        for (int i = 0; i < SV_MAX_SLOTS; i++) {
            Slot *s = &g.slots[i];
            if (!s->used) continue;
            if (pass == 0 && vis) vis[i] = !culled(g.ubo_map, s->origin, scale);
            if (vis && !vis[i]) continue;
            int l0 = pass == 0 ? 0 : 2, l1 = pass == 0 ? 1 : 2, pushed = 0;
            for (int l = l0; l <= l1; l++) {
                if (!s->layer[l].count) continue;
                if (!pushed) {
                    float pc[4] = { s->origin[0], s->origin[1], s->origin[2], 0.0f };
                    vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
                    pushed = 1; if (pass == 0) g.drawn_chunks++;
                }
                vkCmdDraw(g.cmd, 4, s->layer[l].count, 0, s->layer[l].first);
                g.drawn_quads += s->layer[l].count;
            }
        }
    }
    vkCmdEndRenderPass(g.cmd);
    vkEndCommandBuffer(g.cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g.sem_image, .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &g.cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &g.sem_done };
    VkResult sr = vkQueueSubmit(g.queue, 1, &sub, g.fence);
    if (sr != VK_SUCCESS) { die("vkQueueSubmit", (int)sr); return 0; }
    if (g.shot_pending) { g.shot_pending = 0; write_shot(idx); }
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g.sem_done,
        .swapchainCount = 1, .pSwapchains = &g.swap, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(g.queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g.resized = 1;
    return 1;
}

/* ── Screenshot ───────────────────────────────────────────────────────────── */
/* The NEXT frame drawn is written to a binary PPM at the path in the
 * STRATA_SHOT environment variable (default strata_shot.ppm). */
void sv_request_shot(void) {
    const char *p = getenv("STRATA_SHOT");
    snprintf(g.shot_path, sizeof g.shot_path, "%s", (p && *p) ? p : "strata_shot.ppm");
    g.shot_pending = 1;
}

static void write_shot(uint32_t idx) {
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);     /* the frame just submitted */
    VkDeviceSize size = (VkDeviceSize)g.extent.width * g.extent.height * 4;
    VkBuffer buf; VkDeviceMemory mem; void *map = NULL;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(g.dev, &cai, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g.images[idx],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { g.extent.width, g.extent.height, 1 } };
    vkCmdCopyImageToBuffer(cb, g.images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(g.queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    FILE *f = fopen(g.shot_path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", g.extent.width, g.extent.height);
        const unsigned char *px = map;
        int bgr = (g.format == VK_FORMAT_B8G8R8A8_SRGB || g.format == VK_FORMAT_B8G8R8A8_UNORM);
        for (size_t i = 0; i < (size_t)g.extent.width * g.extent.height; i++) {
            unsigned char rgb[3] = { px[i * 4 + (bgr ? 2 : 0)], px[i * 4 + 1], px[i * 4 + (bgr ? 0 : 2)] };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    vkFreeCommandBuffers(g.dev, g.cpool, 1, &cb);
    vkDestroyBuffer(g.dev, buf, NULL); vkFreeMemory(g.dev, mem, NULL);
}
