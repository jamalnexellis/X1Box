/* Validate copy extents before multiplying guest dimensions. */
#ifndef XEMU_VK_SCALED_COPY_H
#define XEMU_VK_SCALED_COPY_H
#include <stdbool.h>
#include <stdint.h>

static inline bool vk_scaled_copy_extent(uint32_t width, uint32_t height,
    uint32_t scale, bool allow_scaled, uint32_t *image_width,
    uint32_t *image_height)
{
    if (!scale || scale > 4 || (scale != 1 && !allow_scaled) ||
        !width || !height || width > 4096 / scale || height > 4096 / scale) {
        return false;
    }
    *image_width = width * scale;
    *image_height = height * scale;
    return true;
}
#endif
