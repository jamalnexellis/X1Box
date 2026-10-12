/* Check owned layouts and the exact command arguments, including empty state. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/vertex-input.h"

static NV2AVkVertexInput expected;
static unsigned calls;

static VKAPI_ATTR void VKAPI_CALL capture(
    VkCommandBuffer cmd, uint32_t binding_count,
    const VkVertexInputBindingDescription2EXT *bindings,
    uint32_t attribute_count,
    const VkVertexInputAttributeDescription2EXT *attributes)
{
    (void)cmd;
    assert(binding_count == expected.binding_count);
    assert(attribute_count == expected.attribute_count);
    assert(!!bindings == !!binding_count);
    assert(!!attributes == !!attribute_count);
    for (uint32_t i = 0; i < binding_count; i++) {
        assert(bindings[i].sType ==
               VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
        assert(!bindings[i].pNext);
        assert(bindings[i].binding == expected.bindings[i].binding);
        assert(bindings[i].stride == expected.bindings[i].stride);
        assert(bindings[i].inputRate == expected.bindings[i].inputRate);
        assert(bindings[i].divisor == 1);
    }
    for (uint32_t i = 0; i < attribute_count; i++) {
        assert(attributes[i].sType ==
               VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
        assert(!attributes[i].pNext);
        assert(attributes[i].location == expected.attributes[i].location);
        assert(attributes[i].binding == expected.attributes[i].binding);
        assert(attributes[i].format == expected.attributes[i].format);
        assert(attributes[i].offset == expected.attributes[i].offset);
    }
    calls++;
}

int main(void)
{
    for (uint32_t count = 0; count <= NV2A_VK_VERTEX_INPUT_MAX; count++) {
        for (unsigned trial = 0; trial < 64; trial++) {
            VkVertexInputBindingDescription bindings[NV2A_VK_VERTEX_INPUT_MAX];
            VkVertexInputAttributeDescription attributes[NV2A_VK_VERTEX_INPUT_MAX];
            for (uint32_t i = 0; i < count; i++) {
                bindings[i] = (VkVertexInputBindingDescription){
                    .binding = i,
                    .stride = (trial + i) * 4,
                    .inputRate = trial & 1 ? VK_VERTEX_INPUT_RATE_INSTANCE :
                                           VK_VERTEX_INPUT_RATE_VERTEX,
                };
                attributes[i] = (VkVertexInputAttributeDescription){
                    .location = i,
                    .binding = i,
                    .format = trial & 1 ? VK_FORMAT_R32G32_SFLOAT :
                                         VK_FORMAT_R8G8B8A8_UNORM,
                    .offset = (trial % 4) * 4,
                };
            }
            NV2AVkVertexInput a, b;
            memset(&a, 0xa5, sizeof(a));
            memset(&b, 0x5a, sizeof(b));
            vk_vertex_input_snapshot(&a, count, count ? bindings : NULL,
                                     count, count ? attributes : NULL);
            vk_vertex_input_snapshot(&b, count, count ? bindings : NULL,
                                     count, count ? attributes : NULL);
            assert(!memcmp(&a, &b, sizeof(a)));
            expected = a;
            /* A deferred draw must survive reuse of both caller arrays. */
            memset(bindings, 0xcc, sizeof(bindings));
            memset(attributes, 0xdd, sizeof(attributes));
            vk_vertex_input_emit(capture, VK_NULL_HANDLE, &a);
        }
    }
    assert(calls == 1088);
    printf("Owned vertex layout checks passed: %u cases, including zero-input state\n",
           calls);
    return 0;
}
