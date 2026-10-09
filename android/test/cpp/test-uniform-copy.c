/* Standalone checks of the production Vulkan uniform packing helpers. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* glsl.h needs only standard headers and ROUND_UP from qemu/osdep.h.
 * Avoid depending on a configured QEMU tree for this standalone test. */
#define QEMU_OSDEP_H
#define ROUND_UP(n, d) (((n) + (d) - 1) & ~((d) - 1))
#include "hw/xbox/nv2a/pgraph/vk/glsl.h"

static void check_copy(size_t components, size_t elements, size_t stride,
                       size_t copied_elements)
{
    uint8_t output[4096], expected[4096];
    uint32_t values[768];
    const size_t offset = 32;
    ShaderUniform uniform = {
        .dim_v = components, .dim_a = elements,
        .stride = stride, .offset = offset,
    };
    ShaderUniformLayout layout = {
        .uniforms = &uniform, .num_uniforms = 1,
        .total_size = sizeof(output) - 32, .allocation = output,
    };
    memset(output, 0xa5, sizeof(output));
    memset(expected, 0xa5, sizeof(expected));
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        /* Include NaN payloads and signed zero: packing must preserve bits. */
        values[i] = i % 2 ? 0x7fc00000u + (uint32_t)i : 0x80000000u;
    }
    for (size_t element = 0; element < copied_elements; element++) {
        for (size_t component = 0; component < components; component++) {
            memcpy(expected + offset + element * stride + component * 4,
                   &values[element * components + component], 4);
        }
    }
    uniform_copy(&layout, 1, values, sizeof(values[0]),
                 components * copied_elements);
    assert(memcmp(output, expected, sizeof(output)) == 0);
}

static void check_std140_padding(void)
{
    ShaderUniform uniforms[] = {
        { .name = "scalar", .dim_v = 1, .dim_a = 1 },
        { .name = "vector", .dim_v = 3, .dim_a = 1 },
        { .name = "array", .dim_v = 2, .dim_a = 3 },
    };
    ShaderUniformLayout layout = {
        .uniforms = uniforms, .num_uniforms = 3,
    };
    uniform_std140(&layout);
    assert(uniforms[2].stride == 16);
    assert(uniforms[2].offset == 16);
    uint8_t output[64], expected[64];
    const uint32_t values[] = { 1, 2, 3, 4, 5, 6 };
    memset(output, 0xa5, sizeof(output));
    memset(expected, 0xa5, sizeof(expected));
    memcpy(expected + 16, values, 8);
    memcpy(expected + 32, values + 2, 8);
    memcpy(expected + 48, values + 4, 8);
    layout.allocation = output;
    uniform_copy(&layout, 3, (void *)values, sizeof(values[0]), 6);
    assert(memcmp(output, expected, sizeof(output)) == 0);
}

int main(void)
{
    check_copy(1, 1, 0, 1);
    check_copy(3, 1, 0, 1);
    check_copy(4, 192, 16, 192); /* Xbox vertex constants */
    check_copy(4, 4, 16, 4);     /* mat4 */
    check_copy(4, 16, 16, 7);    /* partial contiguous update */
    check_copy(1, 4, 16, 4);     /* scalar std140 array */
    check_copy(2, 4, 16, 4);
    check_copy(3, 4, 16, 4);
    check_copy(4, 4, 32, 4);     /* reflected wider stride */
    check_copy(4, 4, 16, 0);
    check_std140_padding();
    puts("PASS: uniform constants, matrices, strided arrays, padding and bit preservation");
    return 0;
}
