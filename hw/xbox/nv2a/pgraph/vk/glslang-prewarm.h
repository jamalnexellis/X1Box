/* Prepare glslang's lazy GLSL 450 built-in tables before guest rendering.
 * No shader module, pipeline or cache entry is created by this operation. */
#ifndef NV2A_VK_GLSLANG_PREWARM_H
#define NV2A_VK_GLSLANG_PREWARM_H

#include <stdbool.h>
#include <glslang/Include/glslang_c_interface.h>

static inline bool nv2a_glslang_prewarm(const glslang_input_t *target)
{
    glslang_input_t input = *target;
    input.stage = GLSLANG_STAGE_VERTEX;
    input.code = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) {
        return false;
    }
    bool ready = glslang_shader_preprocess(shader, &input);
    glslang_shader_delete(shader);
    return ready;
}

#endif
