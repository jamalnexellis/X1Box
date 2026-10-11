/* Shared mirror copy helper with fenced GPU readers and independent snapshots.
 * Transfer reads verify bytes/order, not vertex shader or game behavior. */
#include "hw/xbox/nv2a/pgraph/vk/vertex-propagation.h"

static void test_vertex_propagation(VkQueue queue, VkCommandPool pool,
                                    unsigned slots, bool sparse)
{
    enum { page = 4096, bytes = 4 * page + 12, passes = 24 };
    VkBuffer input[3], output;
    VkDeviceMemory input_memory[3];
    VkDeviceMemory output_memory = make_buffer(bytes * passes, &output);
    uint8_t *mapped[3], *result;
    uint8_t flags[3][5], oracle[bytes] = {0}, expected[passes][bytes];
    VkVertexPending pending[3];
    bool initialized[3] = {true, false, false}, in_flight[3] = {false};
    VkFence fences[3];
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (unsigned i = 0; i < slots; i++) {
        input_memory[i] = make_buffer(bytes, &input[i]);
        CHECK(vkMapMemory(device, input_memory[i], 0, VK_WHOLE_SIZE, 0,
                          (void **)&mapped[i]));
        memset(mapped[i], 0, bytes);
        assert(vk_vertex_pending_init(&pending[i], flags[i], bytes, page));
        CHECK(vkCreateFence(device, &fence_info, NULL, &fences[i]));
    }
    CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&result));
    memset(result, 0xa5, bytes * passes);
    VkCommandBuffer cmds[passes];
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = passes,
    };
    CHECK(vkAllocateCommandBuffers(device, &alloc, cmds));
    unsigned previous = 0;
    for (unsigned pass = 0; pass < passes; pass++) {
        unsigned current = pass % slots;
        if (in_flight[current]) {
            CHECK(vkWaitForFences(device, 1, &fences[current], VK_TRUE, UINT64_MAX));
            CHECK(vkResetFences(device, 1, &fences[current]));
            in_flight[current] = false;
        }
        if (pass) {
            if (!initialized[current]) {
                assert(vk_vertex_pending_mark(&pending[current], 0, bytes));
            }
            VkVertexPropagation copy;
            assert(vk_vertex_pending_copy(&pending[current], mapped[current],
                                          mapped[previous], sparse, &copy));
            initialized[current] = true;
        }
        assert(memcmp(mapped[current], oracle, bytes) == 0);
        if (pass % 5 != 4) {
            size_t offset = pass % 3 == 0 ? 1 :
                (pass % 3 == 1 ? page + 17 : bytes - 1);
            mapped[current][offset] = oracle[offset] = (uint8_t)(pass + 41);
            for (unsigned i = 0; i < slots; i++) {
                if (i != current) {
                    assert(vk_vertex_pending_mark(&pending[i], offset, 1));
                }
            }
        }
        memcpy(expected[pass], oracle, bytes);
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        CHECK(vkBeginCommandBuffer(cmds[pass], &begin));
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = input[current], .size = bytes,
        };
        vkCmdPipelineBarrier(cmds[pass], VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        VkBufferCopy copy = {.dstOffset = (VkDeviceSize)pass * bytes, .size = bytes};
        vkCmdCopyBuffer(cmds[pass], input[current], output, 1, &copy);
        VkBufferMemoryBarrier readback = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = output, .offset = copy.dstOffset, .size = bytes,
        };
        vkCmdPipelineBarrier(cmds[pass], VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &readback, 0, NULL);
        CHECK(vkEndCommandBuffer(cmds[pass]));
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmds[pass],
        };
        CHECK(vkQueueSubmit(queue, 1, &submit, fences[current]));
        in_flight[current] = true;
        previous = current;
    }
    for (unsigned i = 0; i < slots; i++) {
        CHECK(vkWaitForFences(device, 1, &fences[i], VK_TRUE, UINT64_MAX));
    }
    assert(memcmp(result, expected, sizeof(expected)) == 0);
    for (unsigned i = 0; i < slots; i++) {
        vkDestroyFence(device, fences[i], NULL);
        vkUnmapMemory(device, input_memory[i]);
        vkDestroyBuffer(device, input[i], NULL);
        vkFreeMemory(device, input_memory[i], NULL);
    }
    vkFreeCommandBuffers(device, pool, passes, cmds);
    vkUnmapMemory(device, output_memory);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, output_memory, NULL);
    printf("PASS fenced mirror propagation: slots=%u sparse=%d versions=%u\n",
           slots, sparse, passes);
}
