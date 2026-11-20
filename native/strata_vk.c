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
#define STRATA_SHADERS_STAMP "585f8322f8fbc9d5"
#include "strata_shaders.h"
#include "strata_particles.h"
#include "strata_alloc.h"
#include "strata_lamps.h"
#include "strata_text.h"

#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

#define SV_LAYERS 3            /* opaque, cutout, translucent */
#define SV_DEFAULT_SLOTS 16480
#define SV_DEFAULT_QUADS (8u * 1024u * 1024u)   /* 64 MB of 8-byte quads */
#define SV_FRAMES 2            /* frames in flight */
#define SV_SPRAY_CAP 65536     /* particles the spray ring holds */
#define SV_SPRAY_FLOATS PT_FLOATS     /* floats a particle's record */
#define SV_UI_QUADS 16384      /* quads the 2D layer holds a frame */
#define SV_UI_FLOATS 16        /* floats a quad (Strata.Ui) */
#define SV_GLYPH_STAGE ST_UPLOAD_BYTES   /* bytes of new glyph pixels a frame carries to the atlas: all strata_text makes in a frame */
#define SV_GLYPH_RECTS ST_UPLOAD_RECTS
#define SV_LIGHT_PER_FRAME 128 /* chunks whose light bytes one frame may copy into the volume */
#define SV_LIGHT_WRITE_BYTES (32768 + 64 + 16)  /* per write: the voxels, the coarse cells, the owner texel */
/* The light volume keeps a chunk's voxels only when they are not all one
 * class: in a page of a pool, 16 x 16 pages a layer. A chunk all of one class
 * (air, stone, sea) is that class in its owner texel. Measured, 70 to 77% of
 * the chunks a stream holds are all one class; the pool holds half of them. */
#define SV_PAGES_X 16
#define SV_PAGES_Y 16
#define SV_MAX_KEYS 512
#define SV_MAX_TYPED 64
#define SV_MAX_PADS 16

#define SV_RANGES 6            /* 0 opaque, 1 cutout, 2 translucent, 3 cross, 4 loose, 5 spare */
#define SV_PAD_MAX 65536       /* extra zero-count commands sv_pad_commands may append */
#define SV_CMD_STRIDE (g.slot_cap + SV_PAD_MAX)   /* commands per region of the indirect buffer */
/* Regions of the command buffer: one per compute-culled range, then one per
 * frame in flight for the translucent list, which the CPU writes and so must
 * not share with a frame the GPU is still drawing. */
#define SV_REGION_TRANS 6
#define SV_REGIONS (SV_REGION_TRANS + SV_FRAMES)
#define SV_REGION_BATCH SV_REGIONS

typedef SaRange Range;

/* One figure part as the figure pass draws it (std430, 32 bytes): its
 * origin in metres, flags (bit 0: the figure moved since the last frame),
 * and its rotation. */
struct FigInst { float pos[3]; uint32_t flags; float rot[4]; };

typedef struct { float pos[4]; float rot[4]; int32_t dims[4]; float sphere[4]; } BodyRow;    /* std430, 64 bytes: low corner (metres), rotation, size and first word, bounding sphere (voxels) */
typedef struct { int shaped, placed, shared; float pos[3], rot[4], seen_pos[3], seen_rot[4]; int32_t dims[3]; uint32_t first, nwords; float vs; } BodyRec;   /* vs: metres a voxel, 0 the world's */

/* One entry of the chunk table, as the shaders read it (std430, 96 bytes).
 * The CPU writes entries straight into the mapped buffer. A frame in flight
 * may read an entry while it changes; either version is drawable, because a
 * replaced range stays intact for two frames. */
typedef struct {
    int32_t  origin[3];        /* chunk's low corner, in voxels */
    int32_t  level_flags;      /* bits 0-7 level, bit 8 in use, bit 9 a body, bit 10 hidden */
    uint32_t first[SV_RANGES];
    uint32_t count[SV_RANGES];
    float    pos[4];           /* a body slot: its low corner in metres */
    float    rot[4];           /* and its rotation x y z w; the identity for a chunk of the world */
} ChunkEntry;
#define SV_IN_USE 256
#define SV_BODY   512          /* a part of a body: drawn at its pose, never in the light volume */
#define SV_HAS_LIGHT 2048      /* slot owns a cell in the voxel light volume */
#define SV_HIDDEN 1024         /* held but not drawn (Strata.Slots' which-chunk-draws rule); kept across uploads, cleared by a free */
#define SV_SHARED 4096         /* mesh ranges are owned by a pinned prototype */
#define SV_TEMPLATE 8192       /* its geometry is immutable until close */
#define SV_BATCHED 16384       /* shared slot rendered in a mesh batch, not by the chunk cull */
#define SV_MOTION 32768        /* batched slot whose origin the vertex shader moves along a motion record (sv_chunk_motion) */
/* A motion record, as the batched quad shader reads it (16 words a slot): start and end time
 * (seconds since the motion epoch), start and end progress, key count plus 256 for a walker's
 * gait, then up to SV_MOTION_KEYS keys, each dx 10 bits, dz 10, dy 8 (signed, voxels from
 * the slot's origin) and the key's place 0..8 eighths of the way from start to end. */
#define SV_MOTION_WORDS 16
#define SV_MOTION_KEYS 9

typedef struct {
    float extent[3];            /* conservative upper corner of the prototype's mesh, in voxels */
    uint32_t count, next;       /* visible count and write cursor for the current frame */
} MeshBatch;

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
    VkBuffer motion; VkDeviceMemory motion_mem; uint32_t *motion_map;   /* the motion records, synced with the table */
    VkBuffer btable, bcells, blist, bmovers; VkDeviceMemory btable_mem, bcells_mem, blist_mem, bmovers_mem;
    BodyRow *btable_map; uint32_t *bcells_map, *blist_map; float *bmovers_map;
    /* lamps near each cell of the light volume, for the light pass to aim at: per cell (first, count), then the list */
    VkBuffer lcells, llist; VkDeviceMemory lcells_mem, llist_mem; uint32_t *lcells_map; int32_t *llist_map;
    uint32_t lamp_version;         /* the lamp lists this frame's copy holds */
    uint32_t *pending; uint32_t npending; unsigned char *is_pending;   /* slots changed since this Frame's table was last brought up to date */
    VkBuffer lstage; VkDeviceMemory lstage_mem; unsigned char *lstage_map;   /* this frame's light-volume writes, before they are copied in */
    VkBuffer stats; VkDeviceMemory stats_mem; uint32_t *stats_map;   /* written by the cull pass: commands per range [6], chunks, quads */
    VkBuffer ui; VkDeviceMemory ui_mem; float *ui_map;   /* this frame's 2D layer */
    VkBuffer gstage; VkDeviceMemory gstage_mem; unsigned char *gstage_map;   /* this frame's new glyph pixels, before they are copied in */
    VkBuffer finst; VkDeviceMemory finst_mem; struct FigInst *finst_map;   /* this frame's figure parts: where each is drawn */
    uint32_t batch_instances, batch_quads;
    int stats_valid;           /* the cull pass has run for this Frame at least once */
    VkFence fence;             /* signalled when the frame's commands have run */
    VkSemaphore sem_image, sem_done;
    int ts_written;            /* its two timestamp queries hold values */
    int fsm_ts;                /* and its third and fourth: round the figure shadow pass, which ran */
} Frame;

static SaAlloc g_quads;        /* ranges of the quad buffer, in quads */
static SaAlloc g_loose;        /* ranges of the loose-quad buffer, in loose quads */
static SaAlloc g_bbytes;       /* ranges of the body class buffer, in 32-bit words (four voxels each) */

/* Bodies, for the rays: each one a grid of light classes (one byte a voxel,
 * in g.bbytes_map) at a pose. Handles are the game's to choose, below
 * SV_MAX_BODIES. Each frame the CPU writes the placed bodies into a compact
 * table, and a grid of the light volume's chunk cells listing which bodies
 * each cell's chunk overlaps: a ray entering a chunk looks at its list. */
#define SV_MAX_BODIES   16384
#define SV_BODY_WORDS   (16u << 20)      /* 64 MB of classes */
#define SV_BODY_LIST    262144           /* list entries a frame */
#define SV_MOVERS       16               /* boxes a frame where moving bodies unsettle light history */
#define SV_MAX_CELLS    65536            /* light volume cells (chunks) the body lists can cover */
#define SV_CHUNK_LAMPS  64               /* glowing voxels a chunk keeps for the light pass to aim at */
#define SV_LAMP_LIST    65536            /* entries in all cells' lists of nearby lamps, together */
#define SV_LAMP_REACH   16               /* voxels a lamp's light reaches (the light pass fades it out from 12) */
#define SV_MOVING_LAMPS 1024
#define SV_LAMP_MOVING  (1 << 24)
#define SV_LAMP_BEAM    (1 << 25)
#define SV_CELL_LAMPS   16               /* lamps a cell lists at most */

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
    VkFramebuffer fb_g, fb_l[SV_FRAMES], fb_t[SV_FRAMES];
    /* What the light pass remembers between frames, one pair per frame in
     * flight: a frame reads the other's and writes its own. hist: rgb sky
     * light, a sun visibility. hmeta: r samples, g view depth, b face. Both
     * are written by the gather pass, one pixel for each trace_scale x
     * trace_scale block of the screen (STRATA_TRACE_FULL=1: one for each). */
    Target hist[SV_FRAMES], hmeta[SV_FRAMES];
    int trace_scale; VkExtent2D trace_extent;
    /* What the translucent pass remembers between frames, one pair per frame
     * in flight, read and written by the water shader as storage images:
     * whist rgb the mirror colour, a the sun's visibility; wdist the view
     * depth of the surface it belongs to (0: nothing). A water pixel traces
     * one frame in four and reuses the rest, as the light pass does. */
    Target whist[SV_FRAMES], wdist[SV_FRAMES];
    int water_mem;                 /* the device lets a fragment shader write images: water keeps that memory */
    int water_fresh;               /* the water memory images are new and must be cleared before use */
    int hist_reset;                /* frames left in which that memory must not be read (fresh images) */
    VkSampler nearest;             /* for reading the frame's own images, texel for texel */
    int sky_rays, reflections;     /* light quality: sky rays a pixel a sample, 0..8; mirror rays on or off */
    int light_mode;                /* 0: the pre-ray-tracing shading, through the deferred passes */
    /* The light volume: a light class per voxel for every chunk held, the
     * coarse copy (how high each 8-voxel cell's contents reach), and which chunk each cell holds. */
    Target vol, coarse, owner;     /* vol: the pool of pages */
    int32_t *cell_page;            /* per cell: the page its chunk's voxels are in, or -1 */
    int32_t *free_pages; int nfree_pages, npages;   /* pages not in use, as a stack; how many the pool has */
    uint64_t pages_refused;        /* chunks whose voxels found no page: their cells hold nothing */
    int vn, vnv;                   /* its size in chunks: vn across (x and z), vnv up */
    VkBuffer ltable; VkDeviceMemory ltable_mem; int32_t *ltable_map;
    float *tex_mean; uint32_t tex_mean_n;   /* each texture layer's mean colour, linear, three floats a layer */
    struct LightCell { int32_t chunk[3]; int valid; int uniform; uint32_t hash, solid; int nlamps; int32_t *lamps; } *cells;   /* lamps: nlamps glowing voxels, 4 ints each (x, y, z voxels, class) */
    uint32_t lamp_version;        /* bumped whenever the lamp lists change */
    int32_t moving_lamps[SV_MOVING_LAMPS][4];
    int moving_lamp_count, headlights;
    int vn_old_cells;             /* how many cells g.cells has */   /* per cell, as last queued: its chunk, and its class if it is one class throughout, else -1 */
    struct LightWrite { int cell[3]; int32_t chunk[3]; int valid; int uniform; int edit; unsigned char *bytes; } *lq;   /* writes waiting to be copied in, oldest first */
    uint32_t nlq, lq_cap;
    int32_t edit_lo[3], edit_hi[3];   /* the chunks an edit changed, while light_edit counts down */
    int32_t last_lo[3], last_hi[3]; int last_edit;   /* the chunks the last frame's edits changed, if any */
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
    VkRenderPass pass_g, pass_t, pass_l, pass, pass_off;
    VkPipelineLayout layout;
    VkPipeline pipe_opaque, pipe_batch, pipe_blend, pipe_cross, pipe_loose, pipe_cull, pipe_gather, pipe_light, pipe_composite, pipe_spray, pipe_glow, pipe_collide;
    /* Particles: a ring of records in a mapped buffer (strata_particles.h),
     * drawn twice, shaded (pipe_spray) then glowing (pipe_glow); and the
     * effect table they read, PT_EFFECTS effects of PT_EFFECT_FLOATS, effect
     * 0 spray's. [fx_set] marks the effects defined. */
    VkBuffer spray; VkDeviceMemory spray_mem; float *spray_map;
    VkBuffer fx; VkDeviceMemory fx_mem; float *fx_map; uint8_t fx_set[PT_EFFECTS]; double glow_until;   /* when the last glowing particle born so far dies */
    SprayRing ring; float *spray_births;
    /* Particles of an effect that meets the world (its table float 41 not 0) are born with record float 13 at -1;
     * the next frame runs collide.comp over the ring from the first such birth (a count of pushes, as ring.head) to the newest.
     * [meet_last]: how many particles that frame's pass was given; [meet_total]: colliding particles born since the start. */
    int meet_pending; uint64_t meet_first; uint32_t meet_last; uint64_t meet_total;
    /* The 2D layer: the quads the game last handed over, drawn last. Each
     * Frame copies them when it is recorded. */
    VkPipeline pipe_ui;
    VkPipeline pipe_fig;           /* figure parts: shared piece meshes, instanced at their poses */
    float *ui_cpu; uint32_t ui_n; uint64_t ui_dropped;
    Target glyphs; VkSampler glyph_sampler; uint32_t glyph_px, glyph_pages;   /* the glyph atlas: strata_text's pages, one layer each */
    Target ui_art; VkSampler ui_sampler;   /* the interface atlas: one image of a game's own interface art */
    VkBuffer loose; VkDeviceMemory loose_mem; uint32_t *loose_map;       /* model geometry, expanded per instance: 16 words a quad */
    float *models[SV_MAX_MODELS]; uint32_t model_quads[SV_MAX_MODELS];   /* 21 floats a quad, as Strata.Model lays them out */
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    ChunkEntry *entries;           /* the chunk table as the CPU keeps it, slot_cap entries; each Frame has the GPU's copy */
    BodyRec *bodies;               /* SV_MAX_BODIES */
    VkBuffer bbytes; VkDeviceMemory bbytes_mem; uint32_t *bbytes_map;
    VkBuffer cmds;  VkDeviceMemory cmds_mem;  uint32_t *cmds_map;        /* indirect commands, SV_REGIONS * SV_CMD_STRIDE, 4 words each */
    VkBuffer dslot; VkDeviceMemory dslot_mem; uint32_t *dslot_map;       /* the slot each command draws, same shape */
    /* slots that hold translucent quads: the list the CPU orders each frame */
    uint32_t *trans; uint32_t ntrans; int32_t *trans_at;                 /* trans_at[slot] = index in trans, or -1 */
    struct TransKey { float dist; uint32_t slot; } *trans_keys;
    uint16_t *trans_ord;                                                 /* trans_ord[slot * 8 + o]: quads in the slot's ordered list for octant o; all 0: one unordered list */
    uint32_t submit[SV_RANGES];    /* commands the next indirect call of each range will submit */
    VkBuffer blocks; VkDeviceMemory blocks_mem; int32_t *blocks_map;     /* the block table: 8 words a block id */
    uint32_t nblocks;
    /* the texture array: one layer per texture id */
    VkImage tex; VkDeviceMemory tex_mem; VkImageView tex_view; VkSampler sampler;
    uint32_t tile_px, layers; unsigned char *tex_staging; int aniso;
    int batch_shared;
    uint32_t *batch_slots, *batch_at;        /* dense list of active shared slots, and positions plus one */
    uint32_t *motion;                        /* each slot's motion record (SV_MOTION_WORDS words) */
    int16_t *motion_box;                     /* each moving slot's low and high corner, voxels from its origin */
    double motion_epoch; int motion_epoch_set;
    uint32_t *batch_visible, *batch_keys;    /* this frame's cull results and touched prototypes */
    MeshBatch *batches;                      /* indexed by prototype slot */
    uint32_t num_batch_slots, num_batch_keys, batch_draws, batch_visible_count, opaque_commands;
    /* churn between two frames, for devtools' detectors: chunk_instance calls, chunk_motion calls,
     * chunk_commit uploads, chunk-table entries synced; counting now, and the last frame's */
    uint32_t churn[4], churn_last[4];
    uint32_t slot_cap;
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
    double pass_ms[4];             /* last frame's GPU ms by pass: before the geometry, geometry, light, after the light */
    double stat[6];                /* last frame: CPU ms, ms waiting, draw calls, GPU ms, translucent chunks drawn, microseconds ordering them */
    /* a chunk being assembled by sv_chunk_begin .. sv_chunk_commit */
    struct { int64_t slot; int32_t origin[3]; uint32_t *part[4]; uint32_t n[4], cap[4], ord[8]; int open; int body; int fig; int32_t rp; int level; } stage;
    VkQueryPool qpool; int have_ts; float ts_period;   /* four timestamps a Frame: the frame's start and end, the figure shadow pass's */
    VkDeviceSize max_ssbo;         /* the most of a buffer one storage descriptor may reach */
    int64_t refused;               /* uploads turned away because the buffer was full */
    /* stats of the last frame */
    int64_t drawn_chunks, drawn_quads;
    /* input */
    unsigned char keys[SV_MAX_KEYS];
    double mx, my, mdx, mdy, scroll_y; int have_mouse;
    /* this frame's typing, in order: a code point, or minus the code of a key from 256 up (press or repeat) */
    int32_t typed[SV_MAX_TYPED]; int ntyped;
    /* gamepads as of the last poll: GLFW's 15 buttons; axes 0..3 the sticks, 4 and 5 the triggers as 0..1 */
    struct { unsigned char on; unsigned char button[15]; float axis[6]; } pad[SV_MAX_PADS];
    int shot_pending; char shot_path[1024];
} g;

/* The uniform block: view-projection, sun, sky, fog, voxel scale, then 256
 * palette colours indexed by a quad's texture id. Must match the shaders. */
/* The uniform block, in floats (native/shaders/ubo.glsl): vp 0, sun 16,
 * sky 20, fog 24, eye 28, misc 32 (ints), inv_vp 36, prev_vp 52, light 68,
 * sun colour 72, light volume size 76 (ints). */
#define UBO_FLOATS 124
#define UBO_TRACE 120         /* vec4: x screen pixels along each side of one the gather pass traces for */
#define UBO_SUN_VP 100         /* mat4: world to the figure shadow map's clip space */
#define UBO_FSHADOW 116        /* vec4: x the map holds figures this frame, y a texel in its 0..1 coordinates, z its depth per metre */
#define UBO_BODIES 88          /* ints: x moving bodies' boxes in the movers buffer, y bodies in the body table */
#define UBO_WATER 92           /* two vec4: the liquid the eye is in (rgb, and how fast it swallows light; 0: none), and its surface height */
#define UBO_EDIT 80            /* two vec4: the low and high corners (voxels) of what an edit just changed; the first w is 1 while it settles */
#define UBO_VOL 76             /* ints: the light volume's size in chunks */
#define UBO_INV_VP 36
#define UBO_PREV_VP 52
#define UBO_LIGHT 68
#define UBO_SUN_COLOR 72
#define SV_MAX_BLOCKS 65536    /* rows in the block table: block ids are sixteen bits */
#define SV_TOP_OF (SV_MAX_BLOCKS * 8)   /* after the rows, in words: each texture's top texture, for a far chunk's side faces */

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
    for (int f = 0; f < SV_FRAMES; f++) { if (g.fb_l[f]) vkDestroyFramebuffer(g.dev, g.fb_l[f], NULL); g.fb_l[f] = VK_NULL_HANDLE;
        if (g.fb_t[f]) vkDestroyFramebuffer(g.dev, g.fb_t[f], NULL); g.fb_t[f] = VK_NULL_HANDLE; drop_target(&g.hist[f]); drop_target(&g.hmeta[f]); drop_target(&g.whist[f]); drop_target(&g.wdist[f]); }
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
static int make_image_sized(VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { ext.width, ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
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

/* A frame-sized image. */
static int make_image(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    return make_image_sized(g.extent, fmt, usage, aspect, img, mem, view);
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
    /* the gather pass traces one pixel of each trace_scale x trace_scale block (sv_set_trace_scale, default 2);
     * STRATA_TRACE_FULL=1 every pixel whatever the game asked, as before it was split from the light pass */
    { const char *tf = getenv("STRATA_TRACE_FULL"); if (tf && tf[0] == '1') g.trace_scale = 1; else if (!g.trace_scale) g.trace_scale = 2; }
    g.trace_extent = (VkExtent2D){ (ext.width + g.trace_scale - 1) / g.trace_scale, (ext.height + g.trace_scale - 1) / g.trace_scale };
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!make_image_sized(g.trace_extent, FMT_LIT, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.hist[f].img, &g.hist[f].mem, &g.hist[f].view)) return 0;
        if (!make_image_sized(g.trace_extent, FMT_LIT, tu, VK_IMAGE_ASPECT_COLOR_BIT, &g.hmeta[f].img, &g.hmeta[f].mem, &g.hmeta[f].view)) return 0;
        VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!make_image(FMT_LIT, su, VK_IMAGE_ASPECT_COLOR_BIT, &g.whist[f].img, &g.whist[f].mem, &g.whist[f].view)) return 0;
        if (!make_image(VK_FORMAT_R32_SFLOAT, su, VK_IMAGE_ASPECT_COLOR_BIT, &g.wdist[f].img, &g.wdist[f].mem, &g.wdist[f].view)) return 0;
    }
    g.water_fresh = 1;
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
        /* what this frame's gather pass writes, for its light pass to spread over the screen */
        VkDescriptorImageInfo ci[2] = {
            { g.nearest, g.hist[f].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g.nearest, g.hmeta[f].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet cw[2];
        for (uint32_t b = 0; b < 2; b++)
            cw[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 35 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci[b] };
        vkUpdateDescriptorSets(g.dev, 2, cw, 0, NULL);
        /* the water's memory: the other frame's to read, this frame's to write */
        VkDescriptorImageInfo wi[4] = {
            { VK_NULL_HANDLE, g.whist[o].view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, g.wdist[o].view, VK_IMAGE_LAYOUT_GENERAL },
            { VK_NULL_HANDLE, g.whist[f].view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, g.wdist[f].view, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet ww[4];
        for (uint32_t b = 0; b < 4; b++)
            ww[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = 28 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &wi[b] };
        vkUpdateDescriptorSets(g.dev, 4, ww, 0, NULL);
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
        VkImageView ta[2] = { g.hist[f].view, g.hmeta[f].view };
        VkFramebufferCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_t, .attachmentCount = 2,
            .pAttachments = ta, .width = g.trace_extent.width, .height = g.trace_extent.height, .layers = 1 };
        VKOK(vkCreateFramebuffer(g.dev, &ti, NULL, &g.fb_t[f]), "gather framebuffer");
        VkFramebufferCreateInfo li = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_l, .attachmentCount = 1,
            .pAttachments = &g.lit.view, .width = g.extent.width, .height = g.extent.height, .layers = 1 };
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

/* The gather pass (n 2: what it remembers for the next frame) and the light
 * pass (n 1: the lit image), all left ready to be sampled. */
static int make_pass_l(uint32_t n, VkRenderPass *out) {
    VkAttachmentDescription att[3];
    VkAttachmentReference cref[3];
    for (uint32_t i = 0; i < n; i++) {
        att[i] = color_att(FMT_LIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    }
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = n, .pColorAttachments = cref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT } };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, out), "light pass");
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
 * 3: loose quads; 4: the light pass; 5: the lit image to the screen;
 * 6: spray (final pass, blended, after the translucent quads);
 * 7: the 2D layer (final pass, blended, no depth, last of all);
 * 8: figure pieces (geometry pass; quads read from the quad buffer as words);
 * 9: glowing particles (final pass, added, after the shaded ones of kind 6).
 * 10: the gather pass (the light pass's rays, at the trace scale);
 * 11: shared mesh batches (geometry pass, voxel textures).
 * Kinds 0, 2, 3, 8 and 11 belong to the geometry pass, 4 to the light pass, 10 to
 * the trace pass, 1 and 5 to the final pass. */
static int make_pipeline(int kind, VkPipeline *out) {
    int glow = kind == 9, batch = kind == 11;
    int spray = kind == 6 || glow, ui = kind == 7, fig = kind == 8;
    int blend = kind == 1 || spray || ui, cross = kind == 2, loose = kind == 3, full = kind == 4 || kind == 5 || kind == 10, geom = kind == 0 || cross || loose || fig || batch;
    VkShaderModule vs = batch ? shader(strata_quad_batch_vert, sizeof(strata_quad_batch_vert))
                      : fig ? shader(strata_figure_vert, sizeof(strata_figure_vert))
                      : ui ? shader(strata_ui_vert, sizeof(strata_ui_vert))
                      : spray ? shader(strata_particle_vert, sizeof(strata_particle_vert))
                      : cross ? shader(strata_cross_vert, sizeof(strata_cross_vert))
                      : loose ? shader(strata_loose_vert, sizeof(strata_loose_vert))
                      : full  ? shader(strata_fullscreen_vert, sizeof(strata_fullscreen_vert)) : shader(strata_quad_vert, sizeof(strata_quad_vert));
    VkShaderModule fs = ui ? shader(strata_ui_frag, sizeof(strata_ui_frag))
                      : spray ? shader(strata_particle_frag, sizeof(strata_particle_frag))
                      : kind == 4 ? shader(strata_light_frag, sizeof(strata_light_frag))
                      : kind == 10 ? shader(strata_gather_frag, sizeof(strata_gather_frag))
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
    /* a particle's record is sixteen floats */
    VkVertexInputBindingDescription sbind = { .binding = 0, .stride = SV_SPRAY_FLOATS * 4, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription sattrs[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 32 },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 48 } };
    /* a 2D quad is sixteen floats */
    VkVertexInputBindingDescription ubind = { .binding = 0, .stride = SV_UI_FLOATS * 4, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription uattrs[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 32 },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 48 } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = (full || fig || batch) ? 0 : 1, .pVertexBindingDescriptions = ui ? &ubind : spray ? &sbind : (loose ? &lbind : &bind),
        .vertexAttributeDescriptionCount = (full || fig || batch) ? 0 : (ui ? 4 : spray ? 4 : (loose ? 4 : 2)), .pVertexAttributeDescriptions = ui ? uattrs : spray ? sattrs : (loose ? lattrs : attrs) };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (cross || full || spray) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        /* a see-through surface is visible from behind too: water from below */
        .cullMode = (kind == 0 || fig || spray || batch) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* only the geometry pass writes depth; translucent quads test against it; full-screen passes ignore it */
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = (full || ui) ? VK_FALSE : VK_TRUE,
        .depthWriteEnable = geom ? VK_TRUE : VK_FALSE, .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendAttachmentState cba[3];
    cba[0] = (VkPipelineColorBlendAttachmentState){ .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = glow ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = glow ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = glow ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = glow ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF };
    cba[1] = cba[0]; cba[2] = cba[0];
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = (geom || kind == 10) ? 2 : 1, .pAttachments = cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = g.layout,
        .renderPass = geom ? g.pass_g : (kind == 4 ? g.pass_l : kind == 10 ? g.pass_t : g.pass) };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, NULL, out);
    vkDestroyShaderModule(g.dev, vs, NULL); vkDestroyShaderModule(g.dev, fs, NULL);
    if (r != VK_SUCCESS) { die("vkCreateGraphicsPipelines", (int)r); return 0; }
    return 1;
}

/* ── The 2D layer ─────────────────────────────────────────────────────────── */

/* A sampled 2D image (or array of [layers]) filled from [pixels], which
 * holds every layer one after another. Waits for the GPU: rare. */
static int make_sampled(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t bpp, const unsigned char *pixels, int array, Target *out) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VKOK(vkCreateImage(g.dev, &ici, NULL, &out->img), "2D image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, out->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, &out->mem), "2D image memory");
    VKOK(vkBindImageMemory(g.dev, out->img, out->mem, 0), "2D image bind");
    VkDeviceSize bytes = (VkDeviceSize)w * h * bpp * layers;
    VkBuffer sb; VkDeviceMemory sm; void *map = NULL;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map)) return 0;
    if (pixels) memcpy(map, pixels, (size_t)bytes); else memset(map, 0, (size_t)bytes);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(g.dev, &cai, &cb), "2D image command buffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = out->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers }, .imageExtent = { w, h, 1 } };
    vkCmdCopyBufferToImage(cb, sb, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(g.queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.dev, g.cpool, 1, &cb);
    vkDestroyBuffer(g.dev, sb, NULL); vkFreeMemory(g.dev, sm, NULL);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->img, .viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VKOK(vkCreateImageView(g.dev, &vi, NULL, &out->view), "2D image view");
    return 1;
}

static void bind_image(uint32_t binding, VkSampler s, VkImageView v) {
    for (int f = 0; f < SV_FRAMES; f++) {
        if (!g.frames[f].dset) continue;
        VkDescriptorImageInfo ii = { s, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.frames[f].dset, .dstBinding = binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
        vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    }
}

static int ui_art_make(uint32_t w, uint32_t h, const unsigned char *rgba) {
    Target fresh = { 0 };
    if (!make_sampled(VK_FORMAT_R8G8B8A8_SRGB, w, h, 1, 4, rgba, 0, &fresh)) { drop_target(&fresh); return 0; }
    if (!g.ui_sampler) {
        /* nearest when magnified, like the block textures: interface art keeps its pixels */
        VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxAnisotropy = 1.0f };
        VKOK(vkCreateSampler(g.dev, &si, NULL, &g.ui_sampler), "interface sampler");
    }
    drop_target(&g.ui_art);
    g.ui_art = fresh;
    bind_image(22, g.ui_sampler, g.ui_art.view);
    return 1;
}

/* The glyph atlas as the GPU holds it: every page strata_text may make, as
 * layers of one image, empty to begin with. */
static int glyphs_make(void) {
    g.glyph_px = (uint32_t)st_atlas_page_px(); g.glyph_pages = (uint32_t)st_atlas_max_pages();
    if (!g.glyph_px || !g.glyph_pages) { die("text could not start", 0); return 0; }
    if (!make_sampled(VK_FORMAT_R8_UNORM, g.glyph_px, g.glyph_px, g.glyph_pages, 1, NULL, 1, &g.glyphs)) return 0;
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxAnisotropy = 1.0f };
    VKOK(vkCreateSampler(g.dev, &si, NULL, &g.glyph_sampler), "glyph sampler");
    bind_image(23, g.glyph_sampler, g.glyphs.view);
    return 1;
}

/* Copy the glyph cells strata_text has drawn since last time into the atlas,
 * through this Frame's staging buffer. What does not fit waits a frame. */
static void glyphs_flush(Frame *F) {
    static VkBufferImageCopy regs[SV_GLYPH_RECTS];
    uint32_t n = 0; size_t used = 0;
    int w, h;
    while (n < SV_GLYPH_RECTS && st_atlas_dirty_peek(&w, &h) && used + (size_t)w * (size_t)h <= SV_GLYPH_STAGE) {
        int page, x, y, stride; const uint8_t *px;
        if (!st_atlas_dirty(&page, &x, &y, &w, &h, &px, &stride)) break;
        if (page < 0 || (uint32_t)page >= g.glyph_pages || x < 0 || y < 0 || (uint32_t)(x + w) > g.glyph_px || (uint32_t)(y + h) > g.glyph_px) continue;
        for (int row = 0; row < h; row++) memcpy(F->gstage_map + used + (size_t)row * (size_t)w, px + (size_t)(y + row) * (size_t)stride + x, (size_t)w);
        regs[n++] = (VkBufferImageCopy){ .bufferOffset = used, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)page, 1 },
            .imageOffset = { x, y, 0 }, .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
        used += (size_t)w * (size_t)h;
    }
    if (n == 0) return;
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g.glyphs.img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, g.glyph_pages } };
    vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyBufferToImage(F->cmd, F->gstage, g.glyphs.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regs);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* The interface atlas: one image, [w] x [h] RGBA bytes (sRGB colour,
 * straight alpha, rows running downward), holding all of a game's own
 * interface art. Replaces the last one. Rare and slow: it waits for the GPU. */
int64_t sv_ui_atlas(int64_t w, int64_t h, void *rgba_u8) {
    if (!g.ok || !rgba_u8 || w < 1 || h < 1 || w > 8192 || h > 8192 || narr_len(rgba_u8) < w * h * 4) return 0;
    vkDeviceWaitIdle(g.dev);
    return ui_art_make((uint32_t)w, (uint32_t)h, narr_data(rgba_u8));
}

/* The 2D layer for the frames from now on: [nquads] quads of 16 floats
 * (Strata.Ui), drawn in order over the finished frame. They stay until the
 * next call; none clears the layer. Returns how many were taken: quads past
 * the layer's capacity are dropped from the end; sv_stat(9) is how many the
 * last call dropped. */
int64_t sv_ui_set(void *floats, int64_t nquads) {
    g.ui_dropped = 0;
    if (!g.ok || !g.ui_cpu || nquads <= 0 || !floats) { g.ui_n = 0; return 0; }
    int64_t have = narr_len(floats) / SV_UI_FLOATS;
    int64_t n = nquads < have ? nquads : have;
    if (n > SV_UI_QUADS) { g.ui_dropped = (uint64_t)(n - SV_UI_QUADS); n = SV_UI_QUADS; }
    memcpy(g.ui_cpu, narr_data(floats), (size_t)n * SV_UI_FLOATS * sizeof(float));
    g.ui_n = (uint32_t)n;
    return n;
}

/* The window's size in points: what an interface is laid out on. */
int64_t sv_win_w(void) { int w = 0, h = 0; if (g.win) glfwGetWindowSize(g.win, &w, &h); return w; }
int64_t sv_win_h(void) { int w = 0, h = 0; if (g.win) glfwGetWindowSize(g.win, &w, &h); return h; }

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
    /* each texture's top texture: a block's side texture that differs from its top (a grass side) maps to the
     * top, so a chunk above level 0, whose risers are many voxels tall, keeps the ground's colour; the first
     * block to name a side texture decides it; every other texture maps to itself */
    uint32_t *top = (uint32_t *)g.blocks_map + SV_TOP_OF;
    for (uint32_t t = 0; t < 65536; t++) top[t] = t;
    const int32_t *rows = narr_data(words_i32);
    static uint8_t named[65536];
    memset(named, 0, sizeof named);
    for (int64_t b = 0; b < nblocks; b++) {
        int32_t t0 = rows[b * 8];
        for (int f = 2; f < 6; f++) {
            int32_t tf = rows[b * 8 + f];
            if (tf < 0 || tf > 65535 || t0 < 0 || t0 > 65535 || named[tf]) continue;
            named[tf] = 1;
            if (tf != t0) top[tf] = (uint32_t)t0;
        }
    }
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
    uint32_t ncells = n * nv * n;
    uint32_t layers = (ncells / 2 + SV_PAGES_X * SV_PAGES_Y - 1) / (SV_PAGES_X * SV_PAGES_Y);
    if (layers < 1) layers = 1;
    g.npages = (int)(layers * SV_PAGES_X * SV_PAGES_Y);
    if (!make_volume(&g.vol, VK_FORMAT_R8_UINT, SV_PAGES_X * 32, SV_PAGES_Y * 32, layers * 32)) return 0;
    if (!make_volume(&g.coarse, VK_FORMAT_R8_UINT, n * 4, nv * 4, n * 4)) return 0;
    if (!make_volume(&g.owner, VK_FORMAT_R32G32B32A32_SINT, n, nv, n)) return 0;
    if (g.cells) for (int i = 0; i < g.vn_old_cells; i++) free(g.cells[i].lamps);
    free(g.cells); free(g.cell_page); free(g.free_pages);
    g.cells = calloc(ncells, sizeof *g.cells);
    g.vn_old_cells = (int)ncells; g.lamp_version++;
    g.cell_page = malloc(ncells * sizeof *g.cell_page);
    g.free_pages = malloc((size_t)g.npages * sizeof *g.free_pages);
    if (!g.cells || !g.cell_page || !g.free_pages) { g.vn = 0; g.vnv = 0; return 0; }
    for (uint32_t i = 0; i < ncells; i++) g.cell_page[i] = -1;
    for (int i = 0; i < g.npages; i++) g.free_pages[i] = g.npages - 1 - i;
    g.nfree_pages = g.npages; g.pages_refused = 0;
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
    /* Two fingerprints of the voxels: all of them, and only those that block
     * or emit light. A see-through block (water) does neither, so water
     * moving changes the first and not the second, and the light pass keeps
     * what it remembers. */
    uint32_t hash = 2166136261u, solid = 2166136261u;
    if (bytes) for (int i = 0; i < 32768; i++) {
        unsigned char c = bytes[i];
        hash = (hash ^ c) * 16777619u;
        const int32_t *row = g.ltable_map ? g.ltable_map + c * 8 : NULL;
        int matters = row && (row[0] == 1 || row[0] == 2 || row[0] == 4 || row[3] != 0);
        solid = (solid ^ (matters ? c : 0)) * 16777619u;
    } else if (uniform > 0 && g.ltable_map) {
        const int32_t *row = g.ltable_map + uniform * 8;
        solid = (row[0] == 1 || row[0] == 2 || row[0] == 4 || row[3] != 0) ? (uint32_t)uniform * 16777619u : 2166136261u;
    }
    if (valid && same_owner && bytes && c->uniform < 0 && c->hash == hash) { free(bytes); return; }
    /* the chunk's glowing voxels, at most SV_CHUNK_LAMPS of them, spread over all it has */
    {
        int32_t found[SV_CHUNK_LAMPS * 4]; int nf = 0, seen = 0;
        if (valid && g.ltable_map) {
            if (bytes) {
                for (int i = 0; i < 32768; i++) {
                    unsigned char k = bytes[i];
                    if (!k || g.ltable_map[k * 8 + 3] == 0) continue;
                    /* past the cap, a later lamp replaces one at a stride, so the kept ones spread across the chunk */
                    int slot = nf < SV_CHUNK_LAMPS ? nf++ : (seen % SV_CHUNK_LAMPS);
                    seen++;
                    found[slot * 4] = chunk[0] * 32 + (i & 31); found[slot * 4 + 1] = chunk[1] * 32 + (i >> 10); found[slot * 4 + 2] = chunk[2] * 32 + ((i >> 5) & 31); found[slot * 4 + 3] = k;
                }
            } else if (uniform > 0 && g.ltable_map[uniform * 8 + 3] != 0) {
                for (int i = 0; i < SV_CHUNK_LAMPS; i++) { int x = (i & 3) * 8 + 4, y = ((i >> 2) & 3) * 8 + 4, z = (i >> 4) * 8 + 4;
                    found[i * 4] = chunk[0] * 32 + x; found[i * 4 + 1] = chunk[1] * 32 + y; found[i * 4 + 2] = chunk[2] * 32 + z; found[i * 4 + 3] = uniform; }
                nf = SV_CHUNK_LAMPS;
            }
        }
        if (nf != c->nlamps || (nf && memcmp(found, c->lamps, (size_t)nf * 16) != 0)) {
            free(c->lamps); c->lamps = NULL; c->nlamps = 0;
            if (nf) { c->lamps = malloc((size_t)nf * 16); if (c->lamps) { memcpy(c->lamps, found, (size_t)nf * 16); c->nlamps = nf; } }
            g.lamp_version++;
        }
    }
    /* new voxels for a chunk the volume already held: an edit, whose lighting must be re-estimated */
    int edit = valid && same_owner && c->solid != solid;
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
    c->valid = valid; c->uniform = w->uniform; c->hash = hash; c->solid = solid;
}

/* Is light class [c] see-through (kind 3: water, glass)? Rays that pass
 * through those need not look at them. */
static int see_through(unsigned char c) { return g.ltable_map && g.ltable_map[c * 8] == 3; }

/* Record, in this frame's commands, the copies that bring the volume up to
 * date: as many queued writes as one frame may carry, oldest first. */
static void light_flush(Frame *F) {
    if (g.nlq == 0) return;
    uint32_t n = g.nlq < SV_LIGHT_PER_FRAME ? g.nlq : SV_LIGHT_PER_FRAME;
    static VkBufferImageCopy rv[SV_LIGHT_PER_FRAME], rc[SV_LIGHT_PER_FRAME], ro[SV_LIGHT_PER_FRAME];
    uint32_t nv_copies = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct LightWrite *w = &g.lq[i];
        unsigned char *dst = F->lstage_map + (size_t)i * SV_LIGHT_WRITE_BYTES;
        unsigned char *coarse = dst + 32768;
        int32_t *own = (int32_t *)(dst + 32768 + 64);
        int ci = w->cell[0] + g.vn * (w->cell[1] + g.vnv * w->cell[2]);
        /* a chunk with voxels of more than one class keeps its page, or takes one; any other lets its page go */
        int page = -1;
        if (w->bytes && w->valid) {
            page = g.cell_page[ci];
            if (page < 0 && g.nfree_pages > 0) page = g.free_pages[--g.nfree_pages];
            if (page < 0) g.pages_refused++;
        } else if (g.cell_page[ci] >= 0) g.free_pages[g.nfree_pages++] = g.cell_page[ci];
        g.cell_page[ci] = page;
        int any = 0;                 /* how high the chunk's contents reach, 0..32 */
        int hard = 0;                /* ... leaving out see-through classes (kind 3: water, glass), 0..32 */
        if (w->bytes) {
            /* the chunk's flat index is x + 32 * (z + 32 * y); the volume wants x fastest, then y, then z */
            for (int z = 0; z < 32; z++) for (int y = 0; y < 32; y++)
                memcpy(dst + 32 * (y + 32 * z), w->bytes + 32 * (z + 32 * y), 32);
            memset(coarse, 0, 64);
            for (int z = 0; z < 32; z++) for (int y = 0; y < 32; y++) {
                const unsigned char *row = dst + 32 * (y + 32 * z);
                /* A coarse cell holds two heights, 1..8 (0: nothing): in its low four
                 * bits how high its contents reach, in its high four how high its
                 * contents other than see-through classes reach. A ray above the
                 * height that matters to it and rising can cross the cell whole; a
                 * ray that passes through water and glass (sun and sky rays)
                 * crosses a lake eight voxels at a time. */
                for (int x = 0; x < 32; x++) if (row[x]) {
                    unsigned char *cc = &coarse[(x >> 3) + 4 * ((y >> 3) + 4 * (z >> 3))];
                    unsigned char h = (unsigned char)((y & 7) + 1);
                    if ((*cc & 15) < h) *cc = (unsigned char)((*cc & 0xF0) | h);
                    if (any < y + 1) any = y + 1;
                    if (!see_through(row[x])) {
                        if ((*cc >> 4) < h) *cc = (unsigned char)((*cc & 15) | (h << 4));
                        if (hard < y + 1) hard = y + 1;
                    }
                }
            }
        } else {
            int clear = w->uniform == 0, glassy = !clear && see_through((unsigned char)w->uniform);
            memset(coarse, clear ? 0 : (glassy ? 8 : 8 | (8 << 4)), 64);
            any = clear ? 0 : 32;
            hard = clear || glassy ? 0 : 32;
        }
        /* w: 0, no chunk here; else 1 + how high its contents reach, plus 64
         * times how high its contents other than see-through ones reach, plus
         * from bit 12 where its voxels are: 1 + 2 * its page, or 2 * its one class */
        int where = w->bytes ? (page >= 0 ? 1 + 2 * page : -1) : 2 * (w->uniform > 0 ? w->uniform : 0);
        own[0] = w->chunk[0]; own[1] = w->chunk[1]; own[2] = w->chunk[2];
        own[3] = (w->valid && where >= 0) ? (1 + any + 64 * hard) | (where << 12) : 0;
        VkDeviceSize base = (VkDeviceSize)i * SV_LIGHT_WRITE_BYTES;
        /* only a chunk with a page has voxels to copy */
        if (page >= 0) rv[nv_copies++] = (VkBufferImageCopy){ .bufferOffset = base, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (page % SV_PAGES_X) * 32, ((page / SV_PAGES_X) % SV_PAGES_Y) * 32, (page / (SV_PAGES_X * SV_PAGES_Y)) * 32 }, .imageExtent = { 32, 32, 32 } };
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
    uint32_t counts[3] = { nv_copies, n, n };
    for (int k = 0; k < 3; k++) {
        if (counts[k] == 0) continue;
        volume_layout(F->cmd, imgs[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(F->cmd, F->lstage, imgs[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, counts[k], regs[k]);
        volume_layout(F->cmd, imgs[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

/* ── Input callbacks ──────────────────────────────────────────────────────── */
static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (key >= 0 && key < SV_MAX_KEYS) g.keys[key] = (action != GLFW_RELEASE);
    if (key >= 256 && action != GLFW_RELEASE && g.ntyped < SV_MAX_TYPED) g.typed[g.ntyped++] = -key;
}
static void on_char(GLFWwindow *w, unsigned int cp) {
    (void)w;
    if (cp > 0 && cp <= 0x10FFFF && g.ntyped < SV_MAX_TYPED) g.typed[g.ntyped++] = (int32_t)cp;
}
/* Read every connected gamepad into g.pad. A joystick GLFW has no mapping for is not a gamepad and reads as absent. */
static void pads_read(void) {
    for (int p = 0; p < SV_MAX_PADS; p++) {
        GLFWgamepadstate st;
        if (!glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + p) || !glfwGetGamepadState(GLFW_JOYSTICK_1 + p, &st)) { memset(&g.pad[p], 0, sizeof g.pad[p]); continue; }
        g.pad[p].on = 1;
        for (int b = 0; b < 15; b++) g.pad[p].button[b] = (st.buttons[b] == GLFW_PRESS);
        for (int a = 0; a < 4; a++) g.pad[p].axis[a] = st.axes[a];
        for (int a = 4; a < 6; a++) g.pad[p].axis[a] = (st.axes[a] + 1.0f) * 0.5f;   /* GLFW rests a trigger at -1 */
    }
}
static void on_cursor(GLFWwindow *w, double x, double y) {
    (void)w;
    if (g.have_mouse) { g.mdx += x - g.mx; g.mdy += y - g.my; }
    g.mx = x; g.my = y; g.have_mouse = 1;
}
static void on_scroll(GLFWwindow *w, double x, double y) { (void)w; (void)x; g.scroll_y += y; }
static void on_resize(GLFWwindow *w, int width, int height) { (void)w; (void)width; (void)height; g.resized = 1; }

/* ── Open / close ─────────────────────────────────────────────────────────── */
/* Open a window and build the renderer. [quad_capacity] is how many quads
 * the GPU buffer holds; zero or less means the default, 8 388 608 (64 MB). */
/* ── Figures ──────────────────────────────────────────────────────────────── */
/* A rig's part meshes are uploaded once (rig_part_begin, chunk_part,
 * chunk_commit) as pieces of 32 voxels a side and drawn for every figure of
 * the rig. Each frame the pose kernel's draw list (an_x_draw_list in
 * strata_anim.c) gives every visible part of every figure and its matrix;
 * the parts are culled by their bounding spheres, grouped by rig part, and
 * each piece is drawn once, instanced over every figure showing that part.
 * A figure quad's texture field is a palette index: the G-buffer pass takes
 * its colour, roughness and emission from the rig's palette.
 *
 * Absent strata_anim.c, the weak definition below answers no parts, so a
 * game that draws but does not animate still links. */
__attribute__((weak)) int an_x_draw_list(double blend, const int32_t **rig_part, const float **xf, const int32_t **flags) {
    (void)blend; if (rig_part) *rig_part = NULL; if (xf) *xf = NULL; if (flags) *flags = NULL;
    return 0;
}

/* The sun the drawing casts shadows by, given to the kernel before each
 * draw list so it lists figures out of view whose shadows may fall in it
 * (flag bit 1). Absent strata_anim.c it does nothing. */
__attribute__((weak)) void an_x_draw_sun(double x, double y, double z) { (void)x; (void)y; (void)z; }

#define SV_FIG_RIGS 256                   /* AN_RIGS */
#define SV_FIG_PARTS 64                   /* AN_PARTS */
#define SV_FIG_RP (SV_FIG_RIGS * SV_FIG_PARTS)
#define SV_FIG_PIECES 16384
#define SV_FIG_INST 16384                 /* AN_DRAW_MAX: parts in one draw list */
#define SV_FIG_SEEN 32768                 /* figures remembered between frames, to tell which moved (twice the most a list holds) */
#define SV_FIG_TRACED 64                  /* AN_TRACED: figures in the rays' list at once; more are drawn plain */
#define SV_TRACE_ROWS (SV_FIG_TRACED * (1 + SV_FIG_PARTS))   /* body table rows for them: a group row and its parts' */
#define SV_FIG_SETTLE 8                   /* frames a traced figure's surroundings keep settling after it stops */
#define SV_FIG_REACH 24.0f                /* metres along the ground a moving traced figure's shadow is followed for history */
#define SV_FIG_BOXES (2 * SV_FIG_TRACED)  /* history boxes for traced figures: a shadow box and a glow box each */
#define SV_GLOW 5.0f                      /* must equal GLOW in native/shaders/sky.glsl */
#define SV_GLOW_FLOOR 0.01f               /* light (linear, of a sun of about 1) below which a figure's glow is not followed for history */
#define SV_TABLE_ROWS (SV_MAX_BODIES + SV_TRACE_ROWS)
typedef struct { int32_t rp, next; uint32_t first, count; int32_t o[3]; float lo[3], hi[3]; } FigPiece;   /* lo, hi: what its quads cover, voxels of the piece */
static struct {
    FigPiece *pieces; int32_t npieces, free_head;
    int32_t *head;                        /* SV_FIG_RP: the first piece of each rig part, -1 */
    float (*anchor)[4];                   /* SV_FIG_RP: the part's anchor (its voxels) and voxel size (metres) */
    float (*sphere)[4];                   /* SV_FIG_RP: a sphere round the part's pieces, metres in the part's frame; w < 0 until worked out */
    VkBuffer pal; VkDeviceMemory pal_mem; uint32_t *pal_map;   /* SV_FIG_RIGS x 256 entries, two words each */
    int32_t *count, *base, *touched; int32_t ntouched;         /* per rig part: instances this frame, where they start */
    int32_t *order;                       /* the culled draw list's entries, in order */
    uint8_t *moving;                      /* per draw-list entry: its figure moved */
    struct FigSeen { int32_t id; float sum; int32_t still; } *seen[2]; int now;   /* figures drawn last frame and this, by id: a checksum of their matrices, frames since it moved */
    uint8_t *quiet;                       /* per draw-list entry: frames since its figure moved (to 255) */
    uint8_t *placed;                      /* per draw-list entry: placed in the rays' list this frame (left out of the shadow map) */
    /* traced figures (design section 9): each rig part's shape for the rays, palette indices in the body
     * class buffer, shared by every figure of the rig; each frame a group row per traced figure and a row
     * per part with a shape, appended to the body table by bodies_to_frame */
    struct FigShape { uint32_t first, nwords; int32_t dims[3]; int shaped; } *shape;   /* SV_FIG_RP */
    BodyRow *trows; int32_t tgroups, tparts;   /* SV_TRACE_ROWS: groups first, then parts (a group's dims.x counts from the first part row) */
    float (*tmov)[8]; int32_t ntmov;      /* SV_FIG_BOXES: boxes (voxels) round traced figures that moved lately: their shadows (w 1), their glow (w 0) */
    float *glow;                          /* SV_FIG_RP: how far (metres) a part's glowing voxels light what is round them, past SV_GLOW_FLOOR; < 0 until worked out */
    int boxes_off;                        /* STRATA_FIG_BOXES=0: traced figures unsettle no history round them (to show what they prevent) */
    int64_t traced_figs, traced_parts, traced_over;
    struct FigGroup { int32_t rp, base, n; } *groups; int32_t ngroups;
    float blend;
    int64_t parts, draws; double cpu_us, list_us;
    /* the plain figures' sun shadow map (fig_shadow_*): one depth image shared by the frames in flight */
    struct FigGroup *sgroups; int32_t nsgroups;
    VkImage fsm; VkDeviceMemory fsm_mem; VkImageView fsm_view; VkSampler fsm_sampler;
    VkRenderPass fsm_pass; VkFramebuffer fsm_fb; VkPipeline fsm_pipe;
    int fsm_off;                          /* STRATA_FIG_SHADOWS=0: no figure casts a shadow */
    int fsm_fresh;                        /* the map has not yet been put in its sampled layout */
    int64_t sparts, sdraws; double shadow_ms;
    double cpu_worst, shadow_worst;       /* the worst of cpu_us and shadow_ms since the start (stats 20, 21: the cost with no figures) */
} fg;

/* The shadow map: SV_FSM_RES texels a side over a square SV_FSM_HALF x 2
 * metres a side round the eye (so it holds the view out to 64 m whichever
 * way it looks), seen from the sun, its depth reaching SV_FSM_DEPTH metres
 * toward and away from the sun. A texel is 6.25 cm, a figure voxel. */
#define SV_FSM_RES 2048
#define SV_FSM_HALF 64.0f
#define SV_FSM_DEPTH 128.0f
static int fig_shadow_make(void);
static void fig_shadow_free(void);

static int fig_init(void) {
    fg.pieces = calloc(SV_FIG_PIECES, sizeof *fg.pieces);
    fg.head = malloc(SV_FIG_RP * sizeof *fg.head);
    fg.anchor = calloc(SV_FIG_RP, sizeof *fg.anchor);
    fg.sphere = calloc(SV_FIG_RP, sizeof *fg.sphere);
    fg.count = calloc(SV_FIG_RP, sizeof *fg.count);
    fg.base = calloc(SV_FIG_RP, sizeof *fg.base);
    fg.touched = calloc(SV_FIG_RP, sizeof *fg.touched);
    fg.order = calloc(SV_FIG_INST, sizeof *fg.order);
    fg.moving = calloc(SV_FIG_INST, 1);
    fg.groups = calloc(SV_FIG_RP, sizeof *fg.groups);
    fg.seen[0] = calloc(SV_FIG_SEEN, sizeof *fg.seen[0]);
    fg.seen[1] = calloc(SV_FIG_SEEN, sizeof *fg.seen[1]);
    fg.sgroups = calloc(SV_FIG_RP, sizeof *fg.sgroups);
    fg.quiet = calloc(SV_FIG_INST, 1);
    fg.placed = calloc(SV_FIG_INST, 1);
    fg.shape = calloc(SV_FIG_RP, sizeof *fg.shape);
    fg.trows = calloc(SV_TRACE_ROWS, sizeof *fg.trows);
    fg.tmov = calloc(SV_FIG_BOXES, sizeof *fg.tmov);
    fg.glow = malloc(SV_FIG_RP * sizeof *fg.glow);
    if (!fg.quiet || !fg.placed || !fg.shape || !fg.trows || !fg.tmov || !fg.glow) return 0;
    for (int i = 0; i < SV_FIG_RP; i++) fg.glow[i] = -1.0f;
    if (!fg.pieces || !fg.head || !fg.anchor || !fg.sphere || !fg.count || !fg.base || !fg.touched || !fg.order || !fg.moving || !fg.groups || !fg.seen[0] || !fg.seen[1] || !fg.sgroups) return 0;
    for (int i = 0; i < SV_FIG_RP; i++) { fg.head[i] = -1; fg.anchor[i][3] = 1.0f; fg.sphere[i][3] = -1.0f; }
    fg.npieces = 0; fg.free_head = -1; fg.blend = 1.0f;
    void *map = NULL;
    if (!make_buffer((VkDeviceSize)SV_FIG_RIGS * 256 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &fg.pal, &fg.pal_mem, &map)) return 0;
    fg.pal_map = map; memset(fg.pal_map, 0, (size_t)SV_FIG_RIGS * 256 * 8);
    const char *fs = getenv("STRATA_FIG_SHADOWS");
    fg.fsm_off = fs && fs[0] == '0';
    const char *fb = getenv("STRATA_FIG_BOXES");
    fg.boxes_off = fb && fb[0] == '0';
    return fig_shadow_make();
}
static void fig_free(void) {
    if (g.dev) { if (fg.pal) vkDestroyBuffer(g.dev, fg.pal, NULL); if (fg.pal_mem) vkFreeMemory(g.dev, fg.pal_mem, NULL); fig_shadow_free(); }
    free(fg.pieces); free(fg.head); free(fg.anchor); free(fg.sphere); free(fg.count); free(fg.base); free(fg.touched);
    free(fg.order); free(fg.moving); free(fg.groups); free(fg.seen[0]); free(fg.seen[1]); free(fg.sgroups);
    free(fg.quiet); free(fg.placed); free(fg.shape); free(fg.trows); free(fg.tmov); free(fg.glow);
    memset(&fg, 0, sizeof fg);
}

/* Begin uploading one piece of rig [rig]'s part [part]: the 32-cubed piece
 * whose low corner is (ox, oy, oz) in the part's grid. Give its quads with
 * sv_chunk_part(0, ...) and finish with sv_chunk_commit, as for a chunk. It
 * is shared by every figure of the rig and never written into the light
 * volume. A piece given again replaces the one it had; an empty one removes it. */
int64_t sv_rig_part_begin(int64_t rig, int64_t part, int64_t ox, int64_t oy, int64_t oz) {
    g.stage.open = 0;
    if (!g.ok || !fg.pieces || rig < 0 || rig >= SV_FIG_RIGS || part < 0 || part >= SV_FIG_PARTS) return 0;
    for (int i = 0; i < 4; i++) g.stage.n[i] = 0;
    memset(g.stage.ord, 0, sizeof g.stage.ord);
    g.stage.origin[0] = (int32_t)ox; g.stage.origin[1] = (int32_t)oy; g.stage.origin[2] = (int32_t)oz;
    g.stage.rp = (int32_t)(rig * SV_FIG_PARTS + part);
    g.stage.open = 1; g.stage.body = 0; g.stage.fig = 1;
    g.stage_light_n = 0;
    return 1;
}
static int64_t fig_commit(void) {
    g.stage.open = 0; g.stage.fig = 0;
    if (g.stage.n[1] || g.stage.n[2] || g.stage.n[3]) return 0;       /* palette voxels are opaque: one part of quads */
    int32_t rp = g.stage.rp, at = -1, prev = -1;
    for (int32_t i = fg.head[rp]; i >= 0; prev = i, i = fg.pieces[i].next)
        if (fg.pieces[i].o[0] == g.stage.origin[0] && fg.pieces[i].o[1] == g.stage.origin[1] && fg.pieces[i].o[2] == g.stage.origin[2]) { at = i; break; }
    uint32_t n = g.stage.n[0], first = 0;
    if (n > 0 && !sa_alloc(&g_quads, n, &first)) { g.refused++; return 0; }
    if (n > 0) memcpy(g.giga_map + (size_t)first * 2, g.stage.part[0], (size_t)n * 8);
    if (at >= 0) {                        /* frames in flight may still draw the old range: held two frames */
        sa_release(&g_quads, (Range){ fg.pieces[at].first, fg.pieces[at].count }, g.frame_no);
        if (n == 0) {
            if (prev >= 0) fg.pieces[prev].next = fg.pieces[at].next; else fg.head[rp] = fg.pieces[at].next;
            fg.pieces[at].next = fg.free_head; fg.free_head = at;
            fg.sphere[rp][3] = -1.0f;
            return 1;
        }
    } else {
        if (n == 0) return 1;
        if (fg.free_head >= 0) { at = fg.free_head; fg.free_head = fg.pieces[at].next; }
        else if (fg.npieces < SV_FIG_PIECES) at = fg.npieces++;
        else { sa_cancel(&g_quads, (Range){ first, n }); g.refused++; return 0; }
        fg.pieces[at].rp = rp; fg.pieces[at].next = fg.head[rp]; fg.head[rp] = at;
        memcpy(fg.pieces[at].o, g.stage.origin, sizeof fg.pieces[at].o);
    }
    FigPiece *pc = &fg.pieces[at];
    pc->first = first; pc->count = n;
    for (int k = 0; k < 3; k++) { pc->lo[k] = 32.0f; pc->hi[k] = 0.0f; }
    const uint32_t *q = g.stage.part[0];
    for (uint32_t i = 0; i < n; i++) {    /* what each quad covers: Strata.Quad's layout, as quad.vert reads it */
        uint32_t w0 = q[i * 2], face = (w0 >> 25) & 7u;
        float p[3] = { (float)(w0 & 31u), (float)((w0 >> 5) & 31u), (float)((w0 >> 10) & 31u) };
        float w = (float)(((w0 >> 15) & 31u) + 1u), h = (float)(((w0 >> 20) & 31u) + 1u), e[3];
        if (face < 2u) { e[0] = w; e[1] = 1.0f; e[2] = h; } else if (face < 4u) { e[0] = 1.0f; e[1] = h; e[2] = w; } else { e[0] = w; e[1] = h; e[2] = 1.0f; }
        for (int k = 0; k < 3; k++) { if (p[k] < pc->lo[k]) pc->lo[k] = p[k]; if (p[k] + e[k] > pc->hi[k]) pc->hi[k] = p[k] + e[k]; }
    }
    fg.sphere[rp][3] = -1.0f;
    return 1;
}
/* Rig [rig]'s part [part] turns about (ax, ay, az) of its own grid (its
 * anchor, in its voxels), and its voxels are [voxel] metres: a piece's quads
 * sit at (piece corner + quad position - anchor) x voxel in the part's frame. */
int64_t sv_rig_part_anchor(int64_t rig, int64_t part, double ax, double ay, double az, double voxel) {
    if (!g.ok || !fg.anchor || rig < 0 || rig >= SV_FIG_RIGS || part < 0 || part >= SV_FIG_PARTS || !(voxel > 0.0) || !isfinite(ax) || !isfinite(ay) || !isfinite(az) || !isfinite(voxel)) return 0;
    int32_t rp = (int32_t)(rig * SV_FIG_PARTS + part);
    fg.anchor[rp][0] = (float)ax; fg.anchor[rp][1] = (float)ay; fg.anchor[rp][2] = (float)az; fg.anchor[rp][3] = (float)voxel;
    fg.sphere[rp][3] = -1.0f;
    if (fg.glow) fg.glow[rp] = -1.0f;                 /* its glow's reach depends on the voxel size */
    return 1;
}
static uint32_t unorm(double v, double scale) { return v <= 0.0 ? 0u : (v >= 1.0 ? (uint32_t)scale : (uint32_t)(v * scale + 0.5)); }
/* Rig [rig]'s palette: 256 entries of five floats (red, green, blue: linear
 * 0..1; roughness 0..1; emission 0..1). Entry 0 is empty and never drawn.
 * Packed two words an entry: red and green in 11 bits, blue in 10; roughness
 * and emission in 16 bits each. */
int64_t sv_rig_palette(int64_t rig, void *floats) {
    if (!g.ok || !fg.pal_map || rig < 0 || rig >= SV_FIG_RIGS || !floats || narr_len(floats) < 256 * 5) return 0;
    const float *c = narr_data(floats);
    uint32_t *e = fg.pal_map + (size_t)rig * 256 * 2;
    for (int i = 0; i < 256; i++, c += 5) {
        e[i * 2] = unorm(c[0], 2047.0) | (unorm(c[1], 2047.0) << 11) | (unorm(c[2], 1023.0) << 22);
        e[i * 2 + 1] = unorm(c[3], 65535.0) | (unorm(c[4], 65535.0) << 16);
    }
    if (fg.glow) for (int p = 0; p < SV_FIG_PARTS; p++) fg.glow[rig * SV_FIG_PARTS + p] = -1.0f;
    return 1;
}
/* A shape for the rays (a body's light classes, or a figure part's palette
 * indices): its bytes, x fastest, then y, then z, then a coarse layer of one
 * byte a SV_SHAPE_BLOCK^3 block of them (1 if any voxel in the block is not
 * 0), blocks x fastest, starting on the word after the voxels. A ray walks
 * the blocks and steps voxel by voxel only inside the ones that hold
 * something (trace.glsl's body_ray). Allocated from g_bbytes; false if full. */
#define SV_SHAPE_BLOCK 4
static int put_shape(const uint8_t *bytes, int64_t sx, int64_t sy, int64_t sz, uint32_t *first_out, uint32_t *words_out) {
    int64_t n = sx * sy * sz, B = SV_SHAPE_BLOCK;
    int64_t bx = (sx + B - 1) / B, by = (sy + B - 1) / B, bz = (sz + B - 1) / B;
    uint32_t fine = (uint32_t)((n + 3) / 4), words = fine + (uint32_t)((bx * by * bz + 3) / 4), first = 0;
    if (!sa_alloc(&g_bbytes, words, &first)) return 0;
    uint8_t *dst = (uint8_t *)(g.bbytes_map + first);
    memset(dst, 0, (size_t)words * 4);
    memcpy(dst, bytes, (size_t)n);
    uint8_t *coarse = dst + (size_t)fine * 4;
    for (int64_t z = 0; z < sz; z++)
        for (int64_t y = 0; y < sy; y++) {
            const uint8_t *row = bytes + (size_t)(sx * (y + sy * z));
            uint8_t *crow = coarse + (size_t)(bx * (y / B + by * (z / B)));
            for (int64_t x = 0; x < sx; x++) if (row[x]) crow[x / B] = 1;
        }
    *first_out = first; *words_out = words;
    return 1;
}
/* Rig [rig]'s part [part] as the rays see it: one palette index a voxel (0
 * empty), x fastest, then y, then z, [sx] x [sy] x [sz], its low corner the
 * part grid's (the anchor and voxel size are rig_part_anchor's). Shared by
 * every traced figure of the rig, as its meshes are; a ray that hits a voxel
 * takes its colour, roughness and emission from the rig's palette. Replaces
 * the shape the part had. */
int64_t sv_rig_shape(int64_t rig, int64_t part, void *bytes, int64_t n, int64_t sx, int64_t sy, int64_t sz) {
    if (!g.ok || !fg.shape || rig < 0 || rig >= SV_FIG_RIGS || part < 0 || part >= SV_FIG_PARTS || !bytes) return 0;
    if (sx < 1 || sy < 1 || sz < 1 || sx > 128 || sy > 128 || sz > 128 || n < sx * sy * sz) return 0;
    struct FigShape *sh = &fg.shape[rig * SV_FIG_PARTS + part];
    uint32_t words = 0, first = 0;
    if (!put_shape((const uint8_t *)bytes, sx, sy, sz, &first, &words)) { g.refused++; return 0; }
    if (sh->shaped) sa_release(&g_bbytes, (SaRange){ sh->first, sh->nwords }, g.frame_no);
    sh->first = first; sh->nwords = words; sh->dims[0] = (int32_t)sx; sh->dims[1] = (int32_t)sy; sh->dims[2] = (int32_t)sz;
    sh->shaped = 1;
    fg.glow[rig * SV_FIG_PARTS + part] = -1.0f;
    return 1;
}
/* Forget rig [rig] whole: every part's pieces, ray shape, anchor and glow,
 * so a rig uploaded again under the same id (a figure world opened again
 * starts its rig ids at 0) keeps nothing of the last one's. Ranges frames in
 * flight may still draw are held two frames, as a replaced piece's are. */
int64_t sv_rig_clear(int64_t rig) {
    if (!g.ok || !fg.pieces || !fg.shape || rig < 0 || rig >= SV_FIG_RIGS) return 0;
    for (int p = 0; p < SV_FIG_PARTS; p++) {
        int32_t rp = (int32_t)(rig * SV_FIG_PARTS + p);
        for (int32_t i = fg.head[rp], nx; i >= 0; i = nx) {
            nx = fg.pieces[i].next;
            sa_release(&g_quads, (Range){ fg.pieces[i].first, fg.pieces[i].count }, g.frame_no);
            fg.pieces[i].next = fg.free_head; fg.free_head = i;
        }
        fg.head[rp] = -1;
        struct FigShape *sh = &fg.shape[rp];
        if (sh->shaped) sa_release(&g_bbytes, (SaRange){ sh->first, sh->nwords }, g.frame_no);
        sh->shaped = 0;
        fg.anchor[rp][0] = fg.anchor[rp][1] = fg.anchor[rp][2] = 0.0f; fg.anchor[rp][3] = 1.0f;
        fg.sphere[rp][3] = -1.0f;
        if (fg.glow) fg.glow[rp] = -1.0f;
    }
    return 1;
}
/* how far (metres) rig part rp's glowing voxels light their surroundings above
 * SV_GLOW_FLOOR: n emitting voxels of the brightest emission e seen as a blob
 * of n^(2/3) voxel faces, lighting a surface r away by about
 * e x GLOW x area / (pi r^2); 0 when nothing in it glows */
static float fig_glow(int32_t rp) {
    if (fg.glow[rp] >= 0.0f) return fg.glow[rp];
    const struct FigShape *sh = &fg.shape[rp];
    const uint32_t *pal = fg.pal_map + (size_t)(rp / SV_FIG_PARTS) * 256 * 2;
    const unsigned char *b = (const unsigned char *)(g.bbytes_map + sh->first);
    uint32_t nv = (uint32_t)(sh->dims[0] * sh->dims[1] * sh->dims[2]), lit = 0;
    float emax = 0.0f;
    for (uint32_t i = 0; i < nv; i++) {
        if (!b[i]) continue;
        float e = (float)(pal[b[i] * 2 + 1] >> 16) / 65535.0f;
        if (e > 0.0f) { lit++; if (e > emax) emax = e; }
    }
    float v = fg.anchor[rp][3], area = cbrtf((float)lit * (float)lit) * v * v;
    /* SV_GLOW is sky.glsl's GLOW: how bright a fully emitting voxel is against a sun of about 1 */
    fg.glow[rp] = lit ? sqrtf(emax * SV_GLOW * area / (3.14159265f * SV_GLOW_FLOOR)) : 0.0f;
    return fg.glow[rp];
}
/* How far between its last two poses each figure is drawn (0..1), passed to
 * the draw list each frame. 1 until a game sets it: the last pose. */
void sv_set_blend(double blend) { fg.blend = blend > 0.0 ? (blend < 1.0 ? (float)blend : 1.0f) : 0.0f; }

/* a sphere round rig part rp's pieces, in metres in the part's frame */
static void fig_sphere(int32_t rp) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, v = fg.anchor[rp][3];
    for (int32_t i = fg.head[rp]; i >= 0; i = fg.pieces[i].next)
        for (int k = 0; k < 3; k++) {
            float a = ((float)fg.pieces[i].o[k] + fg.pieces[i].lo[k] - fg.anchor[rp][k]) * v, b = ((float)fg.pieces[i].o[k] + fg.pieces[i].hi[k] - fg.anchor[rp][k]) * v;
            if (a < lo[k]) lo[k] = a; if (b > hi[k]) hi[k] = b;
        }
    float r2 = 0.0f;
    for (int k = 0; k < 3; k++) { fg.sphere[rp][k] = 0.5f * (lo[k] + hi[k]); float d = 0.5f * (hi[k] - lo[k]); r2 += d * d; }
    fg.sphere[rp][3] = sqrtf(r2) + 1e-4f;
}
/* a rotation matrix (column-major 3x3) as a unit quaternion x y z w */
static void fig_mat_q(const float *m, float *q) {
    float m00 = m[0], m10 = m[1], m20 = m[2], m01 = m[3], m11 = m[4], m21 = m[5], m02 = m[6], m12 = m[7], m22 = m[8], tr = m00 + m11 + m22;
    if (tr > 0.0f) { float s = sqrtf(tr + 1.0f) * 2.0f; q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s; }
    else if (m00 > m11 && m00 > m22) { float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f; q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s; }
    else if (m11 > m22) { float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f; q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s; }
    else { float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f; q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s; }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 0.0f) for (int k = 0; k < 4; k++) q[k] /= l; else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
}
static uint32_t fig_hash(int32_t id) { uint32_t h = (uint32_t)id * 2654435761u; return (h ^ (h >> 15)) & (SV_FIG_SEEN - 1); }

/* This frame's figure parts into the Frame's instance buffer, grouped by rig
 * part: the draw list at the blend set, each part culled by its sphere
 * against the view, and each figure marked moving when its matrices differ
 * from last frame's (or it was not drawn then). */
static void fig_shadow_prepare(Frame *F, int n, const int32_t *rp, const float *xf);
static void fig_trace_prepare(int n, const int32_t *rp, const float *xf, const int32_t *fl);
/* do the light modes cast rays this frame (2 to 8): else every figure is plain */
static int fig_rays_on(void) { return g.light_mode >= 2 && g.light_mode <= 8; }
static void fig_prepare(Frame *F) {
    fg.ngroups = 0; fg.parts = 0; fg.nsgroups = 0; fg.sparts = 0;
    fg.tgroups = 0; fg.tparts = 0; fg.ntmov = 0; fg.traced_figs = 0; fg.traced_parts = 0; fg.traced_over = 0;
    g.ubo_map[UBO_FSHADOW] = 0.0f;        /* no shadow map this frame unless plain figures fill it below */
    double t0 = glfwGetTime();
    /* the sun the kernel follows figures' shadows into view by: none when no shadow is cast */
    const float *sun = g.ubo_map + 16;
    if (fig_rays_on() && sun[1] > 0.01f) an_x_draw_sun(sun[0], sun[1], sun[2]); else an_x_draw_sun(0.0, 0.0, 0.0);
    const int32_t *rp = NULL, *fl = NULL; const float *xf = NULL;
    int n = an_x_draw_list((double)fg.blend, &rp, &xf, &fl);
    double t1 = glfwGetTime();
    fg.list_us = (t1 - t0) * 1e6;
    if (n <= 0 || !rp || !xf || !fl || !F->finst_map) { fg.cpu_us = fg.list_us; return; }
    if (n > SV_FIG_INST) n = SV_FIG_INST;
    /* the view's six planes, from the view-projection's rows (OpenGL clip volume) */
    const float *m = g.ubo_map;
    float pl[6][4];
    for (int p = 0; p < 6; p++) {
        int axis = p >> 1; float sgn = (p & 1) ? -1.0f : 1.0f, len = 0.0f;
        for (int c = 0; c < 4; c++) pl[p][c] = m[c * 4 + 3] + sgn * m[c * 4 + axis];
        for (int c = 0; c < 3; c++) len += pl[p][c] * pl[p][c];
        len = sqrtf(len); if (len > 0.0f) for (int c = 0; c < 4; c++) pl[p][c] /= len;
    }
    /* which figures moved: a checksum of each figure's matrices against last frame's */
    struct FigSeen *was = fg.seen[fg.now], *is = fg.seen[fg.now ^ 1];
    memset(is, 0, SV_FIG_SEEN * sizeof *is);
    for (int i = 0; i < n; ) {
        int32_t id = fl[i] >> 8; int j = i; float sum = 0.0f;
        for (; j < n && (fl[j] >> 8) == id; j++) for (int k = 0; k < 12; k++) sum += xf[(size_t)j * 12 + k] * (float)(k + 1 + (j - i) * 13);
        int mv = 1, still = 0;
        for (uint32_t h = fig_hash(id), probes = 0; probes < SV_FIG_SEEN; h = (h + 1) & (SV_FIG_SEEN - 1), probes++) {
            if (was[h].id == 0) break;
            if (was[h].id == id + 1) { mv = was[h].sum != sum; still = mv ? 0 : (was[h].still < 255 ? was[h].still + 1 : 255); break; }
        }
        for (uint32_t h = fig_hash(id), probes = 0; probes < SV_FIG_SEEN; h = (h + 1) & (SV_FIG_SEEN - 1), probes++)
            if (is[h].id == 0) { is[h].id = id + 1; is[h].sum = sum; is[h].still = still; break; }
        for (int k = i; k < j; k++) { fg.moving[k] = (uint8_t)mv; fg.quiet[k] = (uint8_t)still; }
        i = j;
    }
    fg.now ^= 1;
    /* cull, and count each rig part's instances */
    int kept = 0;
    fg.ntouched = 0;
    for (int i = 0; i < n; i++) {
        int32_t r = rp[i];
        if ((fl[i] & 2) || r < 0 || r >= SV_FIG_RP || fg.head[r] < 0) continue;   /* bit 1: listed for its shadow only */
        if (fg.sphere[r][3] < 0.0f) fig_sphere(r);
        const float *x = xf + (size_t)i * 12, *s = fg.sphere[r];
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = x[k] * s[0] + x[3 + k] * s[1] + x[6 + k] * s[2] + x[9 + k];
        int out = 0;
        for (int p = 0; p < 6 && !out; p++) if (pl[p][0] * c[0] + pl[p][1] * c[1] + pl[p][2] * c[2] + pl[p][3] < -s[3]) out = 1;
        if (out) continue;
        if (fg.count[r]++ == 0) fg.touched[fg.ntouched++] = r;
        fg.order[kept++] = i;
    }
    int32_t at = 0;
    for (int t = 0; t < fg.ntouched; t++) {
        int32_t r = fg.touched[t];
        fg.groups[t] = (struct FigGroup){ r, at, fg.count[r] };
        fg.base[r] = at; at += fg.count[r]; fg.count[r] = 0;
    }
    fg.ngroups = fg.ntouched;
    for (int k = 0; k < kept; k++) {
        int i = fg.order[k]; int32_t r = rp[i];
        struct FigInst *o = &F->finst_map[fg.base[r] + fg.count[r]++];
        const float *x = xf + (size_t)i * 12;
        o->pos[0] = x[9]; o->pos[1] = x[10]; o->pos[2] = x[11];
        o->flags = fg.moving[i] ? 1u : 0u;
        fig_mat_q(x, o->rot);
    }
    for (int t = 0; t < fg.ntouched; t++) fg.count[fg.touched[t]] = 0;
    fg.parts = kept;
    fig_trace_prepare(n, rp, xf, fl);
    fig_shadow_prepare(F, n, rp, xf);
    fg.cpu_us = (glfwGetTime() - t0) * 1e6;
}
/* Traced figures into the rays' list (design section 9): the first
 * SV_FIG_TRACED figures the draw list flags traced (bit 0), in light modes 2
 * to 8, each as a group row (a sphere round it all, its rig, where its parts'
 * rows start and how many) and a row per part with a shape (rig_shape),
 * placed at the part's drawn pose: the grid's low corner in metres, its
 * rotation, its size and first word, its voxel size in pos.w (a group row
 * has -1 there). Those parts are marked placed and left out of the shadow
 * map; a traced figure past the cap, or a part with no shape, stays plain.
 * A traced figure that moved in the last SV_FIG_SETTLE frames adds up to two
 * boxes to the light pass's history: its shadow's footprint (its bounds and
 * 0.5 m, swept away from the sun until its top has come down 2 m below its
 * feet, at most SV_FIG_REACH m along the ground), where only the sun term
 * samples every frame and lean on 2 past samples (w 1); and, if any of its
 * parts glows, what that glow lights above SV_GLOW_FLOOR (fig_glow), where
 * every term settles (w 0). bodies_to_frame appends them. */
static void fig_trace_prepare(int n, const int32_t *rp, const float *xf, const int32_t *fl) {
    memset(fg.placed, 0, (size_t)n);
    if (!fig_rays_on()) return;
    float scale = g.ubo_map[31] > 0.0f ? g.ubo_map[31] : 1.0f;
    const float *sun = g.ubo_map + 16;
    float sl = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    int shadows = sl > 0.0f && sun[1] / sl > 0.01f;
    for (int i = 0; i < n; ) {
        int32_t id = fl[i] >> 8; int j = i;
        while (j < n && (fl[j] >> 8) == id) j++;
        if (!(fl[i] & 1)) { i = j; continue; }
        if (fg.tgroups >= SV_FIG_TRACED) { fg.traced_over++; i = j; continue; }
        BodyRow *gr = &fg.trows[fg.tgroups];
        int32_t first = fg.tparts, rig = -1;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        float glo[3] = { 1e30f, 1e30f, 1e30f }, ghi[3] = { -1e30f, -1e30f, -1e30f };   /* round its glowing parts, out to their glow's reach */
        for (int k = i; k < j; k++) {
            int32_t r = rp[k];
            if (r < 0 || r >= SV_FIG_RP || !fg.shape[r].shaped) continue;
            const struct FigShape *sh = &fg.shape[r];
            const float *x = xf + (size_t)k * 12, *an = fg.anchor[r];
            float v = an[3];
            BodyRow *pr = &fg.trows[SV_FIG_TRACED + fg.tparts];
            /* the grid's low corner: the part's origin less its anchor, turned */
            for (int c = 0; c < 3; c++) pr->pos[c] = x[9 + c] - (x[c] * an[0] + x[3 + c] * an[1] + x[6 + c] * an[2]) * v;
            pr->pos[3] = v;
            fig_mat_q(x, pr->rot);
            pr->dims[0] = sh->dims[0]; pr->dims[1] = sh->dims[1]; pr->dims[2] = sh->dims[2]; pr->dims[3] = (int32_t)sh->first;
            float h[3] = { sh->dims[0] * v * 0.5f, sh->dims[1] * v * 0.5f, sh->dims[2] * v * 0.5f };
            float rad = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            for (int c = 0; c < 3; c++) {
                float m = pr->pos[c] + x[c] * h[0] + x[3 + c] * h[1] + x[6 + c] * h[2];
                pr->sphere[c] = m / scale;
                if (m - rad < lo[c]) lo[c] = m - rad;
                if (m + rad > hi[c]) hi[c] = m + rad;
            }
            pr->sphere[3] = rad / scale + 0.01f;
            float gr2 = fig_glow(r);
            if (gr2 > 0.0f) {
                gr2 = fminf(gr2, 48.0f * scale) + rad;              /* a sky ray reaches 48 voxels */
                for (int c = 0; c < 3; c++) { float m = pr->sphere[c] * scale; glo[c] = fminf(glo[c], m - gr2); ghi[c] = fmaxf(ghi[c], m + gr2); }
            }
            rig = r / SV_FIG_PARTS;
            fg.placed[k] = 1;
            fg.tparts++;
        }
        if (fg.tparts > first) {
            float c[3], rad2 = 0.0f;
            for (int k = 0; k < 3; k++) { c[k] = 0.5f * (lo[k] + hi[k]); float d = 0.5f * (hi[k] - lo[k]); rad2 += d * d; }
            memset(gr, 0, sizeof *gr);
            gr->pos[3] = -1.0f;
            gr->rot[3] = 1.0f;
            gr->dims[0] = first; gr->dims[1] = fg.tparts - first; gr->dims[2] = rig;
            for (int k = 0; k < 3; k++) gr->sphere[k] = c[k] / scale;
            gr->sphere[3] = sqrtf(rad2) / scale + 0.01f;
            fg.tgroups++;
            fg.traced_figs++;
            if (!fg.boxes_off && fg.quiet[i] < SV_FIG_SETTLE) {
                if (shadows) {   /* its shadow's footprint: its bounds and 0.5 m, swept away from the sun to 2 m under its feet: the sun term alone */
                    float *m = fg.tmov[fg.ntmov++], sweep[3];
                    float s[3] = { sun[0] / sl, sun[1] / sl, sun[2] / sl }, flat = sqrtf(s[0] * s[0] + s[2] * s[2]);
                    float t = (hi[1] - lo[1] + 2.0f) / s[1];
                    if (flat * t > SV_FIG_REACH) t = SV_FIG_REACH / flat;
                    for (int k = 0; k < 3; k++) sweep[k] = -s[k] * t;
                    for (int k = 0; k < 3; k++) {
                        m[k] = (fminf(lo[k], lo[k] + sweep[k]) - 0.5f) / scale;
                        m[4 + k] = (fmaxf(hi[k], hi[k] + sweep[k]) + 0.5f) / scale;
                    }
                    m[3] = 1.0f; m[7] = 0.0f;
                }
                if (glo[0] <= ghi[0]) {   /* what its glow reaches: every term */
                    float *m = fg.tmov[fg.ntmov++];
                    for (int k = 0; k < 3; k++) { m[k] = glo[k] / scale; m[4 + k] = ghi[k] / scale; }
                    m[3] = m[7] = 0.0f;
                }
            }
        }
        i = j;
    }
    fg.traced_parts = fg.tparts;
}
/* Draw this frame's figure parts, in the geometry pass: each piece of each
 * rig part shown, instanced over the figures showing it. Answers the draws. */
static int fig_draw(Frame *F, const struct FigGroup *groups, int32_t ngroups, uint32_t inst0) {
    int draws = 0;
    for (int t = 0; t < ngroups; t++) {
        const struct FigGroup *gr = &groups[t];
        const float *an = fg.anchor[gr->rp];
        for (int32_t i = fg.head[gr->rp]; i >= 0; i = fg.pieces[i].next) {
            const FigPiece *pc = &fg.pieces[i];
            /* base, flags (bit 1: a figure), first quad, quads, first instance, rig; the piece's corner less the anchor; the voxel */
            uint32_t pcw[10] = { 0u, 2u, pc->first, pc->count, inst0 + (uint32_t)gr->base, (uint32_t)(gr->rp / SV_FIG_PARTS), 0, 0, 0, 0 };
            float f4[4] = { (float)pc->o[0] - an[0], (float)pc->o[1] - an[1], (float)pc->o[2] - an[2], an[3] };
            memcpy(pcw + 6, f4, sizeof f4);
            vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcw, pcw);
            vkCmdDraw(F->cmd, 4, pc->count * (uint32_t)gr->n, 0, 0);
            draws++;
        }
    }
    return draws;
}
static int fig_record(Frame *F) {
    fg.draws = 0;
    if (fg.ngroups == 0) return 0;
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_fig);
    fg.draws = fig_draw(F, fg.groups, fg.ngroups, 0);
    return (int)fg.draws;
}

/* ── The plain figures' sun shadow map ──────────────────────────────────────
 * Each frame that draws a plain (not traced) figure in light modes 2 to 8,
 * with the sun up, the figures' pieces are drawn again, depth only, into one
 * square map seen from the sun round the eye (fig_shadow_prepare chooses the
 * parts: the draw list's untraced parts whose spheres reach the map, at
 * instances SV_FIG_INST on in the Frame's buffer). The light pass compares
 * each pixel against it (shade.glsl, fig_shadow) and takes the darker of
 * that and the traced shadow. A frame with no plain figure in reach records
 * no pass and the light pass reads no map (u.fshadow.x 0). */
static int fig_shadow_make(void) {
    const VkFormat fmt = VK_FORMAT_D16_UNORM;      /* 4 mm steps over its 256 m of depth */
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
    int linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { SV_FSM_RES, SV_FSM_RES, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    VKOK(vkCreateImage(g.dev, &ici, NULL, &fg.fsm), "figure shadow map");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, fg.fsm, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKOK(vkAllocateMemory(g.dev, &ai, NULL, &fg.fsm_mem), "figure shadow map memory");
    VKOK(vkBindImageMemory(g.dev, fg.fsm, fg.fsm_mem, 0), "figure shadow map bind");
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = fg.fsm, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    VKOK(vkCreateImageView(g.dev, &vi, NULL, &fg.fsm_view), "figure shadow map view");
    /* a comparing sampler: lit where the point is no farther from the sun than the map's depth; past the edges, lit */
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .compareEnable = VK_TRUE, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, .maxAnisotropy = 1.0f };
    VKOK(vkCreateSampler(g.dev, &si, NULL, &fg.fsm_sampler), "figure shadow sampler");
    if (getenv("STRATA_DEBUG")) fprintf(stderr, "strata_vk: figure shadow map %dx%d D16, %s filtering\n", SV_FSM_RES, SV_FSM_RES, linear ? "linear" : "nearest");
    /* its pass: cleared, drawn, left to be sampled; it follows the last frame's reads of it */
    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkAttachmentReference dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &dref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT } };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = dep };
    VKOK(vkCreateRenderPass(g.dev, &rpi, NULL, &fg.fsm_pass), "figure shadow pass");
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = fg.fsm_pass, .attachmentCount = 1,
        .pAttachments = &fg.fsm_view, .width = SV_FSM_RES, .height = SV_FSM_RES, .layers = 1 };
    VKOK(vkCreateFramebuffer(g.dev, &fi, NULL, &fg.fsm_fb), "figure shadow framebuffer");
    /* its pipeline: the figure pieces, depth only, no fragment stage. The faces turned from the
     * sun are drawn (the front faces culled), so a lit face never shadows itself; a little bias more */
    VkShaderModule vs = shader(strata_figure_shadow_vert, sizeof(strata_figure_shadow_vert));
    VkPipelineShaderStageCreateInfo stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_FRONT_BIT, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        .depthBiasEnable = VK_TRUE, .depthBiasConstantFactor = 2.0f, .depthBiasSlopeFactor = 2.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 0 };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 1, .pStages = &stage,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = g.layout, .renderPass = fg.fsm_pass };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, NULL, &fg.fsm_pipe);
    vkDestroyShaderModule(g.dev, vs, NULL);
    if (r != VK_SUCCESS) { die("figure shadow pipeline", (int)r); return 0; }
    fg.fsm_fresh = 1;                     /* the first frame puts it in the layout its descriptor names (fig_shadow_first) */
    return 1;
}
/* Before any pass of the first frame: the map into the layout its
 * descriptor names, for the light pass reads it every frame (if only to
 * pass over it) whether or not a shadow pass has drawn it. */
static void fig_shadow_first(Frame *F) {
    if (!fg.fsm_fresh || !fg.fsm) return;
    fg.fsm_fresh = 0;
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = fg.fsm,
        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void fig_shadow_free(void) {
    if (fg.fsm_pipe) vkDestroyPipeline(g.dev, fg.fsm_pipe, NULL);
    if (fg.fsm_fb) vkDestroyFramebuffer(g.dev, fg.fsm_fb, NULL);
    if (fg.fsm_pass) vkDestroyRenderPass(g.dev, fg.fsm_pass, NULL);
    if (fg.fsm_sampler) vkDestroySampler(g.dev, fg.fsm_sampler, NULL);
    if (fg.fsm_view) vkDestroyImageView(g.dev, fg.fsm_view, NULL);
    if (fg.fsm) vkDestroyImage(g.dev, fg.fsm, NULL);
    if (fg.fsm_mem) vkFreeMemory(g.dev, fg.fsm_mem, NULL);
}
/* The map's view for this frame (into the uniforms) and the parts it holds
 * (into the Frame's instance buffer from SV_FIG_INST, grouped by rig part).
 * Parts fig_trace_prepare placed in the rays' list are left out, their
 * shadows being traced; a figure flagged traced past the cap, with lighting
 * off, or of a part with no shape is plain and in the map. Figures listed
 * only for their shadow (bit 1, out of view) are in it too. */
static void fig_shadow_prepare(Frame *F, int n, const int32_t *rp, const float *xf) {
    const float *sun = g.ubo_map + 16, *eye = g.ubo_map + 28;
    if (fg.fsm_off || !fg.fsm_pipe || g.light_mode < 2 || g.light_mode > 8 || !(sun[1] > 0.01f)) return;
    float sl = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    float s[3] = { sun[0] / sl, sun[1] / sl, sun[2] / sl };
    /* the map's axes: r and v across it, s toward the sun (r x v = s, as a camera's right, up and back) */
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (fabsf(s[1]) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; }
    float r[3] = { up[1] * s[2] - up[2] * s[1], up[2] * s[0] - up[0] * s[2], up[0] * s[1] - up[1] * s[0] };
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int k = 0; k < 3; k++) r[k] /= rl;
    float v[3] = { s[1] * r[2] - s[2] * r[1], s[2] * r[0] - s[0] * r[2], s[0] * r[1] - s[1] * r[0] };
    /* centred on the eye, moved in whole texels so its edges do not crawl as the eye moves */
    const float texel = 2.0f * SV_FSM_HALF / (float)SV_FSM_RES;
    float cx = floorf((eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2]) / texel) * texel;
    float cy = floorf((eye[0] * v[0] + eye[1] * v[1] + eye[2] * v[2]) / texel) * texel;
    float cz = eye[0] * s[0] + eye[1] * s[1] + eye[2] * s[2];
    /* parts whose spheres reach the map's box, untraced */
    int kept = 0;
    fg.ntouched = 0;
    for (int i = 0; i < n; i++) {
        int32_t q = rp[i];
        if (fg.placed[i] || q < 0 || q >= SV_FIG_RP || fg.head[q] < 0) continue;
        if (fg.sphere[q][3] < 0.0f) fig_sphere(q);
        const float *x = xf + (size_t)i * 12, *sp = fg.sphere[q];
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = x[k] * sp[0] + x[3 + k] * sp[1] + x[6 + k] * sp[2] + x[9 + k];
        float lx = c[0] * r[0] + c[1] * r[1] + c[2] * r[2] - cx, ly = c[0] * v[0] + c[1] * v[1] + c[2] * v[2] - cy, lz = c[0] * s[0] + c[1] * s[1] + c[2] * s[2] - cz;
        if (fabsf(lx) > SV_FSM_HALF + sp[3] || fabsf(ly) > SV_FSM_HALF + sp[3] || fabsf(lz) > SV_FSM_DEPTH + sp[3]) continue;
        if (fg.count[q]++ == 0) fg.touched[fg.ntouched++] = q;
        fg.order[kept++] = i;
    }
    if (kept == 0) { fg.ntouched = 0; return; }
    int32_t at = 0;
    for (int t = 0; t < fg.ntouched; t++) {
        int32_t q = fg.touched[t];
        fg.sgroups[t] = (struct FigGroup){ q, at, fg.count[q] };
        fg.base[q] = at; at += fg.count[q]; fg.count[q] = 0;
    }
    fg.nsgroups = fg.ntouched;
    struct FigInst *out = F->finst_map + SV_FIG_INST;
    for (int k = 0; k < kept; k++) {
        int i = fg.order[k]; int32_t q = rp[i];
        struct FigInst *o = &out[fg.base[q] + fg.count[q]++];
        const float *x = xf + (size_t)i * 12;
        o->pos[0] = x[9]; o->pos[1] = x[10]; o->pos[2] = x[11];
        o->flags = 0u;
        fig_mat_q(x, o->rot);
    }
    for (int t = 0; t < fg.ntouched; t++) fg.count[fg.touched[t]] = 0;
    fg.sparts = kept;
    /* world to the map's clip space (OpenGL conventions, column-major): across it by r and v, depth toward the sun */
    float *m = g.ubo_map + UBO_SUN_VP;
    for (int k = 0; k < 3; k++) { m[k * 4 + 0] = r[k] / SV_FSM_HALF; m[k * 4 + 1] = v[k] / SV_FSM_HALF; m[k * 4 + 2] = -s[k] / SV_FSM_DEPTH; m[k * 4 + 3] = 0.0f; }
    m[12] = -cx / SV_FSM_HALF; m[13] = -cy / SV_FSM_HALF; m[14] = cz / SV_FSM_DEPTH; m[15] = 1.0f;
    float *fs = g.ubo_map + UBO_FSHADOW;
    fs[0] = 1.0f; fs[1] = 1.0f / (float)SV_FSM_RES; fs[2] = 0.5f / SV_FSM_DEPTH; fs[3] = 0.0f;
}
/* Draw the map, between the geometry and light passes, when it holds any
 * part. Answers the draws. Timed on the GPU (stat 14) where timestamps are. */
static int fig_shadow_record(Frame *F, uint32_t q0) {
    fg.sdraws = 0;
    F->fsm_ts = 0;
    if (fg.nsgroups == 0) return 0;
    if (g.have_ts) vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 2);
    VkClearValue clear; clear.depthStencil.depth = 1.0f; clear.depthStencil.stencil = 0;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = fg.fsm_pass, .framebuffer = fg.fsm_fb,
        .renderArea = { { 0, 0 }, { SV_FSM_RES, SV_FSM_RES } }, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(F->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt = { 0, 0, (float)SV_FSM_RES, (float)SV_FSM_RES, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { SV_FSM_RES, SV_FSM_RES } };
    vkCmdSetViewport(F->cmd, 0, 1, &vpt); vkCmdSetScissor(F->cmd, 0, 1, &sc);
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fg.fsm_pipe);
    vkCmdBindDescriptorSets(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &F->dset, 0, NULL);
    fg.sdraws = fig_draw(F, fg.sgroups, fg.nsgroups, SV_FIG_INST);
    vkCmdEndRenderPass(F->cmd);
    if (g.have_ts) { vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 3); F->fsm_ts = 1; }
    return (int)fg.sdraws;
}


int64_t sv_open_slots(int64_t w, int64_t h, int64_t capture_mouse, int64_t quad_capacity, int64_t slots) {
    memset(&g, 0, sizeof g);
    if (slots < SV_DEFAULT_SLOTS || slots > 262144) return 0;
    g.slot_cap = (uint32_t)slots;
    const char *batch = getenv("STRATA_BATCH_SHARED");
    g.batch_shared = !batch || batch[0] != '0';
    if (!glfwInit()) { die("glfwInit", 0); return 0; }
    if (!glfwVulkanSupported()) { die("glfwVulkanSupported (is the Vulkan loader installed?)", 0); return 0; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    /* STRATA_PIXELS=1: the size asked for is in pixels, whatever the display's
     * scale, so a scripted run draws the same image on any screen */
    if (getenv("STRATA_PIXELS") && getenv("STRATA_PIXELS")[0] == '1') glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
    /* STRATA_HIDDEN=1: the window is never shown. For scripted runs that draw
     * offscreen (STRATA_OFFSCREEN=1) and must not take over someone's screen. */
    if (getenv("STRATA_HIDDEN") && getenv("STRATA_HIDDEN")[0] == '1') { glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE); }
    g.win = glfwCreateWindow((int)w, (int)h, "Strata", NULL, NULL);
    if (!g.win) { die("glfwCreateWindow", 0); return 0; }
    glfwSetKeyCallback(g.win, on_key);
    glfwSetCharCallback(g.win, on_char);
    glfwSetCursorPosCallback(g.win, on_cursor);
    glfwSetScrollCallback(g.win, on_scroll);
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
      g.have_ts = qf[g.qfam].timestampValidBits != 0; g.ts_period = pp.limits.timestampPeriod; g.max_ssbo = pp.limits.maxStorageBufferRange; }
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
    VkPhysicalDeviceFeatures feats = { .multiDrawIndirect = VK_TRUE, .drawIndirectFirstInstance = VK_TRUE, .samplerAnisotropy = have.samplerAnisotropy,
        .fragmentStoresAndAtomics = have.fragmentStoresAndAtomics };
    g.water_mem = have.fragmentStoresAndAtomics ? 1 : 0;
    VkPhysicalDeviceVulkan11Features feats11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .shaderDrawParameters = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &feats11, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext, .pEnabledFeatures = &feats };
    VKOK(vkCreateDevice(g.phys, &dci, NULL, &g.dev), "vkCreateDevice");
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

    if (!make_swapchain()) return 0;
    if (!make_render_pass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &g.pass)) return 0;
    if (!make_render_pass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &g.pass_off)) return 0;
    if (!make_pass_g() || !make_pass_l(2, &g.pass_t) || !make_pass_l(1, &g.pass_l)) return 0;
    VkSamplerCreateInfo nsi = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VKOK(vkCreateSampler(g.dev, &nsi, NULL, &g.nearest), "nearest sampler");
    if (!make_framebuffers()) return 0;
    g.swap_ok = 1;

    /* one set layout for every pipeline: 0 uniforms, 1 chunk table, 2 indirect commands, 3 cull statistics, 4 command -> slot */
    VkDescriptorSetLayoutBinding dslb[39] = {
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
        { .binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        /* the frame's own images: 7 colour, 8 normal and material, 9 depth, 10 the lit image */
        { .binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 9, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 10, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* the light volume: 11 classes, 12 the coarse copy, 13 cell owners; 14 the class table
         * (these, the bodies and the palette are seen by vertex shaders too: particles lit once each trace there;
         * and, with the tiles and the effect table, by the compute stage: collide.comp walks particles' flights through them) */
        { .binding = 11, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 12, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 13, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 14, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        /* the light pass's memory of the frame before */
        { .binding = 15, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 16, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* bodies, for the rays: table, classes, cell index, cell lists, moving boxes */
        { .binding = 17, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 18, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 19, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 20, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 21, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        /* the 2D layer's interface atlas */
        { .binding = 22, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* and its glyph atlas */
        { .binding = 23, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* figures: 24 the rigs' palettes, 25 this frame's figure parts, 26 the quad buffer read as words */
        { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 25, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
        { .binding = 26, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
        /* 27 the plain figures' sun shadow map, a comparing sampler */
        { .binding = 27, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* the water's memory: last frame's mirror colour and depth, this frame's */
        { .binding = 28, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 29, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 30, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 31, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* lamps near each light-volume cell: per cell (first, count), and the list */
        { .binding = 32, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 33, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* 34 the particles' effect table */
        { .binding = 34, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        /* 35, 36 what this frame's gather pass wrote, for the light pass */
        { .binding = 35, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 36, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        /* 37 the particles' ring, for the pass that looks for what each new particle will meet (collide.comp) */
        { .binding = 37, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        /* 38 the motion records of moving batched slots */
        { .binding = 38, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT } };
    VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 39, .pBindings = dslb };
    VKOK(vkCreateDescriptorSetLayout(g.dev, &dsli, NULL, &g.dsl), "descriptor set layout");
    /* where the call's range starts in draw_slot, and per-call flags (bit 0: cutout, bit 1: a figure);
     * a figure piece's draw adds eight words more (fig_record) */
    /* and, past them, two words for collide.comp: the first ring slot to look at and how many (its own range, so no draw's push need name the compute stage) */
    VkPushConstantRange pcr[2] = { { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 40 },
                                   { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 40, .size = 8 } };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.dsl,
        .pushConstantRangeCount = 2, .pPushConstantRanges = pcr };
    VKOK(vkCreatePipelineLayout(g.dev, &pli, NULL, &g.layout), "pipeline layout");
    if (!make_pipeline(0, &g.pipe_opaque)) return 0;
    if (g.batch_shared && !make_pipeline(11, &g.pipe_batch)) return 0;
    if (!make_pipeline(1, &g.pipe_blend)) return 0;
    if (!make_pipeline(2, &g.pipe_cross)) return 0;
    if (!make_pipeline(3, &g.pipe_loose)) return 0;
    if (!make_pipeline(4, &g.pipe_light)) return 0;
    if (!make_pipeline(10, &g.pipe_gather)) return 0;
    if (!make_pipeline(5, &g.pipe_composite)) return 0;
    if (!make_pipeline(6, &g.pipe_spray)) return 0;
    if (!make_pipeline(9, &g.pipe_glow)) return 0;
    if (!make_pipeline(7, &g.pipe_ui)) return 0;
    if (!make_pipeline(8, &g.pipe_fig)) return 0;
    g.ui_cpu = calloc((size_t)SV_UI_QUADS * SV_UI_FLOATS, sizeof(float));
    if (!g.ui_cpu) return 0;
    g.ui_n = 0; g.ui_dropped = 0;
    {   void *smap = NULL;
        if (!make_buffer((VkDeviceSize)SV_SPRAY_CAP * SV_SPRAY_FLOATS * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.spray, &g.spray_mem, &smap)) return 0;
        g.spray_map = smap; memset(g.spray_map, 0, (size_t)SV_SPRAY_CAP * SV_SPRAY_FLOATS * 4);
        g.spray_births = calloc(SV_SPRAY_CAP, sizeof(float));
        if (!g.spray_births) return 0;
        sp_init(&g.ring, g.spray_births, SV_SPRAY_CAP);
        if (!make_buffer((VkDeviceSize)PT_EFFECTS * PT_EFFECT_FLOATS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.fx, &g.fx_mem, &smap)) return 0;
        g.fx_map = smap; memset(g.fx_map, 0, (size_t)PT_EFFECTS * PT_EFFECT_FLOATS * 4);
        memset(g.fx_set, 0, sizeof g.fx_set);
        pt_effect_spray(g.fx_map); g.fx_set[0] = 1; g.glow_until = -1.0;
        g.meet_pending = 0; g.meet_first = 0; g.meet_last = 0; g.meet_total = 0;
    }

    void *map = NULL;
    g.quad_cap = (quad_capacity > 0 && quad_capacity < 0x20000000) ? (uint32_t)quad_capacity : SV_DEFAULT_QUADS;
    if (!make_buffer((VkDeviceSize)g.quad_cap * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.giga, &g.giga_mem, &map)) return 0;
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
    g.entries = calloc(g.slot_cap, sizeof(ChunkEntry));
    if (!g.entries) return 0;
    VkDeviceSize cmd_bytes = (VkDeviceSize)SV_REGIONS * SV_CMD_STRIDE * 16;
    if (!make_buffer(cmd_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g.cmds, &g.cmds_mem, &map)) return 0;
    memset(map, 0, (size_t)cmd_bytes);            /* the padding commands stay zero-count for good */
    g.cmds_map = map;
    VkDeviceSize dslot_bytes = (VkDeviceSize)(SV_REGIONS + (g.batch_shared ? SV_FRAMES : 0)) * SV_CMD_STRIDE * 4;
    if (!make_buffer(dslot_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.dslot, &g.dslot_mem, &map)) return 0;
    memset(map, 0, (size_t)dslot_bytes);
    g.dslot_map = map;
    if (g.batch_shared) {
        g.batch_slots = malloc(g.slot_cap * sizeof *g.batch_slots);
        g.batch_at = calloc(g.slot_cap, sizeof *g.batch_at);
        g.batch_visible = malloc(g.slot_cap * sizeof *g.batch_visible);
        g.batch_keys = malloc(g.slot_cap * sizeof *g.batch_keys);
        g.batches = calloc(g.slot_cap, sizeof *g.batches);
        g.motion = calloc((size_t)g.slot_cap * SV_MOTION_WORDS, sizeof *g.motion);
        g.motion_box = calloc((size_t)g.slot_cap * 6, sizeof *g.motion_box);
        if (!g.batch_slots || !g.batch_at || !g.batch_visible || !g.batch_keys || !g.batches || !g.motion || !g.motion_box) return 0;
    }
    g.trans = calloc(g.slot_cap, sizeof *g.trans);
    g.trans_at = malloc(g.slot_cap * sizeof *g.trans_at);
    g.trans_keys = calloc(g.slot_cap, sizeof *g.trans_keys);
    g.trans_ord = calloc((size_t)g.slot_cap * 8, sizeof *g.trans_ord);
    if (!g.trans || !g.trans_at || !g.trans_keys || !g.trans_ord) { die("translucent list", 0); return 0; }
    for (int i = 0; i < g.slot_cap; i++) g.trans_at[i] = -1;

    if (!make_buffer((VkDeviceSize)SV_MAX_BLOCKS * 32 + 65536 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.blocks, &g.blocks_mem, &map)) return 0;
    g.blocks_map = map;
    memset(g.blocks_map, 0, (size_t)SV_MAX_BLOCKS * 32);
    for (uint32_t t = 0; t < 65536; t++) ((uint32_t *)g.blocks_map)[SV_TOP_OF + t] = t;
    VkDescriptorPoolSize dps[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SV_FRAMES }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 19 * SV_FRAMES }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 15 * SV_FRAMES },
                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * SV_FRAMES } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SV_FRAMES, .poolSizeCount = 4, .pPoolSizes = dps };
    VKOK(vkCreateDescriptorPool(g.dev, &dpi, NULL, &g.dpool), "descriptor pool");
    for (int f = 0; f < SV_FRAMES; f++) {
        Frame *F = &g.frames[f];
        if (!make_buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->stats, &F->stats_mem, &map)) return 0;
        F->stats_map = map; memset(F->stats_map, 0, 32);
        if (!make_buffer((VkDeviceSize)SV_UI_QUADS * SV_UI_FLOATS * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &F->ui, &F->ui_mem, &map)) return 0;
        F->ui_map = map;
        if (!make_buffer(SV_GLYPH_STAGE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &F->gstage, &F->gstage_mem, &map)) return 0;
        F->gstage_map = map;
        /* the parts drawn, then (from SV_FIG_INST) the parts in the shadow map */
        if (!make_buffer((VkDeviceSize)SV_FIG_INST * 2 * sizeof(struct FigInst), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->finst, &F->finst_mem, &map)) return 0;
        F->finst_map = map; memset(F->finst_map, 0, (size_t)SV_FIG_INST * 2 * sizeof(struct FigInst));
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g.dpool, .descriptorSetCount = 1, .pSetLayouts = &g.dsl };
        VKOK(vkAllocateDescriptorSets(g.dev, &dai, &F->dset), "descriptor set");
        if (!make_buffer(UBO_FLOATS * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &F->ubo, &F->ubo_mem, &map)) return 0;
        F->ubo_map = map; memset(F->ubo_map, 0, UBO_FLOATS * 4);
        if (!make_buffer((VkDeviceSize)g.slot_cap * sizeof(ChunkEntry), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->table, &F->table_mem, &map)) return 0;
        F->table_map = map; memset(F->table_map, 0, (size_t)g.slot_cap * sizeof(ChunkEntry));
        if (!make_buffer((VkDeviceSize)g.slot_cap * SV_MOTION_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->motion, &F->motion_mem, &map)) return 0;
        F->motion_map = map; memset(F->motion_map, 0, (size_t)g.slot_cap * SV_MOTION_WORDS * 4);
        if (!make_buffer((VkDeviceSize)SV_LIGHT_PER_FRAME * SV_LIGHT_WRITE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &F->lstage, &F->lstage_mem, &map)) return 0;
        F->lstage_map = map;
        if (!make_buffer((VkDeviceSize)SV_TABLE_ROWS * sizeof(BodyRow), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->btable, &F->btable_mem, &map)) return 0;
        F->btable_map = map;
        if (!make_buffer((VkDeviceSize)SV_MAX_CELLS * 8 + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->bcells, &F->bcells_mem, &map)) return 0;
        F->bcells_map = map; memset(F->bcells_map, 0, (size_t)SV_MAX_CELLS * 8 + 16);
        if (!make_buffer((VkDeviceSize)SV_BODY_LIST * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->blist, &F->blist_mem, &map)) return 0;
        F->blist_map = map;
        if (!make_buffer((VkDeviceSize)(SV_MOVERS + SV_FIG_BOXES) * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->bmovers, &F->bmovers_mem, &map)) return 0;
        F->bmovers_map = map; memset(F->bmovers_map, 0, (SV_MOVERS + SV_FIG_BOXES) * 32);
        if (!make_buffer((VkDeviceSize)SV_MAX_CELLS * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->lcells, &F->lcells_mem, &map)) return 0;
        F->lcells_map = map; memset(F->lcells_map, 0, (size_t)SV_MAX_CELLS * 8);
        if (!make_buffer((VkDeviceSize)SV_LAMP_LIST * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &F->llist, &F->llist_mem, &map)) return 0;
        F->llist_map = map; F->lamp_version = (uint32_t)-1;
        F->pending = calloc(g.slot_cap, sizeof *F->pending);
        F->is_pending = calloc(g.slot_cap, 1);
        if (!F->pending || !F->is_pending) return 0;
        VkDescriptorBufferInfo bufs[6] = { { F->ubo, 0, UBO_FLOATS * 4 }, { F->table, 0, VK_WHOLE_SIZE }, { g.cmds, 0, VK_WHOLE_SIZE }, { F->stats, 0, 32 }, { g.dslot, 0, VK_WHOLE_SIZE }, { g.blocks, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet wds[6];
        for (uint32_t b = 0; b < 6; b++)
            wds[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = b, .descriptorCount = 1,
                .descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufs[b] };
        vkUpdateDescriptorSets(g.dev, 6, wds, 0, NULL);
        VkDescriptorBufferInfo mb = { F->motion, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet mw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 38, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &mb };
        vkUpdateDescriptorSets(g.dev, 1, &mw, 0, NULL);
    }
    if (!make_buffer((VkDeviceSize)SV_BODY_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.bbytes, &g.bbytes_mem, &map)) return 0;
    g.bbytes_map = map;
    sa_init(&g_bbytes, SV_BODY_WORDS);
    g.bodies = calloc(SV_MAX_BODIES, sizeof(BodyRec));
    if (!g.bodies) return 0;
    for (int f = 0; f < SV_FRAMES; f++) {
        Frame *F = &g.frames[f];
        VkDescriptorBufferInfo bb[5] = { { F->btable, 0, VK_WHOLE_SIZE }, { g.bbytes, 0, VK_WHOLE_SIZE }, { F->bcells, 0, VK_WHOLE_SIZE }, { F->blist, 0, VK_WHOLE_SIZE }, { F->bmovers, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet bw[5];
        for (uint32_t b = 0; b < 5; b++)
            bw[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 17 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bb[b] };
        vkUpdateDescriptorSets(g.dev, 5, bw, 0, NULL);
        VkDescriptorBufferInfo lb2[3] = { { F->lcells, 0, VK_WHOLE_SIZE }, { F->llist, 0, VK_WHOLE_SIZE }, { g.fx, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet lw2[3];
        for (uint32_t b = 0; b < 3; b++)
            lw2[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 32 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &lb2[b] };
        vkUpdateDescriptorSets(g.dev, 3, lw2, 0, NULL);
        VkDescriptorBufferInfo rb = { g.spray, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet rw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 37, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &rb };
        vkUpdateDescriptorSets(g.dev, 1, &rw, 0, NULL);
    }
    if (!fig_init()) { die("figures", 0); return 0; }
    for (int f = 0; f < SV_FRAMES; f++) {
        Frame *F = &g.frames[f];
        /* the quad buffer as words: no more of it than one storage descriptor may reach (a figure piece past that is not drawn right) */
        VkDeviceSize qrange = (VkDeviceSize)g.quad_cap * 8;
        if (qrange > g.max_ssbo) qrange = g.max_ssbo;
        VkDescriptorBufferInfo fb[3] = { { fg.pal, 0, VK_WHOLE_SIZE }, { F->finst, 0, VK_WHOLE_SIZE }, { g.giga, 0, qrange } };
        VkWriteDescriptorSet fw[4];
        for (uint32_t b = 0; b < 3; b++)
            fw[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 24 + b, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &fb[b] };
        VkDescriptorImageInfo si = { fg.fsm_sampler, fg.fsm_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        fw[3] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F->dset, .dstBinding = 27, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si };
        vkUpdateDescriptorSets(g.dev, 4, fw, 0, NULL);
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
        /* and the particles' collision pipeline */
        cs = shader(strata_collide_comp, sizeof(strata_collide_comp));
        ci.stage.module = cs;
        cr = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g.pipe_collide);
        vkDestroyShaderModule(g.dev, cs, NULL);
        if (cr != VK_SUCCESS) { die("vkCreateComputePipelines (collide)", (int)cr); return 0; }
    }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g.qfam };
    VKOK(vkCreateCommandPool(g.dev, &cpi, NULL, &g.cpool), "command pool");
    {   static const unsigned char white[4] = { 255, 255, 255, 255 };   /* until a game supplies interface art */
        if (!ui_art_make(1, 1, white)) return 0;
        if (!glyphs_make()) return 0;
    }
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
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8 * SV_FRAMES };
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
        if (g.pipe_batch) vkDestroyPipeline(g.dev, g.pipe_batch, NULL);
        if (g.pipe_blend) vkDestroyPipeline(g.dev, g.pipe_blend, NULL);
        if (g.pipe_cull) vkDestroyPipeline(g.dev, g.pipe_cull, NULL);
        if (g.pipe_collide) vkDestroyPipeline(g.dev, g.pipe_collide, NULL);
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
        if (g.pipe_gather) vkDestroyPipeline(g.dev, g.pipe_gather, NULL);
        if (g.pipe_composite) vkDestroyPipeline(g.dev, g.pipe_composite, NULL);
        if (g.pipe_ui) vkDestroyPipeline(g.dev, g.pipe_ui, NULL);
        if (g.pipe_fig) vkDestroyPipeline(g.dev, g.pipe_fig, NULL);
        fig_free();
        for (int f = 0; f < SV_FRAMES; f++) {
            if (g.frames[f].finst) vkDestroyBuffer(g.dev, g.frames[f].finst, NULL);
            if (g.frames[f].finst_mem) vkFreeMemory(g.dev, g.frames[f].finst_mem, NULL);
            if (g.frames[f].ui) vkDestroyBuffer(g.dev, g.frames[f].ui, NULL);
            if (g.frames[f].ui_mem) vkFreeMemory(g.dev, g.frames[f].ui_mem, NULL);
            if (g.frames[f].gstage) vkDestroyBuffer(g.dev, g.frames[f].gstage, NULL);
            if (g.frames[f].gstage_mem) vkFreeMemory(g.dev, g.frames[f].gstage_mem, NULL);
        }
        drop_target(&g.ui_art); drop_target(&g.glyphs);
        if (g.glyph_sampler) vkDestroySampler(g.dev, g.glyph_sampler, NULL);
        if (g.ui_sampler) vkDestroySampler(g.dev, g.ui_sampler, NULL);
        if (g.pipe_spray) vkDestroyPipeline(g.dev, g.pipe_spray, NULL);
        if (g.pipe_glow) vkDestroyPipeline(g.dev, g.pipe_glow, NULL);
        if (g.fx) vkDestroyBuffer(g.dev, g.fx, NULL);
        if (g.fx_mem) vkFreeMemory(g.dev, g.fx_mem, NULL);
        if (g.spray) vkDestroyBuffer(g.dev, g.spray, NULL);
        if (g.spray_mem) vkFreeMemory(g.dev, g.spray_mem, NULL);
        if (g.pass_g) vkDestroyRenderPass(g.dev, g.pass_g, NULL);
        if (g.pass_l) vkDestroyRenderPass(g.dev, g.pass_l, NULL);
        if (g.pass_t) vkDestroyRenderPass(g.dev, g.pass_t, NULL);
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
            if (g.frames[f].motion) vkDestroyBuffer(g.dev, g.frames[f].motion, NULL);
            if (g.frames[f].motion_mem) vkFreeMemory(g.dev, g.frames[f].motion_mem, NULL);
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
    free(g.trans); free(g.trans_at); free(g.trans_keys); free(g.trans_ord);
    free(g.batch_slots); free(g.batch_at); free(g.motion); free(g.motion_box); g.motion = NULL; g.motion_box = NULL; free(g.batch_visible); free(g.batch_keys); free(g.batches);
    free(g.spray_births); g.spray_births = NULL;
    free(g.ui_cpu); g.ui_cpu = NULL; g.ui_n = 0;
    free(g.ubo_map); free(g.entries); if (g.cells) for (int i = 0; i < g.vn_old_cells; i++) free(g.cells[i].lamps); free(g.cells); g.vn_old_cells = 0; free(g.cell_page); free(g.free_pages); free(g.stage_light);
    for (uint32_t i = 0; i < g.nlq; i++) free(g.lq[i].bytes);
    free(g.lq);
    for (int i = 0; i < 4; i++) free(g.stage.part[i]);
    memset(&g, 0, sizeof g);
}

/* ── Window and input queries ─────────────────────────────────────────────── */
int64_t sv_should_close(void) { return (!g.win || glfwWindowShouldClose(g.win)) ? 1 : 0; }
void sv_poll(void) { g.mdx = 0; g.mdy = 0; g.scroll_y = 0; g.ntyped = 0; if (g.win) { glfwPollEvents(); pads_read(); } }
/* Typing during the latest poll, in order: sv_typed(i) is a Unicode code point, or minus a GLFW key code (256 up) pressed or repeated. */
int64_t sv_typed_count(void) { return g.ntyped; }
int64_t sv_typed(int64_t i) { return (i >= 0 && i < g.ntyped) ? g.typed[i] : 0; }
/* Gamepad p (0..15) as of the latest poll. Buttons 0..14 in GLFW's order (0 south, 1 east, 2 west, 3 north, 4 and 5 the
 * bumpers, 6 back, 7 start, 8 guide, 9 and 10 the stick clicks, 11..14 the d-pad up, right, down, left). Axes 0 and 1 the
 * left stick's x and y, 2 and 3 the right stick's, -1..1 with y positive downward; 4 and 5 the left and right triggers, 0..1. */
int64_t sv_pad_connected(int64_t p) { return (p >= 0 && p < SV_MAX_PADS) ? g.pad[p].on : 0; }
int64_t sv_pad_button(int64_t p, int64_t b) { return (p >= 0 && p < SV_MAX_PADS && b >= 0 && b < 15) ? g.pad[p].button[b] : 0; }
double sv_pad_axis(int64_t p, int64_t a) { return (p >= 0 && p < SV_MAX_PADS && a >= 0 && a < 6) ? (double)g.pad[p].axis[a] : 0.0; }
int64_t sv_key(int64_t k) { return (k >= 0 && k < SV_MAX_KEYS) ? g.keys[k] : 0; }
/* Is mouse button b (0 left, 1 right, 2 middle) held? */
int64_t sv_open(int64_t w, int64_t h, int64_t capture_mouse, int64_t quad_capacity) {
    return sv_open_slots(w, h, capture_mouse, quad_capacity, SV_DEFAULT_SLOTS);
}
int64_t sv_mouse_button(int64_t b) { return (g.win && b >= 0 && b <= 7 && glfwGetMouseButton(g.win, (int)b) == GLFW_PRESS) ? 1 : 0; }
/* Capture the cursor (hidden and locked, for looking) or free it (shown, for pointing at an interface). The
 * motion delta restarts so the switch is not read as a jump. */
void sv_set_cursor(int64_t captured) {
    if (!g.win) return;
    glfwSetInputMode(g.win, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    g.have_mouse = 0; g.mdx = 0; g.mdy = 0;
}
double sv_mouse_x(void) { if (g.win && g.have_mouse) return g.mx; double x = 0, y = 0; if (g.win) glfwGetCursorPos(g.win, &x, &y); return x; }
double sv_mouse_y(void) { if (g.win && g.have_mouse) return g.my; double x = 0, y = 0; if (g.win) glfwGetCursorPos(g.win, &x, &y); return y; }
double sv_scroll_y(void) { return g.scroll_y; }
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
 * elements lost because an allocator's list was full. 8 spray particles
 * live, 9 2D quads dropped. Figures, of the last frame: 10 parts drawn
 * (after culling), 11 figure draws issued, 12 microseconds of CPU preparing
 * them (the draw list, culling, grouping), 13 microseconds of that in the
 * draw list alone (an_x_draw_list: the slerps), 14 GPU milliseconds of the
 * figure shadow map's pass (0 when it did not run or there are no
 * timestamps), 15 parts drawn into it, 16 its draws, 17 traced figures in
 * the rays' list, 18 their parts placed there, 19 figures flagged traced
 * past the first SV_FIG_TRACED, drawn plain. Since the start: 20 the worst
 * of stat 12, 21 the worst of stat 14 (what figures cost a run with none).
 * The light volume: 22 pages in use, 23 chunks that found no page, 24 bytes
 * its pool holds (they were 10 to 12, which the figures' stats already used).
 * 25 particles written over a slot still in the live span, since the start
 * (a full ring). 33 ring slots the last frame's collision pass was given (0:
 * it did not run), 34 particles born of effects that meet the world, since
 * the start. (26 to 29 are the GPU's milliseconds by pass, 30 to 32 the mesh
 * batches'.) */
double sv_stat(int64_t which) {
    if (which == 30) return (double)g.batch_visible_count;
    if (which == 31) return (double)g.batch_draws;
    if (which == 32) return (double)g.opaque_commands;
    if (which == 6) return (double)g.rebuilds;
    if (which == 22) return (double)(g.npages - g.nfree_pages);   /* light pages in use */
    if (which == 23) return (double)g.pages_refused;               /* chunks that found no light page */
    if (which == 24) return (double)g.npages * 32768.0;          /* bytes the light volume's pool holds */
    if (which == 7) return (double)g_quads.leaked + (double)g_loose.leaked;
    if (which == 8) return g.spray_births ? (double)sp_count(&g.ring) : 0.0;
    if (which == 9) return (double)g.ui_dropped;
    if (which == 10) return (double)fg.parts;
    if (which == 11) return (double)fg.draws;
    if (which == 12) return fg.cpu_us;
    if (which == 13) return fg.list_us;
    if (which == 14) return fg.shadow_ms;
    if (which == 15) return (double)fg.sparts;
    if (which == 16) return (double)fg.sdraws;
    if (which == 17) return (double)fg.traced_figs;
    if (which == 18) return (double)fg.traced_parts;
    if (which == 19) return (double)fg.traced_over;
    if (which == 20) return fg.cpu_worst;
    if (which == 21) return fg.shadow_worst;
    if (which >= 26 && which <= 29) return g.pass_ms[which - 26];
    if (which == 25) return g.spray_births ? (double)g.ring.overwritten : 0.0;
    if (which == 33) return (double)g.meet_last;
    if (which == 34) return (double)g.meet_total;
    if (which >= 35 && which <= 38) return (double)g.churn_last[which - 35];
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
static void qrot3(const float *q, const float *v, float *out) {
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
/* A body part's turned box of [side] metres: out of view? [mid] gets its centre. */
static int culled_body(const float *m, const ChunkEntry *e, float side, float *mid) {
    float c[8][4];
    for (int i = 0; i < 8; i++) {
        float v[3] = { (i & 1) ? side : 0.0f, (i & 2) ? side : 0.0f, (i & 4) ? side : 0.0f }, w[3];
        qrot3(e->rot, v, w);
        float x = e->pos[0] + w[0], y = e->pos[1] + w[1], z = e->pos[2] + w[2];
        for (int r = 0; r < 4; r++) c[i][r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
    }
    {   float h[3] = { side * 0.5f, side * 0.5f, side * 0.5f }, w[3];
        qrot3(e->rot, h, w);
        mid[0] = e->pos[0] + w[0]; mid[1] = e->pos[1] + w[1]; mid[2] = e->pos[2] + w[2]; }
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
        if (e->level_flags & SV_HIDDEN) continue;
        float side = (float)(32 << (e->level_flags & 255));
        float cx, cy, cz;
        if (e->level_flags & SV_BODY) {
            float c[3];
            if (culled_body(g.ubo_map, e, 32.0f * scale, c)) continue;
            cx = c[0] - eye[0]; cy = c[1] - eye[1]; cz = c[2] - eye[2];
        } else {
            if (culled(g.ubo_map, e->origin, side, scale)) continue;
            cx = ((float)e->origin[0] + side * 0.5f) * scale - eye[0];
            cy = ((float)e->origin[1] + side * 0.5f) * scale - eye[1];
            cz = ((float)e->origin[2] + side * 0.5f) * scale - eye[2];
        }
        g.trans_keys[n].dist = cx * cx + cy * cy + cz * cz;
        g.trans_keys[n].slot = g.trans[i];
        n++;
    }
    qsort(g.trans_keys, n, sizeof *g.trans_keys, far_first);
    uint32_t *cmd = g.cmds_map + (size_t)region * SV_CMD_STRIDE * 4;
    uint32_t *ds = g.dslot_map + (size_t)region * SV_CMD_STRIDE;
    uint32_t nc = 0, room = (uint32_t)g.slot_cap;
    for (uint32_t i = 0; i < n && nc < room; i++) {
        uint32_t slot = g.trans_keys[i].slot;
        const ChunkEntry *e = &g.entries[slot];
        const uint16_t *ord = g.trans_ord + (size_t)slot * 8;
        if (!(ord[0] | ord[1] | ord[2] | ord[3] | ord[4] | ord[5] | ord[6] | ord[7])) {
            /* quads that cannot cover one another: one list, in any order */
            cmd[nc * 4 + 0] = 4; cmd[nc * 4 + 1] = e->count[2]; cmd[nc * 4 + 2] = 0; cmd[nc * 4 + 3] = e->first[2];
            ds[nc++] = slot;
            g.drawn_quads += e->count[2];
            continue;
        }
        /* The chunk carries eight lists, one for each way a ray can run
         * (octant o: down x with bit 0 set, down y with bit 1, down z with
         * bit 2), each sorted far to near for such rays. A list is drawn
         * where rays from the eye run its way, which the shaders clip to;
         * it is left out when no such ray meets the chunk's box. The eye is
         * put in the chunk's own voxels for that. */
        float le[3], box = 32.0f;
        if (e->level_flags & SV_BODY) {
            float d[3] = { eye[0] - e->pos[0], eye[1] - e->pos[1], eye[2] - e->pos[2] }, inv[4] = { -e->rot[0], -e->rot[1], -e->rot[2], e->rot[3] };
            float vs = e->pos[3] > 0.0f ? e->pos[3] : scale;
            qrot3(inv, d, le);
            le[0] /= vs; le[1] /= vs; le[2] /= vs;
        } else {
            float lod = (float)(1 << (e->level_flags & 255));
            for (int k = 0; k < 3; k++) le[k] = (eye[k] / scale - (float)e->origin[k]) / lod;
        }
        uint32_t first = e->first[2];
        for (uint32_t o = 0; o < 8; first += ord[o], o++) {
            if (!ord[o] || nc >= room) continue;
            int meets = 1;
            for (int k = 0; k < 3; k++) if ((o >> k) & 1 ? le[k] <= 0.0f : le[k] >= box) meets = 0;
            if (!meets) continue;
            cmd[nc * 4 + 0] = 4; cmd[nc * 4 + 1] = ord[o]; cmd[nc * 4 + 2] = 0; cmd[nc * 4 + 3] = first;
            ds[nc++] = slot | ((8u | o) << 28);
            g.drawn_quads += ord[o];
        }
    }
    n = nc;
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
 *     sv_chunk_begin(slot, ox, oy, oz, level)
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
 * GPU: ranges the chunk held before are reused two frames later.
 *
 * (ox, oy, oz) is the low corner in voxels of level 0. A level-L chunk
 * (Part F's far field, L 0..30) is drawn with each voxel 2^L voxels of
 * level 0 on a side; its textures repeat per level-0 voxel. Above level 0 a
 * chunk never enters the light volume, and the light pass lights it by the
 * sun and sky alone: its rays would start inside the finer world. */
int64_t sv_chunk_begin(int64_t slot, double ox, double oy, double oz, int64_t level) {
    g.stage.open = 0;
    if (!g.ok || slot < 0 || slot >= g.slot_cap || level < 0 || level > 30 ||
        (g.entries[slot].level_flags & SV_TEMPLATE)) return 0;
    g.stage.level = (int)level;
    g.stage.slot = slot;
    g.stage.origin[0] = (int32_t)ox; g.stage.origin[1] = (int32_t)oy; g.stage.origin[2] = (int32_t)oz;
    for (int i = 0; i < 4; i++) g.stage.n[i] = 0;
    memset(g.stage.ord, 0, sizeof g.stage.ord);
    g.stage.open = 1;
    g.stage.body = 0;
    g.stage.fig = 0;
    g.stage_light_n = 0;
    return 1;
}

/* ── Bodies for the rays ─────────────────────────────────────────────────── */
/* Give body [handle] its shape: one light class byte a voxel, x fastest, then
 * y, then z, [sx] x [sy] x [sz]. Replaces any shape it had. */
int64_t sv_body_shape(int64_t handle, void *bytes, int64_t n, int64_t sx, int64_t sy, int64_t sz) {
    if (!g.ok || !g.bodies || handle < 0 || handle >= SV_MAX_BODIES || !bytes ||
        sx < 1 || sy < 1 || sz < 1 || n < 1 || n > narr_len(bytes) || sx > n / sy / sz) return 0;
    BodyRec *b = &g.bodies[handle];
    uint32_t words = 0, first = 0;
    if (!put_shape(narr_data(bytes), sx, sy, sz, &first, &words)) { g.refused++; return 0; }
    if (b->shaped && !b->shared) sa_release(&g_bbytes, (SaRange){ b->first, b->nwords }, g.frame_no);
    b->first = first; b->nwords = words; b->dims[0] = (int32_t)sx; b->dims[1] = (int32_t)sy; b->dims[2] = (int32_t)sz;
    b->shaped = 1; b->shared = 0;
    return 1;
}
/* Give body [handle] the shape body [from] has, without a copy: the class
 * bytes are [from]'s, which must keep its shape for as long as any body
 * shares it (Strata.Render shares only prototypes, which it never drops). */
int64_t sv_body_share(int64_t handle, int64_t from) {
    if (!g.ok || !g.bodies || handle < 0 || handle >= SV_MAX_BODIES || from < 0 || from >= SV_MAX_BODIES || handle == from) return 0;
    BodyRec *b = &g.bodies[handle], *src = &g.bodies[from];
    if (!src->shaped || src->shared) return 0;
    if (b->shaped && !b->shared) sa_release(&g_bbytes, (SaRange){ b->first, b->nwords }, g.frame_no);
    b->first = src->first; b->nwords = src->nwords; memcpy(b->dims, src->dims, sizeof b->dims);
    b->shaped = 1; b->shared = 1;
    return 1;
}
/* Where body [handle] is: its grid's low corner in metres and its rotation. */
void sv_body_place(int64_t handle, double px, double py, double pz, double qx, double qy, double qz, double qw) {
    if (!g.ok || !g.bodies || handle < 0 || handle >= SV_MAX_BODIES) return;
    BodyRec *b = &g.bodies[handle];
    b->pos[0] = (float)px; b->pos[1] = (float)py; b->pos[2] = (float)pz;
    b->rot[0] = (float)qx; b->rot[1] = (float)qy; b->rot[2] = (float)qz; b->rot[3] = (float)qw;
    if (!b->placed) { memcpy(b->seen_pos, b->pos, sizeof b->pos); memcpy(b->seen_rot, b->rot, sizeof b->rot); }
    b->placed = 1;
}
/* Body [handle]'s voxels are [metres] across; 0 for the world's. */
void sv_body_voxel(int64_t handle, double metres) {
    if (!g.ok || !g.bodies || handle < 0 || handle >= SV_MAX_BODIES) return;
    g.bodies[handle].vs = metres > 0.0 ? (float)metres : 0.0f;
}
/* Body [handle] is gone: the rays no longer see it. */
void sv_body_drop(int64_t handle) {
    if (!g.ok || !g.bodies || handle < 0 || handle >= SV_MAX_BODIES) return;
    BodyRec *b = &g.bodies[handle];
    if (b->shaped && !b->shared) sa_release(&g_bbytes, (SaRange){ b->first, b->nwords }, g.frame_no);
    memset(b, 0, sizeof *b);
}

/* This frame's body table, cell lists and moving boxes. The cells are the
 * light volume's, one a chunk, addressed as the shader addresses the volume
 * (chunk coordinate modulo its size): a ray that enters a chunk reads which
 * bodies overlap it. A body far away may share a cell with a near chunk; the
 * ray's own test of the body's box turns it away. */
static void bodies_to_frame(Frame *F) {
    int32_t *bu = (int32_t *)(g.ubo_map + UBO_BODIES);
    bu[0] = bu[1] = bu[2] = bu[3] = 0;
    if (!g.bodies || g.vn <= 0 || g.vnv <= 0) return;
    float scale = g.ubo_map[31] > 0.0f ? g.ubo_map[31] : 1.0f;
    int nx = g.vn, ny = g.vnv, nz = g.vn;
    uint32_t ncells = (uint32_t)(nx * ny * nz);
    if (ncells > SV_MAX_CELLS) return;
    memset(F->bcells_map, 0, (size_t)ncells * 8);
    /* the table, each body's box in chunks, and how many cells each lists it in */
    static int32_t box[SV_MAX_BODIES + SV_FIG_TRACED][6];
    static uint32_t which[SV_MAX_BODIES];
    uint32_t n = 0, nmov = 0;
    for (uint32_t h = 0; h < SV_MAX_BODIES; h++) {
        BodyRec *b = &g.bodies[h];
        if (!b->shaped || !b->placed) continue;
        BodyRow *r = &F->btable_map[n];
        memcpy(r->pos, b->pos, 12); r->pos[3] = b->vs; memcpy(r->rot, b->rot, 16);
        float bs = b->vs > 0.0f ? b->vs : scale;   /* metres a voxel of its grid */
        r->dims[0] = b->dims[0]; r->dims[1] = b->dims[1]; r->dims[2] = b->dims[2]; r->dims[3] = (int32_t)b->first;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (int c = 0; c < 8; c++) {
            float v[3] = { (c & 1) ? b->dims[0] * bs : 0.0f, (c & 2) ? b->dims[1] * bs : 0.0f, (c & 4) ? b->dims[2] * bs : 0.0f }, w[3];
            qrot3(b->rot, v, w);
            for (int k = 0; k < 3; k++) { float x = (b->pos[k] + w[k]) / scale; if (x < lo[k]) lo[k] = x; if (x > hi[k]) hi[k] = x; }
        }
        for (int k = 0; k < 3; k++) { box[n][k] = (int32_t)floorf(lo[k]) >> 5; box[n][3 + k] = (int32_t)floorf(hi[k]) >> 5; }
        {   /* a sphere round it, in voxels: most rays turn away on it, before any rotation */
            float half[3] = { b->dims[0] * bs * 0.5f, b->dims[1] * bs * 0.5f, b->dims[2] * bs * 0.5f }, w[3];
            qrot3(b->rot, half, w);
            for (int k = 0; k < 3; k++) r->sphere[k] = (b->pos[k] + w[k]) / scale;
            r->sphere[3] = 0.5f * sqrtf((float)(b->dims[0] * b->dims[0] + b->dims[1] * b->dims[1] + b->dims[2] * b->dims[2])) * (bs / scale) + 0.01f;
        }
        /* a body that moved since last frame unsettles light history round it, its shadow included */
        int moved = memcmp(b->pos, b->seen_pos, 12) != 0 || memcmp(b->rot, b->seen_rot, 16) != 0;
        if (moved && nmov < SV_MOVERS) {
            float reach = 0.0f;
            for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > reach) reach = hi[k] - lo[k];
            reach = reach * 2.0f + 8.0f;
            float *m = F->bmovers_map + nmov * 8;
            for (int k = 0; k < 3; k++) { m[k] = lo[k] - reach; m[4 + k] = hi[k] + reach; }
            m[3] = m[7] = 0.0f;
            nmov++;
        }
        memcpy(b->seen_pos, b->pos, 12); memcpy(b->seen_rot, b->rot, 16);
        which[n] = h;
        n++;
    }
    /* then the traced figures (fig_trace_prepare): each one's group row is listed in the cells its
     * sphere reaches, its parts' rows follow the groups; and their boxes after the bodies' */
    uint32_t nlisted = n;
    for (int32_t t = 0; t < fg.tgroups; t++) {
        BodyRow *r = &F->btable_map[n + (uint32_t)t];
        *r = fg.trows[t];
        r->dims[0] += (int32_t)n + fg.tgroups;
        for (int k = 0; k < 3; k++) { box[nlisted][k] = (int32_t)floorf(r->sphere[k] - r->sphere[3]) >> 5; box[nlisted][3 + k] = (int32_t)floorf(r->sphere[k] + r->sphere[3]) >> 5; }
        nlisted++;
    }
    if (fg.tparts > 0) memcpy(F->btable_map + n + fg.tgroups, fg.trows + SV_FIG_TRACED, (size_t)fg.tparts * sizeof(BodyRow));
    for (int32_t t = 0; t < fg.ntmov; t++) { memcpy(F->bmovers_map + nmov * 8, fg.tmov[t], 32); nmov++; }
    /* count, then place: cell c lists its bodies (and traced figures) from its start */
    for (uint32_t i = 0; i < nlisted; i++)
        for (int cz = box[i][2]; cz <= box[i][5]; cz++) for (int cy = box[i][1]; cy <= box[i][4]; cy++) for (int cx = box[i][0]; cx <= box[i][3]; cx++) {
            uint32_t c = (uint32_t)(wrapi(cx, nx) + nx * (wrapi(cy, ny) + ny * wrapi(cz, nz)));
            F->bcells_map[c * 2 + 1]++;
        }
    uint32_t at = 0;
    for (uint32_t c = 0; c < ncells; c++) { F->bcells_map[c * 2] = at; at += F->bcells_map[c * 2 + 1]; F->bcells_map[c * 2 + 1] = 0; }
    for (uint32_t i = 0; i < nlisted; i++)
        for (int cz = box[i][2]; cz <= box[i][5]; cz++) for (int cy = box[i][1]; cy <= box[i][4]; cy++) for (int cx = box[i][0]; cx <= box[i][3]; cx++) {
            uint32_t c = (uint32_t)(wrapi(cx, nx) + nx * (wrapi(cy, ny) + ny * wrapi(cz, nz)));
            uint32_t k = F->bcells_map[c * 2] + F->bcells_map[c * 2 + 1];
            if (k < SV_BODY_LIST) { F->blist_map[k] = i; F->bcells_map[c * 2 + 1]++; }
        }
    (void)which;
    bu[0] = (int32_t)nmov; bu[1] = (int32_t)(n + (uint32_t)fg.tgroups + (uint32_t)fg.tparts);
}

/* Keep the last completed set as well as the current set: vehicles and
 * pedestrians update at different points in the frame. Coordinates are voxels.
 *
 * Broad phase: a footprint sets its bit in every cell of a hashed 16-voxel grid
 * that its bounds (widened by a voxel) overlap, and a query tests only the
 * footprints in the cells its segment's bounds overlap. A footprint can only
 * block a segment that meets its bounds, and hash collisions only add
 * candidates, so a query answers exactly what testing every footprint would.
 * A footprint whose bounds cannot be gridded (not finite, a direction too
 * short, or too large) is tested by every query; a query whose bounds cannot
 * be gridded tests every footprint. */
enum { SV_FOOTPRINTS = 512, SV_FOOTPRINT_CELLS = 4096, SV_FOOTPRINT_WORDS = SV_FOOTPRINTS / 64,
       SV_FOOTPRINT_SPAN = 8 };
static struct Footprint {
    int64_t slot;
    double x, y, z, dx, dz, half_length, half_width;
} footprints[2][SV_FOOTPRINTS];
static unsigned footprint_count[2], footprint_frame;
static uint64_t footprint_grid[2][SV_FOOTPRINT_CELLS][SV_FOOTPRINT_WORDS];
static uint64_t footprint_wide[2][SV_FOOTPRINT_WORDS];

/* The cells [*first, *last] spanning [lo, hi] on one axis, or 0 when there are too many. */
static int footprint_cells(double lo, double hi, int *first, int *last) {
    if (!(lo >= -1e9 && hi <= 1e9 && hi - lo <= 16.0 * (SV_FOOTPRINT_SPAN - 1))) return 0;
    *first = (int)floor(lo / 16.0);
    *last = (int)floor(hi / 16.0);
    return 1;
}
static unsigned footprint_hash(int x, int z) {
    return ((unsigned)x * 73856093u ^ (unsigned)z * 19349663u) & (SV_FOOTPRINT_CELLS - 1);
}

void sv_footprints_begin(void) {
    footprint_frame ^= 1;
    footprint_count[footprint_frame] = 0;
    memset(footprint_grid[footprint_frame], 0, sizeof footprint_grid[0]);
    memset(footprint_wide[footprint_frame], 0, sizeof footprint_wide[0]);
}

void sv_footprint(int64_t slot, double x, double y, double z, double dx, double dz, double length, double width) {
    unsigned index = footprint_count[footprint_frame];
    if (index >= SV_FOOTPRINTS) return;
    struct Footprint v = {slot, x, y, z, dx, dz, length * 0.5 + 0.8, width * 0.5 + 0.8};
    footprints[footprint_frame][index] = v;
    footprint_count[footprint_frame] = index + 1;
    uint64_t bit = UINT64_C(1) << (index % 64);
    /* Inside the footprint |p.d| <= half_length and |p x d| <= half_width, and
     * p = ((p.d) d + (p x d) d_perp) / |d|^2, which bounds each coordinate of p. */
    double n = dx * dx + dz * dz;
    double hl = fabs(v.half_length), hw = fabs(v.half_width);
    double rx = (hl * fabs(dx) + hw * fabs(dz)) / n + 1.0;
    double rz = (hl * fabs(dz) + hw * fabs(dx)) / n + 1.0;
    int x0, x1, z0, z1;
    if (!(n >= 0.25) || !footprint_cells(x - rx, x + rx, &x0, &x1) || !footprint_cells(z - rz, z + rz, &z0, &z1)) {
        footprint_wide[footprint_frame][index / 64] |= bit;
        return;
    }
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++)
            footprint_grid[footprint_frame][footprint_hash(cx, cz)][index / 64] |= bit;
}

static int footprint_axis(double start, double end, double half, double *first, double *last) {
    double motion = end - start;
    if (fabs(motion) < 1e-6) return fabs(start) <= half;
    double a = (-half - start) / motion;
    double b = (half - start) / motion;
    *first = fmax(*first, fmin(a, b));
    *last = fmin(*last, fmax(a, b));
    return *first <= *last;
}

static int footprint_hit(const struct Footprint *v, double x0, double y0, double z0,
                         double x1, double y1, double z1, int64_t ignore) {
    if (v->slot == ignore || v->slot < 0 || v->slot >= g.slot_cap ||
        !(g.entries[v->slot].level_flags & SV_IN_USE)) return 0;
    if (fmin(y0, y1) > v->y + 6.0 || fmax(y0, y1) < v->y - 4.0) return 0;
    double ax = x0 - v->x, az = z0 - v->z, bx = x1 - v->x, bz = z1 - v->z;
    double a = ax * v->dx + az * v->dz, b = bx * v->dx + bz * v->dz;
    double side_a = az * v->dx - ax * v->dz, side_b = bz * v->dx - bx * v->dz;
    /* A pedestrian already clipped by a moving vehicle can step clear. */
    if (fabs(a) <= v->half_length && fabs(side_a) <= v->half_width &&
        bx * bx + bz * bz > ax * ax + az * az) return 0;
    double first = 0.0, last = 1.0;
    return footprint_axis(a, b, v->half_length, &first, &last) &&
        footprint_axis(side_a, side_b, v->half_width, &first, &last);
}

int64_t sv_footprint_blocked(double x0, double y0, double z0, double x1, double y1, double z1, int64_t ignore) {
    if (!g.ok) return 0;
    int cx0, cx1, cz0, cz1;
    int gridded = footprint_cells(fmin(x0, x1), fmax(x0, x1), &cx0, &cx1) &&
        footprint_cells(fmin(z0, z1), fmax(z0, z1), &cz0, &cz1);
    for (unsigned frame = 0; frame < 2; frame++) {
        if (!gridded) {
            for (unsigned i = 0; i < footprint_count[frame]; i++)
                if (footprint_hit(&footprints[frame][i], x0, y0, z0, x1, y1, z1, ignore)) return 1;
            continue;
        }
        uint64_t selected[SV_FOOTPRINT_WORDS];
        memcpy(selected, footprint_wide[frame], sizeof selected);
        for (int cz = cz0; cz <= cz1; cz++)
            for (int cx = cx0; cx <= cx1; cx++) {
                const uint64_t *cell = footprint_grid[frame][footprint_hash(cx, cz)];
                for (unsigned w = 0; w < SV_FOOTPRINT_WORDS; w++) selected[w] |= cell[w];
            }
        for (unsigned w = 0; w < SV_FOOTPRINT_WORDS; w++)
            for (uint64_t bits = selected[w]; bits; bits &= bits - 1) {
                unsigned i = w * 64 + (unsigned)__builtin_ctzll(bits);
                if (footprint_hit(&footprints[frame][i], x0, y0, z0, x1, y1, z1, ignore)) return 1;
            }
    }
    return 0;
}

/* A frozen copy of both footprint frames, for a simulation step running on a worker
 * thread while the main thread renders. sv_footprints_freeze copies the live sets
 * (main thread only), dropping footprints whose slot is no longer in use, so
 * sv_frozen_footprint_blocked reads nothing the renderer touches and never calls into
 * Vulkan. The caller must not freeze again while a worker may still be querying. */
static struct {
    struct Footprint prints[2][SV_FOOTPRINTS];
    unsigned count[2];
    uint64_t grid[2][SV_FOOTPRINT_CELLS][SV_FOOTPRINT_WORDS];
    uint64_t wide[2][SV_FOOTPRINT_WORDS];
} frozen;

void sv_footprints_freeze(void) {
    for (unsigned frame = 0; frame < 2; frame++) {
        unsigned n = g.ok ? footprint_count[frame] : 0;
        frozen.count[frame] = n;
        memcpy(frozen.prints[frame], footprints[frame], n * sizeof footprints[0][0]);
        for (unsigned i = 0; i < n; i++) {
            int64_t slot = frozen.prints[frame][i].slot;
            if (slot < 0 || slot >= g.slot_cap || !(g.entries[slot].level_flags & SV_IN_USE))
                frozen.prints[frame][i].slot = -1;
        }
        memcpy(frozen.grid[frame], footprint_grid[frame], sizeof frozen.grid[0]);
        memcpy(frozen.wide[frame], footprint_wide[frame], sizeof frozen.wide[0]);
    }
}

/* footprint_hit without the live slot check: freezing already cleared dead slots. */
static int frozen_hit(const struct Footprint *v, double x0, double y0, double z0,
                      double x1, double y1, double z1, int64_t ignore) {
    if (v->slot < 0 || v->slot == ignore) return 0;
    if (fmin(y0, y1) > v->y + 6.0 || fmax(y0, y1) < v->y - 4.0) return 0;
    double ax = x0 - v->x, az = z0 - v->z, bx = x1 - v->x, bz = z1 - v->z;
    double a = ax * v->dx + az * v->dz, b = bx * v->dx + bz * v->dz;
    double side_a = az * v->dx - ax * v->dz, side_b = bz * v->dx - bx * v->dz;
    if (fabs(a) <= v->half_length && fabs(side_a) <= v->half_width &&
        bx * bx + bz * bz > ax * ax + az * az) return 0;
    double first = 0.0, last = 1.0;
    return footprint_axis(a, b, v->half_length, &first, &last) &&
        footprint_axis(side_a, side_b, v->half_width, &first, &last);
}

/* sv_footprint_blocked against the frozen copy: safe on any thread. */
int64_t sv_frozen_footprint_blocked(double x0, double y0, double z0, double x1, double y1, double z1, int64_t ignore) {
    int cx0, cx1, cz0, cz1;
    int gridded = footprint_cells(fmin(x0, x1), fmax(x0, x1), &cx0, &cx1) &&
        footprint_cells(fmin(z0, z1), fmax(z0, z1), &cz0, &cz1);
    for (unsigned frame = 0; frame < 2; frame++) {
        if (!gridded) {
            for (unsigned i = 0; i < frozen.count[frame]; i++)
                if (frozen_hit(&frozen.prints[frame][i], x0, y0, z0, x1, y1, z1, ignore)) return 1;
            continue;
        }
        uint64_t selected[SV_FOOTPRINT_WORDS];
        memcpy(selected, frozen.wide[frame], sizeof selected);
        for (int cz = cz0; cz <= cz1; cz++)
            for (int cx = cx0; cx <= cx1; cx++) {
                const uint64_t *cell = frozen.grid[frame][footprint_hash(cx, cz)];
                for (unsigned w = 0; w < SV_FOOTPRINT_WORDS; w++) selected[w] |= cell[w];
            }
        for (unsigned w = 0; w < SV_FOOTPRINT_WORDS; w++)
            for (uint64_t bits = selected[w]; bits; bits &= bits - 1) {
                unsigned i = w * 64 + (unsigned)__builtin_ctzll(bits);
                if (i < frozen.count[frame] &&
                    frozen_hit(&frozen.prints[frame][i], x0, y0, z0, x1, y1, z1, ignore)) return 1;
            }
    }
    return 0;
}

/* Read back the footprints footprint_blocked tests, for a caller that wants to
 * know where no vehicle can block before it builds a segment: set 0 is the
 * current frame's, set 1 the previous frame's. */
int64_t sv_footprint_count(int64_t set) {
    if (set < 0 || set > 1) return 0;
    return (int64_t)footprint_count[footprint_frame ^ (unsigned)set];
}

/* Bounds that hold every point the footprint can block: 0 the least x, 1 the
 * greatest x, 2 the least z, 3 the greatest z, in voxels, as the broad phase
 * grids them (a voxel wider than the footprint). A footprint the broad phase
 * cannot grid answers -1e30 / 1e30, so it bounds everything. */
double sv_footprint_bound(int64_t set, int64_t index, int64_t which) {
    if (set < 0 || set > 1) return 0.0;
    unsigned frame = footprint_frame ^ (unsigned)set;
    if (index < 0 || index >= (int64_t)footprint_count[frame]) return 0.0;
    const struct Footprint *v = &footprints[frame][index];
    double n = v->dx * v->dx + v->dz * v->dz;
    double hl = fabs(v->half_length), hw = fabs(v->half_width);
    double rx = (hl * fabs(v->dx) + hw * fabs(v->dz)) / n + 1.0;
    double rz = (hl * fabs(v->dz) + hw * fabs(v->dx)) / n + 1.0;
    int c0, c1;
    int gridded = n >= 0.25 && footprint_cells(v->x - rx, v->x + rx, &c0, &c1) &&
        footprint_cells(v->z - rz, v->z + rz, &c0, &c1);
    if (!gridded) return (which % 2 == 0) ? -1e30 : 1e30;
    switch (which) {
    case 0: return v->x - rx;
    case 1: return v->x + rx;
    case 2: return v->z - rz;
    default: return v->z + rz;
    }
}

/* Immediate, unshadowed lights. A new batch replaces the previous frame's batch. */
void sv_moving_lights_begin(int64_t headlights) {
    if (g.moving_lamp_count) g.lamp_version++;
    g.moving_lamp_count = 0;
    g.headlights = headlights != 0;
}

static void moving_lamp(double x, double y, double z, int64_t cls, int flags) {
    if (g.light_mode != 8 || cls <= 0 || cls > 255 || g.moving_lamp_count >= SV_MOVING_LAMPS) return;
    /* Store positions to an eighth of a voxel to keep moving beams smooth. */
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
        fabs(x) > 1000000 || fabs(y) > 1000000 || fabs(z) > 1000000) return;
    int32_t *lamp = g.moving_lamps[g.moving_lamp_count++];
    lamp[0] = (int32_t)lround(x * 8.0);
    lamp[1] = (int32_t)lround(y * 8.0);
    lamp[2] = (int32_t)lround(z * 8.0);
    lamp[3] = (int32_t)cls | SV_LAMP_MOVING | flags;
    if (g.moving_lamp_count == 1) g.lamp_version++;
}

int64_t sv_headlight(double x, double y, double z, double dx, double dz, int64_t cls) {
    if (!g.headlights || !isfinite(dx) || !isfinite(dz)) return 0;
    double length = hypot(dx, dz);
    if (length < 1e-6 || !isfinite(length)) return 0;
    int sx = 128 + (int)lround(127.0 * (dx / length));
    int sz = 128 + (int)lround(127.0 * (dz / length));
    moving_lamp(x, y, z, cls, SV_LAMP_BEAM | (sx << 8) | (sz << 16));
    return 1;
}

void sv_beacon(double x, double y, double z, int64_t cls) {
    moving_lamp(x, y, z, cls, 0);
}

/* lamp_keep for a stationary lamp in a list that may also hold moving ones:
 * past the cap it replaces only the farthest stationary lamp, so a vehicle's
 * light (its position in eighths of a voxel) is never weighed as a voxel's. */
static void lamp_keep_still(int32_t *list, uint32_t *k, uint32_t cap, const int32_t *l, int cx, int cy, int cz) {
    if (*k < cap) { lamp_keep(list, k, cap, l, cx, cy, cz); return; }
    uint32_t far = cap; int64_t fd = -1;
    for (uint32_t i = 0; i < cap; i++) {
        if (list[i * 4 + 3] & SV_LAMP_MOVING) continue;
        int64_t d = lamp_cell_d2(list + i * 4, cx, cy, cz);
        if (d > fd) { fd = d; far = i; }
    }
    if (far < cap && lamp_cell_d2(l, cx, cy, cz) < fd)
        for (int a = 0; a < 4; a++) list[far * 4 + a] = l[a];
}

/* List a lamp for each cell whose chunk comes within SV_LAMP_REACH voxels of
 * it; pass 0 counts, pass 1 writes. Moving lamps are listed first and take at
 * most half a cell's list, keeping space for stationary lamps on streets
 * packed with vehicles. Past the cap a cell keeps its nearest stationary
 * lamps (strata_lamps.h): a cell never loses its own lamp to its neighbours'
 * (scene 17's post did, to generated ones). */
static void list_lamp(Frame *F, const int32_t *lamp, int pass, int moving) {
    int lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        int voxel = moving ? lamp[a] >> 3 : lamp[a];
        lo[a] = (voxel - SV_LAMP_REACH) >> 5;
        hi[a] = (voxel + SV_LAMP_REACH) >> 5;
    }
    for (int z = lo[2]; z <= hi[2]; z++) {
        for (int y = lo[1]; y <= hi[1]; y++) {
            for (int x = lo[0]; x <= hi[0]; x++) {
                int cell = wrapi(x, g.vn) + g.vn * (wrapi(y, g.vnv) + g.vnv * wrapi(z, g.vn));
                uint32_t *entry = F->lcells_map + cell * 2;
                if (moving) {
                    if (entry[1] >= SV_CELL_LAMPS / 2) continue;
                    if (pass == 0) { entry[1]++; continue; }
                    if (entry[0] + entry[1] >= SV_LAMP_LIST) continue;
                    memcpy(F->llist_map + (entry[0] + entry[1]) * 4, lamp, 16);
                    entry[1]++;
                    continue;
                }
                if (pass == 0) { entry[1]++; continue; }
                uint32_t k = entry[1];
                if (entry[0] + (k < SV_CELL_LAMPS ? k : SV_CELL_LAMPS - 1) >= SV_LAMP_LIST) continue;
                lamp_keep_still(F->llist_map + entry[0] * 4, &k, SV_CELL_LAMPS, lamp, x, y, z);
                entry[1] = k;
            }
        }
    }
}

/* The lamps near each cell of the light volume, into this frame's copy when
 * they have changed: every glowing voxel a chunk keeps is listed for each
 * cell whose chunk comes within SV_LAMP_REACH voxels of it, at most
 * SV_CELL_LAMPS a cell, the nearest when more can reach it (strata_lamps.h),
 * so a point finds every lamp whose light reaches it while a cell has room. */
static void lamps_to_frame(Frame *F) {
    float *wt = g.ubo_map + UBO_WATER;
    const char *enabled = getenv("STRATA_LAMPS");
    wt[6] = 0.0f;
    if ((enabled && *enabled == '0') || !g.cells || g.vn <= 0 || g.vnv <= 0) return;
    uint32_t ncells = (uint32_t)(g.vn * g.vn * g.vnv);
    if (ncells > SV_MAX_CELLS) return;
    if (F->lamp_version != g.lamp_version) {
        memset(F->lcells_map, 0, (size_t)ncells * 8);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                uint32_t at = 0;
                for (uint32_t c = 0; c < ncells; c++) {
                    uint32_t *entry = F->lcells_map + c * 2;
                    entry[0] = at;
                    at += entry[1] < SV_CELL_LAMPS ? entry[1] : SV_CELL_LAMPS;
                    entry[1] = 0;
                }
            }
            if (g.light_mode == 8) {
                for (int i = 0; i < g.moving_lamp_count; i++) list_lamp(F, g.moving_lamps[i], pass, 1);
            }
            for (uint32_t c = 0; c < ncells; c++) {
                struct LightCell *lc = &g.cells[c];
                if (!lc->valid) continue;
                for (int i = 0; i < lc->nlamps; i++) list_lamp(F, lc->lamps + i * 4, pass, 0);
            }
        }
        F->lamp_version = g.lamp_version;
    }
    for (uint32_t c = 0; c < ncells; c++) {
        if (F->lcells_map[c * 2 + 1]) { wt[6] = 1.0f; break; }
    }
}

/* Begin uploading one part of a body (a 32-cubed piece of its grid) into
 * [slot]; parts and commit as for a chunk. It is drawn at the pose
 * sv_slot_pose gives and never written into the light volume. */
int64_t sv_body_begin(int64_t slot) {
    if (!sv_chunk_begin(slot, 0.0, 0.0, 0.0, 0)) return 0;
    g.stage.body = 1;
    return 1;
}
/* Where a body part is drawn: its low corner in metres, and its rotation. */
void sv_slot_pose(int64_t slot, double px, double py, double pz, double qx, double qy, double qz, double qw) {
    if (!g.ok || slot < 0 || slot >= g.slot_cap) return;
    ChunkEntry *e = &g.entries[slot];
    if (!(e->level_flags & SV_BODY)) return;
    e->pos[0] = (float)px; e->pos[1] = (float)py; e->pos[2] = (float)pz;
    e->rot[0] = (float)qx; e->rot[1] = (float)qy; e->rot[2] = (float)qz; e->rot[3] = (float)qw;
    touch((uint32_t)slot);
}
/* A body part in [slot] has voxels [metres] across; 0 for the world's. */
void sv_slot_voxel(int64_t slot, double metres) {
    if (!g.ok || slot < 0 || slot >= g.slot_cap) return;
    ChunkEntry *e = &g.entries[slot];
    if (!(e->level_flags & SV_BODY)) return;
    e->pos[3] = metres > 0.0 ? (float)metres : 0.0f;
    touch((uint32_t)slot);
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

/* The translucent part of the chunk being assembled ends in eight lists of
 * its own quads, one for each way a ray can run (Strata.Mesh.order_count),
 * of these lengths; what comes before the lists is not drawn. Without this
 * call the part is one list, drawn in any order. */
int64_t sv_chunk_orders(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g7, int64_t h) {
    int64_t v[8] = { a, b, c, d, e, f, g7, h };
    if (!g.stage.open) return 0;
    for (int i = 0; i < 8; i++) { if (v[i] < 0 || v[i] > 65535) { g.stage.open = 0; return 0; } g.stage.ord[i] = (uint32_t)v[i]; }
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

static int64_t fig_commit(void);
static void release_mesh(const ChunkEntry *e) {
    if (e->level_flags & SV_SHARED) return;
    for (int r = 0; r < 4; r++) sa_release(&g_quads, (Range){ e->first[r], e->count[r] }, g.frame_no);
    sa_release(&g_loose, (Range){ e->first[4], e->count[4] }, g.frame_no);
}

static void track_batch(uint32_t slot, int batched) {
    if (!g.batch_shared) return;
    uint32_t at = g.batch_at[slot];
    if (batched && !at) {
        g.batch_slots[g.num_batch_slots++] = slot;
        g.batch_at[slot] = g.num_batch_slots;
    } else if (!batched && at) {
        uint32_t last = g.batch_slots[--g.num_batch_slots];
        g.batch_slots[at - 1] = last;
        g.batch_at[last] = at;
        g.batch_at[slot] = 0;
    }
}

static void batch_mesh_extent(uint32_t prototype, const ChunkEntry *source) {
    float *extent = g.batches[prototype].extent;
    if (extent[0] > 0) return;
    for (uint32_t i = 0; i < source->count[0]; i++) {
        uint32_t w0 = g.giga_map[(size_t)(source->first[0] + i) * 2];
        uint32_t face = (w0 >> 25) & 7u, w = ((w0 >> 15) & 31u) + 1, h = ((w0 >> 20) & 31u) + 1;
        float x = (w0 & 31u) + (face < 2 || face >= 4 ? w : 1);
        float y = ((w0 >> 5) & 31u) + (face < 2 ? 1 : h);
        float z = ((w0 >> 10) & 31u) + (face < 2 ? h : face < 4 ? w : 1);
        if (x > extent[0]) extent[0] = x;
        if (y > extent[1]) extent[1] = y;
        if (z > extent[2]) extent[2] = z;
    }
}

/* Share an opaque level-zero mesh with an independently positioned slot.
 * Once referenced, the prototype's GPU range stays pinned until close. */
int64_t sv_chunk_instance(int64_t slot, int64_t prototype, double x, double y, double z) {
    g.churn[0]++;
    if (!g.ok || slot < 0 || prototype < 0 || slot >= g.slot_cap || prototype >= g.slot_cap || slot == prototype) return 0;
    ChunkEntry *source = &g.entries[prototype], *target = &g.entries[slot];
    if (!(source->level_flags & SV_IN_USE) ||
        (source->level_flags & (255 | SV_SHARED | SV_BODY | SV_HAS_LIGHT)) ||
        !source->count[0] || source->count[1] || source->count[2] || source->count[3] || source->count[4] ||
        (target->level_flags & (SV_TEMPLATE | SV_BODY | SV_HAS_LIGHT))) return 0;
    if ((target->level_flags & SV_SHARED) && !(target->level_flags & SV_MOTION) && target->first[0] == source->first[0] &&
        target->origin[0] == (float)x && target->origin[1] == (float)y && target->origin[2] == (float)z) return 1;
    release_mesh(target);
    trans_track((uint32_t)slot, 0);
    source->level_flags |= SV_TEMPLATE;
    uint32_t hidden = target->level_flags & SV_HIDDEN;
    *target = *source;
    target->origin[0] = (float)x; target->origin[1] = (float)y; target->origin[2] = (float)z;
    /* Use the old slot path when the mesh falls beyond the shader's storage view. */
    int batched = g.batch_shared && ((VkDeviceSize)source->first[0] + source->count[0]) * 8 <= g.max_ssbo;
    if (batched) batch_mesh_extent((uint32_t)prototype, source);
    track_batch((uint32_t)slot, batched);
    target->first[5] = (uint32_t)prototype;
    target->level_flags = SV_IN_USE | SV_SHARED | hidden | (batched ? SV_BATCHED : 0);
    if ((uint32_t)slot + 1 > g.hi_slot) g.hi_slot = (uint32_t)slot + 1;
    touch((uint32_t)slot);
    return 1;
}

/* Draw the body part in [slot] from the mesh of the body part in
 * [prototype]: its own pose and voxel size (sv_slot_pose, sv_slot_voxel) and
 * no mesh of its own, so a thousand instances of one shape are one mesh. The
 * prototype's ranges are then fixed until close; hide it (sv_chunk_show) to
 * keep it undrawn. A prototype with translucent quads is refused: their
 * ordered lists are a slot's own. */
int64_t sv_body_instance(int64_t slot, int64_t prototype) {
    if (!g.ok || slot < 0 || prototype < 0 || slot >= g.slot_cap || prototype >= g.slot_cap || slot == prototype) return 0;
    ChunkEntry *source = &g.entries[prototype], *target = &g.entries[slot];
    if (!(source->level_flags & SV_IN_USE) || !(source->level_flags & SV_BODY) || (source->level_flags & SV_SHARED) ||
        source->count[2] || (target->level_flags & (SV_TEMPLATE | SV_HAS_LIGHT))) return 0;
    release_mesh(target);
    if (target->level_flags & SV_BATCHED) track_batch((uint32_t)slot, 0);
    trans_track((uint32_t)slot, 0);
    source->level_flags |= SV_TEMPLATE;
    *target = *source;
    target->level_flags = SV_IN_USE | SV_BODY | SV_SHARED;
    target->pos[0] = target->pos[1] = target->pos[2] = target->pos[3] = 0.0f;
    target->rot[0] = target->rot[1] = target->rot[2] = 0.0f; target->rot[3] = 1.0f;
    if ((uint32_t)slot + 1 > g.hi_slot) g.hi_slot = (uint32_t)slot + 1;
    touch((uint32_t)slot);
    return 1;
}

/* Draw slot [slot] as prototype [prototype] moving along [n] keys (x, y, z, place: four ints
 * each in [keys_i32], places 0..8 rising from 0 to 8, eighths of the way through the motion),
 * from time [t0] to [t1] (the clock sv_motion_clock sets), its progress going from [p0] to
 * [p1]. With [gait], the shader steps a walker's legs (the mesh's two bottom voxel rows, x 0
 * and x 1) as its progress passes each twelfth. The slot is shown. Returns 0 when a key lies too
 * far from the first (the slot is left as it was) or the prototype cannot be batched (the slot
 * then stands still at the first key). */
int64_t sv_chunk_motion(int64_t slot, int64_t prototype, double t0, double t1, double p0, double p1, void *keys_i32, int64_t n, int64_t gait) {
    g.churn[1]++;
    if (!g.ok || !g.motion || n < 1 || n > SV_MOTION_KEYS || !keys_i32 || narr_len(keys_i32) < n * 4) return 0;
    if (slot < 0 || slot >= g.slot_cap) return 0;
    const int32_t *k = narr_data(keys_i32);
    int32_t ox = k[0], oy = k[1], oz = k[2];
    uint32_t rec[SV_MOTION_WORDS] = { 0 };
    int16_t box[6] = { 0, 0, 0, 0, 0, 0 };
    int last = -1;
    for (int64_t i = 0; i < n; i++) {
        int32_t dx = k[i * 4] - ox, dy = k[i * 4 + 1] - oy, dz = k[i * 4 + 2] - oz, place = k[i * 4 + 3];
        if (dx < -512 || dx > 511 || dz < -512 || dz > 511 || dy < -128 || dy > 127 || place <= last || place > 8) return 0;
        last = place;
        rec[5 + i] = ((uint32_t)dx & 1023u) | (((uint32_t)dz & 1023u) << 10) | (((uint32_t)dy & 255u) << 20) | ((uint32_t)place << 28);
        if (dx < box[0]) box[0] = (int16_t)dx;
        if (dy < box[1]) box[1] = (int16_t)dy;
        if (dz < box[2]) box[2] = (int16_t)dz;
        if (dx > box[3]) box[3] = (int16_t)dx;
        if (dy > box[4]) box[4] = (int16_t)dy;
        if (dz > box[5]) box[5] = (int16_t)dz;
    }
    if (k[3] != 0) return 0;
    g.churn[0]--;   /* the instance below is the motion's own, not a chunk_instance call */
    if (!sv_chunk_instance(slot, prototype, (double)ox, (double)oy, (double)oz)) return 0;
    ChunkEntry *e = &g.entries[slot];
    if (!(e->level_flags & SV_BATCHED)) return 0;
    if (!g.motion_epoch_set) { g.motion_epoch = t0; g.motion_epoch_set = 1; }
    float ft0 = (float)(t0 - g.motion_epoch), ft1 = (float)(t1 - g.motion_epoch), fp0 = (float)p0, fp1 = (float)p1;
    memcpy(&rec[0], &ft0, 4); memcpy(&rec[1], &ft1, 4); memcpy(&rec[2], &fp0, 4); memcpy(&rec[3], &fp1, 4);
    rec[4] = (uint32_t)n | (gait ? 256u : 0u);
    memcpy(g.motion + (size_t)slot * SV_MOTION_WORDS, rec, sizeof rec);
    memcpy(g.motion_box + (size_t)slot * 6, box, sizeof box);
    e->level_flags = (e->level_flags | SV_MOTION) & ~SV_HIDDEN;
    touch((uint32_t)slot);
    return 1;
}

/* The time moving slots are drawn at (the clock sv_chunk_motion's times are on). Times go to
 * the GPU as floats from an epoch; once the clock is 4096 s past it, the epoch moves up to the
 * clock and every motion record is shifted to match, so a long game keeps its precision. */
void sv_motion_clock(double t) {
    if (!g.ok) return;
    if (!g.motion_epoch_set) { g.motion_epoch = t; g.motion_epoch_set = 1; }
    if (g.motion && fabs(t - g.motion_epoch) > 4096.0) {
        float shift = (float)(t - g.motion_epoch);
        for (uint32_t i = 0; i < g.num_batch_slots; i++) {
            uint32_t slot = g.batch_slots[i];
            if (!(g.entries[slot].level_flags & SV_MOTION)) continue;
            float *rec = (float *)(g.motion + (size_t)slot * SV_MOTION_WORDS);
            rec[0] -= shift; rec[1] -= shift;
            touch(slot);
        }
        g.motion_epoch = t;
    }
    g.ubo_map[UBO_TRACE + 1] = (float)(t - g.motion_epoch);
}

int64_t sv_chunk_commit(void) {
    g.churn[2]++;
    if (!g.stage.open) return 0;
    if (g.stage.fig) return fig_commit();
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
    /* a translucent part that ends in ordered lists: only the lists are kept */
    uint32_t lists = 0;
    for (int i = 0; i < 8; i++) lists += g.stage.ord[i];
    if (lists > g.stage.n[2]) lists = 0;
    uint32_t counts[4] = { g.stage.n[0], g.stage.n[1], lists ? lists : g.stage.n[2], ncross }, firsts[4] = { 0, 0, 0, 0 }, lfirst = 0;
    if (!sa_alloc_many(&g_quads, counts, 4, firsts)) { g.refused++; return 0; }
    if (!sa_alloc(&g_loose, nloose, &lfirst)) {
        for (int i = 0; i < 4; i++) sa_cancel(&g_quads, (Range){ firsts[i], counts[i] });
        g.refused++; return 0;
    }
    /* the new ranges have been idle for two frames or were never used: no frame in flight reads them */
    for (int l = 0; l < 3; l++)
        if (counts[l]) memcpy(g.giga_map + (size_t)firsts[l] * 2, g.stage.part[l] + (l == 2 ? (size_t)(g.stage.n[2] - counts[2]) * 2 : 0), (size_t)counts[l] * 8);
    for (int i = 0; i < 8; i++) g.trans_ord[(size_t)slot * 8 + i] = lists ? (uint16_t)g.stage.ord[i] : 0;
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
    release_mesh(e);
    if (e->level_flags & SV_BATCHED) track_batch((uint32_t)slot, 0);
    e->origin[0] = g.stage.origin[0]; e->origin[1] = g.stage.origin[1]; e->origin[2] = g.stage.origin[2];
    for (int r = 0; r < 4; r++) { e->first[r] = firsts[r]; e->count[r] = counts[r]; }
    e->first[4] = lfirst; e->count[4] = nloose;
    e->level_flags = SV_IN_USE | (g.stage.body ? SV_BODY : g.stage.level) | (e->level_flags & SV_HIDDEN);
    if (!g.stage.body) { e->pos[0] = e->pos[1] = e->pos[2] = e->pos[3] = 0.0f; e->rot[0] = e->rot[1] = e->rot[2] = 0.0f; e->rot[3] = 1.0f; }
    if ((uint32_t)slot + 1 > g.hi_slot) g.hi_slot = (uint32_t)slot + 1;
    trans_track((uint32_t)slot, counts[2] > 0);
    touch((uint32_t)slot);
    if (g.stage.body || g.stage.level > 0 || g.stage_light_n == 0) return 1;   /* neither a body nor a coarse chunk is in the light volume */
    e->level_flags |= SV_HAS_LIGHT;
    /* its voxels go into the light volume at the start of the next frame */
    int32_t chunk[3] = { e->origin[0] >> 5, e->origin[1] >> 5, e->origin[2] >> 5 };
    if (g.stage_light_n == 32768) {
        unsigned char *copy = malloc(32768);
        if (copy) { memcpy(copy, g.stage_light, 32768); light_queue(chunk, 1, -1, copy); }
    } else light_queue(chunk, 1, g.stage_light_n == 1 ? g.stage_light[0] : 0, NULL);
    return 1;
}

void sv_chunk_free(int64_t slot) {
    if (!g.ok || slot < 0 || slot >= g.slot_cap) return;
    ChunkEntry *e = &g.entries[slot];
    if (!(e->level_flags & SV_IN_USE)) { e->level_flags = 0; return; }   /* a slot hidden before its first upload landed */
    if (e->level_flags & SV_TEMPLATE) return;
    int has_light = (e->level_flags & SV_HAS_LIGHT) != 0;
    release_mesh(e);
    if (e->level_flags & SV_BATCHED) track_batch((uint32_t)slot, 0);
    e->level_flags = 0;                      /* the cull pass skips it once the frames have their copy */
    if (has_light) {   /* and its cell of the light volume empties, if it is still this chunk's */
        int32_t chunk[3] = { e->origin[0] >> 5, e->origin[1] >> 5, e->origin[2] >> 5 };
        int cell[3];
        struct LightCell *c = cell_of(chunk, cell);
        if (c->valid && c->chunk[0] == chunk[0] && c->chunk[1] == chunk[1] && c->chunk[2] == chunk[2]) light_queue(chunk, 0, 0, NULL);
    }
    trans_track((uint32_t)slot, 0);
    memset(e->first, 0, sizeof e->first);
    memset(e->count, 0, sizeof e->count);
    touch((uint32_t)slot);
}


/* Draw slot [slot]'s mesh ([shown] 1) or keep it undrawn (0): the
 * which-chunk-draws rule of Strata.Slots. Kept across uploads into the slot;
 * a free clears it. The cull pass and the translucent order skip a hidden slot. */
void sv_chunk_show(int64_t slot, int64_t shown) {
    if (!g.ok || slot < 0 || slot >= g.slot_cap) return;
    ChunkEntry *e = &g.entries[slot];
    int32_t was = e->level_flags;
    if (shown) e->level_flags &= ~SV_HIDDEN; else e->level_flags |= SV_HIDDEN;
    if (e->level_flags != was) touch((uint32_t)slot);
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
    if ((int)mode != g.light_mode) {
        g.hist_reset = SV_FRAMES;
        g.lamp_version++;
    }
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
/* How finely the light is traced: one pixel of each [scale] x [scale] block
 * (1..4; 1 every pixel, 2 the default). The screen-sized images are made
 * again before the next frame, and what the light pass remembered starts over. */
void sv_set_trace_scale(int64_t scale) {
    int sc = scale < 1 ? 1 : (scale > 4 ? 4 : (int)scale);
    if (sc == g.trace_scale) return;
    g.trace_scale = sc; g.hist_reset = SV_FRAMES;
    if (g.ok) g.resized = 1;
}
int64_t sv_trace_scale(void) { return g.trace_scale ? g.trace_scale : 2; }
/* Add [count] spray particles (at most 512 a call) at (x, y, z), flying at
 * about (vx, vy, vz) m/s, each velocity component off by up to [spread],
 * living [life] seconds, drawn with texture layer [tex]. They are born at
 * the time last given to sv_set_time. Answers how many were added. */
static float spray_hash(uint32_t i, uint32_t salt) {
    uint32_t h = i * 2654435761u + salt * 40503u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return (float)(h & 0xffffu) / 32768.0f - 1.0f;      /* -1..1 */
}
int64_t sv_spray(double x, double y, double z, double vx, double vy, double vz, int64_t count, double spread, double life, int64_t tex) {
    if (!g.ok || !g.spray_map || life <= 0.0) return 0;
    int64_t n = sp_clamp(count);
    for (int64_t i = 0; i < n; i++) {
        uint32_t id = (uint32_t)g.ring.head;
        uint32_t slot = sp_push(&g.ring, (float)g.time_s, (float)life);
        float *p = g.spray_map + (size_t)slot * SV_SPRAY_FLOATS;
        p[0] = (float)x + 0.35f * spray_hash(id, 1); p[1] = (float)y + 0.15f * spray_hash(id, 2); p[2] = (float)z + 0.35f * spray_hash(id, 3);
        p[3] = (float)g.time_s;
        p[4] = (float)(vx + spread * spray_hash(id, 4)); p[5] = (float)(vy + spread * 0.5 * (spray_hash(id, 5) + 1.0f)); p[6] = (float)(vz + spread * spray_hash(id, 6));
        p[7] = (float)(life * (0.7 + 0.15 * (spray_hash(id, 7) + 1.0f)));
        p[8] = 0.0f; p[9] = 0.16f + 0.07f * spray_hash(id, 8);
        p[10] = 0.5f * (spray_hash(id, 9) + 1.0f); p[11] = 3.14159265f * spray_hash(id, 10); p[12] = (float)tex; p[13] = 0.0f; p[14] = 0.0f; p[15] = 0.0f;
    }
    return n;
}
/* A burst of [count] particles (at most 512) of effect [effect], born now.
 * [f18] holds 18 floats: the segment x0 y0 z0 x1 y1 z1 (metres), the shape's
 * a b c, the mean velocity vx vy vz (m/s), its spread, life, life spread,
 * size spread, and two unused; [shape] 0 a point, 1 a sphere of radius a, 2
 * a box of half sizes a b c (strata_particles.h, pt_write). Answers how many
 * were written: 0 for an effect never set. */
int64_t sv_emit(int64_t effect, int64_t shape, void *f18, int64_t count, int64_t seed) {
    if (!g.ok || !g.spray_map || !f18 || narr_len(f18) < 18 || effect < 0 || effect >= PT_EFFECTS || !g.fx_set[effect]) return 0;
    const float *f = (const float *)narr_data(f18);
    if (f[13] <= 0.0f) return 0;
    int64_t n = sp_clamp(count);
    if (n > 0 && g.fx_map[(size_t)effect * PT_EFFECT_FLOATS] > 0.5f) {
        double until = g.time_s + (double)f[13] * (1.0 + fabs((double)f[14]));
        if (until > g.glow_until) g.glow_until = until;
    }
    /* an effect that meets the world: its particles are marked for the next frame's collision pass */
    int meets = pt_effect_meets(g.fx_map + (size_t)effect * PT_EFFECT_FLOATS);
    if (n > 0 && meets) {
        if (!g.meet_pending) { g.meet_pending = 1; g.meet_first = g.ring.head; }
        g.meet_total += (uint64_t)n;
    }
    for (int64_t i = 0; i < n; i++) {
        float life = f[13] * (1.0f + fabsf(f[14]));
        uint32_t slot = sp_push(&g.ring, (float)g.time_s, life);
        pt_write(g.spray_map + (size_t)slot * SV_SPRAY_FLOATS, (int)effect, (int)shape, f, f + 6, f + 9, f[12], f[13], f[14], f[15],
                 (uint32_t)seed, (uint32_t)i, (uint32_t)n, (float)g.time_s);
        if (meets) g.spray_map[(size_t)slot * SV_SPRAY_FLOATS + 13] = PT_MEET_UNKNOWN;
    }
    return n;
}

/* The time, in seconds, that moving surface detail (water's ripples) runs on. */
void sv_set_time(double seconds) { g.time_s = seconds; }

/* Put effect [index] (1 .. PT_EFFECTS - 1) in the particles' effect table:
 * PT_EFFECT_FLOATS floats laid out as strata_particles.h says. Effect 0 is
 * spray's and cannot be changed. Answers 1, or 0 when refused. */
int64_t sv_effect_set(int64_t index, void *floats) {
    if (!g.ok || !g.fx_map || !floats || index < 1 || index >= PT_EFFECTS || narr_len(floats) < PT_EFFECT_FLOATS) return 0;
    memcpy(g.fx_map + (size_t)index * PT_EFFECT_FLOATS, narr_data(floats), PT_EFFECT_FLOATS * sizeof(float));
    g.fx_set[index] = 1;
    return 1;
}

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
/* The eye is under liquid whose surface is at [surface_y] metres: the world
 * is seen through it, in its colour (linear rgb), fading with distance at
 * [density] per metre, and lit less the deeper it is. A density of 0 means
 * the eye is in air. */
void sv_set_underwater(double surface_y, double r, double gr, double b, double density) {
    if (!g.ok) return;
    float *wt = g.ubo_map + UBO_WATER;
    wt[0] = (float)r; wt[1] = (float)gr; wt[2] = (float)b; wt[3] = density > 0.0 ? (float)density : 0.0f;
    wt[4] = (float)surface_y;   /* wt[5] is the water-memory flag and wt[6] the lamps flag, both set each frame */
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

/* The OpenGL clip planes used by the chunk culler, tested against the mesh bounds. */
static int batch_in_view(uint32_t slot, const ChunkEntry *entry, const MeshBatch *batch, float scale,
                         const float planes[6][4]) {
    /* a moving slot can be anywhere in its motion's box */
    const int16_t *box = (entry->level_flags & SV_MOTION) ? g.motion_box + (size_t)slot * 6 : NULL;
    const float lo[3] = { (entry->origin[0] - 1 + (box ? box[0] : 0)) * scale, (entry->origin[1] - 1 + (box ? box[1] : 0)) * scale,
                          (entry->origin[2] - 1 + (box ? box[2] : 0)) * scale };
    const float hi[3] = { (entry->origin[0] + batch->extent[0] + (box ? box[3] : 0)) * scale,
                          (entry->origin[1] + batch->extent[1] + (box ? box[4] : 0)) * scale,
                          (entry->origin[2] + batch->extent[2] + (box ? box[5] : 0)) * scale };
    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        float d = p[0] * (p[0] < 0 ? lo[0] : hi[0]) + p[1] * (p[1] < 0 ? lo[1] : hi[1]) +
                  p[2] * (p[2] < 0 ? lo[2] : hi[2]) + p[3];
        if (d < -0.0001f) return 0;
    }
    return 1;
}

static void prepare_mesh_batches(Frame *F, uint32_t base) {
    F->batch_instances = F->batch_quads = 0;
    g.num_batch_keys = g.batch_visible_count = g.batch_draws = 0;
    if (!g.num_batch_slots) return;
    const float *vp = g.ubo_map;
    float planes[6][4];
    for (int axis = 0; axis < 3; axis++)
        for (int col = 0; col < 4; col++) {
            planes[axis * 2][col] = vp[col * 4 + 3] + vp[col * 4 + axis];
            planes[axis * 2 + 1][col] = vp[col * 4 + 3] - vp[col * 4 + axis];
        }
    for (uint32_t i = 0; i < g.num_batch_slots; i++) {
        uint32_t slot = g.batch_slots[i];
        const ChunkEntry *entry = &g.entries[slot];
        uint32_t prototype = entry->first[5];
        MeshBatch *batch = &g.batches[prototype];
        if ((entry->level_flags & SV_HIDDEN) || !batch_in_view(slot, entry, batch, vp[31], planes)) continue;
        if (!batch->count++) g.batch_keys[g.num_batch_keys++] = prototype;
        g.batch_visible[g.batch_visible_count++] = slot;
        F->batch_quads += entry->count[0];
    }
    F->batch_instances = g.batch_visible_count;
    uint32_t next = base;
    for (uint32_t i = 0; i < g.num_batch_keys; i++) {
        MeshBatch *batch = &g.batches[g.batch_keys[i]];
        batch->next = next;
        next += batch->count;
    }
    for (uint32_t i = 0; i < g.batch_visible_count; i++) {
        uint32_t slot = g.batch_visible[i];
        MeshBatch *batch = &g.batches[g.entries[slot].first[5]];
        g.dslot_map[batch->next++] = slot;
    }
}

static int draw_mesh_batches(Frame *F) {
    if (!g.num_batch_keys) return 0;
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_batch);
    for (uint32_t i = 0; i < g.num_batch_keys; i++) {
        uint32_t prototype = g.batch_keys[i];
        MeshBatch *batch = &g.batches[prototype];
        const ChunkEntry *mesh = &g.entries[prototype];
        uint32_t left = batch->count, begin = batch->next - left;
        uint32_t limit = INT32_MAX / mesh->count[0];
        while (left) {
            uint32_t instances = left < limit ? left : limit;
            uint32_t pc[4] = { begin, 0, mesh->first[0], mesh->count[0] };
            vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
            vkCmdDraw(F->cmd, 4, instances * mesh->count[0], 0, 0);
            g.batch_draws++;
            begin += instances;
            left -= instances;
        }
        batch->count = 0;
    }
    return (int)g.batch_draws;
}

/* Draw every used slot that survives the frustum test: opaque and cutout
 * first, then translucent with blending. Returns 1 when a frame was drawn,
 * 0 when it was skipped (minimised, swapchain being rebuilt, submit failed). */
int64_t sv_frame(void) {
    if (!g.ok) return 0;
    if (g.resized || !g.swap_ok) { g.resized = 0; if (!rebuild_swapchain()) return 0; }
    Frame *F = &g.frames[g.frame_no % SV_FRAMES];
    uint32_t q0 = (uint32_t)(g.frame_no % SV_FRAMES) * 8;
    double t_start = glfwGetTime();
    memcpy(g.churn_last, g.churn, sizeof g.churn); memset(g.churn, 0, sizeof g.churn);
    /* this Frame was last used two frames ago; when its fence is signalled,
     * everything released up to then is idle */
    vkWaitForFences(g.dev, 1, &F->fence, VK_TRUE, UINT64_MAX);
    double wait_ms = (glfwGetTime() - t_start) * 1000.0;
    sa_collect(&g_quads, g.frame_no);
    sa_collect(&g_loose, g.frame_no);
    sa_collect(&g_bbytes, g.frame_no);
    /* what this Frame's cull pass let through, two frames ago */
    g.drawn_chunks = (int64_t)F->stats_map[6] + F->batch_instances;
    g.drawn_quads = (int64_t)F->stats_map[7] + F->batch_quads;
    g.opaque_commands = F->stats_map[0];
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
        if (vkGetQueryPoolResults(g.dev, g.qpool, q0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            g.stat[3] = (double)(ts[1] - ts[0]) * (double)g.ts_period / 1.0e6;
            uint64_t tp[3];
            if (vkGetQueryPoolResults(g.dev, g.qpool, q0 + 4, 3, sizeof tp, tp, sizeof tp[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                double ms = (double)g.ts_period / 1.0e6;
                g.pass_ms[0] = (double)(tp[0] - ts[0]) * ms;   /* uploads and the cull */
                g.pass_ms[1] = (double)(tp[1] - tp[0]) * ms;   /* geometry, figures included */
                g.pass_ms[2] = (double)(tp[2] - tp[1]) * ms;   /* figure shadow map and the light pass */
                g.pass_ms[3] = (double)(ts[1] - tp[2]) * ms;   /* water, composite, translucent, particles, interface */
            }
        }
        fg.shadow_ms = 0.0;
        if (F->fsm_ts && vkGetQueryPoolResults(g.dev, g.qpool, q0 + 2, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            fg.shadow_ms = (double)(ts[1] - ts[0]) * (double)g.ts_period / 1.0e6;
        if (fg.shadow_ms > fg.shadow_worst) fg.shadow_worst = fg.shadow_ms;
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
    prepare_mesh_batches(F, (SV_REGION_BATCH + (uint32_t)(g.frame_no % SV_FRAMES)) * SV_CMD_STRIDE);
    vkResetCommandBuffer(F->cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(F->cmd, &bi);
    fig_shadow_first(F);
    if (g.have_ts) { vkCmdResetQueryPool(F->cmd, g.qpool, q0, 8); vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.qpool, q0); }
    /* cull: one compute invocation per slot writes the indirect commands */
    int32_t *misc = (int32_t *)(g.ubo_map + 32);
    misc[0] = (int32_t)g.hi_slot; misc[1] = SV_CMD_STRIDE; misc[2] = (int32_t)g.layers; misc[3] = (int32_t)g.nblocks;
    invert4(g.ubo_map, g.ubo_map + UBO_INV_VP);
    memcpy(g.ubo_map + UBO_PREV_VP, g.prev_vp, sizeof g.prev_vp);
    memcpy(g.prev_vp, g.ubo_map, sizeof g.prev_vp);
    g.ubo_map[UBO_LIGHT + 0] = (float)g.light_mode; g.ubo_map[UBO_LIGHT + 1] = (float)g.time_s;
    g.ubo_map[UBO_LIGHT + 2] = (float)(g.frame_no & 0xFFFFFF);
    /* STRATA_SKY_FULL=1: the sky light sampled on the schedule of before the fast paths (every new pixel each frame, a settled one in four),
     * sent as a negative ray count; the stored pictures (scripts/golden.sh) are taken this way, so they compare at frame 60 as they did */
    { static int full = -1; if (full < 0) full = getenv("STRATA_SKY_FULL") && getenv("STRATA_SKY_FULL")[0] == '1';
      g.ubo_map[UBO_LIGHT + 3] = full ? -(float)g.sky_rays : (float)g.sky_rays; }
    g.ubo_map[UBO_SUN_COLOR + 3] = (float)g.reflections;
    g.ubo_map[UBO_TRACE] = (float)g.trace_scale;
    { int32_t *vol = (int32_t *)(g.ubo_map + UBO_VOL); vol[0] = g.vn; vol[1] = g.vnv; vol[2] = g.vn;
      /* how many past samples a pixel may lean on: none while the remembered
       * images are fresh, 2 just after the sun moved, else 255 (sun visibility
       * leans on 64 of them at most). An edit that reaches the volume this
       * frame caps only the pixels near it: its bounds go to the shader. */
      /* the box is this frame's edits and the last frame's, not every edit since the
       * box was last empty: with edits every frame (fire, freezing, flowing water) a
       * box kept growing came to cover the whole field, and every pixel in it kept
       * sampling afresh with a memory of 2, so shadows stayed grainy (13 November) */
      { int32_t lo[3], hi[3]; int any = 0;
        for (uint32_t i = 0; i < g.nlq && i < SV_LIGHT_PER_FRAME; i++) if (g.lq[i].edit) {
            const int32_t *ch = g.lq[i].chunk;
            for (int k = 0; k < 3; k++) {
                if (!any || ch[k] < lo[k]) lo[k] = ch[k];
                if (!any || ch[k] > hi[k]) hi[k] = ch[k];
            }
            any = 1;
        }
        if (any) {
            for (int k = 0; k < 3; k++) {
                g.edit_lo[k] = g.last_edit ? (lo[k] < g.last_lo[k] ? lo[k] : g.last_lo[k]) : lo[k];
                g.edit_hi[k] = g.last_edit ? (hi[k] > g.last_hi[k] ? hi[k] : g.last_hi[k]) : hi[k];
                g.last_lo[k] = lo[k]; g.last_hi[k] = hi[k];
            }
            g.light_edit = 2;
        }
        g.last_edit = any;
        if (getenv("STRATA_LIGHTLOG")) {
            static int frames, edited, relaxed; static int64_t vol_sum;
            frames++; if (g.light_edit > 0) { edited++; vol_sum += (int64_t)(g.edit_hi[0] - g.edit_lo[0] + 1) * (g.edit_hi[1] - g.edit_lo[1] + 1) * (g.edit_hi[2] - g.edit_lo[2] + 1); }
            if (g.light_relax > 0 || g.hist_reset > 0) relaxed++;
            if (frames == 240) { fprintf(stderr, "light: of 240 frames %d had an edit box (mean %.1f chunks), %d a relaxed or reset memory\n", edited, edited ? (double)vol_sum / edited : 0.0, relaxed); frames = edited = relaxed = 0; vol_sum = 0; }
        } }
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
    fig_prepare(F);               /* first: it chooses the traced figures' rows bodies_to_frame appends */
    if (fg.cpu_us > fg.cpu_worst) fg.cpu_worst = fg.cpu_us;
    bodies_to_frame(F);
    lamps_to_frame(F);
    g.ubo_map[UBO_WATER + 5] = g.water_mem ? 1.0f : 0.0f;
    memcpy(F->ubo_map, g.ubo_map, UBO_FLOATS * 4);
    g.churn[3] += F->npending;
    for (uint32_t i = 0; i < F->npending; i++) {
        uint32_t slot = F->pending[i];
        F->table_map[slot] = g.entries[slot];
        if (g.entries[slot].level_flags & SV_MOTION) memcpy(F->motion_map + (size_t)slot * SV_MOTION_WORDS, g.motion + (size_t)slot * SV_MOTION_WORDS, SV_MOTION_WORDS * 4);
        F->is_pending[slot] = 0;
    }
    F->npending = 0;
    light_flush(F);               /* queued chunks into the light volume, before anything reads it */
    glyphs_flush(F);              /* new glyphs into the atlas, before the 2D layer reads it */
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
    /* particles that meet the world: what each one born since the last frame will meet, found once
     * (collide.comp), after the light volume has this frame's chunks. Nothing is bound or run in a
     * frame that bore none. The ring is one buffer for every frame in flight; the pass writes only
     * the last three floats of records no draw has yet read. */
    g.meet_last = 0;
    if (g.meet_pending && g.pipe_collide) {
        uint64_t born = g.ring.head - g.meet_first;
        if (born > SV_SPRAY_CAP) { g.meet_first = g.ring.head - SV_SPRAY_CAP; born = SV_SPRAY_CAP; }
        uint32_t cpc[2] = { (uint32_t)(g.meet_first % SV_SPRAY_CAP), (uint32_t)born };
        vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe_collide);
        vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 40, 8, cpc);
        vkCmdDispatch(F->cmd, (cpc[1] + 63) / 64, 1, 1);
        VkMemoryBarrier pb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT };
        vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &pb, 0, NULL, 0, NULL);
        g.meet_last = cpc[1]; g.meet_pending = 0;
    }
    VkViewport vpt = { 0, 0, (float)g.extent.width, (float)g.extent.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, g.extent };
    VkDeviceSize zero = 0;
    /* per-pass timestamps: 4 after the cull, 5 after the geometry, 6 after the light pass (1 is the frame's end) */
    if (g.have_ts) vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 4);

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
    draws += draw_mesh_batches(F);
    draws += fig_record(F);
    vkCmdEndRenderPass(F->cmd);
    if (g.have_ts) vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 5);
    /* 1b. the plain figures' sun shadow map, when any is in reach */
    draws += fig_shadow_record(F, q0);

    /* 2a. gather: one full-screen triangle at the gather pass's size traces the rays and remembers what they found */
    VkRenderPassBeginInfo tbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass_t, .framebuffer = g.fb_t[g.frame_no % SV_FRAMES],
        .renderArea = { { 0, 0 }, g.trace_extent } };
    vkCmdBeginRenderPass(F->cmd, &tbi, VK_SUBPASS_CONTENTS_INLINE);
    { VkViewport tvp = { 0, 0, (float)g.trace_extent.width, (float)g.trace_extent.height, 0, 1 }; VkRect2D tsc = { { 0, 0 }, g.trace_extent };
      vkCmdSetViewport(F->cmd, 0, 1, &tvp); vkCmdSetScissor(F->cmd, 0, 1, &tsc); }
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_gather);
    vkCmdDraw(F->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(F->cmd);

    /* 2b. light: one full-screen triangle reads those buffers and what the gather pass found, and writes the lit image */
    VkRenderPassBeginInfo lbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass_l, .framebuffer = g.fb_l[g.frame_no % SV_FRAMES],
        .renderArea = { { 0, 0 }, g.extent } };
    vkCmdBeginRenderPass(F->cmd, &lbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(F->cmd, 0, 1, &vpt); vkCmdSetScissor(F->cmd, 0, 1, &sc);
    vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_light);
    vkCmdDraw(F->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(F->cmd);
    if (g.have_ts) vkCmdWriteTimestamp(F->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qpool, q0 + 6);

    /* the water's memory: this frame's depth image is cleared (nothing
     * remembered yet); last frame's writes are made visible to this frame's reads */
    if (g.water_mem) {
        int fi = (int)(g.frame_no % SV_FRAMES);
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkClearColorValue zero_c = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
        if (g.water_fresh) {
            VkImageMemoryBarrier wb[2 * SV_FRAMES];
            for (int k = 0; k < SV_FRAMES; k++) {
                wb[2 * k] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = g.whist[k].img, .subresourceRange = rr };
                wb[2 * k + 1] = wb[2 * k]; wb[2 * k + 1].image = g.wdist[k].img;
            }
            vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2 * SV_FRAMES, wb);
            for (int k = 0; k < SV_FRAMES; k++) {
                vkCmdClearColorImage(F->cmd, g.whist[k].img, VK_IMAGE_LAYOUT_GENERAL, &zero_c, 1, &rr);
                vkCmdClearColorImage(F->cmd, g.wdist[k].img, VK_IMAGE_LAYOUT_GENERAL, &zero_c, 1, &rr);
            }
            g.water_fresh = 0;
        } else {
            VkMemoryBarrier rb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
            vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &rb, 0, NULL, 0, NULL);
            vkCmdClearColorImage(F->cmd, g.wdist[fi].img, VK_IMAGE_LAYOUT_GENERAL, &zero_c, 1, &rr);
        }
        VkMemoryBarrier mb2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier(F->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb2, 0, NULL, 0, NULL);
    }

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
        uint32_t pc[2] = { base, 4u };                        /* 4: the translucent pass (quad.vert) */
        vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_blend);
        vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, pc);
        vkCmdDrawIndirect(F->cmd, g.cmds, (VkDeviceSize)base * 16, ntrans, 16); draws++;
    }
    /* spray last, over the water: the ring's live span, in one draw or, where it wraps, two */
    sp_expire(&g.ring, (float)g.time_s);
    if (sp_count(&g.ring) > 0) {
        uint32_t first = sp_first(&g.ring), n = sp_count(&g.ring);
        uint32_t run = first + n > SV_SPRAY_CAP ? SV_SPRAY_CAP - first : n;
        vkCmdBindVertexBuffers(F->cmd, 0, 1, &g.spray, &zero);
        /* shaded, then glowing (while a glowing particle may be alive): each sends the other look's particles off screen */
        for (int pass = 0; pass < (g.time_s <= g.glow_until ? 2 : 1); pass++) {
            uint32_t ppc[2] = { (uint32_t)pass, 0u };
            vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? g.pipe_spray : g.pipe_glow);
            vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, ppc);
            vkCmdDraw(F->cmd, 36, run, 0, first); draws++;      /* a cube's 36 vertices, or a square's 12 (strata_particles.h) */
            if (run < n) { vkCmdDraw(F->cmd, 36, n - run, 0, 0); draws++; }
        }
    }
    /* the 2D layer over everything: this Frame's own copy of the quads, one draw */
    if (g.ui_n > 0 && F->ui_map) {
        memcpy(F->ui_map, g.ui_cpu, (size_t)g.ui_n * SV_UI_FLOATS * 4);
        float scr[2] = { (float)g.extent.width, (float)g.extent.height };
        vkCmdBindVertexBuffers(F->cmd, 0, 1, &F->ui, &zero);
        vkCmdBindPipeline(F->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_ui);
        vkCmdPushConstants(F->cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, scr);
        vkCmdDraw(F->cmd, 4, g.ui_n, 0, 0); draws++;
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
    st_frame_begin();             /* text asked for from here on belongs to the next frame */
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
