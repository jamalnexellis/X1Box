/* Generated mapped-buffer versions: redundant comparisons must leave prior
 * queued reads intact, and changed bytes require completion before overwrite.
 * Transfer readback checks bytes/order, not vertex shader or game behavior. */
#include "hw/xbox/nv2a/pgraph/vk/vertex-upload.h"

static void test_vertex_upload(VkQueue queue, VkCommandPool pool)
{
    enum { bytes = 4096, versions = 3 };
    VkBuffer input, output;
    VkDeviceMemory input_memory = make_buffer(bytes, &input);
    VkDeviceMemory output_memory = make_buffer(bytes * versions, &output);
    unsigned char *mapped, *result;
    CHECK(vkMapMemory(device, input_memory, 0, VK_WHOLE_SIZE, 0,
                       (void **)&mapped));
    CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                       (void **)&result));
    unsigned char expected[versions][bytes];
    for (unsigned i = 0; i < bytes; i++) {
        expected[0][i] = (unsigned char)(i * 73 + (i >> 8) * 19);
    }
    for (unsigned version = 1; version < versions; version++) {
        memcpy(expected[version], expected[version - 1], bytes);
        expected[version][0] ^= (unsigned char)version;
        expected[version][bytes / 2] ^= (unsigned char)(version * 7);
        expected[version][bytes - 1] ^= (unsigned char)(version * 13);
    }
    memcpy(mapped, expected[0], bytes);
    memset(result, 0xa5, bytes * versions);
    VkCommandBuffer cmds[versions];
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = versions,
    };
    CHECK(vkAllocateCommandBuffers(device, &alloc, cmds));
    VkFence fence;
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    for (unsigned version = 0; version < versions; version++) {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        CHECK(vkBeginCommandBuffer(cmds[version], &begin));
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = input, .size = bytes,
        };
        vkCmdPipelineBarrier(cmds[version], VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        VkBufferCopy copy = { .dstOffset = (VkDeviceSize)version * bytes,
                              .size = bytes };
        vkCmdCopyBuffer(cmds[version], input, output, 1, &copy);
        VkBufferMemoryBarrier readback = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = output, .offset = copy.dstOffset, .size = bytes,
        };
        vkCmdPipelineBarrier(cmds[version], VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &readback, 0, NULL);
        CHECK(vkEndCommandBuffer(cmds[version]));
        /* A recorded read also depends on the allocation retaining its old
         * bytes. The same-bytes helper must not perform a host write. */
        for (unsigned i = 0; i < 64; i++) {
            assert(vk_vertex_upload_unchanged(mapped, bytes, expected[version],
                                              0, bytes, true));
        }
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmds[version],
        };
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        /* Read-only comparisons are also safe while submission is in flight. */
        for (unsigned i = 0; i < 64; i++) {
            assert(vk_vertex_upload_unchanged(mapped, bytes, expected[version],
                                              0, bytes, true));
        }
        if (version + 1 < versions) {
            assert(!vk_vertex_upload_unchanged(mapped, bytes,
                expected[version + 1], 0, bytes, true));
        }
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        assert(memcmp(result + (size_t)version * bytes,
                      expected[version], bytes) == 0);
        CHECK(vkResetFences(device, 1, &fence));
        if (version + 1 < versions) {
            /* The production fallback finishes before overwriting. */
            memcpy(mapped, expected[version + 1], bytes);
        }
    }
    for (unsigned version = 0; version < versions; version++) {
        assert(memcmp(result + (size_t)version * bytes,
                      expected[version], bytes) == 0);
    }
    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, pool, versions, cmds);
    vkUnmapMemory(device, input_memory);
    vkUnmapMemory(device, output_memory);
    vkDestroyBuffer(device, input, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, input_memory, NULL);
    vkFreeMemory(device, output_memory, NULL);
    puts("PASS redundant vertex comparisons and ordered buffer versions");
}
