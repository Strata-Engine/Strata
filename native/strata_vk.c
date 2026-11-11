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

/* Rewritten by build_shaders.sh whenever the shaders change: forge decides
 * whether to recompile this file from its text alone, not from the headers
 * it includes (GAPS S14). */
#define STRATA_SHADERS_STAMP "c8d109e19a29030c"
#include "strata_shaders.h"
#include "strata_alloc.h"

#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

#define SV_LAYERS 3            /* opaque, cutout, translucent */
#define SV_MAX_SLOTS 16384
#define SV_DEFAULT_QUADS (8u * 1024u * 1024u)   /* 64 MB of 8-byte quads */
#define SV_FRAMES 2            /* frames in flight */
#define SV_LIGHT_PER_FRAME 128 /* chunks whose light bytes one frame may copy into the volume */
#define SV_LIGHT_WRITE_BYTES (32768 + 64 + 8)   /* per write: the voxels, the coarse cells, the owner texel */
#define SV_MAX_KEYS 512

#define SV_RANGES 6            /* 0 opaque, 1 cutout, 2 translucent, 3 cross, 4 loose, 5 spare */
#define SV_PAD_MAX 65536       /* extra zero-count commands sv_pad_commands may append */
#define SV_CMD_STRIDE (SV_MAX_SLOTS + SV_PAD_MAX)   /* commands per region of the indirect buffer */
/* Regions of the command buffer: one per compute-culled range, then one per
 * frame in flight for the translucent list, which the CPU writes and so must
 * not share with a frame the GPU is still drawing. */
#define SV_REGION_TRANS 6
#define SV_REGIONS (SV_REGION_TRANS + SV_FRAMES)

typedef SaRange Range;

/* One entry of the chunk table, as the shaders read it (std430, 64 bytes).
 * The CPU writes entries straight into the mapped buffer. A frame in flight
 * may read an entry while it changes; either version is drawable, because a
 * replaced range stays intact for two frames. */
typedef struct {
    int32_t  origin[3];        /* chunk's low corner, in voxels */
    int32_t  level_flags;      /* bits 0-7 level, bit 8 in use */
    uint32_t first[SV_RANGES];
    uint32_t count[SV_RANGES];
} ChunkEntry;
#define SV_IN_USE 256

/* An image a pass renders to and a later pass samples. */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Target;

/* What one frame in flight owns. */
typedef struct {
    VkCommandBuffer cmd;
    VkDescriptorSet dset;
    /* Each frame in flight has its own copy of everything the CPU changes
     * between frames, so a frame the GPU is still drawing never sees it move:
     * the uniform block, and the chunk table with the slots waiting to be
     * copied into it. */
    VkBuffer ubo; VkDeviceMemory ubo_mem; float *ubo_map;
    VkBuffer table; VkDeviceMemory table_mem; ChunkEntry *table_map;
    uint32_t *pending; uint32_t npending; unsigned char *is_pending;   /* slots changed since this Frame's table was last brought up to date */
    VkBuffer lstage; VkDeviceMemory lstage_mem; unsigned char *lstage_map;   /* this frame's light-volume writes, before they are copied in */
    VkBuffer stats; VkDeviceMemory stats_mem; uint32_t *stats_map;   /* written by the cull pass: commands per range [6], chunks, quads */
    int stats_valid;           /* the cull pass has run for this Frame at least once */
    VkFence fence;             /* signalled when the frame's commands have run */
    VkSemaphore sem_image, sem_done;
    int ts_written;            /* its two timestamp queries hold values */
} Frame;

static SaAlloc g_quads;        /* ranges of the quad buffer, in quads */
static SaAlloc g_loose;        /* ranges of the loose-quad buffer, in loose quads */
#define SV_LOOSE_QUADS 262144  /* 16 MB of 64-byte loose quads */
#define SV_MAX_MODELS 4096

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
    VkImage *images; VkImageView *views; VkFramebuffer *fbs;     /* nimages of each */
    VkImage depth; VkDeviceMemory depth_mem; VkImageView depth_view;
    /* what the geometry pass writes and the light pass reads, and the lit image */
    Target gb_albedo, gb_nm, lit;
    VkFramebuffer fb_g, fb_l[SV_FRAMES];
    /* What the light pass remembers between frames, one pair per frame in
     * flight: a frame reads the other's and writes its own. hist: rgb sky
     * light, a sun visibility. hmeta: r samples, g view depth, b face. */
    Target hist[SV_FRAMES], hmeta[SV_FRAMES];
    int hist_reset;                /* frames left in which that memory must not be read (fresh images) */
    VkSampler nearest;             /* for reading the frame's own images, texel for texel */
    int sky_rays, reflections;     /* light quality: sky rays a pixel a sample, 0..8; mirror rays on or off */
    int light_mode;                /* 0: the pre-ray-tracing shading, through the deferred passes */
    /* The light volume: a light class per voxel for every chunk held, the
     * coarse copy (how high each 8-voxel cell's contents reach), and which chunk each cell holds. */
    Target vol, coarse, owner;
    int vn, vnv;                   /* its size in chunks: vn across (x and z), vnv up */
    VkBuffer ltable; VkDeviceMemory ltable_mem; int32_t *ltable_map;
    float *tex_mean; uint32_t tex_mean_n;   /* each texture layer's mean colour, linear, three floats a layer */
    struct LightCell { int32_t chunk[3]; int valid; int uniform; uint32_t hash; } *cells;   /* per cell, as last queued: its chunk, and its class if it is one class throughout, else -1 */
    struct LightWrite { int cell[3]; int32_t chunk[3]; int valid; int uniform; int edit; unsigned char *bytes; } *lq;   /* writes waiting to be copied in, oldest first */
    uint32_t nlq, lq_cap;
    int32_t edit_lo[3], edit_hi[3];   /* the chunks an edit changed, while light_edit counts down */
    int light_relax;               /* frames left in which memory is capped short: the sun moved */
    float sun_seen[3];             /* the sun direction the light pass's memory was gathered under */
    int light_edit;                /* frames left in which the light pass must lean on new samples: an edit just reached the volume */
    unsigned char *stage_light; int stage_light_n;   /* the light bytes of the chunk being assembled */
    float prev_vp[16]; double time_s;
    /* an offscreen target the size of the window, for frames that must not
     * touch the swapchain (timing runs: presentation paces to the display) */
    VkImage off; VkDeviceMemory off_mem; VkImageView off_view; VkFramebuffer off_fb;
    int offscreen;
    /* geometry, light, and the final pass in its two forms (to a swapchain
     * image, to the offscreen target); the two final passes are compatible */
    VkRenderPass pass_g, pass_l, pass, pass_off;
    VkPipelineLayout layout;
    VkPipeline pipe_opaque, pipe_blend, pipe_cross, pipe_loose, pipe_cull, pipe_light, pipe_composite;
    VkBuffer loose; VkDeviceMemory loose_mem; uint32_t *loose_map;       /* model geometry, expanded per instance: 16 words a quad */
    float *models[SV_MAX_MODELS]; uint32_t model_quads[SV_MAX_MODELS];   /* 21 floats a quad, as Strata.Model lays them out */
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    ChunkEntry *entries;           /* the chunk table as the CPU keeps it, SV_MAX_SLOTS entries; each Frame has the GPU's copy */
    VkBuffer cmds;  VkDeviceMemory cmds_mem;  uint32_t *cmds_map;        /* indirect commands, SV_REGIONS * SV_CMD_STRIDE, 4 words each */
    VkBuffer dslot; VkDeviceMemory dslot_mem; uint32_t *dslot_map;       /* the slot each command draws, same shape */
    /* slots that hold translucent quads: the list the CPU orders each frame */
    uint32_t *trans; uint32_t ntrans; int32_t *trans_at;                 /* trans_at[slot] = index in trans, or -1 */
    struct TransKey { float dist; uint32_t slot; } *trans_keys;
    uint32_t submit[SV_RANGES];    /* commands the next indirect call of each range will submit */
    VkBuffer blocks; VkDeviceMemory blocks_mem; int32_t *blocks_map;     /* the block table: 8 words a block id */
    uint32_t nblocks;
    /* the texture array: one layer per texture id */
    VkImage tex; VkDeviceMemory tex_mem; VkImageView tex_view; VkSampler sampler;
    uint32_t tile_px, layers; unsigned char *tex_staging; int aniso;
    uint32_t hi_slot;              /* one past the highest slot ever used: how many the cull pass looks at */
    uint32_t pad_cmds;             /* zero-count commands appended to each indirect call (measurement) */
    VkCommandPool cpool;
    Frame frames[SV_FRAMES];
    uint64_t frame_no;         /* frames begun; frame_no % SV_FRAMES picks the Frame */
    VkBuffer giga; VkDeviceMemory giga_mem; uint32_t *giga_map; uint32_t quad_cap;
    float *ubo_map;                /* the uniform block as the CPU keeps it; each Frame has the GPU's copy */
    uint32_t rebuilds;             /* swapchain rebuilds so far */
    int ok, swap_ok, resized;
    int vsync_off;                 /* ask for the immediate present mode */
    double stat[6];                /* last frame: CPU ms, ms waiting, draw calls, GPU ms, translucent chunks drawn, microseconds ordering them */
    /* a chunk being assembled by sv_chunk_begin .. sv_chunk_commit */
    struct { int64_t slot; int32_t origin[3]; uint32_t *part[4]; uint32_t n[4], cap[4]; int open; } stage;
    VkQueryPool qpool; int have_ts; float ts_period;
    int64_t refused;               /* uploads turned away because the buffer was full */
    /* stats of the last frame */
    int64_t drawn_chunks, drawn_quads;
    /* input */
    unsigned char keys[SV_MAX_KEYS];
    double mx, my, mdx, mdy; int have_mouse;
    int shot_pending; char shot_path[1024];
} g;

/* The uniform block: view-projection, sun, sky, fog, voxel scale, then 256
 * palette colours indexed by a quad's texture id. Must match the shaders. */
/* The uniform block, in floats (native/shaders/ubo.glsl): vp 0, sun 16,
 * sky 20, fog 24, eye 28, misc 32 (ints), inv_vp 36, prev_vp 52, light 68,
 * sun colour 72, light volume size 76 (ints). */
#define UBO_FLOATS 88
#define UBO_EDIT 80            /* two vec4: the low and high corners (voxels) of what an edit just changed; the first w is 1 while it settles */
#define UBO_VOL 76             /* ints: the light volume's size in chunks */
#define UBO_INV_VP 36
#define UBO_PREV_VP 52
#define UBO_LIGHT 68
#define UBO_SUN_COLOR 72
#define SV_MAX_BLOCKS 65536    /* rows in the block table: block ids are sixteen bits */

static void die(const char *what, int code) {
    fprintf(stderr, "strata_vk: %s failed (%d)\n", what, code);
}
#define VKOK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { die(what, (int)r_); return 0; } } while (0)

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

static void drop_target(Target *t) {
    if (t->view) vkDestroyImageView(g.dev, t->view, NULL);
    if (t->img) vkDestroyImage(g.dev, t->img, NULL);
    if (t->mem) vkFreeMemory(g.dev, t->mem, NULL);
    t->view = VK_NULL_HANDLE; t->img = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < g.nimages; i++) {
        if (g.fbs && g.fbs[i]) vkDestroyFramebuffer(g.dev, g.fbs[i], NULL);
        if (g.views && g.views[i]) vkDestroyImageView(g.dev, g.views[i], NULL);
    }
    free(g.images); free(g.views); free(g.fbs);
    g.images = NULL; g.views = NULL; g.fbs = NULL; g.nimages = 0;
    if (g.off_fb) vkDestroyFramebuffer(g.dev, g.off_fb, NULL);
    if (g.off_view) vkDestroyImageView(g.dev, g.off_view, NULL);
    if (g.off) vkDestroyImage(g.dev, g.off, NULL);
    if (g.off_mem) vkFreeMemory(g.dev, g.off_mem, NULL);
    g.off_fb = VK_NULL_HANDLE; g.off_view = VK_NULL_HANDLE; g.off = VK_NULL_HANDLE; g.off_mem = VK_NULL_HANDLE;
    if (g.fb_g) vkDestroyFramebuffer(g.dev, g.fb_g, NULL);
    for (int f = 0; f < SV_FRAMES; f++) { if (g.fb_l[f]) vkDestroyFramebuffer(g.dev, g.fb_l[f], NULL); g.fb_l[f] = VK_NULL_HANDLE; drop_target(&g.hist[f]); drop_target(&g.hmeta[f]); }
    g.fb_g = VK_NULL_HANDLE;
    drop_target(&g.gb_albedo); drop_target(&g.gb_nm); drop_target(&g.lit);
    if (g.depth_view) vkDestroyImageView(g.dev, g.depth_view, NULL);
    if (g.depth) vkDestroyImage(g.dev, g.depth, NULL);
    if (g.depth_mem) vkFreeMemory(g.dev, g.depth_mem, NULL);
    g.depth_view = VK_NULL_HANDLE; g.depth = VK_NULL_HANDLE; g.depth_mem = VK_NULL_HANDLE;
}

/* FIFO (vsync) unless the immediate mode was asked for and the surface offers it. */
static VkPresentModeKHR pick_present_mode(void) {
    if (!g.vsync_off) return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 16; VkPresentModeKHR modes[16];
    VkResult r = vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, modes);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

#define FMT_ALBEDO VK_FORMAT_R8G8B8A8_SRGB
#define FMT_NM     VK_FORMAT_R8G8B8A8_UNORM
#define FMT_LIT    VK_FORMAT_R16G16B16A16_SFLOAT

/* A 2D image with its memory and view. */
static int make_image(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { g.extent.width, g.extent.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
    VKOK(vkCreateImage(g.dev, &ici, NULL, img), "vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, *img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, mem), "image memory");
    VKOK(vkBindImageMemory(g.dev, *img, *mem, 0), "image bind");
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    VKOK(vkCreateImageView(g.dev, &vi, NULL, view), "image view");
    return 1;
}

/* Build the swapchain and everything sized by it. On failure nothing is
 * left half-built: the caller sees 0 and tries again on a later frame. */
static int make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps), "surface caps");
    int fw, fh; glfwGetFramebufferSize(g.win, &fw, &fh);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = (uint32_t)fw; ext.height = (uint32_t)fh; }
    if (ext.width == 0 || ext.height == 0) return 0;          /* minimised */
    uint32_t nfmt = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[64]; if (nfmt > 64) nfmt = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &nfmt, fmts);
    if (nfmt == 0) return 0;
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
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = pick_present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old };
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VKOK(vkCreateSwapchainKHR(g.dev, &sci, NULL, &fresh), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(g.dev, old, NULL);
    g.swap = fresh; g.format = pick.format; g.extent = ext;
    if (getenv("STRATA_DEBUG")) fprintf(stderr, "strata_vk: swapchain %ux%u, present mode %d\n", ext.width, ext.height, (int)sci.presentMode);

    uint32_t n = 0;
    VKOK(vkGetSwapchainImagesKHR(g.dev, g.swap, &n, NULL), "swapchain image count");
    g.images = calloc(n ? n : 1, sizeof *g.images);
    g.views = calloc(n ? n : 1, sizeof *g.views);
    g.fbs = calloc(n ? n : 1, sizeof *g.fbs);
    if (!g.images || !g.views || !g.fbs || n == 0) return 0;
    VKOK(vkGetSwapchainImagesKHR(g.dev, g.swap, &n, g.images), "swapchain images");
    g.nimages = n;
    if (!make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &g.depth, &g.depth_mem, &g.depth_view)) return 0;
    VkImageUsageFlags tu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!make_image(FMT_ALBEDO, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.gb_albedo.img, &g.gb_albedo.mem, &g.gb_albedo.view)) return 0;
    if (!make_image(FMT_NM, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.gb_nm.img, &g.gb_nm.mem, &g.gb_nm.view)) return 0;
    if (!make_image(FMT_LIT, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.lit.img, &g.lit.mem, &g.lit.view)) return 0;
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!make_image(FMT_LIT, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.hist[f].img, &g.hist[f].mem, &g.hist[f].view)) return 0;
        if (!make_image(FMT_LIT, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.hmeta[f].img, &g.hmeta[f].mem, &g.hmeta[f].view)) return 0;
    }
    g.hist_reset = SV_FRAMES;
    if (!make_image(g.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &g.off, &g.off_mem, &g.off_view)) return 0;
    return 1;
}

/* Point every frame's descriptor set at the frame-sized images. Called when
 * they are first made and whenever the swapchain is rebuilt. */
static void bind_targets(void) {
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!g.frames[f].dset) continue;
        VkDescriptorImageInfo ii[4] = {
            { g.nearest, g.gb_albedo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.gb_nm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
            { g.nearest, g.lit.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[4];
        for (uint32_t b = 0; b < 4; b++)
            w[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 7 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[b] };
        vkUpdateDescriptorSets(g.dev, 4, w, 0, NULL);
        /* the light pass's memory: the OTHER frame's, which the frame before this one wrote */
        int o = (f + SV_FRAMES - 1) % SV_FRAMES;
        VkDescriptorImageInfo hi[2] = {
            { g.nearest, g.hist[o].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.hmeta[o].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet hw[2];
        for (uint32_t b = 0; b < 2; b++)
            hw[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 15 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &hi[b] };
        vkUpdateDescriptorSets(g.dev, 2, hw, 0, NULL);
    }
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
    VkImageView att[2] = { g.off_view, g.depth_view };
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_off, .attachmentCount = 2,
        .pAttachments = att, .width = g.extent.width, .height = g.extent.height, .layers = 1 };
    VKOK(vkCreateFramebuffer(g.dev, &fi, NULL, &g.off_fb), "offscreen framebuffer");
    VkImageView ga[3] = { g.gb_albedo.view, g.gb_nm.view, g.depth_view };
    VkFramebufferCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_g, .attachmentCount = 3,
        .pAttachments = ga, .width = g.extent.width, .height = g.extent.height, .layers = 1 };
    VKOK(vkCreateFramebuffer(g.dev, &gi, NULL, &g.fb_g), "geometry framebuffer");
    for (int f = 0; f < SV_FRAMES; f++) {
        VkImageView la[3] = { g.lit.view, g.hist[f].view, g.hmeta[f].view };
        VkFramebufferCreateInfo li = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_l, .attachmentCount = 3,
            .pAttachments = la, .width = g.extent.width, .height = g.extent.height, .layers = 1 };
        VKOK(vkCreateFramebuffer(g.dev, &li, NULL, &g.fb_l[f]), "light framebuffer");
    }
    bind_targets();
    return 1;
}

static VkAttachmentDescription color_att(VkFormat fmt, VkAttachmentLoadOp load, VkImageLayout final) {
    return (VkAttachmentDescription){ .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final };
}

/* The geometry pass: colour and normal/material targets and the depth
 * buffer, all left ready to be read by the passes after it. */
static int make_pass_g(void) {
    VkAttachmentDescription att[3] = {
        color_att(FMT_ALBEDO, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        color_att(FMT_NM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
    VkAttachmentReference cref[2] = { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
    VkAttachmentReference dref = { 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 2, .pColorAttachments = cref, .pDepthStencilAttachment = &dref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT } };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 3, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, &g.pass_g), "geometry pass");
    return 1;
}

/* The light pass: the lit image and what the pass remembers for the next
 * frame, all left ready to be sampled. */
static int make_pass_l(void) {
    VkAttachmentDescription att[3];
    VkAttachmentReference cref[3];
    for (uint32_t i = 0; i < 3; i++) {
        att[i] = color_att(FMT_LIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    }
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 3, .pColorAttachments = cref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT } };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 3, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, &g.pass_l), "light pass");
    return 1;
}

/* The final pass: the lit image goes to the screen and translucent surfaces
 * draw over it, tested against the geometry pass's depth, which is only
 * read. [final] is the layout the colour image is left in: PRESENT_SRC for a
 * swapchain image, TRANSFER_SRC for the offscreen target. The two forms are
 * compatible, so one set of pipelines serves both. */
static int make_render_pass(VkImageLayout final, VkRenderPass *out) {
    VkAttachmentDescription att[2] = {
        color_att(g.format, VK_ATTACHMENT_LOAD_OP_DONT_CARE, final),
        { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, out), "vkCreateRenderPass");
    return 1;
}

/* kind 0: opaque and cutout quads; 1: translucent quads; 2: cross instances;
 * 3: loose quads; 4: the light pass; 5: the lit image to the screen.
 * Kinds 0, 2 and 3 belong to the geometry pass, 4 to the light pass, 1 and 5
 * to the final pass. */
static int make_pipeline(int kind, VkPipeline *out) {
    int blend = kind == 1, cross = kind == 2, loose = kind == 3, full = kind >= 4, geom = kind == 0 || cross || loose;
    VkShaderModule vs = cross ? shader(strata_cross_vert, sizeof(strata_cross_vert))
                      : loose ? shader(strata_loose_vert, sizeof(strata_loose_vert))
                      : full  ? shader(strata_fullscreen_vert, sizeof(strata_fullscreen_vert)) : shader(strata_quad_vert, sizeof(strata_quad_vert));
    VkShaderModule fs = kind == 4 ? shader(strata_light_frag, sizeof(strata_light_frag))
                      : kind == 5 ? shader(strata_composite_frag, sizeof(strata_composite_frag))
                      : blend     ? shader(strata_water_frag, sizeof(strata_water_frag)) : shader(strata_gbuffer_frag, sizeof(strata_gbuffer_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    /* one binding, stepped per INSTANCE: the quad's two packed words */
    VkVertexInputBindingDescription bind = { .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attrs[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 4 } };
    /* a loose quad is 64 bytes: four corners of four words */
    VkVertexInputBindingDescription lbind = { .binding = 0, .stride = 64, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription lattrs[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = 16 },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = 32 },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = 48 } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = full ? 0 : 1, .pVertexBindingDescriptions = loose ? &lbind : &bind,
        .vertexAttributeDescriptionCount = full ? 0 : (loose ? 4 : 2), .pVertexAttributeDescriptions = loose ? lattrs : attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (cross || full) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        /* a see-through surface is visible from behind too: water from below */
        .cullMode = (kind == 0) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* only the geometry pass writes depth; translucent quads test against it; full-screen passes ignore it */
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = full ? VK_FALSE : VK_TRUE,
        .depthWriteEnable = geom ? VK_TRUE : VK_FALSE, .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendAttachmentState cba[3];
    cba[0] = (VkPipelineColorBlendAttachmentState){ .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF };
    cba[1] = cba[0]; cba[2] = cba[0];
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = geom ? 2 : (kind == 4 ? 3 : 1), .pAttachments = cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = g.layout,
        .renderPass = geom ? g.pass_g : (kind == 4 ? g.pass_l : g.pass) };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, NULL, out);
    vkDestroyShaderModule(g.dev, vs, NULL); vkDestroyShaderModule(g.dev, fs, NULL);
    if (r != VK_SUCCESS) { die("vkCreateGraphicsPipelines", (int)r); return 0; }
    return 1;
}

/* ── Textures and the block table ─────────────────────────────────────────── */
int64_t sv_textures_create(int64_t tile_px, int64_t layers);
int64_t sv_texture_set(int64_t layer, void *rgba_u8, int64_t nbytes);
int64_t sv_textures_commit(void);

/* Words 5..7 of each light class: the mean colour of its top texture, linear,
 * scaled to 0..65535. Refreshed when the classes or the textures change. */
static void light_colours(void) {
    if (!g.ltable_map) return;
    for (int c = 0; c < 256; c++) {
        int32_t *row = g.ltable_map + c * 8;
        uint32_t tex = (uint32_t)row[1];
        for (int k = 0; k < 3; k++)
            row[5 + k] = (g.tex_mean && tex < g.tex_mean_n) ? (int32_t)(g.tex_mean[tex * 3 + k] * 65535.0f) : 32768;
    }
}

static void destroy_textures(void) {
    if (g.sampler) vkDestroySampler(g.dev, g.sampler, NULL);
    if (g.tex_view) vkDestroyImageView(g.dev, g.tex_view, NULL);
    if (g.tex) vkDestroyImage(g.dev, g.tex, NULL);
    if (g.tex_mem) vkFreeMemory(g.dev, g.tex_mem, NULL);
    g.sampler = VK_NULL_HANDLE; g.tex_view = VK_NULL_HANDLE; g.tex = VK_NULL_HANDLE; g.tex_mem = VK_NULL_HANDLE;
}

/* Start a texture array: [layers] square tiles of [tile_px] pixels, a power
 * of two from 1 to 256. Pixels are set with sv_texture_set and the array
 * goes live at sv_textures_commit; until then the previous one is drawn. */
int64_t sv_textures_create(int64_t tile_px, int64_t layers) {
    if (!g.dev || tile_px < 1 || tile_px > 256 || (tile_px & (tile_px - 1)) != 0 || layers < 1 || layers > 2048) return 0;
    unsigned char *st = calloc((size_t)tile_px * (size_t)tile_px * 4, (size_t)layers);
    if (!st) return 0;
    free(g.tex_staging);
    g.tex_staging = st; g.tile_px = (uint32_t)tile_px; g.layers = (uint32_t)layers;
    return 1;
}

/* Set one layer from tile_px * tile_px RGBA bytes (sRGB colour, straight
 * alpha), rows running downward. 0 if the layer or the length is wrong. */
int64_t sv_texture_set(int64_t layer, void *rgba_u8, int64_t nbytes) {
    size_t per = (size_t)g.tile_px * g.tile_px * 4;
    if (!g.tex_staging || !rgba_u8 || layer < 0 || layer >= (int64_t)g.layers || nbytes != (int64_t)per || narr_len(rgba_u8) < nbytes) return 0;
    memcpy(g.tex_staging + per * (size_t)layer, narr_data(rgba_u8), per);
    return 1;
}

static void tex_barrier(VkCommandBuffer cb, uint32_t level, uint32_t nlevels, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g.tex,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, nlevels, 0, g.layers } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Build the array from the staged pixels, with a full mip chain, and switch
 * the renderer to it. Rare and slow: it waits for the GPU to go idle. */
int64_t sv_textures_commit(void) {
    if (!g.dev || !g.tex_staging) return 0;
    vkDeviceWaitIdle(g.dev);
    destroy_textures();
    {   /* each layer's mean colour over its opaque texels: what a ray sees of a block it hits off screen */
        float *mean = calloc((size_t)g.layers * 3, sizeof *mean);
        if (mean) {
            size_t per = (size_t)g.tile_px * g.tile_px;
            for (uint32_t l = 0; l < g.layers; l++) {
                const unsigned char *px = g.tex_staging + per * 4 * l;
                double sum[3] = { 0, 0, 0 }; size_t n = 0;
                for (size_t i = 0; i < per; i++) {
                    if (px[i * 4 + 3] < 128) continue;
                    for (int c = 0; c < 3; c++) sum[c] += pow(px[i * 4 + c] / 255.0, 2.2);
                    n++;
                }
                for (int c = 0; c < 3; c++) mean[l * 3 + c] = n ? (float)(sum[c] / (double)n) : 0.0f;
            }
            free(g.tex_mean); g.tex_mean = mean; g.tex_mean_n = g.layers;
            light_colours();
        }
    }
    uint32_t levels = 1; for (uint32_t t = g.tile_px; t > 1; t >>= 1) levels++;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = { g.tile_px, g.tile_px, 1 }, .mipLevels = levels, .arrayLayers = g.layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VKOK(vkCreateImage(g.dev, &ici, NULL, &g.tex), "texture image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, g.tex, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, &g.tex_mem), "texture memory");
    VKOK(vkBindImageMemory(g.dev, g.tex, g.tex_mem, 0), "texture bind");

    VkDeviceSize bytes = (VkDeviceSize)g.tile_px * g.tile_px * 4 * g.layers;
    VkBuffer sb; VkDeviceMemory sm; void *map = NULL;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map)) return 0;
    memcpy(map, g.tex_staging, (size_t)bytes);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(g.dev, &cai, &cb), "texture command buffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    tex_barrier(cb, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, g.layers }, .imageExtent = { g.tile_px, g.tile_px, 1 } };
    vkCmdCopyBufferToImage(cb, sb, g.tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    /* each level is a filtered copy of the one above it */
    for (uint32_t l = 1; l < levels; l++) {
        tex_barrier(cb, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        int32_t sw = (int32_t)(g.tile_px >> (l - 1)), dw = (int32_t)(g.tile_px >> l);
        VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, g.layers }, .srcOffsets = { { 0, 0, 0 }, { sw, sw, 1 } },
                             .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g.layers }, .dstOffsets = { { 0, 0, 0 }, { dw, dw, 1 } } };
        vkCmdBlitImage(cb, g.tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        tex_barrier(cb, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    tex_barrier(cb, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(g.queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.dev, g.cpool, 1, &cb);
    vkDestroyBuffer(g.dev, sb, NULL); vkFreeMemory(g.dev, sm, NULL);

    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g.tex, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = VK_FORMAT_R8G8B8A8_SRGB, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, g.layers } };
    VKOK(vkCreateImageView(g.dev, &vi, NULL, &g.tex_view), "texture view");
    /* nearest when magnified, so blocks keep their pixels; trilinear with anisotropy when minified */
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .anisotropyEnable = g.aniso ? VK_TRUE : VK_FALSE, .maxAnisotropy = g.aniso ? 8.0f : 1.0f,
        .minLod = 0.0f, .maxLod = (float)(levels - 1) };
    VKOK(vkCreateSampler(g.dev, &si, NULL, &g.sampler), "sampler");
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!g.frames[f].dset) continue;
        VkDescriptorImageInfo ii = { g.sampler, g.tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 6, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
        vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    }
    return 1;
}

/* The block table: 8 words a block id (Strata.Blocks.gpu_table). */
int64_t sv_blocks_set(void *words_i32, int64_t nblocks) {
    if (!g.blocks_map || !words_i32 || nblocks < 0 || nblocks > SV_MAX_BLOCKS || narr_len(words_i32) < nblocks * 8) return 0;
    memcpy(g.blocks_map, narr_data(words_i32), (size_t)nblocks * 32);
    g.nblocks = (uint32_t)nblocks;
    return 1;
}

/* ── The light volume ─────────────────────────────────────────────────────── */
static int make_volume(Target *t, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D, .format = fmt,
        .extent = { w, h, d }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VKOK(vkCreateImage(g.dev, &ici, NULL, &t->img), "light volume image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, t->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, &t->mem), "light volume memory");
    VKOK(vkBindImageMemory(g.dev, t->img, t->mem, 0), "light volume bind");
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_3D,
        .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VKOK(vkCreateImageView(g.dev, &vi, NULL, &t->view), "light volume view");
    return 1;
}

static void volume_layout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : (from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT),
        .dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkPipelineStageFlags ss = from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT : (from == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkPipelineStageFlags ds = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Build the volume for a stream of the given radii, in chunks: the stream's
 * box plus the one-chunk ring it keeps on unloading, so no two chunks it
 * holds share a cell. Everything already in the volume is forgotten. */
static int light_box(int64_t radius_h, int64_t radius_v) {
    if (!g.dev || radius_h < 0 || radius_v < 0 || radius_h > 30 || radius_v > 30) return 0;
    vkDeviceWaitIdle(g.dev);
    drop_target(&g.vol); drop_target(&g.coarse); drop_target(&g.owner);
    for (uint32_t i = 0; i < g.nlq; i++) free(g.lq[i].bytes);
    g.nlq = 0;
    g.vn = (int)(2 * radius_h + 3); g.vnv = (int)(2 * radius_v + 3);
    uint32_t n = (uint32_t)g.vn, nv = (uint32_t)g.vnv;
    if (!make_volume(&g.vol, VK_FORMAT_R8_UINT, n * 32, nv * 32, n * 32)) return 0;
    if (!make_volume(&g.coarse, VK_FORMAT_R8_UINT, n * 4, nv * 4, n * 4)) return 0;
    if (!make_volume(&g.owner, VK_FORMAT_R16G16B16A16_SINT, n, nv, n)) return 0;
    free(g.cells);
    g.cells = calloc((size_t)n * nv * n, sizeof *g.cells);
    if (!g.cells) { g.vn = 0; g.vnv = 0; return 0; }
    /* everything starts empty and owned by nobody */
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(g.dev, &cai, &cb), "light volume command buffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    VkImage imgs[3] = { g.vol.img, g.coarse.img, g.owner.img };
    VkClearColorValue zero; memset(&zero, 0, sizeof zero);
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (int i = 0; i < 3; i++) {
        volume_layout(cb, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        volume_layout(cb, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(g.queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.dev, g.cpool, 1, &cb);
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!g.frames[f].dset) continue;
        VkDescriptorImageInfo ii[3] = {
            { g.nearest, g.vol.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.coarse.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.owner.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[3];
        for (uint32_t b = 0; b < 3; b++)
            w[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 11 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[b] };
        vkUpdateDescriptorSets(g.dev, 3, w, 0, NULL);
    }
    return 1;
}

/* Size the light volume for a stream of these radii (chunks). Call once,
 * before the first chunk is uploaded: it empties the volume. */
int64_t sv_set_light_box(int64_t radius_h, int64_t radius_v) { return light_box(radius_h, radius_v); }

/* The light classes: 8 words a class (Strata.Blocks.light_table). */
int64_t sv_light_table_set(void *words_i32, int64_t nclasses) {
    if (!g.ltable_map || !words_i32 || nclasses < 0 || nclasses > 256 || narr_len(words_i32) < nclasses * 8) return 0;
    memset(g.ltable_map, 0, 256 * 32);
    memcpy(g.ltable_map, narr_data(words_i32), (size_t)nclasses * 32);
    light_colours();
    return 1;
}

static int wrapi(int v, int n) { int m = v % n; return m < 0 ? m + n : m; }
static struct LightCell *cell_of(const int32_t *chunk, int *cell) {
    static struct LightCell none;
    if (!g.cells || g.vn <= 0 || g.vnv <= 0) { cell[0] = cell[1] = cell[2] = 0; memset(&none, 0, sizeof none); return &none; }
    cell[0] = wrapi(chunk[0], g.vn); cell[1] = wrapi(chunk[1], g.vnv); cell[2] = wrapi(chunk[2], g.vn);
    return &g.cells[cell[0] + g.vn * (cell[1] + g.vnv * cell[2])];
}

/* Queue a write of one chunk's worth of the volume: [bytes] (32768, taken
 * over by the queue) or, when bytes is NULL, [uniform] throughout. valid 0
 * marks the cell as holding no chunk. Copied in at the start of a frame. */
static void light_queue(const int32_t *chunk, int valid, int uniform, unsigned char *bytes) {
    int cell[3];
    struct LightCell *c = cell_of(chunk, cell);
    int same_owner = c->valid && c->chunk[0] == chunk[0] && c->chunk[1] == chunk[1] && c->chunk[2] == chunk[2];
    /* nothing to do when the cell already holds exactly this: an air chunk re-uploaded, say */
    if (valid == c->valid && (!valid || same_owner) && !bytes && uniform >= 0 && c->uniform == uniform) return;
    /* the same voxels for the chunk the cell already holds (a neighbour's edit remeshed it): nothing to write */
    uint32_t hash = 2166136261u;
    if (bytes) for (int i = 0; i < 32768; i++) hash = (hash ^ bytes[i]) * 16777619u;
    if (valid && same_owner && bytes && c->uniform < 0 && c->hash == hash) { free(bytes); return; }
    /* new voxels for a chunk the volume already held: an edit, whose lighting must be re-estimated */
    int edit = valid && same_owner;
    if (g.nlq == g.lq_cap) {
        uint32_t cap = g.lq_cap ? g.lq_cap * 2 : 256;
        struct LightWrite *grown = realloc(g.lq, cap * sizeof *grown);
        if (!grown) { free(bytes); return; }
        g.lq = grown; g.lq_cap = cap;
    }
    struct LightWrite *w = &g.lq[g.nlq++];
    w->cell[0] = cell[0]; w->cell[1] = cell[1]; w->cell[2] = cell[2];
    w->chunk[0] = chunk[0]; w->chunk[1] = chunk[1]; w->chunk[2] = chunk[2];
    w->valid = valid; w->uniform = bytes ? -1 : uniform; w->bytes = bytes; w->edit = edit;
    c->chunk[0] = chunk[0]; c->chunk[1] = chunk[1]; c->chunk[2] = chunk[2];
    c->valid = valid; c->uniform = w->uniform; c->hash = hash;
}

/* Record, in this frame's commands, the copies that bring the volume up to
 * date: as many queued writes as one frame may carry, oldest first. */
static void light_flush(Frame *F) {
    if (g.nlq == 0) return;
    uint32_t n = g.nlq < SV_LIGHT_PER_FRAME ? g.nlq : SV_LIGHT_PER_FRAME;
    static VkBufferImageCopy rv[SV_LIGHT_PER_FRAME], rc[SV_LIGHT_PER_FRAME], ro[SV_LIGHT_PER_FRAME];
    for (uint32_t i = 0; i < n; i++) {
        struct LightWrite *w = &g.lq[i];
        unsigned char *dst = F->lstage_map + (size_t)i * SV_LIGHT_WRITE_BYTES;
        unsigned char *coarse = dst + 32768;
        int16_t *own = (int16_t *)(dst + 32768 + 64);
        int any = 0;                 /* how high the chunk's contents reach, 0..32 */
        if (w->bytes) {
            /* the chunk's flat index is x + 32 * (z + 32 * y); the volume wants x fastest, then y, then z */
            for (int z = 0; z < 32; z++) for (int y = 0; y < 32; y++)
                memcpy(dst + 32 * (y + 32 * z), w->bytes + 32 * (z + 32 * y), 32);
            memset(coarse, 0, 64);
            for (int z = 0; z < 32; z++) for (int y = 0; y < 32; y++) {
                const unsigned char *row = dst + 32 * (y + 32 * z);
                /* a coarse cell holds how high its contents reach, 1..8 (0: nothing): a ray above that and rising can cross the cell whole */
                for (int x = 0; x < 32; x++) if (row[x]) {
                    unsigned char *cc = &coarse[(x >> 3) + 4 * ((y >> 3) + 4 * (z >> 3))];
                    if (*cc < (y & 7) + 1) *cc = (unsigned char)((y & 7) + 1);
                    if (any < y + 1) any = y + 1;
                }
            }
        } else {
            memset(dst, w->uniform, 32768);
            memset(coarse, w->uniform ? 8 : 0, 64);
            any = w->uniform != 0 ? 32 : 0;
        }
        own[0] = (int16_t)w->chunk[0]; own[1] = (int16_t)w->chunk[1]; own[2] = (int16_t)w->chunk[2]; own[3] = (int16_t)(w->valid ? 1 + any : 0);   /* 0: no chunk here; else 1 + how high its contents reach */
        VkDeviceSize base = (VkDeviceSize)i * SV_LIGHT_WRITE_BYTES;
        rv[i] = (VkBufferImageCopy){ .bufferOffset = base, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { w->cell[0] * 32, w->cell[1] * 32, w->cell[2] * 32 }, .imageExtent = { 32, 32, 32 } };
        rc[i] = (VkBufferImageCopy){ .bufferOffset = base + 32768, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { w->cell[0] * 4, w->cell[1] * 4, w->cell[2] * 4 }, .imageExtent = { 4, 4, 4 } };
        ro[i] = (VkBufferImageCopy){ .bufferOffset = base + 32768 + 64, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { w->cell[0], w->cell[1], w->cell[2] }, .imageExtent = { 1, 1, 1 } };
        free(w->bytes);
    }
    memmove(g.lq, g.lq + n, (size_t)(g.nlq - n) * sizeof *g.lq);
    g.nlq -= n;
    VkImage imgs[3] = { g.vol.img, g.coarse.img, g.owner.img };
    VkBufferImageCopy *regs[3] = { rv, rc, ro };
    for (int k = 0; k < 3; k++) {
        volume_layout(F->cmd, imgs[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(F->cmd, F->lstage, imgs[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regs[k]);
        volume_layout(F->cmd, imgs[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
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
/* Open a window and build the renderer. [quad_capacity] is how many quads
 * the GPU buffer holds; zero or less means the default, 8 388 608 (64 MB). */
int64_t sv_open(int64_t w, int64_t h, int64_t capture_mouse, int64_t quad_capacity) {
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
    { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g.phys, &pp);
      g.have_ts = qf[g.qfam].timestampValidBits != 0; g.ts_period = pp.limits.timestampPeriod; }
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
    /* the GPU-driven path needs these two; both are in the project's baseline */
    VkPhysicalDeviceFeatures have = { 0 }; vkGetPhysicalDeviceFeatures(g.phys, &have);
    VkPhysicalDeviceVulkan11Features have11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceFeatures2 have2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have11 };
    vkGetPhysicalDeviceFeatures2(g.phys, &have2);
    /* every indirect command carries a first instance: where its chunk's quads start */
    if (!have.multiDrawIndirect || !have.drawIndirectFirstInstance || !have11.shaderDrawParameters) { die("device lacks multiDrawIndirect, drawIndirectFirstInstance or shaderDrawParameters", 0); return 0; }
    g.aniso = have.samplerAnisotropy ? 1 : 0;
    VkPhysicalDeviceFeatures feats = { .multiDrawIndirect = VK_TRUE, .drawIndirectFirstInstance = VK_TRUE, .samplerAnisotropy = have.samplerAnisotropy };
    VkPhysicalDeviceVulkan11Features feats11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .shaderDrawParameters = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &feats11, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext, .pEnabledFeatures = &feats };
    VKOK(vkCreateDevice(g.phys, &dci, NULL, &g.dev), "vkCreateDevice");
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

    if (!make_swapchain()) return 0;
    if (!make_render_pass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &g.pass)) return 0;
    if (!make_render_pass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &g.pass_off)) return 0;
    if (!make_pass_g() || !make_pass_l()) return 0;
    VkSamplerCreateInfo nsi = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VKOK(vkCreateSampler(g.dev, &nsi, NULL, &g.nearest), "nearest sampler");
    if (!make_framebuffers()) return 0;
    g.swap_ok = 1;

    /* one set layout for every pipeline: 0 uniforms, 1 chunk table, 2 indirect commands, 3 cull statistics, 4 command -> slot */
    VkDescriptorSetLayoutBinding dslb[17] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        /* 5 the block table, 6 the texture array */
        { .binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
        { .binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* the frame's own images: 7 colour, 8 normal and material, 9 depth, 10 the lit image */
        { .binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 9, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 10, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* the light volume: 11 classes, 12 the coarse copy, 13 cell owners; 14 the class table */
        { .binding = 11, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 12, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 13, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 14, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* the light pass's memory of the frame before */
        { .binding = 15, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 16, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
    VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 17, .pBindings = dslb };
    VKOK(vkCreateDescriptorSetLayout(g.dev, &dsli, NULL, &g.dsl), "descriptor set layout");
    /* where the call's range starts in draw_slot, and per-call flags (bit 0: cutout) */
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VKOK(vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout), "pipeline layout");
    if (!make_pipeline(0, &g.pipe_opaque)) return 0;
    if (!make_pipeline(1, &g.pipe_blend)) return 0;
    if (!make_pipeline(2, &g.pipe_cross)) return 0;
    if (!make_pipeline(3, &g.pipe_loose)) return 0;
    if (!make_pipeline(4, &g.pipe_light)) return 0;
    if (!make_pipeline(5, &g.pipe_composite)) return 0;

    void *map = NULL;
    g.quad_cap = (quad_capacity > 0 && quad_capacity < 0x20000000) ? (uint32_t)quad_capacity : SV_DEFAULT_QUADS;
    if (!make_buffer((VkDeviceSize)g.quad_cap * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.giga, &g.giga_mem, &map)) return 0;
    g.giga_map = map;
    g.ubo_map = calloc(UBO_FLOATS, sizeof(float));
    g.sky_rays = 2; g.reflections = 1; g.light_mode = 4;
    if (g.ubo_map) {
        /* light mode 0's flat sky (sRGB 0.53, 0.72, 0.92) and ambient share; a sun until the game sets one */
        g.ubo_map[19] = 0.30f;
        g.ubo_map[20] = (float)pow(0.53, 2.2); g.ubo_map[21] = (float)pow(0.72, 2.2); g.ubo_map[22] = (float)pow(0.92, 2.2); g.ubo_map[23] = 1.0f;
        g.ubo_map[16] = 0.458f; g.ubo_map[17] = 0.814f; g.ubo_map[18] = 0.356f;
        g.ubo_map[UBO_SUN_COLOR] = 1.05f; g.ubo_map[UBO_SUN_COLOR + 1] = 1.0f; g.ubo_map[UBO_SUN_COLOR + 2] = 0.9f;
    }
    if (!g.ubo_map) return 0;
    sa_init(&g_quads, g.quad_cap);
    if (!make_buffer((VkDeviceSize)SV_LOOSE_QUADS * 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.loose, &g.loose_mem, &map)) return 0;
    g.loose_map = map;
    sa_init(&g_loose, SV_LOOSE_QUADS);
    g.entries = calloc(SV_MAX_SLOTS, sizeof(ChunkEntry));
    if (!g.entries) return 0;
    VkDeviceSize cmd_bytes = (VkDeviceSize)SV_REGIONS * SV_CMD_STRIDE * 16;
    if (!make_buffer(cmd_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g.cmds, &g.cmds_mem, &map)) return 0;
    memset(map, 0, (size_t)cmd_bytes);            /* the padding commands stay zero-count for good */
    g.cmds_map = map;
    if (!make_buffer((VkDeviceSize)SV_REGIONS * SV_CMD_STRIDE * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.dslot, &g.dslot_mem, &map)) return 0;
    memset(map, 0, (size_t)SV_REGIONS * SV_CMD_STRIDE * 4);
    g.dslot_map = map;
    g.trans = calloc(SV_MAX_SLOTS, sizeof *g.trans);
    g.trans_at = malloc(SV_MAX_SLOTS * sizeof *g.trans_at);
    g.trans_keys = calloc(SV_MAX_SLOTS, sizeof *g.trans_keys);
    if (!g.trans || !g.trans_at || !g.trans_keys) { die("translucent list", 0); return 0; }
    for (int i = 0; i < SV_MAX_SLOTS; i++) g.trans_at[i] = -1;

    if (!make_buffer((VkDeviceSize)SV_MAX_BLOCKS * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.blocks, &g.blocks_mem, &map)) return 0;
    g.blocks_map = map;
    memset(g.blocks_map, 0, (size_t)SV_MAX_BLOCKS * 32);
    VkDescriptorPoolSize dps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SV_FRAMES }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * SV_FRAMES }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 * SV_FRAMES } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SV_FRAMES, .poolSizeCount = 3, .pPoolSizes = dps };
    VKOK(vkCreateDescriptorPool(g.dev, &dpi, NULL, &g.dpool), "descriptor pool");
    for (int f = 0; f < SV_FRAMES; f++) {
        Frame *F = &g.frames[f];
        if (!make_buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->stats, &F->stats_mem, &map)) return 0;
        F->stats_map = map; memset(F->stats_map, 0, 32);
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g.dpool, .descriptorSetCount = 1, .pSetLayouts = &g.dsl };
        VKOK(vkAllocateDescriptorSets(g.dev, &dai, &F->dset), "descriptor set");
        if (!make_buffer(UBO_FLOATS * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &F->ubo, &F->ubo_mem, &map)) return 0;
        F->ubo_map = map; memset(F->ubo_map, 0, UBO_FLOATS * 4);
        if (!make_buffer((VkDeviceSize)SV_MAX_SLOTS * sizeof(ChunkEntry), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->table, &F->table_mem, &map)) return 0;
        F->table_map = map; memset(F->table_map, 0, (size_t)SV_MAX_SLOTS * sizeof(ChunkEntry));
        if (!make_buffer((VkDeviceSize)SV_LIGHT_PER_FRAME * SV_LIGHT_WRITE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &F->lstage, &F->lstage_mem, &map)) return 0;
        F->lstage_map = map;
        F->pending = calloc(SV_MAX_SLOTS, sizeof *F->pending);
        F->is_pending = calloc(SV_MAX_SLOTS, 1);
        if (!F->pending || !F->is_pending) return 0;
        VkDescriptorBufferInfo bufs[6] = { { F->ubo, 0, UBO_FLOATS * 4 }, { F->table, 0, VK_WHOLE_SIZE }, { g.cmds, 0, VK_WHOLE_SIZE }, { F->stats, 0, 32 }, { g.dslot, 0, VK_WHOLE_SIZE }, { g.blocks, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet wds[6];
        for (uint32_t b = 0; b < 6; b++)
            wds[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = b, .descriptorCount = 1,
                .descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufs[b] };
        vkUpdateDescriptorSets(g.dev, 6, wds, 0, NULL);
    }
    bind_targets();               /* the sets did not exist when the frame's images were made */
    if (!make_buffer(256 * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.ltable, &g.ltable_mem, &map)) return 0;
    g.ltable_map = map; memset(g.ltable_map, 0, 256 * 32);
    for (int f = 0; f < SV_FRAMES; f++) {
        VkDescriptorBufferInfo lb = { g.ltable, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 14, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &lb };
        vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    }
    {   /* the culling pipeline */
        VkShaderModule cs = shader(strata_cull_comp, sizeof(strata_cull_comp));
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = g.layout,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" } };
        VkResult cr = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g.pipe_cull);
        vkDestroyShaderModule(g.dev, cs, NULL);
        if (cr != VK_SUCCESS) { die("vkCreateComputePipelines", (int)cr); return 0; }
    }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g.qfam };
    VKOK(vkCreateCommandPool(g.dev, &cpi, NULL, &g.cpool), "command pool");
    for (int f = 0; f < SV_FRAMES; f++) {
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VKOK(vkAllocateCommandBuffers(g.dev, &cai, &g.frames[f].cmd), "command buffer");
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKOK(vkCreateSemaphore(g.dev, &si, NULL, &g.frames[f].sem_image), "semaphore");
        VKOK(vkCreateSemaphore(g.dev, &si, NULL, &g.frames[f].sem_done), "semaphore");
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VKOK(vkCreateFence(g.dev, &fi, NULL, &g.frames[f].fence), "fence");
    }
    if (g.have_ts) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * SV_FRAMES };
        if (vkCreateQueryPool(g.dev, &qi, NULL, &g.qpool) != VK_SUCCESS) g.have_ts = 0;
    }
    g.stat[3] = -1.0;
    if (!light_box(0, 0)) return 0;          /* a small volume until the game says how far it streams */
    {   /* until a game supplies textures: one layer, one magenta texel */
        static const unsigned char missing[4] = { 255, 0, 255, 255 };
        if (sv_textures_create(1, 1) != 1) return 0;
        memcpy(g.tex_staging, missing, 4);
        if (sv_textures_commit() != 1) return 0;
    }
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
        if (g.pipe_cull) vkDestroyPipeline(g.dev, g.pipe_cull, NULL);
        if (g.pipe_cross) vkDestroyPipeline(g.dev, g.pipe_cross, NULL);
        if (g.pipe_loose) vkDestroyPipeline(g.dev, g.pipe_loose, NULL);
        drop_target(&g.vol); drop_target(&g.coarse); drop_target(&g.owner);
        if (g.ltable) vkDestroyBuffer(g.dev, g.ltable, NULL);
        if (g.ltable_mem) vkFreeMemory(g.dev, g.ltable_mem, NULL);
        for (int f = 0; f < SV_FRAMES; f++) {
            if (g.frames[f].lstage) vkDestroyBuffer(g.dev, g.frames[f].lstage, NULL);
            if (g.frames[f].lstage_mem) vkFreeMemory(g.dev, g.frames[f].lstage_mem, NULL);
        }
        if (g.pipe_light) vkDestroyPipeline(g.dev, g.pipe_light, NULL);
        if (g.pipe_composite) vkDestroyPipeline(g.dev, g.pipe_composite, NULL);
        if (g.pass_g) vkDestroyRenderPass(g.dev, g.pass_g, NULL);
        if (g.pass_l) vkDestroyRenderPass(g.dev, g.pass_l, NULL);
        if (g.nearest) vkDestroySampler(g.dev, g.nearest, NULL);
        if (g.loose) vkDestroyBuffer(g.dev, g.loose, NULL);
        if (g.loose_mem) vkFreeMemory(g.dev, g.loose_mem, NULL);
        for (int m = 0; m < SV_MAX_MODELS; m++) free(g.models[m]);
        if (g.cmds) vkDestroyBuffer(g.dev, g.cmds, NULL);
        if (g.cmds_mem) vkFreeMemory(g.dev, g.cmds_mem, NULL);
        if (g.dslot) vkDestroyBuffer(g.dev, g.dslot, NULL);
        if (g.dslot_mem) vkFreeMemory(g.dev, g.dslot_mem, NULL);
        if (g.blocks) vkDestroyBuffer(g.dev, g.blocks, NULL);
        if (g.blocks_mem) vkFreeMemory(g.dev, g.blocks_mem, NULL);
        destroy_textures();
        free(g.tex_staging); free(g.tex_mean);
        for (int f = 0; f < SV_FRAMES; f++) {
            if (g.frames[f].stats) vkDestroyBuffer(g.dev, g.frames[f].stats, NULL);
            if (g.frames[f].stats_mem) vkFreeMemory(g.dev, g.frames[f].stats_mem, NULL);
        }
        if (g.layout) vkDestroyPipelineLayout(g.dev, g.layout, NULL);
        if (g.dpool) vkDestroyDescriptorPool(g.dev, g.dpool, NULL);
        if (g.dsl) vkDestroyDescriptorSetLayout(g.dev, g.dsl, NULL);
        if (g.pass) vkDestroyRenderPass(g.dev, g.pass, NULL);
        if (g.giga) vkDestroyBuffer(g.dev, g.giga, NULL);
        if (g.giga_mem) vkFreeMemory(g.dev, g.giga_mem, NULL);
        for (int f = 0; f < SV_FRAMES; f++) {
            if (g.frames[f].ubo) vkDestroyBuffer(g.dev, g.frames[f].ubo, NULL);
            if (g.frames[f].ubo_mem) vkFreeMemory(g.dev, g.frames[f].ubo_mem, NULL);
            if (g.frames[f].table) vkDestroyBuffer(g.dev, g.frames[f].table, NULL);
            if (g.frames[f].table_mem) vkFreeMemory(g.dev, g.frames[f].table_mem, NULL);
            free(g.frames[f].pending); free(g.frames[f].is_pending);
        }
        for (int f = 0; f < SV_FRAMES; f++) {
            if (g.frames[f].sem_image) vkDestroySemaphore(g.dev, g.frames[f].sem_image, NULL);
            if (g.frames[f].sem_done) vkDestroySemaphore(g.dev, g.frames[f].sem_done, NULL);
            if (g.frames[f].fence) vkDestroyFence(g.dev, g.frames[f].fence, NULL);
        }
        if (g.pass_off) vkDestroyRenderPass(g.dev, g.pass_off, NULL);
        if (g.qpool) vkDestroyQueryPool(g.dev, g.qpool, NULL);
        if (g.cpool) vkDestroyCommandPool(g.dev, g.cpool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.surface) vkDestroySurfaceKHR(g.inst, g.surface, NULL);
    if (g.inst) vkDestroyInstance(g.inst, NULL);
    if (g.win) glfwDestroyWindow(g.win);
    glfwTerminate();
    free(g.trans); free(g.trans_at); free(g.trans_keys);
    free(g.ubo_map); free(g.entries); free(g.cells); free(g.stage_light);
    for (uint32_t i = 0; i < g.nlq; i++) free(g.lq[i].bytes);
    free(g.lq);
    for (int i = 0; i < 4; i++) free(g.stage.part[i]);
    memset(&g, 0, sizeof g);
}

/* ── Window and input queries ─────────────────────────────────────────────── */
int64_t sv_should_close(void) { return (!g.win || glfwWindowShouldClose(g.win)) ? 1 : 0; }
void sv_poll(void) { g.mdx = 0; g.mdy = 0; if (g.win) glfwPollEvents(); }
int64_t sv_key(int64_t k) { return (k >= 0 && k < SV_MAX_KEYS) ? g.keys[k] : 0; }
/* Is mouse button b (0 left, 1 right, 2 middle) held? */
int64_t sv_mouse_button(int64_t b) { return (g.win && b >= 0 && b <= 7 && glfwGetMouseButton(g.win, (int)b) == GLFW_PRESS) ? 1 : 0; }
double sv_mouse_dx(void) { return g.mdx; }
double sv_mouse_dy(void) { return g.mdy; }
double sv_time(void) { return glfwGetTime(); }
int64_t sv_fb_w(void) { return (int64_t)g.extent.width; }
int64_t sv_fb_h(void) { return (int64_t)g.extent.height; }
/* 0 asks for the immediate present mode (no vsync) when the surface has it. */
void sv_set_vsync(int64_t on) { int off = on ? 0 : 1; if (g.ok && off != g.vsync_off) { g.vsync_off = off; g.resized = 1; } }
/* Of the last frame: 0 ms of CPU work inside sv_frame, 1 ms it spent waiting
 * (for the previous frame's fence, the next image, and present), 2 draw
 * calls issued, 3 GPU ms or -1 if the driver
 * gives no timestamps, 4 translucent chunks drawn, 5 microseconds spent
 * ordering them. And since the start: 6 swapchain rebuilds, 7 buffer
 * elements lost because an allocator's list was full. */
double sv_stat(int64_t which) {
    if (which == 6) return (double)g.rebuilds;
    if (which == 7) return (double)g_quads.leaked + (double)g_loose.leaked;
    return (which >= 0 && which < 6) ? g.stat[which] : -1.0;
}
int64_t sv_drawn_chunks(void) { return g.drawn_chunks; }
int64_t sv_drawn_quads(void) { return g.drawn_quads; }
int64_t sv_live_quads(void) { return (int64_t)sa_live(&g_quads); }
/* Uploads turned away so far because the quad buffer was full. */
int64_t sv_refused(void) { return g.refused; }
/* Rebuild the swapchain before the next frame, as a resize would. */
void sv_force_rebuild(void) { g.resized = 1; }
/* 1: frames render to an offscreen image and are never presented, so they
 * are not paced by the display. For timing; the window shows nothing new. */
void sv_set_offscreen(int64_t on) { g.offscreen = on ? 1 : 0; }

/* Keep the list of slots holding translucent quads: add or remove one. */
static void trans_track(uint32_t slot, int has) {
    int32_t at = g.trans_at[slot];
    if (has && at < 0) { g.trans_at[slot] = (int32_t)g.ntrans; g.trans[g.ntrans++] = slot; }
    else if (!has && at >= 0) {
        uint32_t last = g.trans[--g.ntrans];
        g.trans[at] = last; g.trans_at[last] = at; g.trans_at[slot] = -1;
    }
}

/* Is the chunk's box outside the clip volume of the view-projection? */
static int culled(const float *m, const int32_t *o, float side, float scale) {
    float c[8][4];
    for (int i = 0; i < 8; i++) {
        float x = ((float)o[0] + ((i & 1) ? side : 0.0f)) * scale;
        float y = ((float)o[1] + ((i & 2) ? side : 0.0f)) * scale;
        float z = ((float)o[2] + ((i & 4) ? side : 0.0f)) * scale;
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

static int far_first(const void *a, const void *b) {
    float da = ((const struct TransKey *)a)->dist, db = ((const struct TransKey *)b)->dist;
    return da < db ? 1 : (da > db ? -1 : 0);
}

/* Write this frame's translucent commands, far to near, into the Frame's own
 * region. Returns how many. Work is proportional to the chunks that hold
 * translucent quads, not to the chunks loaded. */
static uint32_t order_translucent(uint32_t region) {
    double t0 = glfwGetTime();
    float scale = g.ubo_map[31] > 0.0f ? g.ubo_map[31] : 1.0f;
    const float *eye = g.ubo_map + 28;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.ntrans; i++) {
        const ChunkEntry *e = &g.entries[g.trans[i]];
        float side = (float)(32 << (e->level_flags & 255));
        if (culled(g.ubo_map, e->origin, side, scale)) continue;
        float cx = ((float)e->origin[0] + side * 0.5f) * scale - eye[0];
        float cy = ((float)e->origin[1] + side * 0.5f) * scale - eye[1];
        float cz = ((float)e->origin[2] + side * 0.5f) * scale - eye[2];
        g.trans_keys[n].dist = cx * cx + cy * cy + cz * cz;
        g.trans_keys[n].slot = g.trans[i];
        n++;
    }
    qsort(g.trans_keys, n, sizeof *g.trans_keys, far_first);
    uint32_t *cmd = g.cmds_map + (size_t)region * SV_CMD_STRIDE * 4;
    uint32_t *ds = g.dslot_map + (size_t)region * SV_CMD_STRIDE;
    for (uint32_t i = 0; i < n; i++) {
        const ChunkEntry *e = &g.entries[g.trans_keys[i].slot];
        cmd[i * 4 + 0] = 4; cmd[i * 4 + 1] = e->count[2]; cmd[i * 4 + 2] = 0; cmd[i * 4 + 3] = e->first[2];
        ds[i] = g.trans_keys[i].slot;
        g.drawn_quads += e->count[2];
    }
    g.stat[4] = (double)n;
    g.stat[5] = (glfwGetTime() - t0) * 1.0e6;
    return n;
}

/* ── Chunk uploads ────────────────────────────────────────────────────────── */
/* A slot's entry changed: every frame in flight must copy it before it draws. */
static void touch(uint32_t slot) {
    for (int f = 0; f < SV_FRAMES; f++) {
        Frame *F = &g.frames[f];
        if (!F->is_pending[slot]) { F->is_pending[slot] = 1; F->pending[F->npending++] = slot; }
    }
}

/* A chunk is uploaded whole or not at all:
 *
 *     sv_chunk_begin(slot, ox, oy, oz)
 *     sv_chunk_part(0, opaque quads, n)        each part is optional;
 *     sv_chunk_part(1, cutout quads, n)        a part not given is empty
 *     sv_chunk_part(2, translucent quads, n)
 *     sv_chunk_part(3, instances, n)
 *     sv_chunk_commit()
 *
 * Quads and instances are Strata.Quad's two-word records. Each part is copied
 * as it is given, so the caller's arrays need not outlive the call. Commit
 * finds room for everything first; if any of it does not fit, nothing
 * changes and the slot keeps drawing what it had. It never waits for the
 * GPU: ranges the chunk held before are reused two frames later. */
int64_t sv_chunk_begin(int64_t slot, double ox, double oy, double oz) {
    g.stage.open = 0;
    if (!g.ok || slot < 0 || slot >= SV_MAX_SLOTS) return 0;
    g.stage.slot = slot;
    g.stage.origin[0] = (int32_t)ox; g.stage.origin[1] = (int32_t)oy; g.stage.origin[2] = (int32_t)oz;
    for (int i = 0; i < 4; i++) g.stage.n[i] = 0;
    g.stage.open = 1;
    g.stage_light_n = 0;
    return 1;
}

int64_t sv_chunk_part(int64_t part, void *words, int64_t n) {
    if (!g.stage.open || part < 0 || part > 3 || n < 0 || n > 0x4000000) return 0;
    if (n > 0 && (!words || narr_len(words) < n * 2)) { g.stage.open = 0; return 0; }
    if ((uint32_t)n > g.stage.cap[part]) {
        uint32_t *grown = realloc(g.stage.part[part], (size_t)n * 8);
        if (!grown) { g.stage.open = 0; return 0; }
        g.stage.part[part] = grown; g.stage.cap[part] = (uint32_t)n;
    }
    if (n > 0) memcpy(g.stage.part[part], narr_data(words), (size_t)n * 8);
    g.stage.n[part] = (uint32_t)n;
    return 1;
}

static uint32_t fixed16(float v, float offset) {
    float f = (v + offset) * 256.0f;
    return f <= 0.0f ? 0u : (f >= 65535.0f ? 65535u : (uint32_t)(f + 0.5f));
}

/* The light classes of the chunk being assembled: 32768 bytes by the
 * chunk's flat index, or one byte when every voxel has that class. Without
 * this call the chunk is taken to be empty of anything light meets. */
int64_t sv_chunk_light(void *bytes, int64_t n) {
    if (!g.stage.open || !bytes || (n != 32768 && n != 1) || narr_len(bytes) < n) return 0;
    if (!g.stage_light) g.stage_light = malloc(32768);
    if (!g.stage_light) return 0;
    memcpy(g.stage_light, narr_data(bytes), (size_t)n);
    g.stage_light_n = (int)n;
    return 1;
}

int64_t sv_chunk_commit(void) {
    if (!g.stage.open) return 0;
    g.stage.open = 0;
    int64_t slot = g.stage.slot;
    const uint32_t *inst = g.stage.part[3];
    /* instances split two ways: crosses stay 8-byte records in the quad
     * buffer; model instances become loose quads, one per quad of the model */
    uint32_t ncross = 0, nloose = 0;
    for (uint32_t i = 0; i < g.stage.n[3]; i++) {
        uint32_t m1 = inst[i * 2] >> 15;                       /* model + 1 */
        if (m1 == 0) ncross++;
        else if (m1 - 1 < SV_MAX_MODELS) nloose += g.model_quads[m1 - 1];
    }
    uint32_t counts[4] = { g.stage.n[0], g.stage.n[1], g.stage.n[2], ncross }, firsts[4] = { 0, 0, 0, 0 }, lfirst = 0;
    if (!sa_alloc_many(&g_quads, counts, 4, firsts)) { g.refused++; return 0; }
    if (!sa_alloc(&g_loose, nloose, &lfirst)) {
        for (int i = 0; i < 4; i++) sa_cancel(&g_quads, (Range){ firsts[i], counts[i] });
        g.refused++; return 0;
    }
    /* the new ranges have been idle for two frames or were never used: no frame in flight reads them */
    for (int l = 0; l < 3; l++)
        if (counts[l]) memcpy(g.giga_map + (size_t)firsts[l] * 2, g.stage.part[l], (size_t)counts[l] * 8);
    uint32_t *cdst = g.giga_map + (size_t)firsts[3] * 2;
    uint32_t *ldst = g.loose_map + (size_t)lfirst * 16;
    for (uint32_t i = 0; i < g.stage.n[3]; i++) {
        uint32_t w0 = inst[i * 2], m1 = w0 >> 15;
        if (m1 == 0) { *cdst++ = w0; *cdst++ = inst[i * 2 + 1]; continue; }
        if (m1 - 1 >= SV_MAX_MODELS) continue;
        const float *q = g.models[m1 - 1];
        float cx = (float)(w0 & 31), cy = (float)((w0 >> 5) & 31), cz = (float)((w0 >> 10) & 31);
        for (uint32_t k = 0; k < g.model_quads[m1 - 1]; k++, q += 21) {
            uint32_t tex = q[20] <= 0.0f ? 0u : (q[20] >= 65535.0f ? 65535u : (uint32_t)(q[20] + 0.5f));
            for (int c = 0; c < 4; c++) {
                const float *v = q + c * 5;
                *ldst++ = fixed16(cx + v[0], 1.0f) | (fixed16(cy + v[1], 1.0f) << 16);
                *ldst++ = fixed16(cz + v[2], 1.0f) | (fixed16(v[3], 64.0f) << 16);
                *ldst++ = fixed16(v[4], 64.0f) | (tex << 16);
                *ldst++ = 0;
            }
        }
    }
    /* frames in flight may still read the old ranges: hold them two frames */
    ChunkEntry *e = &g.entries[slot];
    for (int r = 0; r < 4; r++) sa_release(&g_quads, (Range){ e->first[r], e->count[r] }, g.frame_no);
    sa_release(&g_loose, (Range){ e->first[4], e->count[4] }, g.frame_no);
    e->origin[0] = g.stage.origin[0]; e->origin[1] = g.stage.origin[1]; e->origin[2] = g.stage.origin[2];
    for (int r = 0; r < 4; r++) { e->first[r] = firsts[r]; e->count[r] = counts[r]; }
    e->first[4] = lfirst; e->count[4] = nloose;
    e->level_flags = SV_IN_USE;
    if ((uint32_t)slot + 1 > g.hi_slot) g.hi_slot = (uint32_t)slot + 1;
    trans_track((uint32_t)slot, counts[2] > 0);
    touch((uint32_t)slot);
    /* its voxels go into the light volume at the start of the next frame */
    int32_t chunk[3] = { e->origin[0] >> 5, e->origin[1] >> 5, e->origin[2] >> 5 };
    if (g.stage_light_n == 32768) {
        unsigned char *copy = malloc(32768);
        if (copy) { memcpy(copy, g.stage_light, 32768); light_queue(chunk, 1, -1, copy); }
    } else light_queue(chunk, 1, g.stage_light_n == 1 ? g.stage_light[0] : 0, NULL);
    return 1;
}

void sv_chunk_free(int64_t slot) {
    if (!g.ok || slot < 0 || slot >= SV_MAX_SLOTS) return;
    ChunkEntry *e = &g.entries[slot];
    if (!(e->level_flags & SV_IN_USE)) return;
    e->level_flags = 0;                      /* the cull pass skips it once the frames have their copy */
    {   /* and its cell of the light volume empties, if it is still this chunk's */
        int32_t chunk[3] = { e->origin[0] >> 5, e->origin[1] >> 5, e->origin[2] >> 5 };
        int cell[3];
        struct LightCell *c = cell_of(chunk, cell);
        if (c->valid && c->chunk[0] == chunk[0] && c->chunk[1] == chunk[1] && c->chunk[2] == chunk[2]) light_queue(chunk, 0, 0, NULL);
    }
    trans_track((uint32_t)slot, 0);
    for (int r = 0; r < 4; r++) { sa_release(&g_quads, (Range){ e->first[r], e->count[r] }, g.frame_no); e->first[r] = 0; e->count[r] = 0; }
    sa_release(&g_loose, (Range){ e->first[4], e->count[4] }, g.frame_no); e->first[4] = 0; e->count[4] = 0;
    touch((uint32_t)slot);
}

/* Give model [index] its drawing geometry: [nquads] quads of 21 floats
 * (Strata.Model). Chunks uploaded afterwards draw it; ones already uploaded
 * keep what they had until they are uploaded again. */
int64_t sv_model_set(int64_t index, void *floats, int64_t nquads) {
    if (index < 0 || index >= SV_MAX_MODELS || nquads < 0 || nquads > 4096) return 0;
    if (nquads > 0 && (!floats || narr_len(floats) < nquads * 21)) return 0;
    float *copy = nquads > 0 ? malloc((size_t)nquads * 21 * sizeof(float)) : NULL;
    if (nquads > 0 && !copy) return 0;
    if (nquads > 0) memcpy(copy, narr_data(floats), (size_t)nquads * 21 * sizeof(float));
    free(g.models[index]);
    g.models[index] = copy; g.model_quads[index] = (uint32_t)nquads;
    return 1;
}

/* Append [n] zero-count commands to every indirect call, to measure what
 * the driver charges for commands that draw nothing. */
void sv_pad_commands(int64_t n) { g.pad_cmds = n < 0 ? 0 : (n > SV_PAD_MAX ? SV_PAD_MAX : (uint32_t)n); }

/* The inverse of a column-major 4 x 4 matrix; the identity if it has none. */
static void invert4(const float *m, float *out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (det == 0.0) { for (int i = 0; i < 16; i++) out[i] = (i % 5 == 0) ? 1.0f : 0.0f; return; }
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] / det);
}

/* How the light pass shades: 0 is the renderer's shading from before ray
 * tracing, carried through the deferred passes. */
void sv_set_lighting(int64_t mode) {
    if ((int)mode != g.light_mode) g.hist_reset = SV_FRAMES;   /* what the pass remembered was gathered another way */
    g.light_mode = (int)mode;
}
/* Light quality: sky rays traced per pixel each time it samples (0..8; 0
 * falls back to a flat sky term), and whether smooth surfaces trace mirrors. */
void sv_set_quality(int64_t sky_rays, int64_t reflections) {
    int rays = sky_rays < 0 ? 0 : (sky_rays > 8 ? 8 : (int)sky_rays);
    if ((rays == 0) != (g.sky_rays == 0)) g.hist_reset = SV_FRAMES;   /* flat sky light and traced sky light do not mix */
    g.sky_rays = rays;
    g.reflections = reflections != 0;
}
/* The time, in seconds, that moving surface detail (water's ripples) runs on. */
void sv_set_time(double seconds) { g.time_s = seconds; }

/* ── Frame parameters ─────────────────────────────────────────────────────── */
/* [vp] is a column-major view-projection in OpenGL clip conventions (y up,
 * z in -1..1); the vertex shader converts to Vulkan's. */
void sv_set_camera(void *vp16) {
    if (!g.ok || !vp16 || narr_len(vp16) < 16) return;
    memcpy(g.ubo_map, narr_data(vp16), 16 * 4);
}
/* The direction to the sun and its colour (linear, as bright as wanted). */
void sv_set_sun(double x, double y, double z, double r, double gr, double b) {
    if (!g.ok) return;
    float l = (float)sqrt(x * x + y * y + z * z); if (l < 1e-6f) l = 1.0f;
    g.ubo_map[16] = (float)x / l; g.ubo_map[17] = (float)y / l; g.ubo_map[18] = (float)z / l;
    g.ubo_map[UBO_SUN_COLOR] = (float)r; g.ubo_map[UBO_SUN_COLOR + 1] = (float)gr; g.ubo_map[UBO_SUN_COLOR + 2] = (float)b;
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
static void write_shot(Frame *F, VkImage image, VkImageLayout layout);

/* ── Draw ─────────────────────────────────────────────────────────────────── */
static int rebuild_swapchain(void) {
    int w = 0, h = 0; glfwGetFramebufferSize(g.win, &w, &h);
    g.swap_ok = 0;
    if (w == 0 || h == 0) return 0;
    vkDeviceWaitIdle(g.dev);
    destroy_swapchain();
    if (!make_swapchain() || !make_framebuffers()) { destroy_swapchain(); return 0; }
    g.swap_ok = 1; g.rebuilds++;
    return 1;
}

/* A fence that can no longer be signalled (its submit failed) would hang
 * every later wait: replace it with a signalled one. */
static void heal_fence(Frame *F) {
    vkDestroyFence(g.dev, F->fence, NULL);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (vkCreateFence(g.dev, &fi, NULL, &F->fence) != VK_SUCCESS) { F->fence = VK_NULL_HANDLE; g.ok = 0; }
}

/* Draw every used slot that survives the frustum test: opaque and cutout
 * first, then translucent with blending. Returns 1 when a frame was drawn,
 * 0 when it was skipped (minimised, swapchain being rebuilt, submit failed). */
int64_t sv_frame(void) {
    if (!g.ok) return 0;
    if (g.resized || !g.swap_ok) { g.resized = 0; if (!rebuild_swapchain()) return 0; }
    Frame *F = &g.frames[g.frame_no % SV_FRAMES];
    uint32_t q0 = (uint32_t)(g.frame_no % SV_FRAMES) * 2;
    double t_start = glfwGetTime();
    /* this Frame was last used two frames ago; when its fence is signalled,
     * everything released up to then is idle */
    vkWaitForFences(g.dev, 1, &F->fence, VK_TRUE, UINT64_MAX);
    double wait_ms = (glfwGetTime() - t_start) * 1000.0;
    sa_collect(&g_quads, g.frame_no);
    sa_collect(&g_loose, g.frame_no);
    /* what this Frame's cull pass let through, two frames ago */
    g.drawn_chunks = F->stats_map[6]; g.drawn_quads = F->stats_map[7];
    /* How many commands to submit for each compacted range: what this Frame's
     * cull pass counted two frames ago, plus a quarter and 256 for a view
     * that has turned since. If that pass overflowed its allowance, or has
     * not run yet, submit every slot once so nothing stays missing. */
    for (int r = 0; r < SV_RANGES; r++) {
        uint32_t seen = F->stats_map[r], allowed = g.submit[r];
        uint32_t next = seen + seen / 4 + 256;
        if (!F->stats_valid || seen > allowed) next = g.hi_slot;
        g.submit[r] = next > g.hi_slot ? g.hi_slot : next;
    }
    memset(F->stats_map, 0, 32);
    F->stats_valid = 1;
    if (g.have_ts && F->ts_written) {
        uint64_t ts[2] = { 0, 0 };
        if (vkGetQueryPoolResults(g.dev, g.qpool, q0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            g.stat[3] = (double)(ts[1] - ts[0]) * (double)g.ts_period / 1.0e6;
    }
    uint32_t idx = 0;
    int offscreen = g.offscreen;
    if (!offscreen) {
        double t_acq = glfwGetTime();
        VkResult ar = vkAcquireNextImageKHR(g.dev, g.swap, UINT64_MAX, F->sem_image, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { g.resized = 1; return 0; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { die("vkAcquireNextImageKHR", (int)ar); return 0; }
        if (idx >= g.nimages) { die("swapchain image index out of range", (int)idx); return 0; }
        wait_ms += (glfwGetTime() - t_acq) * 1000.0;
    }
    int draws = 0;
    /* this Frame's fence has been waited on, so the GPU is done with its region */
    uint32_t trans_region = SV_REGION_TRANS + (uint32_t)(g.frame_no % SV_FRAMES);
    uint32_t ntrans = order_translucent(trans_region);
    vkResetCommandBuffer(F->cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(F->cmd, &bi);
    if (g.have_ts) { vkCmdResetQueryPool(F->cmd, g.qpool, q0, 2); vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.qpool, q0); }
    /* cull: one compute invocation per slot writes the indirect commands */
    int32_t *misc = (int32_t *)(g.ubo_map + 32);
    misc[0] = (int32_t)g.hi_slot; misc[1] = SV_CMD_STRIDE; misc[2] = (int32_t)g.layers; misc[3] = (int32_t)g.nblocks;
    invert4(g.ubo_map, g.ubo_map + UBO_INV_VP);
    memcpy(g.ubo_map + UBO_PREV_VP, g.prev_vp, sizeof g.prev_vp);
    memcpy(g.prev_vp, g.ubo_map, sizeof g.prev_vp);
    g.ubo_map[UBO_LIGHT + 0] = (float)g.light_mode; g.ubo_map[UBO_LIGHT + 1] = (float)g.time_s;
    g.ubo_map[UBO_LIGHT + 2] = (float)(g.frame_no & 0xFFFFFF);
    g.ubo_map[UBO_LIGHT + 3] = (float)g.sky_rays; g.ubo_map[UBO_SUN_COLOR + 3] = (float)g.reflections;
    { int32_t *vol = (int32_t *)(g.ubo_map + UBO_VOL); vol[0] = g.vn; vol[1] = g.vnv; vol[2] = g.vn;
      /* how many past samples a pixel may lean on: none while the remembered
       * images are fresh, 2 just after the sun moved, else 255 (sun visibility
       * leans on 64 of them at most). An edit that reaches the volume this
       * frame caps only the pixels near it: its bounds go to the shader. */
      for (uint32_t i = 0; i < g.nlq && i < SV_LIGHT_PER_FRAME; i++) if (g.lq[i].edit) {
          const int32_t *ch = g.lq[i].chunk;
          for (int k = 0; k < 3; k++) {
              if (g.light_edit == 0 || ch[k] < g.edit_lo[k]) g.edit_lo[k] = ch[k];
              if (g.light_edit == 0 || ch[k] > g.edit_hi[k]) g.edit_hi[k] = ch[k];
          }
          g.light_edit = 2;
      }
      /* a sun that has moved more than a fraction of a degree since the memory was gathered: lean on it less */
      float sd = g.ubo_map[16] * g.sun_seen[0] + g.ubo_map[17] * g.sun_seen[1] + g.ubo_map[18] * g.sun_seen[2];
      if (sd < 0.99999f) { g.light_relax = 2; memcpy(g.sun_seen, g.ubo_map + 16, sizeof g.sun_seen); }
      vol[3] = g.hist_reset > 0 ? 0 : (g.light_relax > 0 ? 2 : 255);
      float *ed = g.ubo_map + UBO_EDIT;
      for (int k = 0; k < 3; k++) { ed[k] = (float)(g.edit_lo[k] * 32); ed[4 + k] = (float)(g.edit_hi[k] * 32 + 32); }
      ed[3] = g.light_edit > 0 ? 1.0f : 0.0f; ed[7] = 0.0f;
      if (g.hist_reset > 0) g.hist_reset--;
      if (g.light_relax > 0) g.light_relax--;
      if (g.light_edit > 0) g.light_edit--; }
    /* this Frame's fence has been waited on, so the GPU is done with its
     * copies: bring them up to date. From here to the next time this Frame
     * comes round, nothing the CPU does can change what it draws. */
    memcpy(F->ubo_map, g.ubo_map, UBO_FLOATS * 4);
    for (uint32_t i = 0; i < F->npending; i++) {
        uint32_t slot = F->pending[i];
        F->table_map[slot] = g.entries[slot];
        F->is_pending[slot] = 0;
    }
    F->npending = 0;
    light_flush(F);               /* queued chunks into the light volume, before anything reads it */
    vkCmdBindDescriptorSets(F->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &F->dset, 0, NULL);
    if (g.hi_slot > 0) {
        /* zero the part of each compacted list that will be submitted, so
         * commands the cull pass does not overwrite draw nothing */
        int filled = 0;
        static const int culled_ranges[4] = { 0, 1, 3, 4 };
        for (int k = 0; k < 4; k++) {
            int r = culled_ranges[k];
            uint32_t n = g.submit[r] + g.pad_cmds;
            if (n) { vkCmdFillBuffer(F->cmd, g.cmds, (VkDeviceSize)r * SV_CMD_STRIDE * 16, (VkDeviceSize)n * 16, 0); filled = 1; }
        }
        if (filled) {
            VkMemoryBarrier fb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT };
            vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fb, 0, NULL, 0, NULL);
        }
        vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe_cull);
        vkCmdDispatch(F->cmd, (g.hi_slot + 63) / 64, 1, 1);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    VkViewport vpt = { 0, 0, (float)g.extent.width, (float)g.extent.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, g.extent };
    VkDeviceSize zero = 0;

    /* 1. geometry: the solid ranges write colour, normal, material and depth.
     * One multi-draw-indirect call per range; the push constant tells the
     * vertex shader where the call's commands start in draw_slot. */
    VkClearValue gclear[3];
    memset(gclear, 0, sizeof gclear);
    gclear[2].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo gbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass_g, .framebuffer = g.fb_g,
        .renderArea = { { 0, 0 }, g.extent }, .clearValueCount = 3, .pClearValues = gclear };
    vkCmdBeginRenderPass(F->cmd, &gbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(F->cmd, 0, 1, &vpt); vkCmdSetScissor(F->cmd, 0, 1, &sc);
    vkCmdBindVertexBuffers(F->cmd, 0, 1, &g.giga, &zero);
    vkCmdBindDescriptorSets(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &F->dset, 0, NULL);
    static const uint32_t solid_ranges[4] = { 0, 1, 3, 4 };    /* opaque quads, cutout quads, cross instances, loose quads */
    for (int k = 0; k < 4; k++) {
        uint32_t r = solid_ranges[k];
        uint32_t n = g.submit[r] + g.pad_cmds, base = r * SV_CMD_STRIDE;
        if (n == 0) continue;
        if (r == 0) vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_opaque);
        if (r == 3) vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_cross);
        if (r == 4) { vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_loose); vkCmdBindVertexBuffers(F->cmd, 0, 1, &g.loose, &zero); }
        uint32_t pc[2] = { base, r >= 1 ? 1u : 0u };          /* everything but plain opaque quads discards see-through pixels */
        vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, pc);
        vkCmdDrawIndirect(F->cmd, g.cmds, (VkDeviceSize)base * 16, n, 16); draws++;
    }
    vkCmdEndRenderPass(F->cmd);

    /* 2. light: one full-screen triangle reads those buffers and writes the lit image */
    VkRenderPassBeginInfo lbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass_l, .framebuffer = g.fb_l[g.frame_no % SV_FRAMES],
        .renderArea = { { 0, 0 }, g.extent } };
    vkCmdBeginRenderPass(F->cmd, &lbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(F->cmd, 0, 1, &vpt); vkCmdSetScissor(F->cmd, 0, 1, &sc);
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_light);
    vkCmdDraw(F->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(F->cmd);

    /* 3. final: the lit image to the screen, then translucent surfaces over
     * it, far to near, blended, tested against the geometry pass's depth */
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = offscreen ? g.pass_off : g.pass, .framebuffer = offscreen ? g.off_fb : g.fbs[idx],
        .renderArea = { { 0, 0 }, g.extent } };
    vkCmdBeginRenderPass(F->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(F->cmd, 0, 1, &vpt); vkCmdSetScissor(F->cmd, 0, 1, &sc);
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_composite);
    vkCmdDraw(F->cmd, 3, 1, 0, 0);
    if (ntrans > 0) {
        vkCmdBindVertexBuffers(F->cmd, 0, 1, &g.giga, &zero);
        uint32_t base = trans_region * SV_CMD_STRIDE;
        uint32_t pc[2] = { base, 0u };
        vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_blend);
        vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, pc);
        vkCmdDrawIndirect(F->cmd, g.cmds, (VkDeviceSize)base * 16, ntrans, 16); draws++;
    }
    vkCmdEndRenderPass(F->cmd);
    if (g.have_ts) vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 1);
    vkEndCommandBuffer(F->cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &F->cmd };
    if (!offscreen) {
        sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &F->sem_image; sub.pWaitDstStageMask = &wait;
        sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &F->sem_done;
    }
    /* the fence is reset only now, and healed if the submit fails, so no
     * later wait can hang on a fence nothing will signal */
    vkResetFences(g.dev, 1, &F->fence);
    double t_sub = glfwGetTime();
    VkResult sr = vkQueueSubmit(g.queue, 1, &sub, F->fence);
    if (sr != VK_SUCCESS) { die("vkQueueSubmit", (int)sr); heal_fence(F); return 0; }
    F->ts_written = g.have_ts;
    double submit_ms = (glfwGetTime() - t_sub) * 1000.0;
    if (g.shot_pending) { g.shot_pending = 0; write_shot(F, offscreen ? g.off : g.images[idx], offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR); }
    if (!offscreen) {
        /* on MoltenVK the display's pacing is paid inside the submit that carries a presentable image */
        wait_ms += submit_ms;
        double t_pres = glfwGetTime();
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &F->sem_done,
            .swapchainCount = 1, .pSwapchains = &g.swap, .pImageIndices = &idx };
        VkResult pr = vkQueuePresentKHR(g.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g.resized = 1;
        wait_ms += (glfwGetTime() - t_pres) * 1000.0;
    }
    g.stat[0] = (glfwGetTime() - t_start) * 1000.0 - wait_ms;
    g.stat[1] = wait_ms;
    g.stat[2] = (double)draws;
    g.frame_no++;
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

static void write_shot(Frame *F, VkImage image, VkImageLayout layout) {
    vkWaitForFences(g.dev, 1, &F->fence, VK_TRUE, UINT64_MAX);     /* the frame just submitted */
    VkDeviceSize size = (VkDeviceSize)g.extent.width * g.extent.height * 4;
    VkBuffer buf; VkDeviceMemory mem; void *map = NULL;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(g.dev, &cai, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { g.extent.width, g.extent.height, 1 } };
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier back = to_src;
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; back.dstAccessMask = 0;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout = layout;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    }
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
