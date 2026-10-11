/* Exact comparison for redundant host vertex uploads. */
#ifndef NV2A_VK_VERTEX_UPLOAD_H
#define NV2A_VK_VERTEX_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Bound the extra CPU read. Larger updates keep the existing upload path. */
#define VK_VERTEX_UPLOAD_COMPARE_LIMIT (64u * 1024u)

static inline bool vk_vertex_upload_unchanged(const void *mapped,
                                              size_t capacity,
                                              const void *incoming,
                                              uint64_t offset, uint64_t size,
                                              bool already_uploaded)
{
    if (!already_uploaded || !mapped || !incoming || !size ||
        size > VK_VERTEX_UPLOAD_COMPARE_LIMIT || offset > capacity ||
        size > capacity - offset) {
        return false;
    }
    /* A hash match is insufficient: earlier draws must retain every byte. */
    return memcmp((const uint8_t *)mapped + (size_t)offset,
                  incoming, (size_t)size) == 0;
}

#endif
