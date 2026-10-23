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

#define FRAMES 2
#define MAX_RT 4
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
    F_ALPHA_TEST = 1 << 4, F_FOG_VERTEX = 1 << 5, F_SHADE_SHIFT = 8,
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
    VkShaderModule pvr_vs, pvr_fs, blit_vs, blit_fs;
    VkSampler samplers[18];
    VkSampler blit_sampler;
    Frame frames[FRAMES];
    unsigned fi;
    RenderTarget rts[MAX_RT];
    Texture tex[MAX_TEX];
    Image white;
    VkDescriptorSet white_set;
    uint32_t scale;
    uint64_t frame_no;
    uint32_t page_gen[VRAM_PAGES];
    uint32_t gen_counter;
    int shot_every;
    int sc_can_read;          /* swapchain images can be copied out (presented screenshots) */
    uint64_t frame_ns;        /* vsync at ~60 Hz: frame period of the display, else 0 */
    uint64_t shot_from;
    uint32_t *decode_buf;
} R;

/* ---------------- helpers ---------------- */

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(R.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    fatal("no suitable Vulkan memory type");
}

static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem,
                        void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKCHECK(vkCreateBuffer(R.dev, &bi, NULL, buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(R.dev, *buf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = find_mem(mr.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
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
    VkVertexInputBindingDescription bind = {0, sizeof(PvrVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PvrVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PvrVertex, u)},
        {2, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(PvrVertex, base)},
        {3, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(PvrVertex, offset)}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1,
                                               .pVertexBindingDescriptions = &bind,
                                               .vertexAttributeDescriptionCount = 4,
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
    R.scale = (uint32_t)g_settings.scale;
    if (R.scale < 1 || R.scale > 8) R.scale = 3;
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
    logf_("vulkan: %s, internal resolution %ux%u", props.deviceName, 640 * R.scale, 480 * R.scale);

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
    create_samplers();
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
void renderer_settings_changed(void) {
    R.sc_dirty = 1;
    if ((uint32_t)g_settings.scale != R.scale) s_rt_rebuild = 1;
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
    R.scale = (uint32_t)g_settings.scale;
    s_rt_rebuild = 0;
    logf_("render resolution now %ux%u", 640 * R.scale, 480 * R.scale);
}

static RenderTarget *get_rt(Frame *f, uint32_t fb_addr) {
    RenderTarget *best = NULL;
    for (int i = 0; i < MAX_RT; i++)
        if (R.rts[i].color.image && R.rts[i].fb_addr == fb_addr) { best = &R.rts[i]; break; }
    if (!best) {
        for (int i = 0; i < MAX_RT; i++)
            if (!best || R.rts[i].last_used < best->last_used) best = &R.rts[i];
        if (!best->color.image) {
            uint32_t w = 640 * R.scale, h = 480 * R.scale;
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
    if (t) {
        uint32_t gen = range_gen(key.addr, key.size);
        if (gen != t->page_gen || key.paletted) {
            uint64_t h = pvr_tex_hash(&key);
            t->page_gen = gen;
            if (h != t->hash) { t->hash = h; upload = 1; }
        }
    } else {
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
    return t;
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
static long s_dump_target = -2, s_dump_count;
void renderer_dump_next(void) { s_dump_target = s_dump_count; }  /* the "dumprender" control command */
static void dump_render(const PvrFrame *pf) {
    long target;
    if (s_dump_target == -2) { const char *e = getenv("SFA3_DUMP_RENDER"); s_dump_target = e ? atol(e) : -1; }
    target = s_dump_target;
    if (s_dump_count++ != target) return;
    logf_("dump render %ld: %u polys", target, pf->npolys);
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
        if (SPRITE_SWAP_IS_TCW(p->tcw)) continue;
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
    }
}

void renderer_draw(const PvrFrame *pf) {
    dump_render(pf);
    if (!pf->npolys) return;
    Frame *f = frame_begin();
    R.frame_no++;
    RenderTarget *rt = get_rt(f, pf->fb_w_sof1 & 0x7FFFFF);
    uint32_t W = 640 * R.scale, H = 480 * R.scale;

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
    static uint32_t sets_cap;
    if (sets_cap < pf->npolys) {
        sets_cap = pf->npolys * 2;
        sets = realloc(sets, sets_cap * sizeof *sets);
    }
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        sets[i] = R.white_set;
        if (!(p->isp & (1u << 25)) || !p->vertex_count) continue;
        Texture *t = get_texture(f, p->tcw, p->tsp);
        if (!t) continue;
        int si = sampler_index(p->tsp);
        if (!t->sets[si]) t->sets[si] = alloc_set(t->img.view, R.samplers[si]);
        sets[i] = t->sets[si];
    }

    VkDeviceSize vbytes = pf->nverts * sizeof(PvrVertex);
    if (f->vused + vbytes > f->vcap) return;  /* overflow: skip this render */
    memcpy((uint8_t *)f->vmap + f->vused, pf->verts, vbytes);
    VkDeviceSize voff = f->vused;
    f->vused += (vbytes + 255) & ~255ull;

    barrier(f->cmd, rt->color.image, VK_IMAGE_ASPECT_COLOR_BIT,
            rt->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(f->cmd, rt->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    rt->initialized = 1;

    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = rt->color.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color = {{0, 0, 0, 1}}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = rt->depth.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .clearValue.depthStencil = {0.0f, 0}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    VkCommandBuffer cmd = f->cmd;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D full = {{0, 0}, {W, H}};
    vkCmdSetScissor(cmd, 0, 1, &full);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->vbuf, &voff);

    PvrPush push = {.scale = {2.0f / gw, 2.0f / gh}, .zscale = 1.0f / (maxz * 1.0001f)};
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

    /* draw order: opaque, punch-through (submission order), translucent */
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
            if (((tsp >> 22) & 3) == 1) flags |= F_FOG_VERTEX;
            flags |= ((tsp >> 6) & 3) << F_SHADE_SHIFT;
            push.flags = flags;
            vkCmdPushConstants(cmd, R.pvr_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof push, &push);
            /* user tile clip (inside mode) */
            if (p->tile_clip == 2) {
                int x0 = p->clip[0] * 32, y0 = p->clip[1] * 32, x1 = (p->clip[2] + 1) * 32, y1 = (p->clip[3] + 1) * 32;
                float sx = W / gw, sy = H / gh;
                VkRect2D sc = {{(int32_t)(x0 * sx), (int32_t)(y0 * sy)},
                               {(uint32_t)((x1 - x0) * sx), (uint32_t)((y1 - y0) * sy)}};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, p->vertex_count, 1, p->first_vertex, 0);
                vkCmdSetScissor(cmd, 0, 1, &full);
            } else {
                vkCmdDraw(cmd, p->vertex_count, 1, p->first_vertex, 0);
            }
        }
    }
    vkCmdEndRendering(cmd);
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
    uint32_t w = 640 * R.scale, h = 480 * R.scale;
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
        if (g_settings.picture == PICTURE_INTEGER) {
            /* largest whole multiple of 640x480 that fits (pixel-perfect); below 1x, fall back to 4:3 */
            int k = 1;
            while (640 * (k + 1) <= sw && 480 * (k + 1) <= sh) k++;
            if (640 * k <= sw && 480 * k <= sh) { w = 640.0f * k; h = 480.0f * k; }
            else { h = sw * 3.0f / 4.0f; if (h > sh) { h = sh; w = sh * 4.0f / 3.0f; } }
        } else if (g_settings.picture == PICTURE_4_3) {
            h = sw * 3.0f / 4.0f;
            if (h > sh) { h = sh; w = sh * 4.0f / 3.0f; }
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
    /* a new render resolution takes effect between frames (after the menu closes,
     * so the paused picture stays visible behind it) */
    if (s_rt_rebuild && !menu_active()) rebuild_render_targets();
}

void renderer_shutdown(void) {
    if (R.dev) vkDeviceWaitIdle(R.dev);
}
