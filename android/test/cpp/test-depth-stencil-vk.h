/* Generated pixels only. Included after the common fixture's Vulkan helpers. */
typedef struct TestPack {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet set;
    uint32_t width, height;
} TestPack;

static VkPipeline test_compute_pipeline(const char *path, VkPipelineLayout layout)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0 && size % 4 == 0 && fseek(file, 0, SEEK_SET) == 0);
    uint32_t *code = malloc(size);
    assert(code && fread(code, 1, size, file) == (size_t)size);
    fclose(file);
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &info, NULL, &module));
    free(code);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
    };
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL);
    return pipeline;
}

static void test_pack(void *user, VkCommandBuffer cmd, VkBuffer input, VkBuffer output)
{
    (void)input; (void)output;
    TestPack *pack = user;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack->layout,
                            0, 1, &pack->set, 0, NULL);
    uint32_t sizes[4] = {pack->width, pack->width, 0, 0};
    vkCmdPushConstants(cmd, pack->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sizes), sizes);
    vkCmdDispatch(cmd, pack->width * pack->height / 256, 1, 1);
}

static void test_stencil(VkQueue queue, VkCommandPool pool, const char *shader,
                         uint32_t width, uint32_t height, VkFormat color_format)
{
    VkFormatProperties support;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &support);
    if (!(support.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        puts("SKIP native D24 unavailable: production uses its depth fallback");
        return;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    size_t pixels = (size_t)width * height, bytes = pixels * 4;
    VkDeviceSize stencil_offset = vk_depth_stencil_offset(width, height,
        props.limits.minStorageBufferOffsetAlignment);
    VkBuffer upload, scratch, packed, readback;
    VkDeviceMemory memories[4] = {make_buffer(pixels * 5 * 3, &upload),
        make_buffer(stencil_offset + pixels, &scratch), make_buffer(bytes, &packed),
        make_buffer(bytes * 3, &readback)};
    unsigned char *input_data;
    uint32_t *output_data;
    CHECK(vkMapMemory(device, memories[0], 0, VK_WHOLE_SIZE, 0, (void **)&input_data));
    CHECK(vkMapMemory(device, memories[3], 0, VK_WHOLE_SIZE, 0, (void **)&output_data));
    for (unsigned pass = 0; pass < 3; pass++) {
        uint32_t *depth = (uint32_t *)(input_data + pixels * 5 * pass);
        unsigned char *stencil = input_data + pixels * 5 * pass + bytes;
        for (size_t i = 0; i < pixels; i++) {
            depth[i] = (i * 7919 + pass * 1013904223u) & 0xffffff;
            if (i % 17 == 0) depth[i] = (i % 34) ? 0xffffff : 0;
            stencil[i] = i * 73 + pass * 19;
        }
    }
    VkImage source, destination;
    VkDeviceMemory source_mem = make_image(width, height, 1, VK_FORMAT_D24_UNORM_S8_UINT, &source);
    VkDeviceMemory destination_mem = make_image(width, height, 1, color_format, &destination);
    VkDescriptorSetLayoutBinding bindings[3];
    for (unsigned i = 0; i < 3; i++) bindings[i] = (VkDescriptorSetLayoutBinding) {
        .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipeline pipeline = test_compute_pipeline(shader, layout);
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 3, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptors));
    VkDescriptorSetLayout layouts[3] = {set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 3, .pSetLayouts = layouts};
    VkDescriptorSet sets[3];
    CHECK(vkAllocateDescriptorSets(device, &set_alloc, sets));
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
    VkCommandBuffer commands[3];
    CHECK(vkAllocateCommandBuffers(device, &alloc, commands));
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    for (unsigned pass = 0; pass < 3; pass++) {
        VkDescriptorBufferInfo buffers[3] = {{scratch, 0, bytes},
            {scratch, stencil_offset, pixels}, {packed, 0, bytes}};
        VkWriteDescriptorSet writes[3];
        for (unsigned i = 0; i < 3; i++) writes[i] = (VkWriteDescriptorSet) {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[pass],
            .dstBinding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .pBufferInfo = &buffers[i]};
        vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
        VkCommandBuffer cmd = commands[pass];
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        transition_aspect(cmd, source, 1, pass ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects);
        VkBufferImageCopy regions[2] = {
            {.bufferOffset = pixels * 5 * pass, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
             .imageExtent = {width, height, 1}},
            {.bufferOffset = pixels * 5 * pass + bytes, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
             .imageExtent = {width, height, 1}}};
        vkCmdCopyBufferToImage(cmd, upload, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
        transition_aspect(cmd, source, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects);
        TestPack pack = {pipeline, layout, sets[pass], width, height};
        vk_depth_stencil_copy_record(cmd, source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            destination, pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            scratch, packed, stencil_offset, width, height, test_pack, &pack);
        transition(cmd, destination, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        regions[0] = (VkBufferImageCopy) {.bufferOffset = bytes * pass,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {width, height, 1}};
        vkCmdCopyImageToBuffer(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, regions);
        transition(cmd, destination, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = readback, .size = VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &host, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    }
    CHECK(vkQueueWaitIdle(queue));
    for (unsigned pass = 0; pass < 3; pass++) {
        uint32_t *depth = (uint32_t *)(input_data + pixels * 5 * pass);
        unsigned char *stencil = input_data + pixels * 5 * pass + bytes;
        for (size_t i = 0; i < pixels; i++) assert(output_data[pixels * pass + i] == (depth[i] << 8 | stencil[i]));
    }
    vkFreeCommandBuffers(device, pool, 3, commands);
    vkDestroyDescriptorPool(device, descriptors, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyImage(device, source, NULL); vkFreeMemory(device, source_mem, NULL);
    vkDestroyImage(device, destination, NULL); vkFreeMemory(device, destination_mem, NULL);
    vkUnmapMemory(device, memories[0]); vkUnmapMemory(device, memories[3]);
    VkBuffer all_buffers[4] = {upload, scratch, packed, readback};
    for (unsigned i = 0; i < 4; i++) {vkDestroyBuffer(device, all_buffers[i], NULL); vkFreeMemory(device, memories[i], NULL);}
    printf("PASS D24 integer pack %ux%u color_fmt=%d, three queued versions and prior reads\n", width, height, color_format);
}

static void test_depth_view(VkQueue queue, VkCommandPool pool, const char *shader, VkFormat format)
{
    VkFormatProperties support;
    vkGetPhysicalDeviceFormatProperties(physical, format, &support);
    if (!(support.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        puts("SKIP sampled depth format unavailable"); return;
    }
    VkImage image;
    VkDeviceMemory image_memory = make_image(16, 16, 1, format, &image);
    VkImageView original = VK_NULL_HANDLE, moved = VK_NULL_HANDLE;
    CHECK(vk_surface_depth_view_get(device, image, format, &original));
    VkImageView first = original;
    for (unsigned i = 0; i < 64; i++) {
        CHECK(vk_surface_depth_view_get(device, image, format, &original)); assert(original == first);
    }
    vk_surface_depth_view_move(&moved, &original);
    assert(original == VK_NULL_HANDLE && moved == first);
    VkBuffer output;
    VkDeviceMemory output_memory = make_buffer(256 * sizeof(float) * 3, &output);
    float *results;
    CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0, (void **)&results));
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .minFilter = VK_FILTER_NEAREST, .magFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipeline pipeline = test_compute_pipeline(shader, layout);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 3, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptors));
    VkDescriptorSetLayout layouts[3] = {set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 3, .pSetLayouts = layouts};
    VkDescriptorSet sets[3];
    CHECK(vkAllocateDescriptorSets(device, &set_alloc, sets));
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
    VkCommandBuffer commands[3];
    CHECK(vkAllocateCommandBuffers(device, &alloc, commands));
    for (unsigned pass = 0; pass < 3; pass++) {
        CHECK(vk_surface_depth_view_get(device, image, format, &moved)); assert(moved == first);
        VkDescriptorImageInfo sample = {sampler, moved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo buffer = {output, 256 * sizeof(float) * pass, 256 * sizeof(float)};
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[pass], .dstBinding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &sample},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[pass], .dstBinding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .pBufferInfo = &buffer}};
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
        VkCommandBuffer cmd = commands[pass];
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        transition_aspect(cmd, image, 1, pass ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);
        VkImageSubresourceRange range = {aspect, 0, 1, 0, 1};
        VkClearDepthStencilValue value = {(pass + 1) * 0.25f, pass};
        vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        transition_aspect(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, aspect);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[pass], 0, NULL);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = output, .offset = buffer.offset, .size = buffer.range};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &host, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    }
    CHECK(vkQueueWaitIdle(queue));
    for (unsigned pass = 0; pass < 3; pass++) for (unsigned i = 0; i < 256; i++) {
        float difference = results[256 * pass + i] - (pass + 1) * 0.25f;
        assert(difference >= -0.00000012f && difference <= 0.00000012f);
    }
    vkFreeCommandBuffers(device, pool, 3, commands);
    vkDestroyDescriptorPool(device, descriptors, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroySampler(device, sampler, NULL);
    vk_surface_depth_view_release(device, &moved);
    assert(moved == VK_NULL_HANDLE);
    vk_surface_depth_view_release(device, &original);
    vkDestroyImage(device, image, NULL); vkFreeMemory(device, image_memory, NULL);
    vkUnmapMemory(device, output_memory);
    vkDestroyBuffer(device, output, NULL); vkFreeMemory(device, output_memory, NULL);
    printf("PASS persistent sampled depth view fmt=%d: reuse, ownership move, three queued samples and retirement\n", format);
}
