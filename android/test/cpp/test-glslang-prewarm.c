#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glslang/Public/resource_limits_c.h>
#include "hw/xbox/nv2a/pgraph/vk/glslang-prewarm.h"

static int64_t now_us(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static unsigned int *compile(const glslang_input_t *input, size_t *size,
                             int64_t *preprocess_us)
{
    glslang_shader_t *shader = glslang_shader_create(input);
    assert(shader);
    int64_t start = now_us();
    assert(glslang_shader_preprocess(shader, input));
    *preprocess_us = now_us() - start;
    assert(glslang_shader_parse(shader, input));
    glslang_program_t *program = glslang_program_create();
    assert(program);
    glslang_program_add_shader(program, shader);
    assert(glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                         GLSLANG_MSG_VULKAN_RULES_BIT));
    glslang_spv_options_t options = { .validate = true };
    glslang_program_SPIRV_generate_with_options(program, input->stage, &options);
    *size = glslang_program_SPIRV_get_size(program) * sizeof(unsigned int);
    assert(*size >= 20);
    unsigned int *words = malloc(*size);
    assert(words);
    glslang_program_SPIRV_get(program, words);
    assert(words[0] == 0x07230203);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return words;
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && strcmp(argv[1], "--warm") == 0));
    bool warm = argc == 2;
    assert(glslang_initialize_process());
    const glslang_target_client_version_t clients[] = {
        GLSLANG_TARGET_VULKAN_1_1, GLSLANG_TARGET_VULKAN_1_2,
        GLSLANG_TARGET_VULKAN_1_3,
    };
    const glslang_target_language_version_t versions[] = {
        GLSLANG_TARGET_SPV_1_3, GLSLANG_TARGET_SPV_1_5, GLSLANG_TARGET_SPV_1_6,
    };
    const glslang_stage_t stages[] = {
        GLSLANG_STAGE_VERTEX, GLSLANG_STAGE_FRAGMENT, GLSLANG_STAGE_GEOMETRY,
    };
    const char *const sources[] = {
        "#version 450\nlayout(location=0) in vec4 position;\n"
        "void main() { gl_Position = position; }\n",
        "#version 450\nlayout(location=0) out vec4 color;\n"
        "void main() { color = vec4(0.25, 0.5, 0.75, 1.0); }\n",
        "#version 450\nlayout(points) in;\n"
        "layout(points, max_vertices=1) out;\n"
        "void main() { gl_Position=gl_in[0].gl_Position; EmitVertex(); EndPrimitive(); }\n",
    };
    for (unsigned target = 0; target < 3; target++) {
        glslang_input_t input = {
            .language = GLSLANG_SOURCE_GLSL,
            .stage = GLSLANG_STAGE_VERTEX,
            .client = GLSLANG_CLIENT_VULKAN,
            .client_version = clients[target],
            .target_language = GLSLANG_TARGET_SPV,
            .target_language_version = versions[target],
            .code = sources[0],
            .default_version = 460,
            .default_profile = GLSLANG_NO_PROFILE,
            .messages = GLSLANG_MSG_DEFAULT_BIT,
            .resource = glslang_default_resource(),
        };
        int64_t start = now_us();
        if (warm) {
            assert(nv2a_glslang_prewarm(&input));
        }
        printf("warm target=%u enabled=%d us=%" PRId64 "\n",
               target, warm, now_us()-start);
        for (unsigned stage = 0; stage < 3; stage++) {
            input.stage = stages[stage];
            input.code = sources[stage];
            size_t first_size, second_size;
            int64_t preprocess, repeated;
            unsigned int *first = compile(&input, &first_size, &preprocess);
            /* Warming again must not mutate a compiled shader or its output. */
            assert(nv2a_glslang_prewarm(&input));
            unsigned int *second = compile(&input, &second_size, &repeated);
            assert(first_size == second_size);
            assert(memcmp(first, second, first_size) == 0);
            uint64_t hash = UINT64_C(14695981039346656037);
            for (size_t byte = 0; byte < first_size; byte++) {
                hash ^= ((const uint8_t *)first)[byte];
                hash *= UINT64_C(1099511628211);
            }
            printf("shader target=%u stage=%u bytes=%zu hash=%016" PRIx64
                   " preprocess_us=%" PRId64 " repeat_us=%" PRId64 "\n",
                   target, stage, first_size, hash, preprocess, repeated);
            free(first);
            free(second);
        }
    }
    glslang_finalize_process();
    puts("GLSL compiler warmup byte-preservation checks passed");
    return 0;
}
