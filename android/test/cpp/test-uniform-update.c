/* Differential packing through repeated updates and shared layout reuse. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define QEMU_OSDEP_H
#define ROUND_UP(n, d) (((n) + (d) - 1) & ~((d) - 1))
#include "hw/xbox/nv2a/pgraph/vk/glsl.h"

static uint32_t random_word(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

int main(void)
{
    uint32_t seed = 0x6382135u;
    uint8_t reference[8192], exact[8192], before[8192];
    uint32_t source[768], temporary[768];
    const unsigned lengths[] = {1, 2, 4, 8, 16, 192};
    unsigned cases = 0, unchanged = 0, changed = 0;
    for (unsigned components = 1; components <= 4; components++) {
        for (unsigned length = 0; length < sizeof(lengths) / sizeof(lengths[0]); length++) {
            for (unsigned wide = 0; wide < 3; wide++) {
                size_t stride = wide == 0 ? components * 4 : wide == 1 ? 16 : 32;
                ShaderUniform field = {.dim_v = components, .dim_a = lengths[length],
                    .stride = stride, .offset = 32};
                ShaderUniformLayout a = {.uniforms = &field, .num_uniforms = 1,
                    .total_size = sizeof(reference) - 32, .allocation = reference};
                ShaderUniformLayout b = a; b.allocation = exact;
                memset(reference, 0xa5, sizeof(reference));
                memcpy(exact, reference, sizeof(exact));
                for (unsigned i = 0; i < 768; i++) source[i] = random_word(&seed);
                for (unsigned update = 0; update < 512; update++) {
                    size_t count = components * (update % (lengths[length] + 1));
                    if (update % 5 == 0 && count) {
                        unsigned index = random_word(&seed) % count;
                        source[index] = update & 1 ? 0x7fc00001u + update :
                                        update & 2 ? 0x80000000u : random_word(&seed);
                    }
                    if (update % 71 == 0) {
                        /* New/cold module allocation or a replaced live layout. */
                        memset(reference, (int)(update & 255), sizeof(reference));
                        memcpy(exact, reference, sizeof(exact));
                    }
                    memcpy(before, reference, sizeof(before));
                    /* Scalar reference goes through the old intermediate array. */
                    memcpy(temporary, source, sizeof(source));
                    uniform_copy(&a, 1, temporary, 4, count);
                    bool result = uniform_copy_if_changed(&b, 1, source, 4, count);
                    assert(!memcmp(reference, exact, sizeof(reference)));
                    assert(result == !!memcmp(before, reference, sizeof(reference)));
                    if (result) changed++; else unchanged++;
                    /* A repeated update must leave values and padding untouched. */
                    assert(!uniform_copy_if_changed(&b, 1, source, 4, count));
                    assert(!memcmp(reference, exact, sizeof(reference)));
                    cases++;
                }
            }
        }
    }
    assert(unchanged && changed);
    printf("PASS exact uniform updates=%u changed=%u unchanged=%u; repeated, partial, strided, cold-layout and raw-bit equivalence\n",
           cases, changed, unchanged);
    return 0;
}
