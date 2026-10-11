/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"
#include "texture-upload.h"
#include "cubemap-copy-vk.h"
#include "surface-copy.h"
#include "surface-copy-vk.h"
#include "depth-copy.h"
#include "depth-copy-vk.h"
#include "surface-depth-view-vk.h"
#include "depth-stencil-copy-vk.h"
#include "qemu/timer.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);
static bool image_pool_acquire(PGRAPHVkState *r, const TextureImageConfig *config,
                               VkImage *out_image, VmaAllocation *out_allocation);
static void image_pool_drain(PGRAPHVkState *r);
static bool cube_binding_needs_refresh(PGRAPHState *pg, TextureBinding *binding);

static void texture_upload_compute_destroy(PGRAPHVkState *r)
{
    vkDestroyPipeline(r->device, r->texture_upload.pipeline, NULL);
    vkDestroyPipeline(r->device, r->texture_upload.r16_pipeline, NULL);
    vkDestroyPipelineLayout(r->device, r->texture_upload.layout, NULL);
    vkDestroyDescriptorPool(r->device, r->texture_upload.pool, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->texture_upload.set_layout, NULL);
    memset(&r->texture_upload, 0, sizeof(r->texture_upload));
}

static void texture_upload_compute_init(PGRAPHVkState *r)
{
    bool requested = false;
#ifdef __ANDROID__
    /* Limit the default to the device this path is measured and pixel-tested
     * on. Other renderers keep the transfer path. */
    requested = pgraph_vk_is_qualcomm_blob(r) &&
                strstr(r->device_props.deviceName, "Adreno (TM) 830");
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.xemu.vk.compute_upload", prop) > 0 &&
        (prop[0] == '0' || prop[0] == '1')) {
        requested = prop[0] == '1';
    }
#endif
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->physical_device,
                                        VK_FORMAT_R8G8B8A8_UNORM, &props);
    if (!requested ||
        !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        return;
    }
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical_device,
                                             &family_count, NULL);
    g_autofree VkQueueFamilyProperties *families =
        g_new(VkQueueFamilyProperties, family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical_device,
                                             &family_count, families);
    QueueFamilyIndices indices = pgraph_vk_find_queue_families(r->physical_device);
    if (indices.queue_family < 0 ||
        !(families[indices.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
        get_staging_buffer(r, BUFFER_STAGING_SRC)->buffer_size >
            r->device_props.limits.maxStorageBufferRange) {
        return;
    }

    g_autofree VkDescriptorSetLayout *layouts = NULL;
    VkDescriptorSetLayoutBinding bindings[] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings), .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(r->device, &set_info, NULL,
                                     &r->texture_upload.set_layout) != VK_SUCCESS) {
        goto fail;
    }
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &r->texture_upload.set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    if (vkCreatePipelineLayout(r->device, &layout_info, NULL,
                                &r->texture_upload.layout) != VK_SUCCESS) {
        goto fail;
    }
    ShaderModuleInfo *module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_COMPUTE_BIT, texture_upload_glsl);
    if (!module) {
        goto fail;
    }
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = r->texture_upload.layout,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main",
                   .module = module->module },
    };
    VkResult result = vkCreateComputePipelines(r->device, r->vk_pipeline_cache,
        1, &pipeline_info, NULL, &r->texture_upload.pipeline);
    pgraph_vk_destroy_shader_module(r, module);
    if (result != VK_SUCCESS) {
        goto fail;
    }
    vkGetPhysicalDeviceFormatProperties(r->physical_device,
                                        VK_FORMAT_R16_UINT, &props);
    VkImageFormatProperties image_props;
    if (r->enabled_physical_device_features.shaderStorageImageExtendedFormats &&
        (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
        vkGetPhysicalDeviceImageFormatProperties(r->physical_device,
            VK_FORMAT_R5G6B5_UNORM_PACK16, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            &image_props) == VK_SUCCESS) {
        module = pgraph_vk_create_shader_module_from_glsl(
            r, VK_SHADER_STAGE_COMPUTE_BIT, texture_upload_r16_glsl);
        if (module) {
            pipeline_info.stage.module = module->module;
            result = vkCreateComputePipelines(r->device, r->vk_pipeline_cache,
                1, &pipeline_info, NULL, &r->texture_upload.r16_pipeline);
            pgraph_vk_destroy_shader_module(r, module);
            if (result != VK_SUCCESS) r->texture_upload.r16_pipeline = VK_NULL_HANDLE;
        }
    }
    uint32_t count = ARRAY_SIZE(r->texture_upload.sets);
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count },
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = count, .poolSizeCount = ARRAY_SIZE(sizes), .pPoolSizes = sizes,
    };
    if (vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                &r->texture_upload.pool) != VK_SUCCESS) {
        goto fail;
    }
    layouts = g_new(VkDescriptorSetLayout, count);
    for (uint32_t i = 0; i < count; i++) {
        layouts[i] = r->texture_upload.set_layout;
    }
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->texture_upload.pool,
        .descriptorSetCount = count, .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(r->device, &alloc_info,
                                  r->texture_upload.sets) != VK_SUCCESS) {
        goto fail;
    }
    r->texture_upload.enabled = true;
    VK_LOG_ERROR("RGBA8 compute upload enabled on %s; RGB565=%s",
                 r->device_props.deviceName,
                 r->texture_upload.r16_pipeline ? "enabled" : "transfer fallback");
    return;
fail:
    VK_LOG_ERROR("RGBA8 compute upload unavailable; using buffer/image copies");
    texture_upload_compute_destroy(r);
}

static bool texture_can_compute_upload(PGRAPHVkState *r, TextureShape state,
                                        VkFormat format)
{
    /* Linear images use their explicit rectangle and pitch. Their border
     * source flag never adds the virtual border used by swizzled textures;
     * get_texture_layout() only expands it for non-linear formats. */
    bool has_texture_border = state.border &&
        !kelvin_color_format_info_map[state.color_format].linear;
    bool supported_format = format == VK_FORMAT_R8G8B8A8_UNORM ||
        (format == VK_FORMAT_R5G6B5_UNORM_PACK16 && r->texture_upload.r16_pipeline);
    return r->texture_upload.enabled && supported_format &&
           state.dimensionality == 2 && !has_texture_border &&
           state.levels <= 16;
}

static void texture_compute_upload(PGRAPHVkState *r, TextureBinding *binding,
                                    StorageBuffer *staging, VkCommandBuffer cmd,
                                    const VkBufferImageCopy *regions, int count)
{
    bool packed16 = binding->image_config.format == VK_FORMAT_R5G6B5_UNORM_PACK16;
    texture_upload_image_barrier(cmd, binding->image,
        binding->image_config.mip_levels, binding->image_config.array_layers,
        binding->current_layout, VK_IMAGE_LAYOUT_GENERAL,
        binding->current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                       packed16 ? r->texture_upload.r16_pipeline :
                                  r->texture_upload.pipeline);
    for (int i = 0; i < count; i++) {
        if (!binding->upload_views[i]) {
            VkImageViewUsageCreateInfo usage = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT,
            };
            VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &usage,
                .image = binding->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = packed16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                    regions[i].imageSubresource.mipLevel, 1,
                    regions[i].imageSubresource.baseArrayLayer, 1 },
            };
            VK_CHECK(vkCreateImageView(r->device, &view_info, NULL,
                                       &binding->upload_views[i]));
        }
        VkDescriptorSet set = r->texture_upload.sets[r->texture_upload.set_index++];
        VkDescriptorBufferInfo buffer_info = { staging->buffer, 0, staging->buffer_size };
        VkDescriptorImageInfo image_info = {
            .imageView = binding->upload_views[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet writes[] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .pBufferInfo = &buffer_info },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .pImageInfo = &image_info },
        };
        vkUpdateDescriptorSets(r->device, ARRAY_SIZE(writes), writes, 0, NULL);
        texture_upload_dispatch(cmd, r->texture_upload.layout, set,
            regions[i].bufferOffset, regions[i].imageExtent.width,
            regions[i].imageExtent.height);
    }
    texture_upload_image_barrier(cmd, binding->image,
        binding->image_config.mip_levels, binding->image_config.array_layers,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* Xbox DXT1/3/5 blocks are bit-identical to Vulkan BC1/BC2/BC3. Where the
 * device can sample those formats we hand the blocks straight to the driver
 * instead of running s3tc_decompress_* on the CPU and storing the result at
 * 4-8x the size. See init_texture_format_map() for where the map is switched
 * over, and texture_format_for_shape() for the cases that must stay
 * decompressed. */
static bool format_is_block_compressed(VkFormat format)
{
    return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
           format == VK_FORMAT_BC2_UNORM_BLOCK ||
           format == VK_FORMAT_BC3_UNORM_BLOCK;
}

/* Block upload covers plain 2D and cubemap textures. Volume textures are
 * excluded because a 3D image in a BC format is not required to be supported,
 * and bordered textures because the border is cropped by resampling the
 * decompressed texels. Both keep the software path. */
static bool texture_shape_uses_blocks(PGRAPHVkState *r, TextureShape s)
{
    return format_is_block_compressed(
               r->texture_format_map[s.color_format].vk_format) &&
           s.dimensionality == 2 && !s.border;
}

/* The image format actually used for a texture, which is the block format only
 * when the shape can be uploaded as blocks. */
static VkColorFormatInfo texture_format_for_shape(PGRAPHVkState *r,
                                                  TextureShape s,
                                                  uint32_t address)
{
    VkColorFormatInfo vkf = r->texture_format_map[s.color_format];
    if (format_is_block_compressed(vkf.vk_format) &&
        !texture_shape_uses_blocks(r, s)) {
        vkf = kelvin_color_format_vk_map[s.color_format];
    }
    if (vkf.vk_format == VK_FORMAT_B8G8R8A8_UNORM &&
        texture_can_compute_upload(r, s, VK_FORMAT_R8G8B8A8_UNORM) &&
        GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU) !=
            NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER &&
        GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV) !=
            NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER) {
        /* Custom border colors with nonidentity views require an additional
         * Vulkan feature. Keep border-addressed BGRA on the original path. */
        vkf.vk_format = VK_FORMAT_R8G8B8A8_UNORM;
        vkf.component_map = texture_upload_bgra_mapping(vkf.component_map);
    }
    return vkf;
}

/*
 * Whether the tex_surf_range_cache memo is consulted at all.
 *
 * The memo skips pgraph_vk_download_surfaces_in_range_if_dirty() for a VRAM
 * range it believes is still clean. That download is also what *writes* the
 * range in VRAM, and that write is what sets DIRTY_MEMORY_NV2A_TEX, which is
 * what check_texture_dirty() reports and what makes create_texture() re-upload.
 * So a wrong skip does not merely miss a download -- it leaves the texture
 * binding frozen at whatever it held the first time, with no later event able
 * to refresh it.
 *
 * `debug.xemu.vk.surf_range_memo=0` disables the memo so the scan always runs,
 * which is what the code did before the memo was keyed by range.
 */
static bool surf_range_memo_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = 1;
        if (__system_property_get("debug.xemu.vk.surf_range_memo", prop) > 0 &&
            prop[0] == '0') {
            enabled = 0;
        }
        __android_log_print(ANDROID_LOG_INFO, "hakuX-vk",
                            "texture surface-range memo: %s",
                            enabled ? "on" : "off (always scan)");
    }
    return enabled;
#else
    return true;
#endif
}

/* Direct-mapped index for tex_surf_range_cache, keyed by the VRAM range. */
static inline unsigned surf_range_cache_index(hwaddr addr, hwaddr length)
{
    uint64_t h = (uint64_t)addr * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)length * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    return (unsigned)(h & (TEX_SURF_RANGE_CACHE_SIZE - 1));
}

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

static bool pgraph_vk_texture_range_valid(NV2AState *d,
                                          hwaddr vram_offset,
                                          size_t length)
{
    hwaddr vram_size = memory_region_size(d->vram);

    if (length == 0 || vram_offset >= vram_size) {
        return false;
    }

    return length <= (vram_size - vram_offset);
}

static void pgraph_vk_log_invalid_texture_range(NV2AState *d,
                                                int unit,
                                                const char *range_name,
                                                const TextureShape *shape,
                                                hwaddr vram_offset,
                                                size_t length)
{
    hwaddr vram_size = memory_region_size(d->vram);

    NV2A_XPRINTF(true,
                 "Skipping %s for stage %d: offset=0x%" HWADDR_PRIx
                 " length=0x%zx vram=0x%" HWADDR_PRIx
                 " dim=%u fmt=0x%X levels=%u border=%d cubemap=%d\n",
                 range_name, unit, vram_offset, length, vram_size,
                 shape->dimensionality, shape->color_format, shape->levels,
                 shape->border, shape->cubemap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN, "hakuX",
                        "Skipping %s for stage %d: offset=0x%" HWADDR_PRIx
                        " length=0x%zx vram=0x%" HWADDR_PRIx
                        " dim=%u fmt=0x%X levels=%u border=%d cubemap=%d",
                        range_name, unit, vram_offset, length, vram_size,
                        shape->dimensionality, shape->color_format,
                        shape->levels, shape->border, shape->cubemap);
#endif
}

static void pgraph_vk_bind_invalid_texture(PGRAPHVkState *r, int texture_idx)
{
    r->tex_binding_cache[texture_idx].key_hash = 0;
    r->tex_binding_cache[texture_idx].binding = NULL;
    r->texture_bindings[texture_idx] = &r->dummy_texture;
    r->tex_surface_direct[texture_idx] = false;
    r->tex_surface_direct_views[texture_idx] = VK_NULL_HANDLE;
    r->tex_surface_direct_layout[texture_idx] =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(false);
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    const bool upload_blocks = texture_shape_uses_blocks(r, s);

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;
                    size_t block_bytes =
                        physical_width / 4 * physical_height / 4 * block_size;

                    size_t converted_size;
                    uint8_t *converted;

                    if (upload_blocks) {
                        converted_size = block_bytes;
                        converted = g_malloc(block_bytes);
                        memcpy(converted, texture_data_ptr, block_bytes);
                    } else {
                        converted_size = width * height * 4;
                        converted = s3tc_decompress_2d(
                            kelvin_format_to_s3tc_format(s.color_format),
                            texture_data_ptr, width, height);
                    }
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += block_bytes;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

static void mark_texture_range_dirty(NV2AState *d, hwaddr addr, hwaddr size,
                                     bool confirmed_dirty)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    bool any_newly_dirty = false;
    TextureBinding *tnode;
    QTAILQ_FOREACH(tnode, &r->texture_active_list, active_entry) {
        if (tnode->possibly_dirty && !confirmed_dirty) {
            continue;
        }

        uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
        uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
        bool overlapping = !(addr > k_tex_end || k_tex_addr > end);

        if (tnode->key.palette_length > 0) {
            uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
            uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
            overlapping |= !(addr > k_pal_end || k_pal_addr > end);
        }

        if (overlapping) {
            any_newly_dirty |= !tnode->possibly_dirty;
            if (confirmed_dirty) {
                tnode->dirty_check_frame = d->pgraph.frame_time;
                tnode->dirty_check_result = true;
                tnode->cube_copy_cache.valid = false;
            }
        }
        tnode->possibly_dirty |= overlapping;
    }
    if (any_newly_dirty) {
        r->texture_vram_gen++;
    }
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    mark_texture_range_dirty(d, addr, size, false);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr vram_size = memory_region_size(d->vram);

    if (size == 0 || addr >= vram_size) {
        return false;
    }
    if (size > vram_size - addr) {
        size = vram_size - addr;
    }

    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    if (end > vram_size) {
        end = vram_size;
    }
    if (end <= addr) {
        return false;
    }
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

bool pgraph_vk_poll_texture_memory_dirty(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool found_dirty = false;

    /* Guest CPU writes do not change texture registers, so the register and
     * pipeline generation fast paths cannot notice them.  Poll the dirty
     * bitmap for the currently bound texture ranges before those fast paths
     * run, then invalidate every cached binding which aliases a dirty page. */
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *binding = r->texture_bindings[i];
        if (!binding || binding == &r->dummy_texture) {
            continue;
        }

        bool texture_dirty = check_texture_dirty(
            d, binding->key.texture_vram_offset,
            binding->key.texture_length);

        /* Draws do not dirty CPU VRAM until readback. Check all GPU cubemap
         * faces before register/pipeline fast paths can skip texture binding.
         * Leave CPU dirty polling and its invalidation below in place. */
        if (binding->cube_surface_copy &&
            cube_binding_needs_refresh(&d->pgraph, binding)) {
            found_dirty = true;
            if (!binding->possibly_dirty) {
                r->texture_vram_gen++;
            }
            binding->possibly_dirty = true;
            binding->dirty_check_frame = d->pgraph.frame_time;
            binding->dirty_check_result = true;
        }
        if (texture_dirty) {
            found_dirty = true;
            mark_texture_range_dirty(d, binding->key.texture_vram_offset,
                                     binding->key.texture_length, true);
        }
        if (binding->key.palette_length > 0 &&
            check_texture_dirty(d, binding->key.palette_vram_offset,
                                binding->key.palette_length)) {
            found_dirty = true;
            mark_texture_range_dirty(d, binding->key.palette_vram_offset,
                                     binding->key.palette_length, true);
        }
    }

    return found_dirty;
}

static void resolve_possibly_dirty_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *b = r->texture_bindings[i];
        if (!b || b == &r->dummy_texture || !b->possibly_dirty) continue;
        if (b->dirty_check_frame == pg->frame_time) continue;

        bool vram_dirty = check_texture_dirty(
            d, b->key.texture_vram_offset, b->key.texture_length);
        if (b->key.palette_length > 0) {
            vram_dirty |= check_texture_dirty(
                d, b->key.palette_vram_offset, b->key.palette_length);
        }
        b->dirty_check_frame = pg->frame_time;
        b->dirty_check_result = vram_dirty;
        if (!vram_dirty) {
            b->possibly_dirty = false;
        }
    }
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    NV2A_PHASE_TIMER_BEGIN(texture_upload);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = texture_format_for_shape(r, *state, binding->key.address);

    VK_LOG("upload_texture: idx=%d fmt=%d %ux%u cubemap=%d levels=%d",
           texture_idx, state->color_format, state->width, state->height,
           state->cubemap, state->levels);

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    int64_t decode_start = nv2a_stall_start();
    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    nv2a_stall_record(NV2A_STALL_TEXTURE_DECODE, decode_start, 0);
    int64_t stage_start = nv2a_stall_start();

    /*
     * During a diag capture, write out the CPU-side decoded level 0 exactly as
     * it is about to be handed to the driver. Every code-path difference
     * between this device and a desktop has been eliminated, so the remaining
     * question is simply whether the bytes we upload are correct. This answers
     * it directly instead of by inference.
     */
    if (nv2a_dbg_diag_frame_active()) {
        const char *ddir = nv2a_dbg_diag_dir();
        TextureLevel *l0 = &layout->layers[0].levels[0];
        if (ddir && l0->decoded_data && l0->width && l0->height) {
            char tp[900];
            snprintf(tp, sizeof(tp), "%s/tex_%08" HWADDR_PRIx "_f%02x_%ux%u.ppm",
                     ddir, binding->key.texture_vram_offset,
                     state->color_format, l0->width, l0->height);
            FILE *tf = fopen(tp, "wb");
            if (tf) {
                fprintf(tf, "P6\n%u %u\n255\n", l0->width, l0->height);
                const uint8_t *px = l0->decoded_data;
                size_t npx = (size_t)l0->width * l0->height;
                /* decoded_data is RGBA8 here; drop alpha for the PPM. */
                if (l0->decoded_size >= npx * 4) {
                    for (size_t k = 0; k < npx; k++) {
                        fwrite(px + k * 4, 1, 3, tf);
                    }
                }
                fclose(tf);
            }
        }
    }
    const int num_layers = state->cubemap ? 6 : 1;
    bool compute_upload = texture_can_compute_upload(r, *state, vkf.vk_format);
    if (compute_upload && r->texture_upload.set_index + num_layers * state->levels >
            ARRAY_SIZE(r->texture_upload.sets)) {
        /* Submit recorded work before recycling descriptors. This must happen
         * before staging allocation, since finish can rotate the frame slot. */
        int64_t wait_start = nv2a_stall_start();
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        pgraph_vk_flush_all_frames(pg);
        nv2a_stall_record(NV2A_STALL_TEXTURE_CAPACITY_WAIT, wait_start, 0);
    }

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += compute_upload ? texture_upload_level_size(size) : size;
        }
    }

    VkDeviceSize staging_base = pgraph_vk_staging_alloc(pg, texture_data_size);
    if (staging_base == VK_WHOLE_SIZE) {
        int64_t wait_start = nv2a_stall_start();
        OPT_STAT_INC(buf_stg_full);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        staging_base = pgraph_vk_staging_alloc(pg, texture_data_size);
        if (staging_base == VK_WHOLE_SIZE) {
            pgraph_vk_flush_all_frames(pg);
            pgraph_vk_staging_reset(pg);
            staging_base = pgraph_vk_staging_alloc(pg, texture_data_size);
            assert(staging_base != VK_WHOLE_SIZE);
        }
        nv2a_stall_record(NV2A_STALL_TEXTURE_CAPACITY_WAIT, wait_start, 0);
    }
    StorageBuffer *staging = get_staging_buffer(r, BUFFER_STAGING_SRC);
    uint8_t *mapped_memory_ptr = (uint8_t *)staging->mapped;

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = staging_base;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            if (compute_upload) {
                memset(mapped_memory_ptr + buffer_offset + level->decoded_size, 0,
                       texture_upload_level_size(level->decoded_size) - level->decoded_size);
            }
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += compute_upload ?
                texture_upload_level_size(level->decoded_size) : level->decoded_size;
            region++;
        }
    }
    assert(buffer_offset <= staging->buffer_size);

    vmaFlushAllocation(r->allocator, staging->allocation,
                       staging_base, buffer_offset - staging_base);
    nv2a_stall_record(NV2A_STALL_TEXTURE_STAGE, stage_start, texture_data_size);
    int64_t record_start = nv2a_stall_start();

    OPT_STAT_INC(nd_texup);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = compute_upload ? VK_ACCESS_SHADER_READ_BIT :
                                         VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging->buffer,
        .offset = staging_base,
        .size = buffer_offset - staging_base
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         compute_upload ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    int64_t copy_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (compute_upload) {
        texture_compute_upload(r, binding, staging, cmd, regions, num_regions);
    } else {
        pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        vkCmdCopyBufferToImage(cmd, staging->buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

        pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

#if defined(__ANDROID__) && NV2A_PERF_LOG
    static uint64_t copies[2], bytes[2], copy_ns[2], max_ns[2];
    int path = compute_upload ? 1 : 0;
    uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - copy_start;
    copies[path]++;
    bytes[path] += texture_data_size;
    copy_ns[path] += elapsed;
    max_ns[path] = MAX(max_ns[path], elapsed);
    if ((copies[0] + copies[1]) % 64 == 0) {
        __android_log_print(ANDROID_LOG_INFO, "xemu-upload",
            "transfer=%" PRIu64 " %.1fMB %.2fms max=%.2fms; "
            "compute=%" PRIu64 " %.1fMB %.2fms max=%.2fms; "
            "last=fmt%02x %ux%u dim%u cube%d mips%u linear%d border%d addr%u/%u path%d",
            copies[0], bytes[0] / 1048576.0, copy_ns[0] / 1e6, max_ns[0] / 1e6,
            copies[1], bytes[1] / 1048576.0, copy_ns[1] / 1e6, max_ns[1] / 1e6,
            state->color_format, state->width, state->height, state->dimensionality,
            state->cubemap, state->levels,
            kelvin_color_format_info_map[state->color_format].linear, state->border,
            GET_MASK(binding->key.address, NV_PGRAPH_TEXADDRESS0_ADDRU),
            GET_MASK(binding->key.address, NV_PGRAPH_TEXADDRESS0_ADDRV), path);
    }
#else
    (void)copy_start;
#endif


    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    // Release decoded texture data
    nv2a_stall_record(NV2A_STALL_TEXTURE_RECORD, record_start, texture_data_size);
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
    NV2A_PHASE_TIMER_END(texture_upload);
}

/* D16 depth images and R16 textures have the same two-byte texel
 * representation, but Vulkan cannot copy directly between depth and color
 * aspects. Route the copy through a buffer when direct surface descriptors
 * are disabled for a mobile driver. */
static void copy_depth_surface_to_texture(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          TextureBinding *texture)
{
    assert(!surface->color);
    assert(surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM);
    assert(!(surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT));

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = r->texture_format_map[state->color_format];
    assert(vkf.vk_format == VK_FORMAT_R16_UNORM);

    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_draw_queue(d);
    }

    if (pgraph_vk_compute_needs_finish(r)) {
        OPT_STAT_INC(buf_compute_full);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        pgraph_vk_flush_all_frames(pg);
        pgraph_vk_compute_finish_complete(r);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

#if OPT_SURF_TO_TEX_INLINE
    OPT_STAT_INC(nd_copysurf);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copy_size = scaled_width * scaled_height *
                       surface->host_fmt.host_bytes_per_pixel;
    StorageBuffer *copy_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(copy_size <= copy_buffer->buffer_size);

    VkImageLayout original_surface_layout = surface->image_layout;
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        original_surface_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferMemoryBarrier prepare_buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .offset = 0,
        .size = copy_size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &prepare_buffer, 0, NULL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1,
        },
        .imageExtent = { scaled_width, scaled_height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           copy_buffer->buffer, 1, &region);

    VkBufferMemoryBarrier copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .offset = 0,
        .size = copy_size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd, copy_buffer->buffer, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original_surface_layout);
    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_TO_TEX_INLINE
    pgraph_vk_end_nondraw_commands(pg, cmd);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif

    texture->draw_time = surface->draw_time;
}

// Direct depth-stencil to texture: samples depth from the surface image
// directly in a compute shader (eliminating the depth image→buffer copy),
// copies only the stencil aspect to a buffer, and packs the result.
static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    assert(!surface->color);
    if (!(surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        copy_depth_surface_to_texture(pg, surface, texture);
        return;
    }
    assert(surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);

    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_draw_queue(d);
    }

    if (pgraph_vk_compute_needs_finish(r)) {
        OPT_STAT_INC(buf_compute_full);
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        pgraph_vk_flush_all_frames(pg);
        pgraph_vk_compute_finish_complete(r);
    }

    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = r->texture_format_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

#if OPT_SURF_TO_TEX_INLINE
    OPT_STAT_INC(nd_copysurf);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    // Step 1: Copy only the stencil aspect to buffer (1 byte/pixel)
    StorageBuffer *stencil_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    size_t stencil_buffer_offset =
        ROUND_UP(scaled_width * scaled_height * 4,
                 r->device_props.limits.minStorageBufferOffsetAlignment);
    size_t stencil_size = scaled_width * scaled_height;

    VkBufferImageCopy stencil_region = {
        .bufferOffset = stencil_buffer_offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->image_layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        stencil_buffer->buffer, 1, &stencil_region);

    // Step 2: Transition depth surface to read-only for compute sampling
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    surface->image_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    /* Descriptors recorded below outlive this function. Keep the view with
     * its surface instead of destroying it before the command is submitted. */
    VK_CHECK(vk_surface_depth_view_get(r->device, surface->image,
        surface->host_fmt.vk_format, &surface->depth_view));

    // Step 3: Barrier for stencil buffer availability
    VkBufferMemoryBarrier stencil_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = stencil_buffer->buffer,
        .offset = stencil_buffer_offset,
        .size = stencil_size,
    };
    VkBufferMemoryBarrier output_pre_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = scaled_width * scaled_height * 4,
    };
    VkBufferMemoryBarrier pre_barriers[] = { stencil_barrier, output_pre_barrier };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                         ARRAY_SIZE(pre_barriers), pre_barriers, 0, NULL);

    // Step 4: Compute shader samples depth + reads stencil → packs output
    pgraph_vk_pack_depth_stencil_direct(
        pg, surface, cmd, surface->depth_view,
        stencil_buffer->buffer, stencil_buffer_offset, stencil_size,
        r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

    // Step 5: Barrier for packed output buffer → transfer read
    size_t packed_size = scaled_width * scaled_height * 4;
    VkBufferMemoryBarrier post_compute_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = packed_size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                         1, &post_compute_barrier, 0, NULL);

    // Step 6: Copy packed buffer → texture image
    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy output_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &output_region);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = packed_size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_TO_TEX_INLINE
    pgraph_vk_end_nondraw_commands(pg, cmd);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif

    texture->draw_time = surface->draw_time;
}

// Direct surface sampling: end the render pass and insert a barrier so the
// surface image (already in GENERAL layout) can be sampled as a texture
// without copying.  The caller stores the surface image_view in
// tex_surface_direct_views[] for the descriptor binding code.
static bool can_bind_surface_direct(PGRAPHVkState *r,
                                    const SurfaceBinding *surface)
{
#ifndef __ANDROID__
    /* Reproduce the mobile surface-to-texture copy path on desktop. */
    {
        const char *env = getenv("XEMU_VK_SURF_DIRECT");
        if (env && env[0] == '0') {
            return false;
        }
    }
#endif
#ifdef __ANDROID__
    /* Some mobile drivers dereference transient render-surface views while
     * processing combined-image-sampler descriptor writes.  Mali-G715 r54 and
     * Qualcomm's Adreno drivers both eventually segfault inside
     * push_descriptor_set/vkUpdateDescriptorSets.  Use the existing
     * surface-to-texture copy so descriptors reference cache-owned views.
     *
     * This applies to side-loaded Qualcomm blobs as well: they are often built
     * for a different GPU generation than the part they end up running on.
     * Turnip does not have the bug and binds directly.
     *
     * The copy is not cheap: a Forza race issues ~8 surface-to-texture copies
     * per frame, each one also ending the render pass to record its transfer.
     * `setprop debug.xemu.vk.surf_direct 1` re-enables direct binding so the
     * cost of this workaround can be measured against the crash it prevents.
     */
    static int force_direct = -1;
    if (force_direct < 0) {
        char prop[PROP_VALUE_MAX] = {};
        force_direct = 0;
        if (__system_property_get("debug.xemu.vk.surf_direct", prop) > 0) {
            force_direct = (prop[0] == '1');
        }
        __android_log_print(ANDROID_LOG_INFO, "hakuX-vk",
                            "surface direct binding: %s",
                            force_direct ? "forced on (diagnostic)"
                                         : "off (driver workaround)");
    }
    if (!force_direct && (r->device_props.vendorID == 0x13B5u ||
                          pgraph_vk_is_qualcomm_blob(r))) {
        return false;
    }
#endif
    return surface->color ||
           !(surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
}

static void bind_surface_as_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_draw_queue(d);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    // End render pass to flush tile writes, then barrier for shader reads
    OPT_STAT_INC(nd_bindsurf);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = surface->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

// Direct zeta surface sampling: transition the depth image to a read-only
// layout so it can be sampled as a texture without copying. Only valid for
// depth-only surfaces (D16) where the depth float value matches the expected
// texture format (R16_UNORM).
static void bind_zeta_surface_as_texture(PGRAPHState *pg,
                                         SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!surface->color);
    assert(!(surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT));

    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_draw_queue(d);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    OPT_STAT_INC(nd_bindsurf);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = surface->image_layout,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = surface->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    surface->image_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static bool surface_queue_copy_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.xemu.vk.surf_copy_queue", prop) > 0 &&
                  prop[0] == '1';
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "surface queued-copy experiment=%d", enabled);
    }
    return enabled;
#else
    return false;
#endif
}

static bool scaled_copy_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.xemu.vk.scaled_copy", prop) > 0 &&
                  prop[0] == '1';
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "scaled GPU copy experiment=%d", enabled);
    }
    return enabled;
#else
    return false;
#endif
}

static bool depth_queue_copy_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.xemu.vk.depth_copy_queue", prop) > 0 &&
                  prop[0] == '1';
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "depth queued-copy experiment=%d", enabled);
    }
    return enabled;
#else
    return false;
#endif
}

static bool zeta_queue_copy_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.xemu.vk.zeta_copy_queue", prop) > 0 &&
                  prop[0] == '1';
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "depth-stencil queued-copy experiment=%d", enabled);
    }
    return enabled;
#else
    return false;
#endif
}

static bool can_queue_depth_copy(PGRAPHState *pg, const SurfaceBinding *s,
                                 const TextureBinding *t)
{
#if OPT_SURF_TO_TEX_INLINE
    bool stencil = s->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT;
    if (stencil ? !zeta_queue_copy_enabled() : !depth_queue_copy_enabled()) return false;
    PGRAPHVkState *r = pg->vk_renderer_state;
    const TextureShape *shape = &t->key.state;
    VkDepthCopyPolicy policy = {
        .width = shape->width, .height = shape->height,
        .scale = pg->surface_scale_factor, .depth = shape->depth,
        .dimensions = shape->dimensionality, .levels = shape->levels,
        .layers = t->image_config.array_layers,
        .source_width = s->width, .source_height = s->height,
        .source_image_width = s->image_width, .source_image_height = s->image_height,
        .destination_width = t->image_config.width,
        .destination_height = t->image_config.height,
        .scratch_capacity = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
        .output_capacity = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer_size,
        .stencil_alignment = r->device_props.limits.minStorageBufferOffsetAlignment,
        .max_workgroup_x = r->device_props.limits.maxComputeWorkGroupSize[0],
        .max_invocations = r->device_props.limits.maxComputeWorkGroupInvocations,
        .max_group_count = r->device_props.limits.maxComputeWorkGroupCount[0],
        .stencil = stencil,
        .allow_scaled = scaled_copy_enabled(), .cubemap = shape->cubemap,
        .border = vk_surface_copy_storage_border(shape->border,
            kelvin_color_format_info_map[shape->color_format].linear),
        .active = s->access_cb != NULL,
        .initialized = s->initialized && s->image_layout != VK_IMAGE_LAYOUT_UNDEFINED,
        .upload_pending = s->upload_pending, .download_pending = s->download_pending,
        .native_compatible = !s->color &&
            (stencil ?
                (s->host_fmt.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
                 s->host_fmt.host_bytes_per_pixel == 4 &&
                 (t->image_config.format == VK_FORMAT_R8G8B8A8_UNORM ||
                  t->image_config.format == VK_FORMAT_B8G8R8A8_UNORM)) :
                (s->host_fmt.aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
                 s->host_fmt.vk_format == VK_FORMAT_D16_UNORM &&
                 s->host_fmt.host_bytes_per_pixel == 2 &&
                 t->image_config.format == VK_FORMAT_R16_UNORM)) &&
            texture_format_for_shape(r, *shape, t->key.address).vk_format == t->image_config.format &&
            t->image_config.image_type == VK_IMAGE_TYPE_2D &&
            t->image_config.depth == 1 && t->image_config.mip_levels == 1 &&
            (t->image_config.usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) ==
                (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        .distinct_images = s->image && t->image && s->image != t->image,
    };
    return vk_depth_copy_eligible(&policy) &&
        r->storage_buffers[BUFFER_COMPUTE_DST].buffer &&
        (!stencil || (r->storage_buffers[BUFFER_COMPUTE_SRC].buffer &&
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer !=
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer)) &&
        (t->current_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
         t->current_layout == VK_IMAGE_LAYOUT_UNDEFINED);
#else
    (void)pg; (void)s; (void)t;
    return false;
#endif
}

static bool can_queue_surface_copy(PGRAPHState *pg, const SurfaceBinding *s,
                                    const TextureBinding *t)
{
#if OPT_SURF_TO_TEX_INLINE
    if (!surface_queue_copy_enabled()) {
        return false;
    }
    PGRAPHVkState *r = pg->vk_renderer_state;
    const TextureShape *shape = &t->key.state;
    VkFormat format = t->image_config.format;
    bool linear = kelvin_color_format_info_map[shape->color_format].linear;
    VkSurfaceCopyPolicy policy = {
        .width = shape->width, .height = shape->height, .depth = shape->depth,
        .dimensions = shape->dimensionality, .levels = shape->levels,
        .layers = t->image_config.array_layers, .scale = pg->surface_scale_factor,
        .source_width = s->width, .source_height = s->height,
        .source_image_width = s->image_width,
        .source_image_height = s->image_height,
        .destination_width = t->image_config.width,
        .destination_height = t->image_config.height,
        .allow_scaled = scaled_copy_enabled(),
        .cubemap = shape->cubemap,
        .border = vk_surface_copy_storage_border(shape->border, linear),
        .color = s->color,
        .active = s->access_cb != NULL,
        .initialized = s->initialized && s->image_layout != VK_IMAGE_LAYOUT_UNDEFINED,
        .upload_pending = s->upload_pending, .download_pending = s->download_pending,
        .byte_copy_compatible = s->host_fmt.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
            vk_surface_copy_formats_compatible(s->host_fmt.vk_format, format) &&
            format == texture_format_for_shape(r, *shape, t->key.address).vk_format &&
            t->image_config.image_type == VK_IMAGE_TYPE_2D &&
            (t->image_config.usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) ==
                (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
            t->image_config.depth == 1 &&
            t->image_config.mip_levels == 1,
        .distinct_images = s->image && t->image && s->image != t->image,
    };
    uint32_t reasons = vk_surface_copy_rejections(&policy);
    if (t->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        t->current_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        reasons |= VK_SURFACE_COPY_LAYOUT;
    }
#ifdef __ANDROID__
    static unsigned rejected_count;
    static uint32_t last_width, last_height, last_reasons;
    if (reasons && rejected_count < 24 &&
        (last_width != policy.width || last_height != policy.height ||
         last_reasons != reasons) && nv2a_stall_start()) {
        rejected_count++;
        last_width = policy.width;
        last_height = policy.height;
        last_reasons = reasons;
        __android_log_print(ANDROID_LOG_INFO, "xemu-stall-detail",
            "surface queued-copy rejected: reasons=%u width=%u height=%u "
            "depth=%u dimensions=%u levels=%u layers=%u scale=%u "
            "linear=%d border_flag=%d storage_border=%d color=%d active=%d "
            "initialized=%d upload=%d download=%d copy_compatible=%d "
            "distinct=%d source_fmt=%d destination_fmt=%d source_layout=%d "
            "destination_layout=%d allocation_width=%u allocation_height=%u "
            "allocation_depth=%u allocation_levels=%u allocation_type=%d usage=%u",
            reasons, policy.width, policy.height, policy.depth, policy.dimensions,
            policy.levels, policy.layers, policy.scale, linear, shape->border,
            policy.border, policy.color, policy.active, policy.initialized,
            policy.upload_pending, policy.download_pending,
            policy.byte_copy_compatible, policy.distinct_images,
            s->host_fmt.vk_format, format, s->image_layout, t->current_layout,
            t->image_config.width, t->image_config.height, t->image_config.depth,
            t->image_config.mip_levels, t->image_config.image_type,
            t->image_config.usage);
    }
#endif
    return reasons == 0;
#else
    (void)pg; (void)s; (void)t;
    return false;
#endif
}

static void wait_texture_reuse(PGRAPHState *pg, TextureBinding *texture,
                               SurfaceBinding *surface)
{
    int64_t start = nv2a_stall_start();
    pgraph_vk_flush_all_frames(pg);
    nv2a_stall_record(NV2A_STALL_TEXTURE_REUSE_WAIT, start, 0);
#ifdef __ANDROID__
    static unsigned detail_count;
    if (start && detail_count < 24) {
        int64_t elapsed = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start;
        if (elapsed >= 2000) {
            detail_count++;
            __android_log_print(ANDROID_LOG_INFO, "xemu-stall-detail",
                "texture_reuse_wait route=%s width=%u height=%u levels=%u "
                "source_fmt=%d destination_fmt=%d us=%" PRId64,
                surface ? (surface->color ? "color" : "depth") : "cpu",
                texture->key.state.width, texture->key.state.height,
                texture->key.state.levels,
                surface ? surface->host_fmt.vk_format : VK_FORMAT_UNDEFINED,
                texture->image_config.format, elapsed);
        }
    }
#else
    (void)texture; (void)surface;
#endif
}

typedef struct DepthStencilPackContext {
    PGRAPHState *pg;
    SurfaceBinding *surface;
} DepthStencilPackContext;

static void record_depth_stencil_pack(void *user, VkCommandBuffer cmd,
                                       VkBuffer input, VkBuffer output)
{
    DepthStencilPackContext *context = user;
    pgraph_vk_pack_depth_stencil(context->pg, context->surface, cmd,
                                 input, output, false);
}

static bool copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture, bool queue_copy)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!surface->color && can_queue_depth_copy(pg, surface, texture)) {
        queue_copy = true;
    }

    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        pgraph_vk_flush_draw_queue(d);
    }
    bool eligible = queue_copy && (surface->color ?
        can_queue_surface_copy(pg, surface, texture) :
        can_queue_depth_copy(pg, surface, texture));
    if (queue_copy && !eligible &&
        texture->submit_time + r->num_active_frames > r->submit_count) {
        /* Prior draw emission can change surface state. If the strict policy
         * stops matching, restore the ordinary wait before the fallback. */
        wait_texture_reuse(pg, texture, surface);
    }
    if (eligible) {
        bool stencil = !surface->color &&
            surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT;
        if (stencil && pgraph_vk_compute_needs_finish(r)) {
            /* Descriptor exhaustion still requires retirement; do not rewrite
             * a descriptor referenced by a submitted or recorded dispatch. */
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
            pgraph_vk_flush_all_frames(pg);
        }
        /* Prior users are now recorded. Preserve stable allocations and views:
         * the queue barriers order their reads before this in-place write. */
        int64_t start = nv2a_stall_start();
        OPT_STAT_INC(nd_copysurf);
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        if (surface->color) {
            vk_surface_copy_record(cmd, surface->image, surface->image_layout,
                texture->image, texture->current_layout,
                surface->image_width, surface->image_height);
        } else if (stencil) {
            DepthStencilPackContext context = {pg, surface};
            vk_depth_stencil_copy_record(cmd, surface->image, surface->image_layout,
                texture->image, texture->current_layout,
                r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
                vk_depth_stencil_offset(surface->image_width, surface->image_height,
                    r->device_props.limits.minStorageBufferOffsetAlignment),
                surface->image_width, surface->image_height,
                record_depth_stencil_pack, &context);
        } else {
            vk_depth_copy_record(cmd, surface->image, surface->image_layout,
                texture->image, texture->current_layout,
                r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
                surface->image_width, surface->image_height);
        }
        pgraph_vk_end_nondraw_commands(pg, cmd);
        texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        texture->draw_time = surface->draw_time;
        nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
        nv2a_stall_record(NV2A_STALL_TEXTURE_RECORD, start, 0);
#ifdef __ANDROID__
        static bool engaged, depth_engaged, stencil_engaged;
        if (stencil && !stencil_engaged) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                "depth-stencil queued-copy path engaged: D24 integer pack scale=%d "
                "copy_width=%u copy_height=%u", pg->surface_scale_factor,
                surface->image_width, surface->image_height);
            stencil_engaged = true;
        }
        if (!surface->color && !stencil && !depth_engaged) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                "depth queued-copy path engaged: D16 to R16 scale=%d "
                "copy_width=%u copy_height=%u", pg->surface_scale_factor,
                surface->image_width, surface->image_height);
            depth_engaged = true;
        }
        if (surface->color && !engaged) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                "surface queued-copy path engaged: byte-preserving color 2D "
                "scale=%d copy_width=%u copy_height=%u",
                pg->surface_scale_factor, surface->image_width, surface->image_height);
            engaged = true;
        }
        static struct {
            VkFormat source, destination;
            uint32_t width, height;
        } reinterpret_shapes[8];
        static unsigned reinterpret_count;
        if (surface->color && surface->host_fmt.vk_format != texture->image_config.format) {
            bool seen = false;
            for (unsigned i = 0; i < reinterpret_count; i++) {
                seen |= reinterpret_shapes[i].source == surface->host_fmt.vk_format &&
                    reinterpret_shapes[i].destination == texture->image_config.format &&
                    reinterpret_shapes[i].width == surface->width &&
                    reinterpret_shapes[i].height == surface->height;
            }
            if (!seen && reinterpret_count < ARRAY_SIZE(reinterpret_shapes)) {
                reinterpret_shapes[reinterpret_count].source = surface->host_fmt.vk_format;
                reinterpret_shapes[reinterpret_count].destination = texture->image_config.format;
                reinterpret_shapes[reinterpret_count].width = surface->width;
                reinterpret_shapes[reinterpret_count].height = surface->height;
                reinterpret_count++;
                __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                    "surface queued-copy reinterpret path engaged: source_fmt=%d "
                    "destination_fmt=%d width=%u height=%u",
                    surface->host_fmt.vk_format, texture->image_config.format,
                    surface->width, surface->height);
            }
        }
#endif
        return true;
    }
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return false;
    }
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = r->texture_format_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

#if OPT_SURF_TO_TEX_INLINE
    OPT_STAT_INC(nd_copysurf);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->image_layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->image_layout);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_TO_TEX_INLINE
    pgraph_vk_end_nondraw_commands(pg, cmd);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif

    texture->draw_time = surface->draw_time;
    return false;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    /* Only formats that may be sampled straight out of a surface belong
     * above. Returning 0 for anything else means the caller falls back to a
     * normal texture upload, which is what formats that need a component
     * swizzle or a signed reinterpretation want -- aliasing a surface skips
     * both.
     */
    default:                                return 0;
    }
}

static bool check_surface_to_texture_compatiblity(PGRAPHVkState *r,
                                                  const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if (surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return false;
    }

    if (!surface->color) {
        return true;
    }

    VkColorFormatInfo tex_vkf = r->texture_format_map[shape->color_format];
    return tex_vkf.vk_format &&
           surface->host_fmt.host_bytes_per_pixel == vk_format_texel_size(tex_vkf.vk_format);
}

static bool cube_copy_enabled(void)
{
#ifdef __ANDROID__
    static int enabled = -1;
    if (enabled < 0) {
        char prop[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.xemu.vk.cube_copy", prop) > 0 &&
                  prop[0] == '1';
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "GPU cubemap copy experiment=%d", enabled);
    }
    return enabled;
#else
    return false;
#endif
}

static bool get_cube_copy_sources(PGRAPHState *pg, const TextureShape *shape,
    hwaddr base, size_t length, SurfaceBinding *sources[6], VkCubeCopyKey *key)
{
    if (!cube_copy_enabled() || !shape->cubemap ||
        shape->color_format != NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5 ||
        shape->levels != 1 || shape->border) {
        return false;
    }
    uint32_t image_width, image_height;
    if (!vk_scaled_copy_extent(shape->width, shape->height,
            pg->surface_scale_factor, scaled_copy_enabled(),
            &image_width, &image_height)) {
        return false;
    }
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    size_t layer_size = get_cubemap_layer_size(pg, *shape);
    if (!layer_size || layer_size > SIZE_MAX / 6 ||
        length != layer_size * 6 || base > UINT64_MAX - length) {
        return false;
    }
    VkCubeCopyFace faces[6] = { 0 };
    memset(key, 0, sizeof(*key));
    key->surface_list_gen = r->surface_list_gen;
    for (unsigned i = 0; i < 6; i++) {
        SurfaceBinding *s = pgraph_vk_surface_get(d, base + layer_size * i);
        if (!s) {
            return false;
        }
        sources[i] = s;
        faces[i] = (VkCubeCopyFace) {
            .address = s->vram_addr, .size = s->size, .image = (uint64_t)s->image,
            .width = s->width, .height = s->height, .pitch = s->pitch,
            .image_width = s->image_width, .image_height = s->image_height,
            .active = s->access_cb != NULL, .color = s->color,
            .swizzle = s->swizzle,
            .initialized = s->initialized && s->image_layout != VK_IMAGE_LAYOUT_UNDEFINED,
            .native_rgb565 = s->host_fmt.vk_format == VK_FORMAT_R5G6B5_UNORM_PACK16 &&
                s->host_fmt.host_bytes_per_pixel == 2 &&
                s->shape.color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5,
            .upload_pending = s->upload_pending,
            .download_pending = s->download_pending,
        };
        key->image[i] = (uint64_t)s->image;
        key->draw_time[i] = s->draw_time;
        key->draw_generation[i] = s->draw_generation;
    }
    return vk_cube_copy_eligible(base, length, layer_size, shape->width,
        shape->height, shape->depth, shape->dimensionality, shape->levels,
        pg->surface_scale_factor, shape->cubemap, shape->border,
        r->texture_format_map[shape->color_format].vk_format ==
            VK_FORMAT_R5G6B5_UNORM_PACK16, scaled_copy_enabled(), faces);
}

static bool cube_has_pending_source_draws(PGRAPHVkState *r,
                                          const VkCubeCopyKey *key)
{
    /* Deferred draw generations advance when entries are emitted. A queued
     * write to a source must be emitted before checking/reusing its cube. */
    return (r->reorder_window.count > 0 || r->draw_queue.count > 0) &&
        r->color_binding &&
        vk_cube_copy_has_source(key, (uint64_t)r->color_binding->image);
}

static bool cube_binding_needs_refresh(PGRAPHState *pg, TextureBinding *binding)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (cube_has_pending_source_draws(r, &binding->cube_copy_cache.key)) {
        return true;
    }
    if (binding->cube_copy_cache.valid &&
        binding->cube_seen_draw_gen == r->surface_draw_gen &&
        binding->cube_seen_list_gen == r->surface_list_gen) {
        return false;
    }
    SurfaceBinding *sources[6];
    VkCubeCopyKey key;
    bool eligible = get_cube_copy_sources(pg, &binding->key.state,
        binding->key.texture_vram_offset, binding->key.texture_length,
        sources, &key);
    if (!eligible || vk_cube_copy_needs_refresh(&binding->cube_copy_cache, &key)) {
        return true;
    }
    binding->cube_seen_draw_gen = r->surface_draw_gen;
    binding->cube_seen_list_gen = r->surface_list_gen;
    return false;
}

static void copy_cube_surfaces_to_texture(PGRAPHState *pg,
    SurfaceBinding *sources[6], TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    /* A scaled GPU cube has its own cache key and allocation. Never copy into
     * the native-size CPU fallback binding or a stale scale allocation. */
    assert(texture->image_config.width == sources[0]->image_width &&
           texture->image_config.height == sources[0]->image_height &&
           texture->image_config.depth == 1 &&
           texture->image_config.array_layers == 6 &&
           texture->image_config.mip_levels == 1 &&
           texture->image_config.format == VK_FORMAT_R5G6B5_UNORM_PACK16);
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    VkCubeCopyKey before = { .surface_list_gen = r->surface_list_gen };
    for (unsigned i = 0; i < 6; i++) {
        before.image[i] = (uint64_t)sources[i]->image;
        before.draw_time[i] = sources[i]->draw_time;
        before.draw_generation[i] = sources[i]->draw_generation;
    }
    if (!vk_cube_copy_needs_refresh(&texture->cube_copy_cache, &before) &&
        !cube_has_pending_source_draws(r, &before)) {
        /* An unchanged cube does not break batching of unrelated draws. */
        texture->cube_seen_draw_gen = r->surface_draw_gen;
        texture->cube_seen_list_gen = r->surface_list_gen;
        texture->possibly_dirty = false;
        return;
    }
    /* Record prior users before an in-place GPU update. Descriptors and
     * allocations stay live under existing retirement policies; GPU barriers
     * preserve required queue dependencies without a host readback wait. */
    if (r->reorder_window.count > 0) {
        pgraph_vk_flush_reorder_window(d);
    }
    if (r->draw_queue.count > 0) {
        pgraph_vk_flush_draw_queue(d);
    }
    VkCubeCopyKey key = { .surface_list_gen = r->surface_list_gen };
    VkImage images[6];
    VkImageLayout layouts[6];
    for (unsigned i = 0; i < 6; i++) {
        images[i] = sources[i]->image;
        layouts[i] = sources[i]->image_layout;
        key.image[i] = (uint64_t)images[i];
        key.draw_time[i] = sources[i]->draw_time;
        key.draw_generation[i] = sources[i]->draw_generation;
    }
    if (vk_cube_copy_needs_refresh(&texture->cube_copy_cache, &key)) {
        int64_t start = nv2a_stall_start();
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        vk_cube_copy_record(cmd, images, layouts, texture->image,
            texture->current_layout, texture->image_config.width,
            texture->image_config.height);
        pgraph_vk_end_nondraw_commands(pg, cmd);
        texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        texture->cube_copy_cache.key = key;
        texture->cube_copy_cache.valid = true;
        nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
        nv2a_stall_record(NV2A_STALL_TEXTURE_RECORD, start, 0);
#ifdef __ANDROID__
        static bool engaged;
        static unsigned detail_count;
        if (!engaged) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                "GPU cubemap copy path engaged: RGB565 six faces "
                "scale=%d copy_width=%u copy_height=%u",
                pg->surface_scale_factor, texture->image_config.width,
                texture->image_config.height);
            engaged = true;
        }
        if (start && detail_count++ < 16) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-stall-detail",
                "cube_gpu_copy faces=6 width=%u height=%u us=%" PRId64,
                texture->key.state.width, texture->key.state.height,
                qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start);
        }
#endif
    }
    texture->cube_surface_copy = true;
    texture->draw_time = 0;
    texture->cube_seen_draw_gen = r->surface_draw_gen;
    texture->cube_seen_list_gen = r->surface_list_gen;
    texture->possibly_dirty = false;
}

#ifdef __ANDROID__
/* Bounded shape-only evidence for the expensive CPU fallback. No addresses,
 * texture contents or shader source are emitted. Collect before download so
 * dirty counts still identify the surfaces which required synchronization. */
typedef struct TextureReadbackDetail {
    unsigned overlaps, dirty;
    bool has_surface, exact, source_dirty, color, swizzle, upload;
    unsigned format, width, height, pitch, bpp;
} TextureReadbackDetail;

static unsigned texture_readback_details_emitted;

static TextureReadbackDetail texture_readback_detail(PGRAPHState *pg,
                                                     hwaddr start, size_t length)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureReadbackDetail detail = {};
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->vram_addr >= start + length ||
            start >= surface->vram_addr + surface->size) {
            continue;
        }
        detail.overlaps++;
        detail.dirty += surface->draw_dirty;
        /* Prefer the exact-base surface; otherwise retain the first dirty
         * overlap, or the first overlap if none is dirty. */
        if (!detail.has_surface ||
            (!detail.exact && (surface->vram_addr == start ||
                              (surface->draw_dirty && !detail.source_dirty)))) {
            detail.has_surface = true;
            detail.exact = surface->vram_addr == start;
            detail.source_dirty = surface->draw_dirty;
            detail.color = surface->color;
            detail.swizzle = surface->swizzle;
            detail.upload = surface->upload_pending;
            detail.format = surface->color ? surface->shape.color_format :
                                            surface->shape.zeta_format;
            detail.width = surface->width;
            detail.height = surface->height;
            detail.pitch = surface->pitch;
            detail.bpp = surface->host_fmt.host_bytes_per_pixel;
        }
    }
    return detail;
}

static void log_texture_readback_detail(const TextureShape *shape,
                                       TextureReadbackDetail detail,
                                       int64_t elapsed)
{
    if (elapsed < 2000 ||
        qatomic_fetch_inc(&texture_readback_details_emitted) >= 16) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "xemu-stall-detail",
        "texture_fallback us=%" PRId64 " fmt=%u width=%u height=%u "
        "depth=%u dim=%u mips=%u cube=%d border=%d linear=%d pitch=%u "
        "overlaps=%u dirty=%u exact=%d surface=%d color=%d "
        "surface_fmt=%u surface_width=%u surface_height=%u "
        "surface_pitch=%u surface_bpp=%u swizzle=%d upload=%d",
        elapsed, shape->color_format, shape->width, shape->height,
        shape->depth, shape->dimensionality, shape->levels, shape->cubemap,
        shape->border, kelvin_color_format_info_map[shape->color_format].linear,
        shape->pitch, detail.overlaps, detail.dirty, detail.exact,
        detail.has_surface, detail.color, detail.format, detail.width,
        detail.height, detail.pitch, detail.bpp, detail.swizzle, detail.upload);
}
#endif

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    StorageBuffer *dummy_staging = get_staging_buffer(r, BUFFER_STAGING_SRC);
    mapped_memory_ptr = (uint8_t *)dummy_staging->mapped;
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator, dummy_staging->allocation, 0,
                       texture_data_size);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, dummy_staging->buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image_config = {
            .format = VK_FORMAT_R8_UNORM,
            .image_type = VK_IMAGE_TYPE_2D,
            .width = image_create_info.extent.width,
            .height = image_create_info.extent.height,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .flags = 0,
        },
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
#if OPT_BINDLESS_TEXTURES
        .bindless_slot = 0,
#endif
    };

#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        VkDescriptorImageInfo bl_info = {
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = texture_image_view,
            .sampler = texture_sampler,
        };
        VkWriteDescriptorSet bl_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->bindless_descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &bl_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &bl_write, 0, NULL);
    }
#endif
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    VK_LOG("create_texture: idx=%d", texture_idx);
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_vk_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_vk_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_vk_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_vk_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;
    key.max_anisotropy = max_anisotropy;

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;
    if (r->tex_surface_direct[texture_idx]) {
        /* The descriptor may still reference the surface image.  Ensure a
         * transition back to the texture cache refreshes its image view and
         * layout even when the cached TextureBinding itself is unchanged. */
        r->texture_bindings_changed = true;
    }
    r->tex_surface_direct[texture_idx] = false;

    if (!pgraph_vk_texture_range_valid(d, texture_vram_offset,
                                       texture_length)) {
        pgraph_vk_log_invalid_texture_range(d, texture_idx, "texture",
                                            &state, texture_vram_offset,
                                            texture_length);
        pgraph_vk_bind_invalid_texture(r, texture_idx);
        NV2A_VK_DGROUP_END();
        return;
    }

    if (is_indexed &&
        !pgraph_vk_texture_range_valid(d, texture_palette_vram_offset,
                                       texture_palette_data_size)) {
        pgraph_vk_log_invalid_texture_range(d, texture_idx, "palette",
                                            &state,
                                            texture_palette_vram_offset,
                                            texture_palette_data_size);
        pgraph_vk_bind_invalid_texture(r, texture_idx);
        NV2A_VK_DGROUP_END();
        return;
    }

    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(r, surface, &state);

        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }

        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    SurfaceBinding *cube_sources[6];
    VkCubeCopyKey cube_key;
    bool cube_to_texture = get_cube_copy_sources(pg, &state,
        texture_vram_offset, texture_length, cube_sources, &cube_key);
    bool gpu_texture = surface_to_texture || cube_to_texture;

    /*
     * Shelved surfaces are deliberately not used as texture sources.  A
     * shelved surface has no CPU access callback registered, so guest
     * writes to its VRAM range go untracked, and the surface_to_texture
     * path below refreshes only when surface->draw_time advances --
     * which never happens for a surface nothing draws to.  Binding one
     * here would freeze the texture at the shelved image's contents.
     *
     * Shelving now reads the surface back to VRAM, so the normal upload
     * path below sees the correct pixels.
     */

    if (!gpu_texture) {
        unsigned cache_idx =
            surf_range_cache_index(texture_vram_offset, texture_length);
        typeof(r->tex_surf_range_cache[0]) *range_ent =
            &r->tex_surf_range_cache[cache_idx];

        bool skip_surf_scan = false;
        if (surf_range_memo_enabled() && range_ent->valid &&
            range_ent->vram_addr == texture_vram_offset &&
            range_ent->length == texture_length &&
            range_ent->surface_list_gen == r->surface_list_gen &&
            (/* No overlap last time — nothing to download */
             !range_ent->had_overlap ||
             /* Had overlap but no surface has been drawn to since last
              * download — overlapping surfaces are still clean. */
             range_ent->surface_draw_gen == r->surface_draw_gen)) {
            skip_surf_scan = true;
        }

        if (skip_surf_scan) { OPT_STAT_INC(dif_scan_skip); }
        else { OPT_STAT_INC(dif_scan_run); }

        if (!skip_surf_scan) {
            int64_t readback_start = nv2a_stall_start();
#ifdef __ANDROID__
            bool collect_detail = readback_start &&
                qatomic_read(&texture_readback_details_emitted) < 16;
            TextureReadbackDetail detail = {};
            if (collect_detail) {
                detail = texture_readback_detail(pg, texture_vram_offset,
                                                 texture_length);
            }
#endif
            bool had_overlap = pgraph_vk_download_surfaces_in_range_if_dirty(
                pg, texture_vram_offset, texture_length);
            if (had_overlap) {
                nv2a_stall_record(NV2A_STALL_TEXTURE_RANGE_READBACK,
                                 readback_start, texture_length);
#ifdef __ANDROID__
                if (collect_detail) {
                    log_texture_readback_detail(&state, detail,
                        qemu_clock_get_us(QEMU_CLOCK_REALTIME) - readback_start);
                }
#endif
            }
            range_ent->vram_addr = texture_vram_offset;
            range_ent->length = texture_length;
            range_ent->had_overlap = had_overlap;
            range_ent->surface_list_gen = r->surface_list_gen;
            range_ent->surface_draw_gen = r->surface_draw_gen;
            range_ent->valid = true;
        }
    }

    if (gpu_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    TextureBinding *snode;
    bool binding_found;

    if (r->tex_binding_cache[texture_idx].key_hash == key_hash &&
        r->tex_binding_cache[texture_idx].binding &&
        r->tex_binding_cache[texture_idx].binding->image != VK_NULL_HANDLE) {
        snode = r->tex_binding_cache[texture_idx].binding;
        binding_found = true;
    } else {
        LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
        if (!node) {
            /* LRU exhausted — all texture slots in-flight. Skip this
             * texture bind and use whatever was previously bound. */
            return;
        }
        snode = container_of(node, TextureBinding, node);
        binding_found = snode->image != VK_NULL_HANDLE;
        r->tex_binding_cache[texture_idx].key_hash = key_hash;
        r->tex_binding_cache[texture_idx].binding = binding_found ? snode : NULL;
    }

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
        if (!gpu_texture &&
            snode->dirty_check_frame == pg->frame_time) {
            possibly_dirty_checked = true;
            possibly_dirty |= snode->dirty_check_result;
        }
    } else {
        possibly_dirty = true;
    }

    if (!gpu_texture && !possibly_dirty_checked) {
        bool skip_dirty_check = binding_found &&
            snode->dirty_check_frame == pg->frame_time &&
            !snode->dirty_check_result;
        if (!skip_dirty_check) {
            bool vram_dirty = check_texture_dirty(
                d, texture_vram_offset, texture_length);
            if (texture_palette_data_size) {
                vram_dirty |= check_texture_dirty(
                    d, texture_palette_vram_offset, texture_palette_data_size);
            }
            if (vram_dirty) {
                possibly_dirty = true;
            }
            if (binding_found) {
                snode->dirty_check_frame = pg->frame_time;
                snode->dirty_check_result = vram_dirty;
            }
        }
    }

    if (binding_found && possibly_dirty && !gpu_texture) {
        bool vram_confirmed_clean =
            snode->dirty_check_frame == pg->frame_time &&
            !snode->dirty_check_result;
        if (vram_confirmed_clean) {
            snode->possibly_dirty = false;
            possibly_dirty = false;
        }
    }

    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    /*
     * A cached texture last filled from a render surface (draw_time set) is
     * stale once the range is no longer a compatible surface: re-read memory
     * instead of reusing the surface's copy. Tron 2.0 clears a swizzled
     * texture by briefly making it a linear render target (upstream PR
     * #3064).
     */
    bool was_surface_copy = binding_found &&
        (snode->draw_time || snode->cube_surface_copy);

    uint64_t content_hash = 0;
    if (!gpu_texture && (was_surface_copy || possibly_dirty)) {
        int64_t hash_start = nv2a_stall_start();
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
        nv2a_stall_record(NV2A_STALL_TEXTURE_HASH, hash_start, texture_length);
    }

    if (binding_found) {
        bool did_s2t_copy = false;
        bool did_upload = false;
        if (cube_to_texture) {
            copy_cube_surfaces_to_texture(pg, cube_sources, snode);
        } else if (surface_to_texture) {
            if (surface->draw_time != snode->draw_time) {
                bool can_direct_bind = can_bind_surface_direct(r, surface);
                bool queue_copy = !can_direct_bind &&
                    (can_queue_surface_copy(pg, surface, snode) ||
                     can_queue_depth_copy(pg, surface, snode));
                bool cached_inflight =
                    snode->submit_time + r->num_active_frames > r->submit_count;
                if (!queue_copy &&
                    cached_inflight) {
                    wait_texture_reuse(pg, snode, surface);
                }

                if (can_direct_bind) {
                    VkImageLayout direct_layout;
                    if (surface->color) {
                        bind_surface_as_texture(pg, surface, snode);
                        direct_layout = VK_IMAGE_LAYOUT_GENERAL;
                    } else {
                        bind_zeta_surface_as_texture(pg, surface, snode);
                        direct_layout =
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    }
                    r->tex_surface_direct[texture_idx] = true;
                    r->tex_surface_direct_views[texture_idx] =
                        surface->image_view;
                    r->tex_surface_direct_layout[texture_idx] = direct_layout;
                    r->texture_bindings_changed = true;
#if OPT_BINDLESS_TEXTURES
                    if (r->bindless_textures_supported) {
                        VkDescriptorImageInfo bl_info = {
                            .imageLayout = direct_layout,
                            .imageView = surface->image_view,
                            .sampler = snode->sampler,
                        };
                        VkWriteDescriptorSet bl_write = {
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .dstSet = r->bindless_descriptor_set,
                            .dstBinding = snode->bindless_binding,
                            .dstArrayElement = snode->bindless_slot,
                            .descriptorType =
                                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            .descriptorCount = 1,
                            .pImageInfo = &bl_info,
                        };
                        vkUpdateDescriptorSets(r->device, 1, &bl_write, 0,
                                               NULL);
                    }
#endif
                } else {
                    bool queued = copy_surface_to_texture(pg, surface, snode, queue_copy);
#ifdef __ANDROID__
                    static bool bypass_logged;
                    if (queued && cached_inflight && !bypass_logged) {
                        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "surface queued-copy bypassed host texture reuse wait");
                        bypass_logged = true;
                    }
#else
                    (void)queued;
#endif
                }
                did_s2t_copy = true;
            } else if (can_bind_surface_direct(r, surface)) {
                // Same draw_time: surface hasn't changed, reuse direct view
                r->tex_surface_direct[texture_idx] = true;
                r->tex_surface_direct_views[texture_idx] =
                    surface->image_view;
            }
        } else {
            if (was_surface_copy ||
                (possibly_dirty && content_hash != snode->hash)) {
                if (snode->submit_time + r->num_active_frames > r->submit_count) {
                    wait_texture_reuse(pg, snode, NULL);
                }
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
                snode->draw_time = 0;
                snode->cube_surface_copy = false;
                snode->cube_copy_cache.valid = false;
                did_upload = true;
            }
            snode->possibly_dirty = false;
        }

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;

    VkColorFormatInfo vkf = texture_format_for_shape(r, state, address);
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width, // FIXME: Use adjusted size?
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (gpu_texture) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }
    if (texture_can_compute_upload(r, state, vkf.vk_format)) {
        image_create_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (vkf.vk_format == VK_FORMAT_R5G6B5_UNORM_PACK16) {
            image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                       VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
    }

    TextureImageConfig pool_cfg = {
        .format = image_create_info.format,
        .image_type = image_create_info.imageType,
        .width = image_create_info.extent.width,
        .height = image_create_info.extent.height,
        .depth = image_create_info.extent.depth,
        .mip_levels = image_create_info.mipLevels,
        .array_layers = image_create_info.arrayLayers,
        .flags = image_create_info.flags,
        .usage = image_create_info.usage,
    };
    snode->image_config = pool_cfg;

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    int64_t image_start = nv2a_stall_start();
    VkResult create_result = VK_SUCCESS;
    if (image_pool_acquire(r, &pool_cfg, &snode->image, &snode->allocation)) {
        snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        OPT_STAT_INC(tex_pool_hits);
    } else {
        create_result = vmaCreateImage(r->allocator, &image_create_info,
                                       &alloc_create_info, &snode->image,
                                       &snode->allocation, NULL);
        OPT_STAT_INC(tex_pool_misses);
    }
    if (create_result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        create_result == VK_ERROR_OUT_OF_HOST_MEMORY) {
        const VkPhysicalDeviceMemoryProperties *props;
        vmaGetMemoryProperties(r->allocator, &props);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(r->allocator, budgets);
        for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
            VmaBudget *b = &budgets[i];
            VK_LOG_ERROR("OOM DIAG heap[%u]: alloc=%uMB usage=%uMB budget=%uMB "
                         "blockBytes=%uMB flags=0x%x",
                         i,
                         (unsigned)(b->statistics.allocationBytes >> 20),
                         (unsigned)(b->usage >> 20),
                         (unsigned)(b->budget >> 20),
                         (unsigned)(b->statistics.blockBytes >> 20),
                         props->memoryHeaps[i].flags);
        }
        VK_LOG_ERROR("OOM creating texture: %ux%ux%u fmt=%d mips=%u layers=%u "
                     "(result=%d)",
                     image_create_info.extent.width,
                     image_create_info.extent.height,
                     image_create_info.extent.depth,
                     image_create_info.format,
                     image_create_info.mipLevels,
                     image_create_info.arrayLayers,
                     create_result);

        image_pool_drain(r);
        pgraph_vk_flush_all_frames(pg);

        for (int evict_pass = 0; evict_pass < 64; evict_pass++) {
            if (!lru_try_evict_one(&r->texture_cache)) break;
        }
        image_pool_drain(r);

        create_result = vmaCreateImage(r->allocator, &image_create_info,
                                       &alloc_create_info, &snode->image,
                                       &snode->allocation, NULL);
        if (create_result != VK_SUCCESS) {
            VK_LOG_ERROR("OOM retry FAILED (result=%d), flushing all textures",
                         create_result);
            lru_flush(&r->texture_cache);
            image_pool_drain(r);
            create_result = vmaCreateImage(r->allocator, &image_create_info,
                                           &alloc_create_info, &snode->image,
                                           &snode->allocation, NULL);
        }
    }
    if (create_result != VK_SUCCESS) {
        VK_LOG_ERROR("vmaCreateImage FATAL: result=%d", create_result);
    }
    assert(create_result == VK_SUCCESS && "vmaCreateImage failed");

    VkImageViewUsageCreateInfo sampled_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &sampled_usage,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = vkf.vk_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = vkf.component_map,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));
    nv2a_stall_record(NV2A_STALL_TEXTURE_IMAGE, image_start, 0);


    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = vkf.vk_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = image_view_create_info.format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || image_create_info.mipLevels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    float lod_bias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
        lod_bias = r->device_props.limits.maxSamplerLodBias;
    } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
        lod_bias = -r->device_props.limits.maxSamplerLodBias;
    }
    bool force_lod0 = false;
#ifdef __ANDROID__
    {
        static int flag = -1;
        if (flag < 0) {
            char prop[PROP_VALUE_MAX] = {};
            flag = (__system_property_get("debug.xemu.vk.force_lod0", prop) > 0
                    && prop[0] == '1');
            __android_log_print(ANDROID_LOG_INFO, "hakuX-vk",
                                "force LOD0: %s", flag ? "on" : "off");
        }
        force_lod0 = flag;
    }
#endif
    uint32_t sampler_max_anisotropy =
        MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
        .anisotropyEnable =
            r->enabled_physical_device_features.samplerAnisotropy &&
            sampler_max_anisotropy > 1,
        .maxAnisotropy = sampler_max_anisotropy,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        /*
         * `setprop debug.xemu.vk.force_lod0 1` clamps sampling to mip 0.
         *
         * Diagnostic: a texture that is provably correct on the CPU side and
         * has a sane sampler, yet renders as its own average colour, is
         * sampling its smallest mip. That is decided by the texture-coordinate
         * derivatives at runtime, not by anything set here. Pinning maxLod to
         * 0 separates "wrong LOD is being chosen" from "the texture is wrong".
         */
        .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
        .maxLod = force_lod0 ? 0.0f
                             : (mipmap_en ? MIN(state.max_mipmap_level,
                                                state.levels - 1) : 0.0),
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &snode->sampler));

    set_texture_label(pg, snode);

#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        uint32_t bl_binding;
        if (state.cubemap) {
            bl_binding = 2;
        } else if (state.dimensionality == 3) {
            bl_binding = 1;
        } else {
            bl_binding = 0;
        }
        snode->bindless_binding = bl_binding;

        VkDescriptorImageInfo bl_info = {
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = snode->image_view,
            .sampler = snode->sampler,
        };
        VkWriteDescriptorSet bl_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->bindless_descriptor_set,
            .dstBinding = bl_binding,
            .dstArrayElement = snode->bindless_slot,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &bl_info,
        };
        vkUpdateDescriptorSets(r->device, 1, &bl_write, 0, NULL);
    }
#endif

    r->texture_bindings[texture_idx] = snode;

    if (cube_to_texture) {
        copy_cube_surfaces_to_texture(pg, cube_sources, snode);
    } else if (surface_to_texture) {
        bool can_direct_bind = can_bind_surface_direct(r, surface);

        if (can_direct_bind) {
            VkImageLayout direct_layout;
            if (surface->color) {
                bind_surface_as_texture(pg, surface, snode);
                direct_layout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                bind_zeta_surface_as_texture(pg, surface, snode);
                direct_layout =
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
            r->tex_surface_direct[texture_idx] = true;
            r->tex_surface_direct_views[texture_idx] = surface->image_view;
            r->tex_surface_direct_layout[texture_idx] = direct_layout;
#if OPT_BINDLESS_TEXTURES
            if (r->bindless_textures_supported) {
                VkDescriptorImageInfo bl_info = {
                    .imageLayout = direct_layout,
                    .imageView = surface->image_view,
                    .sampler = snode->sampler,
                };
                VkWriteDescriptorSet bl_write = {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = r->bindless_descriptor_set,
                    .dstBinding = snode->bindless_binding,
                    .dstArrayElement = snode->bindless_slot,
                    .descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .pImageInfo = &bl_info,
                };
                vkUpdateDescriptorSets(r->device, 1, &bl_write, 0, NULL);
            }
#endif
        } else {
            copy_surface_to_texture(pg, surface, snode,
                                    can_queue_surface_copy(pg, surface, snode));
        }
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *binding = r->texture_bindings[i];
        if (!binding || pg->texture_dirty[i] ||
            (binding != &r->dummy_texture && binding->possibly_dirty)) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

bool pgraph_vk_check_textures_fast_skip(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i] ||
            (r->texture_bindings[i] != &r->dummy_texture &&
             r->texture_bindings[i]->possibly_dirty)) {
            return false;
        }
    }
    return true;
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        return;
    }

    resolve_possibly_dirty_textures(d);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            if (r->texture_bindings[i] != &r->dummy_texture) {
                r->texture_bindings[i] = &r->dummy_texture;
                r->texture_bindings_changed = true;
            }
            pg->texture_dirty[i] = false;
            continue;
        }

        if (!pg->texture_dirty[i] && r->texture_bindings[i] &&
            r->texture_bindings[i] != &r->dummy_texture &&
            !r->texture_bindings[i]->possibly_dirty) {
            continue;
        }

        if (!pg->texture_dirty[i] && r->texture_bindings[i] &&
            r->texture_bindings[i] != &r->dummy_texture &&
            r->texture_bindings[i]->possibly_dirty) {
            if (r->texture_bindings[i]->dirty_check_frame == pg->frame_time &&
                !r->texture_bindings[i]->dirty_check_result) {
                r->texture_bindings[i]->possibly_dirty = false;
                continue;
            }
        }

        if (pg->texture_dirty[i] && r->tex_reg_cache[i].valid &&
            r->texture_bindings[i] &&
            r->texture_bindings[i] != &r->dummy_texture &&
            r->texture_bindings[i]->dirty_check_frame == pg->frame_time &&
            !r->texture_bindings[i]->dirty_check_result &&
            !r->texture_bindings[i]->possibly_dirty) {
            uint32_t cur[8] = {
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i * 4),
                pgraph_vk_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i * 4),
            };
            uint32_t sp = (pgraph_vk_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (i * 5)) & 0x1F;
            if (memcmp(cur, r->tex_reg_cache[i].regs, sizeof(cur)) == 0 &&
                sp == r->tex_reg_cache[i].shaderprog_bits) {
                pg->texture_dirty[i] = false;
                continue;
            }
        }

        TextureBinding *prev_binding = r->texture_bindings[i];
        uint64_t prev_seq = prev_binding ? prev_binding->seq : 0;
        create_texture(pg, i);

        r->tex_reg_cache[i].regs[0] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4);
        r->tex_reg_cache[i].regs[1] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        r->tex_reg_cache[i].regs[2] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
        r->tex_reg_cache[i].regs[3] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i * 4);
        r->tex_reg_cache[i].regs[4] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
        r->tex_reg_cache[i].regs[5] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4);
        r->tex_reg_cache[i].regs[6] = pgraph_vk_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i * 4);
        r->tex_reg_cache[i].regs[7] = pgraph_vk_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i * 4);
        r->tex_reg_cache[i].shaderprog_bits =
            (pgraph_vk_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (i * 5)) & 0x1F;
        r->tex_reg_cache[i].valid = true;

        /* The pointer alone is not enough: LRU nodes get recycled, so a
         * recreated texture can land in the same node. Compare the
         * per-creation sequence id as well. */
        if (r->texture_bindings[i] != prev_binding ||
            r->texture_bindings[i]->seq != prev_seq) {
            r->texture_bindings_changed = true;
        }

        pg->texture_dirty[i] = false;
    }

    if (r->texture_bindings_changed) {
        r->pipeline_state_dirty = true;
    }
    update_timestamps(r);
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
    memset(snode->upload_views, 0, sizeof(snode->upload_views));
    snode->submit_time = 0;
    snode->cube_surface_copy = false;
    snode->cube_copy_cache.valid = false;
    snode->cube_seen_draw_gen = 0;
    snode->cube_seen_list_gen = 0;
    snode->dirty_check_frame = 0;
    snode->dirty_check_result = false;
    snode->seq = ++r->texture_binding_seq;

#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        snode->bindless_slot = 0;
        for (int w = 0; w < MAX_BINDLESS_TEXTURES / 64; w++) {
            if (r->bindless_slot_bitmap[w] != UINT64_MAX) {
                int bit = __builtin_ctzll(~r->bindless_slot_bitmap[w]);
                snode->bindless_slot = w * 64 + bit;
                r->bindless_slot_bitmap[w] |= (1ULL << bit);
                break;
            }
        }
    }
#endif

    if (!snode->in_active_list) {
        QTAILQ_INSERT_HEAD(&r->texture_active_list, snode, active_entry);
        snode->in_active_list = true;
    }
}

static void image_pool_init(PGRAPHVkState *r)
{
    QTAILQ_INIT(&r->image_pool);
    r->image_pool_count = 0;
}

static bool image_pool_config_match(const TextureImageConfig *a,
                                    const TextureImageConfig *b)
{
    return a->format == b->format &&
           a->usage == b->usage &&
           a->image_type == b->image_type &&
           a->width == b->width &&
           a->height == b->height &&
           a->depth == b->depth &&
           a->mip_levels == b->mip_levels &&
           a->array_layers == b->array_layers &&
           a->flags == b->flags;
}

static bool image_pool_acquire(PGRAPHVkState *r,
                               const TextureImageConfig *config,
                               VkImage *out_image,
                               VmaAllocation *out_allocation)
{
    PooledImage *entry;
    QTAILQ_FOREACH(entry, &r->image_pool, entry) {
        if (image_pool_config_match(&entry->config, config)) {
            *out_image = entry->image;
            *out_allocation = entry->allocation;
            QTAILQ_REMOVE(&r->image_pool, entry, entry);
            g_free(entry);
            r->image_pool_count--;
            return true;
        }
    }
    return false;
}

static void image_pool_release(PGRAPHVkState *r,
                               const TextureImageConfig *config,
                               VkImage image, VmaAllocation allocation)
{
    int pool_max = r->image_pool_max ? r->image_pool_max : IMAGE_POOL_MAX_SIZE;
    if (r->image_pool_count >= pool_max) {
        PooledImage *oldest = QTAILQ_FIRST(&r->image_pool);
        assert(oldest != NULL);
        QTAILQ_REMOVE(&r->image_pool, oldest, entry);
        vmaDestroyImage(r->allocator, oldest->image, oldest->allocation);
        g_free(oldest);
        r->image_pool_count--;
    }

    PooledImage *pe = g_malloc(sizeof(PooledImage));
    pe->config = *config;
    pe->image = image;
    pe->allocation = allocation;
    QTAILQ_INSERT_TAIL(&r->image_pool, pe, entry);
    r->image_pool_count++;
}

static void image_pool_drain(PGRAPHVkState *r)
{
    PooledImage *entry, *next;
    QTAILQ_FOREACH_SAFE(entry, &r->image_pool, entry, next) {
        QTAILQ_REMOVE(&r->image_pool, entry, entry);
        vmaDestroyImage(r->allocator, entry->image, entry->allocation);
        g_free(entry);
    }
    r->image_pool_count = 0;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    pgraph_vk_invalidate_desc_set_cache(r);
    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;
    for (size_t i = 0; i < ARRAY_SIZE(snode->upload_views); i++) {
        vkDestroyImageView(r->device, snode->upload_views[i], NULL);
        snode->upload_views[i] = VK_NULL_HANDLE;
    }

    if (snode->image != VK_NULL_HANDLE) {
        image_pool_release(r, &snode->image_config, snode->image,
                           snode->allocation);
    }
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    if (snode->submit_time + r->num_active_frames > r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    if (snode->submit_time + r->num_active_frames > r->submit_count) {
        VK_LOG_ERROR("DIAG: texture EVICTED while in-flight! "
                     "image=%p st=%u sc=%u",
                     (void *)snode->image,
                     snode->submit_time, r->submit_count);
    }
    if (snode->in_active_list) {
        QTAILQ_REMOVE(&r->texture_active_list, snode, active_entry);
        snode->in_active_list = false;
    }
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (r->tex_binding_cache[i].binding == snode) {
            r->tex_binding_cache[i].binding = NULL;
        }
    }
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported && snode->bindless_slot > 0) {
        int w = snode->bindless_slot / 64;
        int bit = snode->bindless_slot % 64;
        r->bindless_slot_bitmap[w] &= ~(1ULL << bit);
    }
#endif
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size =
        r->texture_cache_target ? r->texture_cache_target : 1024;
    size_t texture_hash_buckets = texture_cache_size * 2;
    lru_init(&r->texture_cache, texture_hash_buckets);
    QTAILQ_INIT(&r->texture_active_list);
    image_pool_init(r);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        r->texture_cache_entries[i].in_active_list = false;
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    image_pool_drain(r);
    lru_destroy(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    /*
     * Eviction normally moves images into the reuse pool. Under memory
     * pressure the memory must be returned to the driver instead.
     */
    image_pool_drain(r);

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted,
                    r->texture_cache.num_used);
}

static bool format_can_be_sampled(PGRAPHVkState *r, VkFormat format)
{
    if (format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, format, &props);
    return (props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

/* Stricter than format_can_be_sampled(): the textures this is used for are
 * mipmapped and sampled with a linear filter, which is a separate format
 * feature from plain sampling. A driver that reports SAMPLED_IMAGE but not
 * SAMPLED_IMAGE_FILTER_LINEAR would leave every filtered fetch undefined. */
static bool format_can_be_sampled_filtered(PGRAPHVkState *r, VkFormat format)
{
    if (format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, format, &props);
    return (props.optimalTilingFeatures & required) == required;
}

/*
 * Whether native BC upload may be used at all on this device.
 *
 * `debug.xemu.vk.dxt_blocks` forces the answer either way (1 = on, 0 = off) so
 * the two paths can be A/B compared on one build.
 */
static bool device_allows_dxt_blocks(PGRAPHVkState *r)
{
#ifndef __ANDROID__
    (void)r;
    /* Lets the desktop build exercise the software-decompress path, which is
     * what mobile GPUs without real BC support always take. Without this the
     * two paths can only be compared across two different machines. */
    const char *env = getenv("XEMU_VK_DXT_BLOCKS");
    if (env && (env[0] == '0' || env[0] == '1')) {
        return env[0] == '1';
    }
#else
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.xemu.vk.dxt_blocks", prop) > 0 &&
        (prop[0] == '0' || prop[0] == '1')) {
        bool forced = (prop[0] == '1');
        __android_log_print(ANDROID_LOG_WARN, "hakuX-vk",
                            "DXT block upload: forced %s by property",
                            forced ? "on" : "off");
        return forced;
    }

    /*
     * Adreno has no hardware BC decode. Qualcomm's driver nevertheless
     * advertises textureCompressionBC and reports SAMPLED_IMAGE for
     * BC1/BC2/BC3, so the feature and format-property checks below both pass
     * and are not sufficient on their own.
     *
     * Observed on an Adreno 610 (driver 0615.86, Snapdragon 662): with BC
     * images in use, the textures that go through this path -- the sky and the
     * road in Forza Motorsport, both large mipmapped DXT -- stop rendering.
     * Keep the software decompress on Qualcomm's driver; correctness outranks
     * the upload saving. Turnip decodes BC properly and keeps block upload.
     */
    if (pgraph_vk_is_qualcomm_blob(r)) {
        __android_log_print(ANDROID_LOG_INFO, "hakuX-vk",
                            "DXT block upload: disabled on Qualcomm driver "
                            "(no hardware BC despite advertised support)");
        return false;
    }
#endif
    return true;
}

/* Pick the effective Vulkan format for each Kelvin texture format.
 *
 * The preferred format is not always available. A4R4G4B4 in particular only
 * became core in Vulkan 1.3 (VK_EXT_4444_formats before that), so plenty of
 * Vulkan 1.1 mobile drivers do not expose it at all. Creating an image with an
 * unsupported format leaves the texture unusable and it samples as nothing --
 * which shows up as missing text, since that is the format games commonly pick
 * for font glyph atlases.
 *
 * Where the exact format is missing, read the same 16-bit texel through
 * another 4444 layout and swizzle the image view back into ARGB order. That
 * keeps the texel size and the upload path unchanged.
 */
static void init_texture_format_map(PGRAPHVkState *r)
{
    memcpy(r->texture_format_map, kelvin_color_format_vk_map,
           sizeof(r->texture_format_map));

    /* DXT blocks upload as-is where BC is supported, which removes the
     * software decompress from every texture cache miss and stores the texture
     * at its compressed size. Requires all three, so a texture never silently
     * changes cost depending on which DXT variant a game picked. */
    static const struct {
        int kelvin_format;
        VkFormat block_format;
    } dxt_block_formats[] = {
        { NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5,
          VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
        { NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8,
          VK_FORMAT_BC2_UNORM_BLOCK },
        { NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8,
          VK_FORMAT_BC3_UNORM_BLOCK },
    };

    bool all_blocks_supported = device_allows_dxt_blocks(r) &&
                                r->enabled_physical_device_features
                                    .textureCompressionBC;
    for (int i = 0; i < ARRAY_SIZE(dxt_block_formats) && all_blocks_supported;
         i++) {
        all_blocks_supported = format_can_be_sampled_filtered(
            r, dxt_block_formats[i].block_format);
    }

    if (all_blocks_supported) {
        for (int i = 0; i < ARRAY_SIZE(dxt_block_formats); i++) {
            r->texture_format_map[dxt_block_formats[i].kelvin_format]
                .vk_format = dxt_block_formats[i].block_format;
        }
    }

    /* Xbox A4R4G4B4 packs A into bits 15..12, R into 11..8, G into 7..4 and B
     * into 3..0. Each candidate below names the channel each of those nibbles
     * lands in for that Vulkan format, so the swizzle maps them back to RGBA.
     */
    static const VkColorFormatInfo argb4444_candidates[] = {
        /* A:15..12 R:11..8 G:7..4 B:3..0 -- exact match, no swizzle. */
        { VK_FORMAT_A4R4G4B4_UNORM_PACK16,
          { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY } },
        /* B:15..12 G:11..8 R:7..4 A:3..0 -- so R=G, G=R, B=A, A=B. */
        { VK_FORMAT_B4G4R4A4_UNORM_PACK16,
          { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B } },
        /* R:15..12 G:11..8 B:7..4 A:3..0 -- so R=G, G=B, B=A, A=R. */
        { VK_FORMAT_R4G4B4A4_UNORM_PACK16,
          { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R } },
    };

    static const int argb4444_formats[] = {
        NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4,
        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4,
    };

    const VkColorFormatInfo *argb4444 = NULL;
    for (int i = 0; i < ARRAY_SIZE(argb4444_candidates); i++) {
        if (format_can_be_sampled(r, argb4444_candidates[i].vk_format)) {
            argb4444 = &argb4444_candidates[i];
            break;
        }
    }

    if (argb4444 == NULL) {
        fprintf(stderr,
                "nv2a: no supported 4444 texture format; A4R4G4B4 textures "
                "will not render\n");
        return;
    }

    if (argb4444 != &argb4444_candidates[0]) {
        fprintf(stderr,
                "nv2a: VK_FORMAT_A4R4G4B4_UNORM_PACK16 unsupported, using "
                "format %d with a swizzled view instead\n",
                argb4444->vk_format);
    }

    for (int i = 0; i < ARRAY_SIZE(argb4444_formats); i++) {
        r->texture_format_map[argb4444_formats[i]] = *argb4444;
    }
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    init_texture_format_map(r);
    texture_upload_compute_init(r);
    create_dummy_texture(pg);

#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_INFO, "hakuX-vk", "DXT upload: %s",
        format_is_block_compressed(
            r->texture_format_map[NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5]
                .vk_format)
            ? "native BC blocks"
            : "software decompress to RGBA8");
#endif

    r->texture_format_properties = g_malloc0_n(
        KELVIN_COLOR_FORMAT_COUNT, sizeof(VkFormatProperties));
    for (int i = 0; i < KELVIN_COLOR_FORMAT_COUNT; i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, r->texture_format_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);
    texture_upload_compute_destroy(r);

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
