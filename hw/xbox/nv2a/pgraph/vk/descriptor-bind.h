/* Exact last graphics-descriptor command, owned by one recording context. */
#ifndef NV2A_VK_DESCRIPTOR_BIND_H
#define NV2A_VK_DESCRIPTOR_BIND_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct NV2AVkDescriptorBind {
    VkCommandBuffer command;
    VkPipelineLayout layout;
    VkDescriptorSet set;
    uint32_t first_set, offset_count, offsets[2];
    bool valid;
} NV2AVkDescriptorBind;

static inline void nv2a_vk_descriptor_bind_invalidate(NV2AVkDescriptorBind *last)
{
    last->valid = false;
}

/* Call only for a single graphics set; invalidate on intervening state writes
 * and every new recording, including reuse of the same command-buffer handle.
 * This is actual recorded Vulkan state, not guest state saved by draw queues. */
static inline bool nv2a_vk_descriptor_bind_required(NV2AVkDescriptorBind *last,
    VkCommandBuffer command, VkPipelineLayout layout, uint32_t first_set,
    VkDescriptorSet set, uint32_t offset_count, const uint32_t *offsets)
{
    assert(command && layout && set && offset_count <= 2);
    assert(!offset_count || offsets);
    bool same = last->valid && last->command == command &&
                last->layout == layout && last->set == set &&
                last->first_set == first_set && last->offset_count == offset_count;
    for (uint32_t i = 0; same && i < offset_count; i++) {
        same = last->offsets[i] == offsets[i];
    }
    if (same) {
        return false;
    }
    *last = (NV2AVkDescriptorBind){ .command = command, .layout = layout,
        .set = set, .first_set = first_set, .offset_count = offset_count,
        .valid = true };
    for (uint32_t i = 0; i < offset_count; i++) {
        last->offsets[i] = offsets[i];
    }
    return true;
}

#endif
