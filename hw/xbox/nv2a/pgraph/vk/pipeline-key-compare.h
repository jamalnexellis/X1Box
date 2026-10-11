/* Compact lookup keys retain a complete shader-state collision check. */
#ifndef NV2A_VK_PIPELINE_KEY_COMPARE_H
#define NV2A_VK_PIPELINE_KEY_COMPARE_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool vk_pipeline_keys_differ(const void *cached_key,
                                          const void *requested_key,
                                          size_t key_size, bool clear,
                                          const void *cached_shader_state,
                                          const void *requested_shader_state,
                                          size_t shader_state_size)
{
    if (memcmp(cached_key, requested_key, key_size)) {
        return true;
    }
    if (clear) {
        return false;
    }
    assert(cached_shader_state && requested_shader_state);
    return memcmp(cached_shader_state, requested_shader_state,
                  shader_state_size) != 0;
}

#endif
