/* Vulkan backend: draws decoded PowerVR display lists at a higher internal
 * resolution and presents the framebuffer the game is scanning out. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "cpu.h"
#include "menu.h"
#include "settings.h"
#include "pvr.h"
#include "pvr_tex.h"
#include "runtime.h"
#include "sprite_swap.h"

/* SPIR-V generated at build time by glslc -mfmt=c */
static const uint32_t pvr_vert_spv[] =
#include "pvr.vert.inc"
    ;
static const uint32_t pvr_frag_spv[] =
#include "pvr.frag.inc"
    ;
static const uint32_t blit_vert_spv[] =
#include "blit.vert.inc"
    ;
static const uint32_t blit_frag_spv[] =
#include "blit.frag.inc"
    ;
static const uint32_t ext_vert_spv[] =
#include "ext.vert.inc"
    ;
static const uint32_t ext_frag_spv[] =
#include "ext.frag.inc"
    ;

#define FRAMES 2
#define MAX_RT 4
#define EXT_MAX 6  /* background layers' margins extended at once (widescreen.c: a layer's left and right apiece) */
/* layer captures (widescreen.c): ATLAS_W map units by 480 guest rows each, polygons from ATLAS_X on in guest
 * pixels (the layouts put them there), ATLAS_SLOT apart: slot = layer x 3 + segment, 384 units a segment */
#define ATLAS_LAYERS 3
#define ATLAS_W 1024
#define ATLAS_H 480
#define ATLAS_ROWS 240
#define ATLAS_X 8192.0f
#define ATLAS_SLOT 1024.0f
#define CAP_PALS 64
#define MAX_TEX 2048
#define VRAM_PAGES (8u << 20 >> 12)

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) fatal("%s failed: %d", #x, r_); } while (0)

typedef struct {
    float scale[2];
    float zscale;
    uint32_t flags;
    float alpha_ref;
    float pad[3];
    float fog[4];
} PvrPush;

enum {
    F_TEXTURE = 1 << 0, F_OFFSET = 1 << 1, F_USE_ALPHA = 1 << 2, F_IGNORE_TEX_ALPHA = 1 << 3,
    F_ALPHA_TEST = 1 << 4, F_FOG_VERTEX = 1 << 5, F_TEXEL_BOX = 1 << 6, F_SHADE_SHIFT = 8,
};

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
} Image;

typedef struct {
    uint32_t fb_addr;      /* FB_W_SOF1 this target stands in for */
    uint64_t last_used;
    Image color, depth;
    VkDescriptorSet blit_set;
    int initialized;
} RenderTarget;

typedef struct {
    TexKey key;
    uint64_t hash;
    uint32_t page_gen;     /* VRAM write generation when last validated */
    uint64_t checked;      /* the render it was last found up to date in (the guest is stopped during one) */
    uint64_t last_used;
    Image img;
    VkDescriptorSet sets[18];
    int valid;
    int ready;             /* image holds decoded data (not just allocated) */
} Texture;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    VkBuffer vbuf;
    VkDeviceMemory vmem;
    void *vmap;
    VkDeviceSize vcap, vused;
    VkBuffer sbuf;         /* staging for texture uploads */
    VkDeviceMemory smem;
    void *smap;
    VkDeviceSize scap, sused;
    int recording;
    int atlas_layers;      /* widescreen: the layer captures this frame copied out (bits), read at its fence */
    int timed;             /* the game's render timed (timestamps 2 x frame index, read at its fence) */
} Frame;

static struct {
    SDL_Window *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkCommandPool pool;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
    VkSemaphore sc_rendered[8];
    int sc_dirty;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkPipelineLayout pvr_layout, blit_layout;
    VkPipeline pipelines[2][8][8];  /* [blend enabled][src][dst] */
    VkPipeline blit_pipeline, overlay_pipeline;
    Image overlay;            /* settings menu canvas */
    VkDescriptorSet overlay_set;
    int overlay_ready;
    VkShaderModule pvr_vs, pvr_fs, blit_vs, blit_fs, ext_vs, ext_fs;
    /* 16:9 background extension (widescreen.c): a background layer drawn alone, and the pipeline that continues
     * it past its art's edge */
    VkPipelineLayout ext_layout;
    VkPipeline ext_pipeline;

    /* a stage's layers captured alone over their maps (widescreen.c's filler analysis), and its extension fill */
    Image atlas_img[ATLAS_LAYERS], atlas_depth;
    VkBuffer atlas_buf;
    VkDeviceMemory atlas_mem;
    void *atlas_map;
    Image mask_img, fill_img;  /* the 16:9 extension's fill (widescreen.c): modes and palettes (point-sampled), colours */
    /* each layer's palettes as captured: the fill takes each texel's from the art it continues, and follows their
     * colours (a stage's palette animation: flames, lights, signs; ext.frag) */
    struct {
        int n;
        uint32_t base[CAP_PALS], count[CAP_PALS];
        float hist[CAP_PALS][256], mean[CAP_PALS][3];
    } cap_pal[ATLAS_LAYERS];
    uint8_t cap_palmap[ATLAS_LAYERS][ATLAS_W * ATLAS_ROWS];  /* each capture texel's palette: cap_pal's + 1, 0 none */
    float pal_table[ATLAS_LAYERS * 2][CAP_PALS][4];          /* their colour now against the capture's: times, plus */
    uint32_t pal_ram[1024];                                  /* the palette RAM the table was worked out from (those entries) */
    int pal_dirty;
    Image pal_img;
    VkDescriptorSet mask_set, fill_set, pal_set;
    float cap_shade[ATLAS_LAYERS][6];  /* each layer's shading as captured (poly_shading) */
    uint8_t *mask_pending;     /* a new fill to upload: the colours (RGBA), then the modes */
    VkQueryPool timestamps;    /* 2 a frame: the game's render's start and end on the GPU (perf log) */
    float ts_period;           /* ns per timestamp tick */
    VkSampler samplers[18];
    VkSampler blit_sampler;
    Frame frames[FRAMES];
    unsigned fi;
    RenderTarget rts[MAX_RT];
    Texture tex[MAX_TEX];
    Image white;
    VkDescriptorSet white_set;
    uint32_t height;          /* the render targets' height in pixels (the RENDER RES setting) */
    uint64_t frame_no;
    uint32_t page_gen[VRAM_PAGES];
    uint32_t gen_counter;
    float pic_w;              /* the picture's width in guest pixels: 640, wider with widescreen (widescreen.c) */
    int shot_every;
    int sc_can_read;          /* swapchain images can be copied out (presented screenshots) */
    uint64_t frame_ns;        /* vsync at ~60 Hz: frame period of the display, else 0 */
    uint64_t shot_from;
    uint32_t *decode_buf;
} R;

/* the render targets' width in pixels: the picture's shape at the RENDER RES height (16:9 at 2160: 3840) */
static uint32_t rt_width(void) { return (uint32_t)(R.pic_w * (float)R.height / 480.0f + 0.5f); }

/* ---------------- helpers ---------------- */

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(R.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    fatal("no suitable Vulkan memory type");
}

/* a host-visible buffer, mapped; one the GPU only writes (TRANSFER_DST: read back by the CPU, the layer captures and
 * screenshots) in cached memory where there is some: the CPU reads uncached memory a transfer at a time (a layer
 * capture took 0.1 s to read) */
static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem,
                        void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKCHECK(vkCreateBuffer(R.dev, &bi, NULL, buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(R.dev, *buf, &mr);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    if (usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(R.phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                    (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                type = i;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = type != UINT32_MAX ? type : find_mem(mr.memoryTypeBits, want)};
    VKCHECK(vkAllocateMemory(R.dev, &ai, NULL, mem));
    VKCHECK(vkBindBufferMemory(R.dev, *buf, *mem, 0));
    VKCHECK(vkMapMemory(R.dev, *mem, 0, size, 0, map));
}

static void make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                       Image *img) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(R.dev, &ii, NULL, &img->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(R.dev, img->image, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = find_mem(mr.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHECK(vkAllocateMemory(R.dev, &ai, NULL, &img->mem));
    VKCHECK(vkBindImageMemory(R.dev, img->image, img->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = img->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    VKCHECK(vkCreateImageView(R.dev, &vi, NULL, &img->view));
}

static void destroy_image(Image *img) {
    if (!img->image) return;
    vkDestroyImageView(R.dev, img->view, NULL);
    vkDestroyImage(R.dev, img->image, NULL);
    vkFreeMemory(R.dev, img->mem, NULL);
    memset(img, 0, sizeof *img);
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                    VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &di);
}

static VkShaderModule shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m;
    VKCHECK(vkCreateShaderModule(R.dev, &ci, NULL, &m));
    return m;
}

static VkDescriptorSet alloc_set(VkImageView view, VkSampler sampler) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = R.desc_pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &R.set_layout};
    VkDescriptorSet set;
    VKCHECK(vkAllocateDescriptorSets(R.dev, &ai, &set));
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(R.dev, 1, &w, 0, NULL);
    return set;
}

static void free_set(VkDescriptorSet *s) {
    if (*s) vkFreeDescriptorSets(R.dev, R.desc_pool, 1, s);
    *s = VK_NULL_HANDLE;
}

/* ---------------- swapchain ---------------- */

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < R.sc_count; i++) {
        vkDestroyImageView(R.dev, R.sc_views[i], NULL);
        vkDestroySemaphore(R.dev, R.sc_rendered[i], NULL);
    }
    if (R.swapchain) vkDestroySwapchainKHR(R.dev, R.swapchain, NULL);
    R.swapchain = VK_NULL_HANDLE;
    R.sc_count = 0;
}

static void create_swapchain(void) {
    vkDeviceWaitIdle(R.dev);
    destroy_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.phys, R.surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &n, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (n > 64) n = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &n, fmts);
    /* The picture is already sRGB-encoded: present it to an 8-bit UNORM image
     * in the standard sRGB colour space. Desktops with colour management or
     * HDR (e.g. KDE on Wayland) also offer linear / extended colour spaces for
     * the same formats; picking one of those makes the compositor read our
     * values as linear light, and the whole picture comes out washed out. */
    VkSurfaceFormatKHR fmt = fmts[0];
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        int unorm = fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM;
        int srgb_space = fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        int score = unorm * 2 + srgb_space * 4;
        if (score > best) { best = score; fmt = fmts[i]; }
    }
    static int logged;
    if (!logged++) {
        for (uint32_t i = 0; i < n; i++) logf_("surface format %u: format=%d colorspace=%d", i, fmts[i].format, fmts[i].colorSpace);
        logf_("swapchain format=%d colorspace=%d", fmt.format, fmt.colorSpace);
    }
    R.sc_format = fmt.format;
    R.sc_can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    int w, h;
    SDL_GetWindowSizeInPixels(R.window, &w, &h);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (ext.width == 0 || ext.height == 0) ext = (VkExtent2D){640, 480};
    R.sc_extent = ext;
    logf_("swapchain: %ux%u", ext.width, ext.height);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = R.surface,
                                   .minImageCount = count,
                                   .imageFormat = fmt.format,
                                   .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                 (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                                   .preTransform = caps.currentTransform,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                   .clipped = VK_TRUE};
    uint32_t pm_n = 8;
    VkPresentModeKHR pms[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &pm_n, pms);
    /* vsync on: FIFO (wait for the display). Off: mailbox (no tearing, no waiting),
     * else immediate; FIFO is the only mode every driver must offer. */
    int has_mailbox = 0, has_immediate = 0;
    for (uint32_t i = 0; i < pm_n; i++) {
        has_mailbox |= pms[i] == VK_PRESENT_MODE_MAILBOX_KHR;
        has_immediate |= pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    /* VSync on: the game's frame clock follows the display's refresh rate when
     * it is close to the game's 59.94 Hz (irq.c), and frames go out through
     * mailbox: one game frame per refresh, no tearing, and the game never waits
     * on the display. VSync off: the original 59.94 Hz with immediate
     * presentation (lowest latency, may tear). FIFO is the fallback every
     * driver has. */
    float hz = 0;
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(R.window));
    if (dm) hz = dm->refresh_rate;
    R.frame_ns = g_settings.vsync && hz >= 58.5f && hz <= 61.5f ? (uint64_t)(1e9 / hz) : 0;
    if (g_settings.vsync)
        ci.presentMode = has_mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    else
        ci.presentMode = has_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                         : has_mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                       : VK_PRESENT_MODE_FIFO_KHR;
    VKCHECK(vkCreateSwapchainKHR(R.dev, &ci, NULL, &R.swapchain));
    logf_("swapchain present mode: %s, frame clock %.3f Hz (display %.2f Hz)",
          ci.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate",
          R.frame_ns ? 1e9 / R.frame_ns : 59.94, hz);
    R.sc_count = 8;
    vkGetSwapchainImagesKHR(R.dev, R.swapchain, &R.sc_count, R.sc_images);
    for (uint32_t i = 0; i < R.sc_count; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = R.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = R.sc_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHECK(vkCreateImageView(R.dev, &vi, NULL, &R.sc_views[i]));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(R.dev, &si, NULL, &R.sc_rendered[i]));
    }
    R.sc_dirty = 0;
}

/* ---------------- pipelines ---------------- */

static const VkBlendFactor s_blend_src[8] = {
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
static const VkBlendFactor s_blend_dst[8] = {
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};

static VkPipeline pvr_pipeline(int blend, int src, int dst) {
    VkPipeline *slot = &R.pipelines[blend][src][dst];
    if (*slot) return *slot;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = R.pvr_vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = R.pvr_fs, .pName = "main"}};
    VkVertexInputBindingDescription bind[2] = {{0, sizeof(PvrVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                               {1, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PvrVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PvrVertex, u)},
        {2, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(PvrVertex, base)},
        {3, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(PvrVertex, offset)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};  /* its polygon's texture coordinates' box (uv_boxes) */
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 2,
                                               .pVertexBindingDescriptions = bind,
                                               .vertexAttributeDescriptionCount = 5,
                                               .pVertexAttributeDescriptions = attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = s_blend_src[src],
        .dstColorBlendFactor = s_blend_dst[dst],
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = s_blend_src[src],
        .dstAlphaBlendFactor = s_blend_dst[dst],
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cb};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn};
    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &color_fmt,
                                        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cbs,
                                       .pDynamicState = &dy,
                                       .layout = R.pvr_layout};
    VKCHECK(vkCreateGraphicsPipelines(R.dev, VK_NULL_HANDLE, 1, &pi, NULL, slot));
    return *slot;
}

/* 16:9 background extension: a quad over the picture's columns a background layer lacks art in, its fill from the
 * stage's precomputed images (ext.frag) at a depth just behind or just over the layer's: it writes depth, so that
 * nearer layers cover it and it covers farther ones */
static void create_ext_pipeline(void) {
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
    VkDescriptorSetLayout sl[3] = {R.set_layout, R.set_layout, R.set_layout};  /* the fill's colours, its modes, palettes */
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                       .pSetLayouts = sl, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pr};
    VKCHECK(vkCreatePipelineLayout(R.dev, &plci, NULL, &R.ext_layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = R.ext_vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = R.ext_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL};
    VkPipelineColorBlendAttachmentState cb = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cb};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_fmt,
                                        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri, .stageCount = 2, .pStages = stages,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = R.ext_layout};
    VKCHECK(vkCreateGraphicsPipelines(R.dev, VK_NULL_HANDLE, 1, &pi, NULL, &R.ext_pipeline));
}

static void create_blit_pipeline(int blend, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = R.blit_vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = R.blit_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cb = {.colorWriteMask = 0xF};
    if (blend) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cb};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &R.sc_format};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri, .stageCount = 2, .pStages = stages,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                       .pDynamicState = &dy, .layout = R.blit_layout};
    VKCHECK(vkCreateGraphicsPipelines(R.dev, VK_NULL_HANDLE, 1, &pi, NULL, out));
}

/* ---------------- init ---------------- */

static void create_samplers(void) {
    /* index = filter * 9 + addr_u * 3 + addr_v; addr: 0 repeat, 1 mirror, 2 clamp */
    static const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int f = 0; f < 2; f++)
        for (int u = 0; u < 3; u++)
            for (int v = 0; v < 3; v++) {
                VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .magFilter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .minFilter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .addressModeU = modes[u],
                                          .addressModeV = modes[v],
                                          .addressModeW = modes[2],
                                          .maxLod = 0};
                VKCHECK(vkCreateSampler(R.dev, &si, NULL, &R.samplers[f * 9 + u * 3 + v]));
            }
    VkSamplerCreateInfo bi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VKCHECK(vkCreateSampler(R.dev, &bi, NULL, &R.blit_sampler));
}

static void alloc_vbuf(Frame *f) {
    f->vcap = 16u << 20;
    make_buffer(f->vcap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &f->vbuf, &f->vmem, &f->vmap);
}

int renderer_init(void *window) {
    R.window = window;
    R.height = (uint32_t)g_settings.render_height;
    if (R.height < 240 || R.height > 4320) R.height = 1440;
    R.pic_w = 640;
    const char *shot = getenv("SFA3_SHOT_EVERY");
    R.shot_every = shot ? atoi(shot) : 0;
    const char *shot_from = getenv("SFA3_SHOT_FROM");
    R.shot_from = shot_from ? strtoull(shot_from, NULL, 10) : 0;

    Uint32 n_ext = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&n_ext);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "SFA3 Native",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = exts};
    VKCHECK(vkCreateInstance(&ici, NULL, &R.instance));
    if (!SDL_Vulkan_CreateSurface(window, R.instance, NULL, &R.surface)) fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());

    uint32_t np = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(R.instance, &np, pds);
    for (uint32_t i = 0; i < np && !R.phys; i++) {
        uint32_t nq = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, R.surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                R.phys = pds[i];
                R.qfam = q;
                break;
            }
        }
    }
    if (!R.phys) fatal("no Vulkan device with graphics + present");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(R.phys, &props);
    logf_("vulkan: %s, internal resolution %ux%u", props.deviceName, rt_width(), R.height);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = R.qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                              .ppEnabledExtensionNames = dexts};
    VKCHECK(vkCreateDevice(R.phys, &dci, NULL, &R.dev));
    vkGetDeviceQueue(R.dev, R.qfam, 0, &R.queue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = R.qfam};
    VKCHECK(vkCreateCommandPool(R.dev, &pci, NULL, &R.pool));

    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &b};
    VKCHECK(vkCreateDescriptorSetLayout(R.dev, &lci, NULL, &R.set_layout));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                      .maxSets = 16384, .poolSizeCount = 1, .pPoolSizes = &ps};
    VKCHECK(vkCreateDescriptorPool(R.dev, &dpi, NULL, &R.desc_pool));

    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PvrPush)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &R.set_layout, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pr};
    VKCHECK(vkCreatePipelineLayout(R.dev, &plci, NULL, &R.pvr_layout));
    VkPushConstantRange bpr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    plci.pPushConstantRanges = &bpr;
    VKCHECK(vkCreatePipelineLayout(R.dev, &plci, NULL, &R.blit_layout));

    R.pvr_vs = shader(pvr_vert_spv, sizeof pvr_vert_spv);
    R.pvr_fs = shader(pvr_frag_spv, sizeof pvr_frag_spv);
    R.blit_vs = shader(blit_vert_spv, sizeof blit_vert_spv);
    R.blit_fs = shader(blit_frag_spv, sizeof blit_frag_spv);
    R.ext_vs = shader(ext_vert_spv, sizeof ext_vert_spv);
    R.ext_fs = shader(ext_frag_spv, sizeof ext_frag_spv);
    create_samplers();
    create_ext_pipeline();
    {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = FRAMES * 2};
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(R.phys, &pp);
        R.ts_period = pp.limits.timestampPeriod;
        if (pp.limits.timestampComputeAndGraphics) vkCreateQueryPool(R.dev, &qi, NULL, &R.timestamps);
    }
    create_swapchain();
    create_blit_pipeline(0, &R.blit_pipeline);
    create_blit_pipeline(1, &R.overlay_pipeline);

    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &R.frames[i];
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = R.pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(R.dev, &cai, &f->cmd));
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VKCHECK(vkCreateFence(R.dev, &fci, NULL, &f->fence));
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(R.dev, &sci, NULL, &f->acquired));
        alloc_vbuf(f);
        f->scap = 64u << 20;
        make_buffer(f->scap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &f->sbuf, &f->smem, &f->smap);
    }
    R.decode_buf = malloc(1024 * 1024 * 4);
    return 1;
}

/* ---------------- frame recording ---------------- */

static Frame *frame_begin(void) {
    Frame *f = &R.frames[R.fi];
    if (f->recording) return f;
    uint64_t t0 = SDL_GetTicksNS();
    vkWaitForFences(R.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    g_perf.gpu_wait_ns += SDL_GetTicksNS() - t0;
    vkResetFences(R.dev, 1, &f->fence);
    if (f->timed) {  /* the GPU's time on the game's render in this frame */
        uint64_t ts[2];
        if (vkGetQueryPoolResults(R.dev, R.timestamps, (uint32_t)(f - R.frames) * 2, 2, sizeof ts, ts, sizeof ts[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
            g_perf.gpu_ns += (uint64_t)((double)(ts[1] - ts[0]) * R.ts_period);
            g_perf.gpu_frames++;
        }
        f->timed = 0;
    }
    if (f->atlas_layers) {  /* layer captures this frame made, done now (widescreen.c) */
        void widescreen_atlas_ready(int layers, const uint8_t *rgba, const uint8_t *pal, int w, int h);
        uint64_t t0 = SDL_GetTicksNS();
        widescreen_atlas_ready(f->atlas_layers, R.atlas_map, &R.cap_palmap[0][0], ATLAS_W, ATLAS_H);
        g_perf.wide_ns += SDL_GetTicksNS() - t0;
        for (int k = 0; k < ATLAS_LAYERS; k++)
            if (f->atlas_layers & (1 << k)) g_perf.upload_bytes += (uint64_t)ATLAS_W * ATLAS_H * 4;
        f->atlas_layers = 0;
    }
    vkResetCommandBuffer(f->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(f->cmd, &bi);
    f->vused = 0;
    f->sused = 0;
    f->recording = 1;
    if (!R.white.image) {
        /* 1x1 white texture for untextured polys (descriptor must be valid) */
        make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, &R.white);
        memset(f->smap, 0xFF, 4);
        barrier(f->cmd, R.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
        vkCmdCopyBufferToImage(f->cmd, f->sbuf, R.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(f->cmd, R.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        f->sused = 256;
        R.white_set = alloc_set(R.white.view, R.samplers[0]);
    }
    return f;
}

/* Game frame period to use (ns), or 0 for the Dreamcast's own 59.94 Hz. */
uint64_t renderer_frame_ns(void) { return R.frame_ns; }

/* Settings that need GPU objects recreated (vsync: swapchain; render scale:
 * render targets). Applied between frames. */
static int s_rt_rebuild;
static int s_margin_wanted;  /* widescreen: the picture's margin wanted (units; PICTURE 16:9) */
int widescreen_margin(void);
float widescreen_margin_px(void);
int widescreen_wanted(void);
void widescreen_set(int units);
#include "widescreen.h"
void renderer_settings_changed(void) {
    R.sc_dirty = 1;
    if ((uint32_t)g_settings.render_height != R.height) s_rt_rebuild = 1;
}

static void rebuild_render_targets(void) {
    vkDeviceWaitIdle(R.dev);
    for (int i = 0; i < MAX_RT; i++) {
        RenderTarget *rt = &R.rts[i];
        free_set(&rt->blit_set);
        destroy_image(&rt->color);
        destroy_image(&rt->depth);
        memset(rt, 0, sizeof *rt);
    }
    R.height = (uint32_t)g_settings.render_height;
    widescreen_set(s_margin_wanted);
    R.pic_w = 640.0f + 2.0f * widescreen_margin_px();
    s_rt_rebuild = 0;
    logf_("render resolution now %ux%u", rt_width(), R.height);
}

static RenderTarget *get_rt(Frame *f, uint32_t fb_addr) {
    RenderTarget *best = NULL;
    for (int i = 0; i < MAX_RT; i++)
        if (R.rts[i].color.image && R.rts[i].fb_addr == fb_addr) { best = &R.rts[i]; break; }
    if (!best) {
        for (int i = 0; i < MAX_RT; i++)
            if (!best || R.rts[i].last_used < best->last_used) best = &R.rts[i];
        if (!best->color.image) {
            uint32_t w = rt_width(), h = R.height;
            make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &best->color);
            make_image(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, &best->depth);
            best->blit_set = alloc_set(best->color.view, R.blit_sampler);
            best->initialized = 0;
        }
        best->fb_addr = fb_addr;
    }
    best->last_used = R.frame_no;
    (void)f;
    return best;
}

/* SFA3_VRAM_WATCH=start,end (64-bit offsets) logs writes into that range. */
extern uint64_t g_vblank_count;
static void vram_watch(const char *how, uint32_t o64, uint32_t bytes) {
    static int init, n;
    static uint32_t lo, hi;
    if (!init) {
        init = 1;
        const char *e = getenv("SFA3_VRAM_WATCH");
        if (e) sscanf(e, "%x,%x", &lo, &hi);
    }
    if (hi && g_vblank_count > 500 && o64 < hi && o64 + bytes > lo && n++ < 2000)
        logf_("vram watch: %s %06X+%X vbl=%llu pr=%08X", how, o64, bytes, (unsigned long long)g_vblank_count, g.pr);
}

/* VRAM write tracking for the texture cache */
void renderer_vram_written(uint32_t p, int size) {
    uint32_t off = p & 0x7FFFFF;
    if (p & 0x01000000) off = 0;  /* 32-bit area writes: be conservative below */
    R.gen_counter++;
    if (p & 0x01000000) {
        /* 32-bit path interleaves banks; mark both candidate pages */
        uint32_t o = p & 0x7FFFFF;
        uint32_t bank = (o >> 22) & 1;
        uint32_t o64 = ((o & 0x3FFFFC) << 1) | (bank << 2);
        R.page_gen[o64 >> 12] = R.gen_counter;
        vram_watch("cpu32", o64, 4);
        return;
    }
    vram_watch("cpu64", off, (uint32_t)size);
    R.page_gen[off >> 12] = R.gen_counter;
    if (size > 1) R.page_gen[((off + (uint32_t)size - 1) & 0x7FFFFF) >> 12] = R.gen_counter;
}

void renderer_vram_block_written(uint32_t off64, uint32_t bytes) {
    vram_watch("block", off64, bytes);
    R.gen_counter++;
    for (uint32_t p = off64 >> 12; p <= ((off64 + bytes - 1) & 0x7FFFFF) >> 12; p++) R.page_gen[p] = R.gen_counter;
}

static uint32_t range_gen(uint32_t addr, uint32_t size) {
    uint32_t m = 0;
    for (uint32_t p = addr >> 12; p <= ((addr + size - 1) & 0x7FFFFF) >> 12; p++)
        if (R.page_gen[p] > m) m = R.page_gen[p];
    return m;
}

/* RGBA8 pixels -> the texture's image (staging space must have been checked) */
static void upload_pixels(Frame *f, Texture *t, uint32_t w, uint32_t h, const uint32_t *px) {
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    memcpy((uint8_t *)f->smap + f->sused, px, bytes);
    barrier(f->cmd, t->img.image, VK_IMAGE_ASPECT_COLOR_BIT,
            t->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.bufferOffset = f->sused,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(f->cmd, f->sbuf, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(f->cmd, t->img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    f->sused = (f->sused + bytes + 255) & ~255ull;
    t->ready = 1;
}

/* The port's own images (sprite_swap.c), drawn by polygons whose texture
 * word is SPRITE_SWAP_TCW(id): uploaded once. */
static Texture *custom_texture(Frame *f, uint32_t id) {
    static Texture tex[32];
    uint32_t w, h;
    const uint32_t *px = sprite_swap_image(id, &w, &h);
    if (id >= 32 || !px) return NULL;
    Texture *t = &tex[id];
    if (t->ready) return t;
    if (f->sused + (VkDeviceSize)w * h * 4 > f->scap) return NULL;
    make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, &t->img);
    upload_pixels(f, t, w, h, px);
    return t;
}

static Texture *get_texture(Frame *f, uint32_t tcw, uint32_t tsp) {
    if (SPRITE_SWAP_IS_TCW(tcw)) return custom_texture(f, SPRITE_SWAP_ID(tcw));
    TexKey key;
    pvr_tex_info(tcw, tsp, &key);
    if (key.w > 1024 || key.h > 1024 || !key.w || !key.h) return NULL;
    Texture *t = NULL, *victim = NULL;
    for (int i = 0; i < MAX_TEX; i++) {
        Texture *c = &R.tex[i];
        if (c->valid && c->key.tcw == key.tcw && c->key.w == key.w && c->key.h == key.h &&
            c->key.pal == key.pal) { t = c; break; }
        if (!victim || (!c->valid && victim->valid) || (c->valid == victim->valid && c->last_used < victim->last_used))
            victim = c;
    }
    int upload = 0;
    if (t && t->checked != R.frame_no) {  /* once a render: VRAM and the palettes can't change during one, and a
                                            * paletted texture is hashed whole (a stage's tiles: 57 MB a frame) */
        uint32_t gen = range_gen(key.addr, key.size);
        if (gen != t->page_gen || key.paletted) {
            uint64_t h = pvr_tex_hash(&key);
            t->page_gen = gen;
            if (h != t->hash) { t->hash = h; upload = 1; }
        }
    } else if (!t) {
        t = victim;
        if (t->valid && t->last_used == R.frame_no) return NULL;  /* cache full this frame */
        destroy_image(&t->img);
        for (int s = 0; s < 18; s++) free_set(&t->sets[s]);
        memset(t, 0, sizeof *t);
        t->key = key;
        t->valid = 1;
        t->hash = pvr_tex_hash(&key);
        t->page_gen = range_gen(key.addr, key.size);
        make_image(key.w, key.h, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &t->img);
        upload = 2;
    }
    t->last_used = R.frame_no;
    if (upload) {
        VkDeviceSize bytes = (VkDeviceSize)key.w * key.h * 4;
        if (f->sused + bytes > f->scap) {
            /* staging full: force a retry next frame */
            t->page_gen = ~0u;
            t->hash = 0;
            return t->ready ? t : NULL;
        }
        uint64_t t0 = SDL_GetTicksNS();
        /* decode in ordinary memory: the staging buffer is write-combined
         * (the decoder reads back what it wrote, which is slow there) */
        pvr_tex_decode(&key, R.decode_buf);
        g_perf.tex_ns += SDL_GetTicksNS() - t0;
        g_perf.tex_decodes++;
        upload_pixels(f, t, key.w, key.h, R.decode_buf);
    }
    t->checked = R.frame_no;
    return t;
}

/* each vertex: its polygon's texture coordinates' box (u0, v0, u1, v1), for pvr.frag (F_TEXEL_BOX). The game puts
 * a quad's corners a hair past whole texels (16.0064 at a 16-texel tile's right edge, of a 64-texel texture); at
 * 640x480 no pixel centre comes within that hair of the edge, but scaled by a fraction (4.5 at 2160) one can: it
 * took the next tile's texels, a one-pixel line down the quad */
static void uv_boxes(const PvrFrame *pf, float *box) {
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        if (!p->vertex_count || p->first_vertex + p->vertex_count > pf->nverts) continue;
        const PvrVertex *v = &pf->verts[p->first_vertex];
        float b[4] = {v->u, v->v, v->u, v->v};
        for (uint32_t k = 1; k < p->vertex_count; k++) {
            b[0] = fminf(b[0], v[k].u), b[1] = fminf(b[1], v[k].v);
            b[2] = fmaxf(b[2], v[k].u), b[3] = fmaxf(b[3], v[k].v);
        }
        for (uint32_t k = 0; k < p->vertex_count; k++) memcpy(&box[4 * (p->first_vertex + k)], b, sizeof b);
    }
}

static int sampler_index(uint32_t tsp) {
    int filter = ((tsp >> 13) & 3) != 0 && !g_settings.nearest;
    int flip_u = (tsp >> 18) & 1, flip_v = (tsp >> 17) & 1;
    int clamp_u = (tsp >> 16) & 1, clamp_v = (tsp >> 15) & 1;
    int au = clamp_u ? 2 : flip_u ? 1 : 0, av = clamp_v ? 2 : flip_v ? 1 : 0;
    return filter * 9 + au * 3 + av;
}

static const VkCompareOp s_depth_op[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                          VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                          VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                          VK_COMPARE_OP_ALWAYS};

static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
const uint32_t *pvr_palette(void);

/* SFA3_DUMP_RENDER=N logs every polygon of the Nth render (debug aid). */
static long s_dump_target = -2, s_dump_count, s_dump_n = 1;
/* the "dumprender [n]" control command: the next n renders */
void renderer_dump_next(int n) { s_dump_target = s_dump_count, s_dump_n = n > 0 ? n : 1; }
static void dump_render(const PvrFrame *pf) {
    long target;
    if (s_dump_target == -2) { const char *e = getenv("SFA3_DUMP_RENDER"); s_dump_target = e ? atol(e) : -1; }
    target = s_dump_target;
    long at = s_dump_count++;
    if (target < 0 || at < target || at >= target + s_dump_n) return;
    logf_("dump render %ld: %u polys", at, pf->npolys);
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (uint32_t k = 0; k < p->vertex_count; k++) {
            const PvrVertex *v = &pf->verts[p->first_vertex + k];
            if (v->x < x0) x0 = v->x; if (v->x > x1) x1 = v->x;
            if (v->y < y0) y0 = v->y; if (v->y > y1) y1 = v->y;
            if (v->u < u0) u0 = v->u; if (v->u > u1) u1 = v->u;
            if (v->v < v0) v0 = v->v; if (v->v > v1) v1 = v->v;
        }
        if (SPRITE_SWAP_IS_TCW(p->tcw)) {  /* a sprite drawn from its swapped textures: where only */
            logf_("  #%u list=%d sprite tcw=%08X box=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.3f,%.3f)-(%.3f,%.3f)", i, p->list,
                  p->tcw, x0, y0, x1, y1, u0, v0, u1, v1);
            continue;
        }
        TexKey key;
        pvr_tex_info(p->tcw, p->tsp, &key);
        int seen = 0;
        for (uint32_t j = 0; j < i; j++) seen |= pf->polys[j].tcw == p->tcw;
        if (!seen && (p->isp & (1u << 25)) && key.w && key.w <= 1024 && key.h <= 1024) {
            uint32_t *px = malloc((size_t)key.w * key.h * 4);
            pvr_tex_decode(&key, px);
            char path[128];
            snprintf(path, sizeof path, "%s/tex_%08X.png", g_data_dir, p->tcw);
            write_png(path, (const uint8_t *)px, key.w, key.h);
            free(px);
        }
        if (!seen && key.paletted) {
            const uint32_t *pal = pvr_palette();
            char b[200];
            int k = 0;
            for (int j = 0; j < 16; j++) k += snprintf(b + k, sizeof b - k, " %04X", pal[key.pal + j] & 0xFFFF);
            logf_("  palette ctrl=%u base=%u:%s", pvr_reg(PVR_PAL_RAM_CTRL) & 3, key.pal, b);
        }
        logf_("  z=%f", pf->verts[p->first_vertex].z);
        logf_("  #%u list=%d n=%u isp=%08X tsp=%08X tcw=%08X tex=%ux%u@%06X box=(%.0f,%.0f)-(%.0f,%.0f) uv=(%.3f,%.3f)-(%.3f,%.3f) col=%08X",
              i, p->list, p->vertex_count, p->isp, p->tsp, p->tcw, key.w, key.h, key.addr, x0, y0, x1, y1,
              u0, v0, u1, v1, pf->verts[p->first_vertex].base);
        if (!(p->isp & (1u << 25)) && p->vertex_count <= 8) {
            char b[256];
            int k = 0;
            for (uint32_t j = 0; j < p->vertex_count; j++)
                k += snprintf(b + k, sizeof b - k, " (%.3f,%.3f)", pf->verts[p->first_vertex + j].x, pf->verts[p->first_vertex + j].y);
            logf_("    verts:%s", b);
        }
    }
}

/* the frame's polygons, in the PowerVR's order: opaque, punch-through (submission order), translucent (tr_order);
 * zonly > 0: only those at that depth (one background layer: widescreen's extension) */
static void draw_polys(VkCommandBuffer cmd, const PvrFrame *pf, const VkDescriptorSet *sets, const uint32_t *tr_order,
                       uint32_t ntr, int autosort, PvrPush push, VkRect2D full, float xoff, float gw, float gh,
                       uint32_t W, uint32_t H, float zonly, const int8_t *atlas, int atlas_layer) {
    static const int order[3] = {LIST_OPAQUE, LIST_PUNCH, LIST_TRANS};
    VkPipeline cur_pipe = VK_NULL_HANDLE;
    VkDescriptorSet cur_set = VK_NULL_HANDLE;
    int cur_cull = -1, cur_zop = -1, cur_zw = -1;
    for (int pass = 0; pass < 3; pass++) {
        int list = order[pass];
        uint32_t count = list == LIST_TRANS ? ntr : pf->npolys;
        for (uint32_t j = 0; j < count; j++) {
            uint32_t i = list == LIST_TRANS ? tr_order[j] : j;
            const PvrPoly *p = &pf->polys[i];
            if ((int)p->list != list || p->vertex_count < 3) continue;
            if (atlas && atlas[i] != atlas_layer) continue;  /* a layer capture's (widescreen.c) or not */
            if (zonly > 0) {
                float z = pf->verts[p->first_vertex].z;
                if (z < zonly * 0.999f || z > zonly * 1.001f) continue;
            }
            uint32_t isp = p->isp, tsp = p->tsp;
            int blend = list == LIST_TRANS;
            int src = blend ? (tsp >> 29) & 7 : 1, dst = blend ? (tsp >> 26) & 7 : 0;
            VkPipeline pipe = pvr_pipeline(blend, src, dst);
            if (pipe != cur_pipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                cur_pipe = pipe;
                cur_set = VK_NULL_HANDLE;
            }
            if (sets[i] != cur_set) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pvr_layout, 0, 1, &sets[i], 0, NULL);
                cur_set = sets[i];
            }
            int zop = (int)(isp >> 29) & 7;
            int zwrite = !((isp >> 26) & 1);
            if (list == LIST_TRANS) {
                zwrite = 0;
                if (autosort) zop = 6;  /* autosort forces GREATER_EQUAL */
            }
            if (zop != cur_zop) { vkCmdSetDepthCompareOp(cmd, s_depth_op[zop]); cur_zop = zop; }
            if (zwrite != cur_zw) { vkCmdSetDepthWriteEnable(cmd, (VkBool32)zwrite); cur_zw = zwrite; }
            if (cur_cull < 0) { vkCmdSetDepthTestEnable(cmd, VK_TRUE); }
            int cull = (isp >> 27) & 3;
            if (cull != cur_cull) {
                vkCmdSetCullMode(cmd, cull == 2 ? VK_CULL_MODE_BACK_BIT : cull == 3 ? VK_CULL_MODE_FRONT_BIT
                                                                                  : VK_CULL_MODE_NONE);
                cur_cull = cull;
            }
            uint32_t flags = 0;
            if (isp & (1u << 25)) flags |= F_TEXTURE;
            if (isp & (1u << 24)) flags |= F_OFFSET;
            if (tsp & (1u << 20)) flags |= F_USE_ALPHA;
            if (tsp & (1u << 19)) flags |= F_IGNORE_TEX_ALPHA;
            if (list == LIST_PUNCH) flags |= F_ALPHA_TEST;
            /* a layer capture (widescreen.c) records what the PowerVR shows: an opaque-list texel is opaque */
            if (atlas_layer >= 0 && list == LIST_OPAQUE) flags |= F_IGNORE_TEX_ALPHA;
            if (((tsp >> 22) & 3) == 1) flags |= F_FOG_VERTEX;
            /* point-sampled: its own texels only (pvr.frag); the layer captures as they were */
            if ((flags & F_TEXTURE) && atlas_layer < 0 && sampler_index(tsp) < 9) flags |= F_TEXEL_BOX;
            flags |= ((tsp >> 6) & 3) << F_SHADE_SHIFT;
            push.flags = flags;
            vkCmdPushConstants(cmd, R.pvr_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof push, &push);
            /* user tile clip (inside mode) */
            if (p->tile_clip == 2) {
                float x0 = (float)(p->clip[0] * 32) + xoff, x1 = (float)((p->clip[2] + 1) * 32) + xoff;
                int y0 = p->clip[1] * 32, y1 = (p->clip[3] + 1) * 32;
                float sx = W / (gw + 2 * xoff), sy = H / gh;
                int32_t sx0 = (int32_t)(x0 * sx), sx1 = (int32_t)(x1 * sx);  /* within the bars */
                sx0 = sx0 < full.offset.x ? full.offset.x : sx0;
                sx1 = sx1 > full.offset.x + (int32_t)full.extent.width ? full.offset.x + (int32_t)full.extent.width : sx1;
                VkRect2D sc = {{sx0, (int32_t)(y0 * sy)}, {(uint32_t)(sx1 > sx0 ? sx1 - sx0 : 0), (uint32_t)((y1 - y0) * sy)}};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, p->vertex_count, 1, p->first_vertex, 0);
                vkCmdSetScissor(cmd, 0, 1, &full);
            } else {
                vkCmdDraw(cmd, p->vertex_count, 1, p->first_vertex, 0);
            }
        }
    }
}


void renderer_filler_masks(const uint8_t *modes, const uint32_t *fill) {
    size_t n = (size_t)WIDE_MASK_W * WIDE_MASK_ROWS * 3;
    free(R.mask_pending);
    R.mask_pending = malloc(n * 6);  /* the colours, then the modes and palettes */
    if (!R.mask_pending) return;
    memcpy(R.mask_pending, fill, n * 4);
    memcpy(R.mask_pending + n * 4, modes, n * 2);
}

/* a palette's colour, its entries weighted by how much its textures use each (cap_pal): RGB 0-1 */
static void pal_mean(uint32_t base, uint32_t count, const float *hist, float *out) {
    double sum[3] = {0, 0, 0}, wsum = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (hist[i] <= 0) continue;
        uint32_t c = pvr_pal_color(base + i);
        double w = hist[i] * (double)(c >> 24) / 255.0;  /* (transparent entries aren't seen) */
        for (int k = 0; k < 3; k++) sum[k] += w * (double)((c >> (8 * k)) & 255) / 255.0;
        wsum += w;
    }
    for (int k = 0; k < 3; k++) out[k] = wsum > 0 ? (float)(sum[k] / wsum) : 0.0f;
}

/* a capture's polygon's palette (cap_pal[k]'s + 1, 0 none): each texture's use counted once */
static int cap_pal_slot(int k, const TexKey *key) {
    static uint32_t seen[ATLAS_LAYERS][256], nseen[ATLAS_LAYERS];
    if (!key->paletted) return 0;
    if (!R.cap_pal[k].n) nseen[k] = 0;
    int s = 0;
    while (s < R.cap_pal[k].n && R.cap_pal[k].base[s] != key->pal) s++;
    if (s == R.cap_pal[k].n) {
        if (s == CAP_PALS) return 0;
        R.cap_pal[k].n++;
        R.cap_pal[k].base[s] = key->pal;
        R.cap_pal[k].count[s] = ((key->tcw >> 27) & 7) == 5 ? 16 : 256;
        memset(R.cap_pal[k].hist[s], 0, sizeof R.cap_pal[k].hist[s]);
    }
    uint32_t id = key->tcw ^ key->pal * 2654435761u;
    int counted = 0;
    for (uint32_t j = 0; j < nseen[k]; j++) counted |= seen[k][j] == id;
    if (!counted && nseen[k] < 256) {
        seen[k][nseen[k]++] = id;
        pvr_tex_pal_hist(key, R.cap_pal[k].hist[s]);
    }
    return s + 1;
}

/* how a polygon shades its texels (pvr.frag): times its colour (modulate), plus its offset colour: rgb x3, rgb x3 */
static void poly_shading(const PvrFrame *pf, const PvrPoly *p, float out[6]) {
    const PvrVertex *v = &pf->verts[p->first_vertex];
    uint32_t shade = (p->tsp >> 6) & 3;
    for (int c = 0; c < 3; c++) {
        out[c] = (p->isp & (1u << 25)) && (shade == 1 || shade == 3) ? (float)((v->base >> (16 - 8 * c)) & 255) / 255.0f
                                                                     : 1.0f;
        out[3 + c] = p->isp & (1u << 24) ? (float)((v->offset >> (16 - 8 * c)) & 255) / 255.0f : 0.0f;
    }
}

void renderer_draw(const PvrFrame *pf) {
    dump_render(pf);
    if (!pf->npolys) return;
    Frame *f = frame_begin();
    R.frame_no++;
    RenderTarget *rt = get_rt(f, pf->fb_w_sof1 & 0x7FFFFF);
    uint32_t W = rt_width(), H = R.height;
    float xoff = (R.pic_w - 640) / 2;  /* the 640 in the middle of a wide picture */

    /* guest framebuffer size from the clip registers */
    float gw = (float)((pf->fb_x_clip >> 16) & 0x7FF) + 1, gh = (float)((pf->fb_y_clip >> 16) & 0x3FF) + 1;
    if (gw < 320 || gw > 1024) gw = 640;
    if (gh < 240 || gh > 1024) gh = 480;

    /* depth normalisation: largest 1/w in the frame */
    float maxz = 1e-6f;
    for (uint32_t i = 0; i < pf->nverts; i++)
        if (pf->verts[i].z > maxz && pf->verts[i].z < 1e30f) maxz = pf->verts[i].z;

    /* resolve textures before the rendering scope (uploads are transfer ops) */
    static VkDescriptorSet *sets;
    static int8_t *atlas;  /* each polygon: a layer capture's (widescreen.c: its layer) or -1 */
    static uint32_t sets_cap;
    if (sets_cap < pf->npolys) {
        sets_cap = pf->npolys * 2;
        sets = realloc(sets, sets_cap * sizeof *sets);
        atlas = realloc(atlas, sets_cap);
    }
    int atlas_layers = 0;
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        atlas[i] = -1;
        if (p->vertex_count && pf->verts[p->first_vertex].x >= ATLAS_X - 512) {
            float x0 = 1e9f;
            for (uint32_t k = 0; k < p->vertex_count; k++)
                x0 = pf->verts[p->first_vertex + k].x < x0 ? pf->verts[p->first_vertex + k].x : x0;
            int slot = (int)((x0 - ATLAS_X + 256) / ATLAS_SLOT);
            if (slot >= 0 && slot < ATLAS_LAYERS * 3) {
                int k = slot / 3;
                if (!(atlas_layers & (1 << k))) {  /* as captured (widescreen.c) */
                    poly_shading(pf, p, R.cap_shade[k]);
                    R.cap_pal[k].n = 0;
                    memset(R.cap_palmap[k], 0, sizeof R.cap_palmap[k]);
                }
                atlas[i] = (int8_t)k, atlas_layers |= 1 << k;
            }
        }
        sets[i] = R.white_set;
        if (!(p->isp & (1u << 25)) || !p->vertex_count) continue;
        Texture *t = get_texture(f, p->tcw, p->tsp);
        if (!t) continue;
        int si = sampler_index(p->tsp);
        if (atlas[i] >= 0) si %= 9;  /* point-sampled: no blends for the analysis */
        if (!t->sets[si]) t->sets[si] = alloc_set(t->img.view, R.samplers[si]);
        sets[i] = t->sets[si];
    }

    VkDeviceSize vbytes = pf->nverts * sizeof(PvrVertex), bbytes = pf->nverts * 4 * sizeof(float);
    if (f->vused + ((vbytes + 255) & ~255ull) + bbytes > f->vcap) return;  /* overflow: skip this render */
    memcpy((uint8_t *)f->vmap + f->vused, pf->verts, vbytes);
    float stage_bar;  /* widescreen: the picture's sides a stage's art doesn't reach (widescreen.c) */
    int wide_fight = widescreen_stage_frame(pf->wide_list, &stage_bar) && xoff > 0;  /* a fight's frame, drawn wide */
    if (!widescreen_list_captures(pf->wide_list)) atlas_layers = 0;  /* (only where widescreen.c asked for them) */
    if (wide_fight || atlas_layers) {
        PvrVertex *vv = (PvrVertex *)((uint8_t *)f->vmap + f->vused);
        for (uint32_t i = 0; i < pf->npolys; i++) {  /* the left margin's stage, laid out 4096 to the right */
            const PvrPoly *p = &pf->polys[i];
            float x0 = 1e9f;
            for (uint32_t k = 0; k < p->vertex_count; k++) x0 = vv[p->first_vertex + k].x < x0 ? vv[p->first_vertex + k].x : x0;
            if (atlas[i] >= 0) {  /* a layer capture: its segment's place in the capture (guest pixels) */
                int slot = (int)((x0 - ATLAS_X + 256) / ATLAS_SLOT);
                float dx = -(ATLAS_X + (float)slot * ATLAS_SLOT) + (float)(slot % 3) * 640.0f;
                for (uint32_t k = 0; k < p->vertex_count; k++) vv[p->first_vertex + k].x += dx;
                if (p->isp & (1u << 25)) {  /* its palette over its texels (a tile: its bounding box) */
                    TexKey key;
                    pvr_tex_info(p->tcw, p->tsp, &key);
                    int ps = cap_pal_slot(atlas[i], &key);
                    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
                    for (uint32_t k = 0; k < p->vertex_count; k++) {
                        const PvrVertex *v = &vv[p->first_vertex + k];
                        bx0 = v->x < bx0 ? v->x : bx0, bx1 = v->x > bx1 ? v->x : bx1;
                        by0 = v->y < by0 ? v->y : by0, by1 = v->y > by1 ? v->y : by1;
                    }
                    int u0 = (int)floorf(bx0 * 384.0f / 640.0f + 0.5f), u1 = (int)floorf(bx1 * 384.0f / 640.0f + 0.5f);
                    int r0 = (int)floorf(by0 / 2.0f + 0.5f), r1 = (int)floorf(by1 / 2.0f + 0.5f);
                    u0 = u0 < 0 ? 0 : u0, u1 = u1 > ATLAS_W ? ATLAS_W : u1, r0 = r0 < 0 ? 0 : r0, r1 = r1 > ATLAS_ROWS ? ATLAS_ROWS : r1;
                    for (int r = r0; r < r1; r++) memset(&R.cap_palmap[atlas[i]][r * ATLAS_W + u0], ps, u1 > u0 ? (size_t)(u1 - u0) : 0);
                }
            } else if (wide_fight && x0 >= 2048.0f) {
                for (uint32_t k = 0; k < p->vertex_count; k++) vv[p->first_vertex + k].x -= 4096.0f;
            }
        }
    }
    for (int k = 0; k < ATLAS_LAYERS; k++)  /* the capture's palettes' colours */
        if (atlas_layers & (1 << k)) {
            for (int s = 0; s < R.cap_pal[k].n; s++)
                pal_mean(R.cap_pal[k].base[s], R.cap_pal[k].count[s], R.cap_pal[k].hist[s], R.cap_pal[k].mean[s]);
            R.pal_dirty = 1;
        }
    if (wide_fight) {
        /* a wide fight: the untextured shapes as wide as the 4:3 screen (the borders at the top and bottom,
         * flashes, the super moves' dimming) as wide as the picture; the game's thin borders at the sides (0 - 0.1
         * and 640 - 640.23, laid out with the stage, so the margins' layouts put more of them about) out of it */
        PvrVertex *vv = (PvrVertex *)((uint8_t *)f->vmap + f->vused);
        for (uint32_t i = 0; i < pf->npolys; i++) {
            const PvrPoly *p = &pf->polys[i];
            if ((p->isp & (1u << 25)) || p->vertex_count < 3 || atlas[i] >= 0) continue;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t k = 0; k < p->vertex_count; k++) {
                const PvrVertex *v = &vv[p->first_vertex + k];
                x0 = v->x < x0 ? v->x : x0, x1 = v->x > x1 ? v->x : x1;
                y0 = v->y < y0 ? v->y : y0, y1 = v->y > y1 ? v->y : y1;
            }
            if (x1 - x0 < 1.0f && y1 - y0 >= gh - 1.0f) {
                for (uint32_t k = 0; k < p->vertex_count; k++) vv[p->first_vertex + k].x = -xoff - 8;
                continue;
            }
            if (x0 > 0.5f || x1 < gw - 0.5f) continue;  /* only what spans the whole screen: not the HUD's shapes */
            for (uint32_t k = 0; k < p->vertex_count; k++) {
                PvrVertex *v = &vv[p->first_vertex + k];
                if (v->x <= 0.5f) v->x = -xoff;
                else if (v->x >= gw - 0.5f) v->x = gw + xoff;
            }
        }
    }
    /* 16:9 background extension (widescreen.c, wide_filler.c's wide_extend): each background layer that lacks art in
     * view gets its precomputed fill drawn under the frame (ext.frag) */
    uint64_t ext_t0 = SDL_GetTicksNS();
    WideExt ext[EXT_MAX];
    int emode = 0;
    float etint[EXT_MAX][6];  /* the layer's shading now against the capture's (a super darkens it): times, plus */
    int next = wide_fight && R.fill_set ? widescreen_extensions(pf->wide_list, ext, EXT_MAX, &emode) : 0;
    for (int k = 0; k < next; k++) {
        for (int c = 0; c < 6; c++) etint[k][c] = c < 3 ? 1.0f : 0.0f;
        for (uint32_t i = 0; i < pf->npolys; i++) {
            const PvrPoly *p = &pf->polys[i];
            if (!p->vertex_count || atlas[i] >= 0) continue;
            const PvrVertex *v = &pf->verts[p->first_vertex];
            if (v->z < ext[k].z * 0.999f || v->z > ext[k].z * 1.001f) continue;
            /* the fill's colours are the capture's, shaded as it was: now's relative to that */
            float now[6], *cap = R.cap_shade[ext[k].layer];
            poly_shading(pf, p, now);
            for (int c = 0; c < 3; c++) {
                float mul = cap[c] > 0.05f ? now[c] / cap[c] : 1.0f;
                etint[k][c] = mul, etint[k][3 + c] = now[3 + c] - cap[3 + c] * mul;
            }
            break;
        }
    }
    g_perf.wide_ns += SDL_GetTicksNS() - ext_t0;
    /* a dumped render (dump_render): its extensions too */
    if (s_dump_target >= 0 && s_dump_count - 1 >= s_dump_target && s_dump_count - 1 < s_dump_target + s_dump_n)
        for (int k = 0; k < next; k++)
            logf_("  ext %d: z %g layer %d cols %.2f..%.2f u0 %.2f v0 %.2f", k, ext[k].z, ext[k].layer, ext[k].q0,
                  ext[k].q1, ext[k].u0, ext[k].v0);
    VkDeviceSize voff = f->vused, boff = voff + ((vbytes + 255) & ~255ull);
    uv_boxes(pf, (float *)((uint8_t *)f->vmap + boff));
    f->vused = boff + ((bbytes + 255) & ~255ull);
    VkBuffer vbufs[2] = {f->vbuf, f->vbuf};
    VkDeviceSize voffs[2] = {voff, boff};

    barrier(f->cmd, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT,
            rt->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(f->cmd, rt->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    rt->initialized = 1;

    static int magenta = -1;  /* SFA3_CLEAR_MAGENTA: what nothing covers shows magenta (testing widescreen) */
    static float clear_rgb[3] = {0, 0, 0};
    if (magenta < 0) {
        magenta = getenv("SFA3_CLEAR_MAGENTA") != NULL;
        if (magenta) clear_rgb[0] = clear_rgb[2] = 1;
        int r, g, b;  /* SFA3_CLEAR_COLOR=r,g,b: any colour (testing) */
        if (getenv("SFA3_CLEAR_COLOR") && sscanf(getenv("SFA3_CLEAR_COLOR"), "%d,%d,%d", &r, &g, &b) == 3)
            clear_rgb[0] = r / 255.0f, clear_rgb[1] = g / 255.0f, clear_rgb[2] = b / 255.0f;
    }
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = rt->color.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color = {{clear_rgb[0], clear_rgb[1], clear_rgb[2], 1}}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = rt->depth.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .clearValue.depthStencil = {0.0f, 0}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    VkCommandBuffer cmd = f->cmd;
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D full = {{0, 0}, {W, H}};
    float bar_l = wide_fight ? stage_bar : 0, bar_r = bar_l;
    if (xoff > 0 && !wide_fight) bar_l = bar_r = xoff;  /* a wide picture outside a fight: 4:3, black at the sides
                                                          * (what the game draws past its screen's edges stays out) */
    if (xoff > 0 && (bar_l > 0 || bar_r > 0)) {
        float sx = (float)W / (gw + 2 * xoff);
        int32_t x0 = (int32_t)(bar_l * sx + 0.5f), x1 = (int32_t)((float)W - bar_r * sx + 0.5f);
        full = (VkRect2D){{x0, 0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), H}};
    }
    vkCmdSetScissor(cmd, 0, 1, &full);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, voffs);

    PvrPush push = {.scale = {2.0f / (gw + 2 * xoff), 2.0f / gh}, .zscale = 1.0f / (maxz * 1.0001f)};
    push.pad[0] = xoff;  /* the shader's xoff */
    uint32_t fc = pf->fog_col_vert;
    push.fog[0] = ((fc >> 16) & 255) / 255.0f;
    push.fog[1] = ((fc >> 8) & 255) / 255.0f;
    push.fog[2] = (fc & 255) / 255.0f;
    push.alpha_ref = (pf->pt_alpha_ref & 255) / 255.0f;

    /* Translucent autosort: hardware sorts per pixel by depth; approximate it by
     * drawing translucent polygons far to near (1/w ascending), ties in submission order. */
    int autosort = !(pf->isp_feed_cfg & 1);
    static uint32_t *tr_order;
    static float *tr_key;
    static uint32_t tr_cap;
    uint32_t ntr = 0;
    if (pf->npolys > tr_cap) {
        tr_cap = pf->npolys * 2;
        tr_order = realloc(tr_order, tr_cap * sizeof *tr_order);
        tr_key = realloc(tr_key, tr_cap * sizeof *tr_key);
    }
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        if (p->list != LIST_TRANS) continue;
        float z = 1e30f;
        for (uint32_t k = 0; k < p->vertex_count; k++) {
            float vz = pf->verts[p->first_vertex + k].z;
            if (vz < z) z = vz;
        }
        tr_key[i] = z;
        tr_order[ntr++] = i;
    }
    if (autosort) {
        /* insertion sort: stable and fast for mostly ordered lists */
        for (uint32_t a = 1; a < ntr; a++) {
            uint32_t v = tr_order[a];
            float kv = tr_key[v];
            uint32_t b = a;
            while (b > 0 && tr_key[tr_order[b - 1]] > kv) { tr_order[b] = tr_order[b - 1]; b--; }
            tr_order[b] = v;
        }
    }

    int fidx = (int)(f - R.frames);
    if (R.timestamps && !f->timed) {
        vkCmdResetQueryPool(cmd, R.timestamps, (uint32_t)fidx * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, R.timestamps, (uint32_t)fidx * 2);
    }
    g_perf.ext_passes += (uint64_t)next;
    /* a stage's new extension fill (widescreen.c) into its images: the colours (bilinear) and the modes */
    if (R.mask_pending) {
        VkDeviceSize n = (VkDeviceSize)WIDE_MASK_W * WIDE_MASK_ROWS * 3;
        if (!R.mask_img.image) {
            make_image(WIDE_MASK_W, WIDE_MASK_ROWS * 3, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       &R.fill_img);
            make_image(WIDE_MASK_W, WIDE_MASK_ROWS * 3, VK_FORMAT_R8G8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       &R.mask_img);
            make_image(CAP_PALS, ATLAS_LAYERS * 2, VK_FORMAT_R32G32B32A32_SFLOAT,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       &R.pal_img);
            R.mask_set = alloc_set(R.mask_img.view, R.samplers[8]);  /* point-sampled, clamped */
            R.pal_set = alloc_set(R.pal_img.view, R.samplers[8]);
            R.pal_dirty = 1;
            for (int i = 0; i < 3; i++)
                barrier(cmd, i == 2 ? R.pal_img.image : i ? R.mask_img.image : R.fill_img.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                        0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        if (f->sused + n * 6 + 256 <= f->scap) {
            memcpy((uint8_t *)f->smap + f->sused, R.mask_pending, n * 6);
            g_perf.upload_bytes += n * 6;
            for (int i = 0; i < 2; i++) {
                VkImage img = i ? R.mask_img.image : R.fill_img.image;
                barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy c = {.bufferOffset = f->sused + (i ? n * 4 : 0),
                                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {WIDE_MASK_W, WIDE_MASK_ROWS * 3, 1}};
                vkCmdCopyBufferToImage(cmd, f->sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            f->sused += (n * 6 + 255) & ~255ull;
            free(R.mask_pending);
            R.mask_pending = NULL;
            if (!R.fill_set) R.fill_set = alloc_set(R.fill_img.view, R.blit_sampler);  /* bilinear, clamped */
        }
    }

    /* the fill's palettes' colours now against the capture's (cap_pal): worked out again when one of those palettes
     * changes (not the fighters' or the HUD's, which change all the time) */
    int pal_changed = 0;
    if (next && R.pal_img.image && f->sused + sizeof R.pal_table + 256 <= f->scap) {
        const uint32_t *pr = pvr_palette();
        for (int k = 0; k < ATLAS_LAYERS; k++)
            for (int s = 0; s < CAP_PALS; s++) {
                float now[3], *mul = R.pal_table[k * 2][s], *add = R.pal_table[k * 2 + 1][s];
                if (s >= R.cap_pal[k].n) {
                    if (R.pal_dirty) mul[0] = mul[1] = mul[2] = mul[3] = 1, add[0] = add[1] = add[2] = add[3] = 0;
                    continue;
                }
                uint32_t b = R.cap_pal[k].base[s] & 1023, c = R.cap_pal[k].count[s];
                if (b + c > 1024) c = 1024 - b;
                if (!R.pal_dirty && !memcmp(pr + b, R.pal_ram + b, c * sizeof *pr)) continue;  /* (unchanged) */
                pal_changed = 1;
                mul[0] = mul[1] = mul[2] = mul[3] = 1, add[0] = add[1] = add[2] = add[3] = 0;
                pal_mean(R.cap_pal[k].base[s], R.cap_pal[k].count[s], R.cap_pal[k].hist[s], now);
                for (int c2 = 0; c2 < 3; c2++) {
                    float cap = R.cap_pal[k].mean[s][c2];
                    if (cap > 0.05f) mul[c2] = now[c2] / cap;
                    else add[c2] = now[c2] - cap;
                }
            }
        pal_changed |= R.pal_dirty;
        if (pal_changed) memcpy(R.pal_ram, pr, sizeof R.pal_ram);
        R.pal_dirty = 0;
    }
    if (pal_changed) {
        memcpy((uint8_t *)f->smap + f->sused, R.pal_table, sizeof R.pal_table);
        g_perf.upload_bytes += sizeof R.pal_table;
        barrier(cmd, R.pal_img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {.bufferOffset = f->sused, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {CAP_PALS, ATLAS_LAYERS * 2, 1}};
        vkCmdCopyBufferToImage(cmd, f->sbuf, R.pal_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, R.pal_img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        f->sused += (sizeof R.pal_table + 255) & ~255ull;
    }

    /* layer captures (widescreen.c): each layer's polygons laid out at ATLAS_X into its own image, a map unit a
     * pixel, then copied out for the filler analysis (read when this frame's fence has passed: frame_begin) */
    if (atlas_layers) {
        if (!R.atlas_buf) {
            make_buffer((VkDeviceSize)ATLAS_W * ATLAS_H * 4 * ATLAS_LAYERS, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        &R.atlas_buf, &R.atlas_mem, &R.atlas_map);
            make_image(ATLAS_W, ATLAS_H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, &R.atlas_depth);
            for (int k = 0; k < ATLAS_LAYERS; k++)
                make_image(ATLAS_W, ATLAS_H, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &R.atlas_img[k]);
        }
        PvrPush apush = push;
        float aw = (float)ATLAS_W * 640.0f / 384.0f;  /* the capture's width in guest pixels */
        apush.scale[0] = 2.0f / aw;
        apush.pad[0] = 0;
        VkRect2D afull = {{0, 0}, {ATLAS_W, ATLAS_H}};
        for (int k = 0; k < ATLAS_LAYERS; k++) {
            if (!(atlas_layers & (1 << k))) continue;
            barrier(cmd, R.atlas_img[k].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barrier(cmd, R.atlas_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            VkRenderingAttachmentInfo aca = ca, ada = da;
            aca.imageView = R.atlas_img[k].view;
            aca.clearValue.color = (VkClearColorValue){{0, 0, 0, 0}};
            ada.imageView = R.atlas_depth.view;
            VkRenderingInfo ari = ri;
            ari.renderArea = afull;
            ari.pColorAttachments = &aca;
            ari.pDepthAttachment = &ada;
            vkCmdBeginRendering(cmd, &ari);
            VkViewport avp = {0, 0, (float)ATLAS_W, (float)ATLAS_H, 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &avp);
            vkCmdSetScissor(cmd, 0, 1, &afull);
            vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
            vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, voffs);
            draw_polys(cmd, pf, sets, tr_order, ntr, autosort, apush, afull, 0, aw, gh, ATLAS_W, ATLAS_H, 0, atlas, k);
            vkCmdEndRendering(cmd);
            barrier(cmd, R.atlas_img[k].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)k * ATLAS_W * ATLAS_H * 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {ATLAS_W, ATLAS_H, 1}};
            vkCmdCopyImageToBuffer(cmd, R.atlas_img[k].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.atlas_buf, 1, &c);
            f->atlas_layers |= 1 << k;
        }
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cmd, &di);
        vkCmdSetViewport(cmd, 0, 1, &vp);
    }

    vkCmdBeginRendering(cmd, &ri);
    vkCmdSetScissor(cmd, 0, 1, &full);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, voffs);
    if (next) {
        float sx = (float)W / (gw + 2 * xoff), sy = (float)H / gh;  /* render target pixels per guest pixel */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ext_pipeline);
        VkDescriptorSet sets3[3] = {R.fill_set, R.mask_set, R.pal_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ext_layout, 0, 3, sets3, 0, NULL);
        for (int k = 0; k < next; k++) {
            float y0 = ext[k].r0 / gh * 2 - 1, y1 = ext[k].r1 / gh * 2 - 1;
            float pc[20] = {ext[k].u0, 384.0f / 640.0f / sx, ext[k].v0, 0.5f / sy,
                            (float)(ext[k].layer * WIDE_MASK_ROWS), ext[k].z * push.zscale, (float)ext[k].backdrop, (float)emode,
                            ext[k].q0 * sx / (float)W * 2 - 1, ext[k].q1 * sx / (float)W * 2 - 1, etint[k][0], etint[k][1],
                            etint[k][2], etint[k][3], etint[k][4], etint[k][5],
                            y0 < -1 ? -1 : y0, y1 > 1 ? 1 : y1, 0, 0};
            vkCmdPushConstants(cmd, R.ext_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof pc, pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }
    extern int g_wide_probe;
    extern float g_wide_probe_z;
    draw_polys(cmd, pf, sets, tr_order, ntr, autosort, push, full, xoff, gw, gh, W, H,
               g_wide_probe && wide_fight ? g_wide_probe_z : 0, atlas, -1);
    vkCmdEndRendering(cmd);
    if (R.timestamps && !f->timed) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, R.timestamps, (uint32_t)fidx * 2 + 1);
        f->timed = 1;
    }
    barrier(cmd, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

/* ---------------- screenshots (PNG, stored deflate) ---------------- */

static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t *d, size_t n, uint32_t c) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0);
    c = crc32_(data, n, c);
    uint8_t cb[4];
    be32(cb, c);
    fwrite(cb, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (w * 3 + 1);
    uint8_t *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * (w * 3 + 1);
        row[0] = 0;
        for (uint32_t x = 0; x < w; x++) memcpy(row + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        uint16_t n = (uint16_t)(raw_n - off > 65535 ? 65535 : raw_n - off);
        *p++ = off + n >= raw_n;
        *p++ = n & 255; *p++ = n >> 8; *p++ = ~n & 255; *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        p += n;
    }
    be32(p, b << 16 | a);
    p += 4;
    png_chunk(f, "IDAT", z, (uint32_t)(p - z));
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static void screenshot(RenderTarget *rt) {
    uint32_t w = rt_width(), h = R.height;
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    make_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = R.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(R.dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    barrier(cmd, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd, rt->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    barrier(cmd, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    vkQueueSubmit(R.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(R.queue);
    char path[256];
    extern const char *g_instance;
    snprintf(path, sizeof path, "%s/shot_%s%05llu.png", g_data_dir, g_instance ? g_instance : "", (unsigned long long)R.frame_no);
    write_png(path, map, w, h);
    logf_("screenshot %s", path);
    vkFreeCommandBuffers(R.dev, R.pool, 1, &cmd);
    vkDestroyBuffer(R.dev, buf, NULL);
    vkFreeMemory(R.dev, mem, NULL);
}

/* ---------------- present ---------------- */

static uint64_t s_vblanks;

/* Copy the settings menu canvas into its texture when it changed. */
static void upload_overlay(Frame *f) {
    int dirty;
    const uint32_t *px = menu_canvas(&dirty);
    if (!R.overlay.image) {
        make_image(MENU_W, MENU_H, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &R.overlay);
        R.overlay_set = alloc_set(R.overlay.view, R.samplers[0 * 9 + 2 * 3 + 2]);  /* nearest, clamped */
        dirty = 1;
    }
    VkDeviceSize bytes = MENU_W * MENU_H * 4;
    if (!dirty || f->sused + bytes > f->scap) return;
    memcpy((uint8_t *)f->smap + f->sused, px, bytes);
    barrier(f->cmd, R.overlay.image, VK_IMAGE_ASPECT_COLOR_BIT,
            R.overlay_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.bufferOffset = f->sused, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {MENU_W, MENU_H, 1}};
    vkCmdCopyBufferToImage(f->cmd, f->sbuf, R.overlay.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(f->cmd, R.overlay.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    f->sused = (f->sused + bytes + 255) & ~255ull;
    R.overlay_ready = 1;
}

void renderer_present_fb(void) {
    s_vblanks++;
    Frame *f = frame_begin();
    uint32_t fb = pvr_reg(PVR_FB_R_SOF1) & 0x7FFFFF;
    RenderTarget *rt = NULL;
    for (int i = 0; i < MAX_RT; i++)
        if (R.rts[i].color.image && R.rts[i].initialized && R.rts[i].fb_addr == fb) rt = &R.rts[i];
    if (!rt) /* fall back to the most recent render */
        for (int i = 0; i < MAX_RT; i++)
            if (R.rts[i].initialized && (!rt || R.rts[i].last_used > rt->last_used)) rt = &R.rts[i];

    if (s_vblanks % 120 == 1)
        logf_("present: fb_r=%06X rt=%p rts=[%06X/%d %06X/%d %06X/%d %06X/%d]", fb, (void *)rt,
              R.rts[0].fb_addr, R.rts[0].initialized, R.rts[1].fb_addr, R.rts[1].initialized,
              R.rts[2].fb_addr, R.rts[2].initialized, R.rts[3].fb_addr, R.rts[3].initialized);
    /* not every platform reports a resized window as an out-of-date swapchain
     * (Wayland), so compare sizes directly */
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(R.window, &pw, &ph);
    if (pw > 0 && ph > 0 && ((uint32_t)pw != R.sc_extent.width || (uint32_t)ph != R.sc_extent.height)) R.sc_dirty = 1;
    if (R.sc_dirty) create_swapchain();
    uint32_t idx;
    VkResult ar = vkAcquireNextImageKHR(R.dev, R.swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        R.sc_dirty = 1;
        /* still submit the recorded work so fences stay consistent */
        vkEndCommandBuffer(f->cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &f->cmd};
        vkQueueSubmit(R.queue, 1, &si, f->fence);
        f->recording = 0;
        R.fi = (R.fi + 1) % FRAMES;
        return;
    }
    VkCommandBuffer cmd = f->cmd;
    if (menu_overlay_visible()) upload_overlay(f);
    barrier(cmd, R.sc_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = R.sc_views[idx],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color = {{0, 0, 0, 1}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, R.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cmd, &ri);
    if (rt) {
        float sw = (float)R.sc_extent.width, sh = (float)R.sc_extent.height;
        float w = sw, h = sh;
        {  /* the picture's shape, 4:3 or 16:9 (4:3 in an online game), as big as fits: black at the sides of a
            * wider screen, above and below on a taller one */
            float aspect = R.pic_w / 480.0f;
            h = sw / aspect;
            if (h > sh) { h = sh; w = sh * aspect; }
        }
        /* whole-pixel placement keeps integer scaling crisp */
        float ox = (float)(int)((sw - w) / 2), oy = (float)(int)((sh - h) / 2);
        /* rect, then CRT strength / scanline count (240 lines, like a CRT) / mode / picture height */
        /* COLOR: STANDARD is the game's own output (as through a VGA cable). TV
         * applies video levels: the game's darkest grey (16) shows as black and
         * 235 as white, as on a television or in a video capture. VIVID adds
         * saturation on top. {gamma, saturation, unused, tv levels} */
        static const float color[3][4] = {{1.0f, 1.0f, 0, 0}, {1.0f, 1.0f, 0, 1}, {1.0f, 1.25f, 0, 1}};
        float cc[4];
        memcpy(cc, color[g_settings.color < 0 || g_settings.color > 2 ? 0 : g_settings.color], sizeof cc);
        cc[2] = (float)g_settings.scanlines;  /* CRT style */
        float push[12] = {-1.0f + 2 * ox / sw, -1.0f + 2 * oy / sh, 2 * w / sw, 2 * h / sh,
                          g_settings.scanlines ? g_settings.scanline_strength / 100.0f : 0.0f, 240.0f, 0, h,
                          cc[0], cc[1], cc[2], cc[3]};
        VkViewport vp = {0, 0, sw, sh, 0, 1};
        VkRect2D sc = {{0, 0}, R.sc_extent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.blit_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.blit_layout, 0, 1, &rt->blit_set, 0, NULL);
        vkCmdPushConstants(cmd, R.blit_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof push, push);
        vkCmdDraw(cmd, 4, 1, 0, 0);
        if (menu_overlay_visible() && R.overlay_ready) {
            push[4] = 0;
            push[6] = 1;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.overlay_pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.blit_layout, 0, 1, &R.overlay_set, 0,
                                    NULL);
            vkCmdPushConstants(cmd, R.blit_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof push, push);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    /* "presented" screenshot: exactly what is on screen (scanlines, menu) */
    extern int g_shot_present_request;
    VkBuffer pbuf = VK_NULL_HANDLE;
    VkDeviceMemory pmem = VK_NULL_HANDLE;
    void *pmap = NULL;
    VkImageLayout final_from = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (g_shot_present_request && R.sc_can_read) {
        make_buffer((VkDeviceSize)R.sc_extent.width * R.sc_extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    &pbuf, &pmem, &pmap);
        barrier(cmd, R.sc_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {R.sc_extent.width, R.sc_extent.height, 1}};
        vkCmdCopyImageToBuffer(cmd, R.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pbuf, 1, &c);
        final_from = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barrier(cmd, R.sc_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, final_from,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &f->acquired,
                       .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &R.sc_rendered[idx]};
    VKCHECK(vkQueueSubmit(R.queue, 1, &si, f->fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &R.sc_rendered[idx], .swapchainCount = 1,
                           .pSwapchains = &R.swapchain, .pImageIndices = &idx};
    VkResult pr = vkQueuePresentKHR(R.queue, &pi);

    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) R.sc_dirty = 1;
    if (pbuf) {
        g_shot_present_request = 0;
        vkQueueWaitIdle(R.queue);
        uint32_t n = R.sc_extent.width * R.sc_extent.height;
        uint8_t *p = pmap;
        if (R.sc_format == VK_FORMAT_B8G8R8A8_UNORM || R.sc_format == VK_FORMAT_B8G8R8A8_SRGB)
            for (uint32_t i = 0; i < n; i++) { uint8_t t = p[i * 4]; p[i * 4] = p[i * 4 + 2]; p[i * 4 + 2] = t; }
        char path[256];
        static unsigned seq;
        extern const char *g_instance;
        snprintf(path, sizeof path, "%s/shot_%s%05llu_presented_%u.png", g_data_dir, g_instance ? g_instance : "",
                 (unsigned long long)R.frame_no, seq++);
        write_png(path, p, R.sc_extent.width, R.sc_extent.height);
        logf_("screenshot %s", path);
        vkDestroyBuffer(R.dev, pbuf, NULL);
        vkFreeMemory(R.dev, pmem, NULL);
    }
    f->recording = 0;
    R.fi = (R.fi + 1) % FRAMES;
    extern int g_shot_request;
    if (rt && (g_shot_request ||
               (R.shot_every && s_vblanks >= R.shot_from && s_vblanks % (uint64_t)R.shot_every == 0))) {
        g_shot_request = 0;
        vkQueueWaitIdle(R.queue);
        screenshot(rt);
    }
    /* widescreen: the picture's margin (PICTURE 16:9; widescreen.c); a new one, like a new render
     * resolution, takes effect between frames (after the menu closes, so the paused picture stays
     * visible behind it) */
    s_margin_wanted = widescreen_wanted();
    if (s_margin_wanted != widescreen_margin()) s_rt_rebuild = 1;
    if (s_rt_rebuild && !menu_active()) rebuild_render_targets();
}

void renderer_shutdown(void) {
    if (R.dev) vkDeviceWaitIdle(R.dev);
}
