/* Recording resets and owned dynamic offsets must never reuse stale state. */
#include <stdio.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/descriptor-bind.h"

#define HANDLE(type, value) ((type)(uintptr_t)(value))

int main(void)
{
    NV2AVkDescriptorBind last = {0};
    VkCommandBuffer command = HANDLE(VkCommandBuffer, 0x1000);
    VkPipelineLayout layout = HANDLE(VkPipelineLayout, 0x2000);
    VkDescriptorSet set = HANDLE(VkDescriptorSet, 0x3000);
    unsigned checks = 0;
    for (unsigned i = 0; i < 4096; i++) {
        uint32_t offsets[2] = {i * 256, (4095 - i) * 256};
        nv2a_vk_descriptor_bind_invalidate(&last);
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 0, set, 2, offsets)); checks++;
        memset(offsets, 0xff, sizeof(offsets));
        uint32_t owned[2] = {i * 256, (4095 - i) * 256};
        assert(!nv2a_vk_descriptor_bind_required(&last, command, layout, 0, set, 2, owned)); checks++;
        owned[0] ^= 0x80000000u;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 0, set, 2, owned)); checks++;
        owned[1] ^= 0x80000000u;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 0, set, 2, owned)); checks++;
        assert(!nv2a_vk_descriptor_bind_required(&last, command, layout, 0, set, 2, owned)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 1, set, 2, owned)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 1, set, 0, NULL)); checks++;
        assert(!nv2a_vk_descriptor_bind_required(&last, command, layout, 1, set, 0, NULL)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 1, set, 2, owned)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, command, layout, 1, HANDLE(VkDescriptorSet, 0x3001), 2, owned)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, command, HANDLE(VkPipelineLayout, 0x2001), 1, set, 2, owned)); checks++;
        assert(nv2a_vk_descriptor_bind_required(&last, HANDLE(VkCommandBuffer, 0x1001), layout, 1, set, 2, owned)); checks++;
        assert(!nv2a_vk_descriptor_bind_required(&last, HANDLE(VkCommandBuffer, 0x1001), layout, 1, set, 2, owned)); checks++;
        /* A foreign set write, pipeline change or reset of the same Vk handle. */
        nv2a_vk_descriptor_bind_invalidate(&last);
        assert(nv2a_vk_descriptor_bind_required(&last, HANDLE(VkCommandBuffer, 0x1001), layout, 1, set, 2, owned)); checks++;
    }
    assert(checks == 57344);
    printf("PASS descriptor bind checks=%u; owned offsets, layouts, sets, first-set, offset-count, foreign writes and reused recordings\n", checks);
    return 0;
}
