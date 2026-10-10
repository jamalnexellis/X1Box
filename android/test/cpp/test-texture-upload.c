/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Run on the actual Android Vulkan driver; checks the production compute
 * upload kernel and barriers against exact RGBA bytes, including every mip,
 * non-multiple workgroups, nonzero staging offsets, and repeated writes. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "hw/xbox/nv2a/pgraph/vk/texture-upload.h"

#define CHECK(call) do { VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) { \
        fprintf(stderr, "%s: %d\n", #call, result_); abort(); \
    } } while (0)

static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool command_pool;
static VkDescriptorPool descriptor_pool;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkPipeline r16_pipeline;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    abort();
}

static VkDeviceMemory bind_buffer(VkBuffer buffer)
{
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &info, NULL, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    return memory;
}

static void test_image(uint32_t width, uint32_t height, uint32_t levels, bool packed16)
{
    VkBufferImageCopy regions[16] = {0};
    VkImageView views[16];
    VkDescriptorSet sets[16];
    size_t size = 16; /* Guard prefix exercises offset_words. */
    for (uint32_t i = 0; i < levels; i++) {
        uint32_t w = width >> i, h = height >> i;
        if (!w) w = 1;
        if (!h) h = 1;
        regions[i] = (VkBufferImageCopy){
            .bufferOffset = size,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
            .imageExtent = { w, h, 1 },
        };
        size += texture_upload_level_size((size_t)w * h * (packed16 ? 2 : 4));
    }
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer src, dst;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &src));
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &dst));
    VkDeviceMemory src_memory = bind_buffer(src), dst_memory = bind_buffer(dst);
    uint8_t *src_data, *dst_data;
    CHECK(vkMapMemory(device, src_memory, 0, VK_WHOLE_SIZE, 0, (void **)&src_data));
    CHECK(vkMapMemory(device, dst_memory, 0, VK_WHOLE_SIZE, 0, (void **)&dst_data));
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = packed16 ? VK_FORMAT_R5G6B5_UNORM_PACK16 : VK_FORMAT_R8G8B8A8_UNORM,
        .flags = packed16 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                            VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0,
        .extent = { width, height, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkImage image;
    CHECK(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(req.memoryTypeBits, 0),
    };
    VkDeviceMemory image_memory;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &image_memory));
    CHECK(vkBindImageMemory(device, image, image_memory, 0));
    for (uint32_t i = 0; i < levels; i++) {
        VkImageViewUsageCreateInfo usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT,
        };
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &usage,
            .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = packed16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 },
        };
        CHECK(vkCreateImageView(device, &view_info, NULL, &views[i]));
        VkDescriptorSetAllocateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descriptor_pool, .descriptorSetCount = 1,
            .pSetLayouts = &set_layout,
        };
        CHECK(vkAllocateDescriptorSets(device, &set_info, &sets[i]));
        VkDescriptorBufferInfo buffer = { src, 0, size };
        VkDescriptorImageInfo dest = { .imageView = views[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet writes[] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i],
              .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i],
              .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dest },
        };
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }
    VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    for (uint32_t pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < size; i++) src_data[i] = (uint8_t)(i * 73 + pass * 91);
        memset(dst_data, 0xa5, size);
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = src, .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        texture_upload_image_barrier(cmd, image, levels,
            pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, pass ? VK_ACCESS_MEMORY_READ_BIT : 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, packed16 ? r16_pipeline : pipeline);
        for (uint32_t i = 0; i < levels; i++) {
            texture_upload_dispatch(cmd, layout, sets[i], regions[i].bufferOffset,
                regions[i].imageExtent.width, regions[i].imageExtent.height);
        }
        texture_upload_image_barrier(cmd, image, levels, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                dst, levels, regions);
        texture_upload_image_barrier(cmd, image, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        VkBufferMemoryBarrier read = host;
        read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        read.buffer = dst;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &read, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        for (size_t i = 0; i < 16; i++) assert(dst_data[i] == 0xa5);
        for (uint32_t i = 0; i < levels; i++) {
            size_t offset = regions[i].bufferOffset;
            size_t bytes = (size_t)regions[i].imageExtent.width *
                regions[i].imageExtent.height * (packed16 ? 2 : 4);
            assert(memcmp(src_data + offset, dst_data + offset, bytes) == 0);
            for (size_t pad = bytes; pad < texture_upload_level_size(bytes); pad++)
                assert(dst_data[offset + pad] == 0xa5);
        }
        CHECK(vkResetCommandBuffer(cmd, 0));
    }
    vkFreeCommandBuffers(device, command_pool, 1, &cmd);
    for (uint32_t i = 0; i < levels; i++) vkDestroyImageView(device, views[i], NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkUnmapMemory(device, src_memory);
    vkUnmapMemory(device, dst_memory);
    vkDestroyBuffer(device, src, NULL);
    vkDestroyBuffer(device, dst, NULL);
    vkFreeMemory(device, src_memory, NULL);
    vkFreeMemory(device, dst_memory, NULL);
    CHECK(vkResetDescriptorPool(device, descriptor_pool, 0));
    printf("PASS %ux%u %u mips, three exact %s uploads\n", width, height, levels,
           packed16 ? "RGB565" : "RGBA8");
}

static VkDeviceMemory bind_image(VkImage image)
{
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(req.memoryTypeBits, 0),
    };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &info, NULL, &memory));
    CHECK(vkBindImageMemory(device, image, memory, 0));
    return memory;
}

static VkShaderModule load_module(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0 && size % 4 == 0);
    rewind(file);
    uint32_t *code = malloc((size_t)size);
    assert(code && fread(code, (size_t)size, 1, file) == 1);
    fclose(file);
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size, .pCode = code };
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &info, NULL, &module));
    free(code);
    return module;
}

/* Compare actual shader sampling of compute-uploaded RGBA images through the
 * production BGRA map against BGRA images populated by a raw surface copy.
 * Both must match the independent byte-order expectation, with real and forced
 * alpha. This catches a red/blue swap which a raw readback alone would miss. */
static void test_sampling(const char *sample_shader, bool packed16)
{
    const uint32_t width = packed16 ? 256 : 17, height = packed16 ? 256 : 31;
    const size_t size = 16 + width * height * 4;
    uint8_t *sample_reference = malloc(size);
    assert(sample_reference);
    VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer src, dst;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &src));
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &dst));
    VkDeviceMemory src_memory = bind_buffer(src), dst_memory = bind_buffer(dst);
    uint8_t *src_data, *dst_data;
    CHECK(vkMapMemory(device, src_memory, 0, VK_WHOLE_SIZE, 0, (void **)&src_data));
    CHECK(vkMapMemory(device, dst_memory, 0, VK_WHOLE_SIZE, 0, (void **)&dst_data));
    VkImage images[3];
    VkDeviceMemory memories[3];
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = packed16 && i < 2 ? VK_FORMAT_R5G6B5_UNORM_PACK16 :
                i == 1 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
            .flags = packed16 && i == 0 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                         VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0,
            .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (i == 1 ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
        };
        CHECK(vkCreateImage(device, &info, NULL, &images[i]));
        memories[i] = bind_image(images[i]);
    }
    VkImageView storage[2];
    for (int i = 0; i < 2; i++) {
        VkImageViewUsageCreateInfo usage = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT };
        VkImageViewCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &usage,
            .image = images[i * 2], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = packed16 && i == 0 ? VK_FORMAT_R16_UINT : VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        CHECK(vkCreateImageView(device, &info, NULL, &storage[i]));
    }
    VkDescriptorSetAllocateInfo upload_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
    VkDescriptorSet upload;
    CHECK(vkAllocateDescriptorSets(device, &upload_info, &upload));
    VkDescriptorBufferInfo source_buffer = { src, 0, size };
    VkDescriptorImageInfo upload_image = { .imageView = storage[0], .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet uploads[] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = upload,
          .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &source_buffer },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = upload,
          .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &upload_image },
    };
    vkUpdateDescriptorSets(device, 2, uploads, 0, NULL);

    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo set_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings };
    VkDescriptorSetLayout sample_set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &sample_set_layout));
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &sample_set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    VkPipelineLayout sample_layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &sample_layout));
    VkShaderModule module = load_module(sample_shader);
    VkComputePipelineCreateInfo pipeline_info = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = sample_layout, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" } };
    VkPipeline sample_pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &sample_pipeline));
    vkDestroyShaderModule(device, module, NULL);
    VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
    VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes };
    VkDescriptorPool sample_pool;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &sample_pool));
    VkDescriptorSetAllocateInfo alloc_set = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = sample_pool, .descriptorSetCount = 1, .pSetLayouts = &sample_set_layout };
    VkDescriptorSet sample_set;
    CHECK(vkAllocateDescriptorSets(device, &alloc_set, &sample_set));
    VkSamplerCreateInfo sampler_info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST };
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));
    VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    VkBufferImageCopy region = { .bufferOffset = 16,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { width, height, 1 } };
    bool first_output = true;
    for (uint32_t pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < size; i++) src_data[i] = (uint8_t)(i * 73 + pass * 91);
        if (packed16) {
            for (uint32_t i = 0; i < width * height; i++) {
                uint16_t word = (uint16_t)(i + pass * 17027);
                memcpy(src_data + 16 + i * 2, &word, 2);
            }
        }
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = src, .size = VK_WHOLE_SIZE };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0, 0, NULL, 1, &host, 0, NULL);
        texture_upload_image_barrier(cmd, images[0], 1,
            pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, pass ? VK_ACCESS_SHADER_READ_BIT : 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, packed16 ? r16_pipeline : pipeline);
        texture_upload_dispatch(cmd, layout, upload, 16, width, height);
        texture_upload_image_barrier(cmd, images[0], 1, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        texture_upload_image_barrier(cmd, images[1], 1,
            pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pass ? VK_ACCESS_SHADER_READ_BIT : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { width, height, 1 } };
        vkCmdCopyImage(cmd, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        texture_upload_image_barrier(cmd, images[0], 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        texture_upload_image_barrier(cmd, images[1], 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImage(cmd, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (packed16) {
            memset(dst_data, 0xa5, size);
            texture_upload_image_barrier(cmd, images[0], 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferMemoryBarrier raw = host;
            raw.buffer = dst;
            raw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, NULL, 1, &raw, 0, NULL);
            vkCmdCopyImageToBuffer(cmd, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
            raw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            raw.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                  0, 0, NULL, 1, &raw, 0, NULL);
        }
        for (int i = 0; i < 2; i++) {
            texture_upload_image_barrier(cmd, images[i], 1,
                i || packed16 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                i || packed16 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        if (packed16) {
            assert(memcmp(src_data + 16, dst_data + 16, width * height * 2) == 0);
            for (size_t i = 0; i < 16; i++) assert(dst_data[i] == 0xa5);
        }
        CHECK(vkResetCommandBuffer(cmd, 0));
        for (int alpha_one = 0; alpha_one < 2; alpha_one++) {
            for (int source = 0; source < 2; source++) {
                VkComponentMapping map = { 0 };
                if (alpha_one) map.a = VK_COMPONENT_SWIZZLE_ONE;
                if (source == 0 && !packed16) map = texture_upload_bgra_mapping(map);
                VkImageViewUsageCreateInfo usage = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT };
                VkImageViewCreateInfo view_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .pNext = &usage,
                    .image = images[source], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = packed16 ? VK_FORMAT_R5G6B5_UNORM_PACK16 :
                        source ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                    .components = map, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                VkImageView view;
                CHECK(vkCreateImageView(device, &view_info, NULL, &view));
                VkDescriptorImageInfo sample_src = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkDescriptorImageInfo sample_dst = { .imageView = storage[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                VkWriteDescriptorSet writes[] = {
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sample_set, .dstBinding = 0,
                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sample_src },
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sample_set, .dstBinding = 1,
                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &sample_dst },
                };
                vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
                memset(dst_data, 0xa5, size);
                CHECK(vkBeginCommandBuffer(cmd, &begin));
                texture_upload_image_barrier(cmd, images[2], 1,
                    first_output ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, first_output ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                first_output = false;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sample_pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sample_layout, 0, 1, &sample_set, 0, NULL);
                uint32_t params[] = { width, height };
                vkCmdPushConstants(cmd, sample_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
                vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
                texture_upload_image_barrier(cmd, images[2], 1, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferMemoryBarrier output = host;
                output.buffer = dst;
                output.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0, 0, NULL, 1, &output, 0, NULL);
                vkCmdCopyImageToBuffer(cmd, images[2], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
                output.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                output.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                      0, 0, NULL, 1, &output, 0, NULL);
                CHECK(vkEndCommandBuffer(cmd));
                CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
                CHECK(vkQueueWaitIdle(queue));
                for (size_t i = 0; i < 16; i++) assert(dst_data[i] == 0xa5);
                for (size_t i = 16; i < size; i += 4) {
                    if (packed16) {
                        uint16_t word;
                        memcpy(&word, src_data + 16 + (i - 16) / 2, 2);
                        int red = ((word >> 11) * 255 + 15) / 31;
                        int green = (((word >> 5) & 63) * 255 + 31) / 63;
                        int blue = ((word & 31) * 255 + 15) / 31;
                        assert(abs((int)dst_data[i] - red) <= 1);
                        assert(abs((int)dst_data[i + 1] - green) <= 1);
                        assert(abs((int)dst_data[i + 2] - blue) <= 1);
                        assert(dst_data[i + 3] == 255);
                    } else {
                        assert(dst_data[i] == src_data[i + 2]);
                        assert(dst_data[i + 1] == src_data[i + 1]);
                        assert(dst_data[i + 2] == src_data[i]);
                        assert(dst_data[i + 3] == (alpha_one ? 255 : src_data[i + 3]));
                    }
                }
                if (source == 0) memcpy(sample_reference, dst_data, size);
                else assert(memcmp(sample_reference, dst_data, size) == 0);
                CHECK(vkResetCommandBuffer(cmd, 0));
                vkDestroyImageView(device, view, NULL);
            }
        }
    }
    vkFreeCommandBuffers(device, command_pool, 1, &cmd);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyPipeline(device, sample_pipeline, NULL);
    vkDestroyPipelineLayout(device, sample_layout, NULL);
    vkDestroyDescriptorPool(device, sample_pool, NULL);
    vkDestroyDescriptorSetLayout(device, sample_set_layout, NULL);
    for (int i = 0; i < 2; i++) vkDestroyImageView(device, storage[i], NULL);
    for (int i = 0; i < 3; i++) {
        vkDestroyImage(device, images[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
    }
    vkUnmapMemory(device, src_memory);
    vkUnmapMemory(device, dst_memory);
    vkDestroyBuffer(device, src, NULL);
    vkDestroyBuffer(device, dst, NULL);
    vkFreeMemory(device, src_memory, NULL);
    vkFreeMemory(device, dst_memory, NULL);
    CHECK(vkResetDescriptorPool(device, descriptor_pool, 0));
    free(sample_reference);
    puts(packed16 ? "PASS all 65536 RGB565 words, exact sampling and raw image copies" :
                   "PASS BGRA/forced-alpha shader sampling and raw RGBA/BGRA image copies");
}

int main(int argc, char **argv)
{
    assert(argc == 4 && strlen(texture_upload_glsl) > 100);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long code_size = ftell(f);
    assert(code_size > 0 && code_size % 4 == 0);
    rewind(f);
    uint32_t *code = malloc((size_t)code_size);
    assert(code && fread(code, (size_t)code_size, 1, f) == 1);
    fclose(f);
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info };
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("GPU: %s driver=%u\n", props.deviceName, props.driverVersion);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
    }
    free(families);
    assert(family != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info };
    VkPhysicalDeviceFeatures available, enabled = {0};
    vkGetPhysicalDeviceFeatures(physical, &available);
    assert(available.shaderStorageImageExtendedFormats);
    enabled.shaderStorageImageExtendedFormats = VK_TRUE;
    device_info.pEnabledFeatures = &enabled;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &command_pool));
    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo set_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings };
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 } };
    VkDescriptorPoolCreateInfo descriptor_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 16, .poolSizeCount = 2, .pPoolSizes = sizes };
    CHECK(vkCreateDescriptorPool(device, &descriptor_info, NULL, &descriptor_pool));
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkShaderModuleCreateInfo shader_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)code_size, .pCode = code };
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &module));
    free(code);
    VkComputePipelineCreateInfo pipeline_info = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" } };
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL);
    module = load_module(argv[3]);
    pipeline_info.stage.module = module;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &r16_pipeline));
    vkDestroyShaderModule(device, module, NULL);
    for (int packed16 = 0; packed16 < 2; packed16++) {
        test_image(1, 1, 1, packed16);
        test_image(17, 31, 1, packed16);
        test_image(257, 129, 9, packed16);
        test_image(512, 512, 10, packed16);
        test_image(640, 480, 1, packed16);
        test_sampling(argv[2], packed16);
    }
    vkDestroyPipeline(device, r16_pipeline, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("All texture upload GPU comparisons passed");
    return 0;
}
