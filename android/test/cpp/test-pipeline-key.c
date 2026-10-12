/* Compare compact lookups with the previous complete-state key predicate. */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/pipeline-key-compare.h"

#ifdef __ANDROID__
/* Existing renderer headers have mixed-sign comparisons and partially
 * initialized constant tables. Exempt only those headers from the warnings;
 * the fixture itself remains -Werror. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"
#pragma clang diagnostic pop
#else
/* Host checks exercise the size-independent comparator under sanitizers.
 * Android checks below use the actual renderer and ShaderState types. */
#define NV2A_VERTEXSHADER_ATTRIBUTES 16
#define OPT_DYNAMIC_STATES 1
typedef struct { uint32_t values[5]; } RenderPassState;
typedef struct { uint32_t values[3]; } VkVertexInputBindingDescription;
typedef struct { uint32_t values[4]; } VkVertexInputAttributeDescription;
typedef struct { uint8_t bytes[4096]; } ShaderState;
typedef struct {
    bool clear;
    bool dynamic_vertex_input;
    RenderPassState render_pass_state;
    uint64_t shader_state_hash;
    uint32_t regs[6];
    VkVertexInputBindingDescription binding_descriptions[16];
    VkVertexInputAttributeDescription attribute_descriptions[16];
} PipelineKey;
#endif

typedef struct {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[OPT_DYNAMIC_STATES ? 6 : 9];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} LegacyPipelineKey;

static uint32_t random_state = 0x373635u;
static uint32_t random_word(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void fill(void *data, size_t n)
{
    uint8_t *p = data;
    for (size_t i = 0; i < n; i++) {
        p[i] = random_word() >> 24;
    }
}

static uint64_t hash_state(const ShaderState *state)
{
    /* A deterministic fixture hash; forced collisions are intentional. */
    const uint8_t *p = (const uint8_t *)state;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof(*state); i++) {
        hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static void legacy(LegacyPipelineKey *old, const PipelineKey *key,
                   const ShaderState *state)
{
    memset(old, 0, sizeof(*old));
    old->clear = key->clear;
    old->render_pass_state = key->render_pass_state;
    if (!key->clear) {
        memcpy(&old->shader_state, state, sizeof(*state));
    }
    memcpy(old->regs, key->regs, sizeof(old->regs));
    memcpy(old->binding_descriptions, key->binding_descriptions,
           sizeof(old->binding_descriptions));
    memcpy(old->attribute_descriptions, key->attribute_descriptions,
           sizeof(old->attribute_descriptions));
}

static void check(const PipelineKey *a, const PipelineKey *b,
                  const ShaderState *sa, const ShaderState *sb)
{
    LegacyPipelineKey la, lb;
    legacy(&la, a, sa);
    legacy(&lb, b, sb);
    bool expected = memcmp(&la, &lb, sizeof(la)) != 0;
    bool actual = vk_pipeline_keys_differ(a, b, sizeof(*a), b->clear,
                                        sa, sb, sizeof(*sa));
    assert(actual == expected);
}

int main(void)
{
    unsigned int checks = 0;
    for (unsigned int trial = 0; trial < 12000; trial++) {
        PipelineKey a, b;
        ShaderState sa, sb, retained;
        memset(&a, 0, sizeof(a));
        fill(&a.render_pass_state, sizeof(a.render_pass_state));
        fill(a.regs, sizeof(a.regs));
        fill(a.binding_descriptions, sizeof(a.binding_descriptions));
        fill(a.attribute_descriptions, sizeof(a.attribute_descriptions));
        fill(&sa, sizeof(sa));
        memcpy(&sb, &sa, sizeof(sa));
        a.shader_state_hash = hash_state(&sa);
        b = a;
        check(&a, &b, &sa, &sb); checks++;

        /* Changed state with a different hash, and with a forced collision. */
        ((uint8_t *)&sb)[trial % sizeof(sb)] ^= 1;
        b.shader_state_hash = hash_state(&sb);
        check(&a, &b, &sa, &sb); checks++;
        b.shader_state_hash = a.shader_state_hash;
        check(&a, &b, &sa, &sb); checks++;

        memcpy(&sb, &sa, sizeof(sa));
        b = a;
        b.regs[trial % (sizeof(b.regs) / sizeof(b.regs[0]))] ^= 1;
        check(&a, &b, &sa, &sb); checks++;
        b = a;
        ((uint8_t *)&b.render_pass_state)[trial % sizeof(b.render_pass_state)] ^= 1;
        check(&a, &b, &sa, &sb); checks++;
        b = a;
        ((uint8_t *)b.binding_descriptions)[trial % sizeof(b.binding_descriptions)] ^= 1;
        check(&a, &b, &sa, &sb); checks++;
        b = a;
        ((uint8_t *)b.attribute_descriptions)[trial % sizeof(b.attribute_descriptions)] ^= 1;
        check(&a, &b, &sa, &sb); checks++;

        /* Simulate reuse of the shader LRU's memory after the pipeline saved
         * its own state. The retained copy must reject the new contents. */
        memcpy(&retained, &sa, sizeof(sa));
        fill(&sa, sizeof(sa));
        b = a; /* Force the same compact key despite different contents. */
        check(&a, &b, &retained, &sa); checks++;

        a.clear = b.clear = true;
        a.shader_state_hash = b.shader_state_hash = 0;
        check(&a, &b, &sa, &sb); checks++;
        assert(!vk_pipeline_keys_differ(&a, &b, sizeof(a), true,
                                       NULL, NULL, sizeof(sa))); checks++;
        b.clear = false;
        check(&a, &b, &sa, &sb); checks++;
    }
    assert(checks == 132000);
    PipelineKey static_key = {0}, dynamic_key = {0};
    ShaderState shader = {0};
    dynamic_key.dynamic_vertex_input = true;
    assert(vk_pipeline_keys_differ(&static_key, &dynamic_key,
                                  sizeof(static_key), false,
                                  &shader, &shader, sizeof(shader)));
    printf("Pipeline key equivalence passed: %u cases; legacy=%zu compact=%zu shader=%zu bytes\n",
           checks, sizeof(LegacyPipelineKey), sizeof(PipelineKey), sizeof(ShaderState));
    return 0;
}
