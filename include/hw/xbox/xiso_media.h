/* Read-only PGR 2 XISO media compatibility. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOX_XISO_MEDIA_H
#define XBOX_XISO_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*XisoMediaRead)(void *opaque, uint64_t offset,
                             void *buffer, size_t length);

/* Returns a disc byte offset, or -1 if no unique supported patch is found. */
int64_t xbox_xiso_media_find_patch(XisoMediaRead read, void *opaque,
                                  uint64_t image_size);
void xbox_xiso_media_apply(int64_t patch_offset, uint64_t read_offset,
                          uint8_t *buffer, size_t length);

#endif
