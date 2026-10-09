/* Synthetic XISO regression cases. SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "hw/xbox/xiso_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE (512u * 1024u)
#define XBE_START (34u * 2048u)
#define SECTION_START (XBE_START + 0x1000u)
#define PATTERN_START (SECTION_START + 65532u)
#define PATCH_OFFSET (PATTERN_START + 7u)

typedef struct Image {
    uint8_t *data;
    bool fail;
} Image;

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++) {
        p[i] = value >> (i * 8);
    }
}

static bool read_image(void *opaque, uint64_t offset, void *buffer, size_t length)
{
    Image *image = opaque;
    assert(offset <= IMAGE_SIZE && length <= IMAGE_SIZE - offset);
    if (image->fail) {
        return false;
    }
    memcpy(buffer, image->data + offset, length);
    return true;
}

static bool read_file(void *opaque, uint64_t offset, void *buffer, size_t length)
{
    FILE *file = opaque;
    return !fseeko(file, offset, SEEK_SET) && fread(buffer, 1, length, file) == length;
}

static void fixture(Image *image)
{
    static const uint8_t pattern[] = {
        0xe8, 0xca, 0xfd, 0xff, 0xff, 0x85, 0xc0, 0x7d, 0x05
    };
    uint8_t *header, *node, *xbe, *section;
    memset(image->data, 0, IMAGE_SIZE);
    image->fail = false;
    header = image->data + 0x10000;
    memcpy(header, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(header + 2048 - 20, "MICROSOFT*XBOX*MEDIA", 20);
    put32(header + 20, 33);
    put32(header + 24, 28);
    node = image->data + 33 * 2048;
    put32(node + 4, 34);
    put32(node + 8, 0x30000);
    node[12] = 0x20;
    node[13] = 11;
    memcpy(node + 14, "DEFAULT.XBE", 11);
    xbe = image->data + XBE_START;
    memcpy(xbe, "XBEH", 4);
    put32(xbe + 0x104, 0x10000);
    put32(xbe + 0x118, 0x10200);
    put32(xbe + 0x11c, 1);
    put32(xbe + 0x120, 0x10400);
    put32(xbe + 0x200, 0x1ec);
    put32(xbe + 0x208, 0x4d53004b);
    section = xbe + 0x400;
    put32(section, 4);
    put32(section + 12, 0x1000);
    put32(section + 16, 0x10080);
    memcpy(image->data + PATTERN_START, pattern, sizeof(pattern));
}

int main(int argc, char **argv)
{
    Image image = { .data = malloc(IMAGE_SIZE) };
    uint8_t buffer[2048], original[2048];
    assert(image.data);
    fixture(&image);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == PATCH_OFFSET);
    /* The short branch displacement varies between XAPI versions; it is
     * not part of extract-xiso's eight-byte media-enable signature. */
    image.data[PATCH_OFFSET + 1] = 0x09;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == PATCH_OFFSET);
    image.data[PATCH_OFFSET + 1] = 0x05;
    memcpy(buffer, image.data + PATCH_OFFSET - 1024, sizeof(buffer));
    memcpy(original, buffer, sizeof(buffer));
    xbox_xiso_media_apply(PATCH_OFFSET, PATCH_OFFSET - 1024, buffer, sizeof(buffer));
    assert(buffer[1024] == 0xeb);
    buffer[1024] = original[1024];
    assert(!memcmp(buffer, original, sizeof(buffer)));
    assert(image.data[PATCH_OFFSET] == 0x7d);
    xbox_xiso_media_apply(PATCH_OFFSET, PATCH_OFFSET + 1, buffer, sizeof(buffer));
    xbox_xiso_media_apply(PATCH_OFFSET, PATCH_OFFSET - sizeof(buffer), buffer, sizeof(buffer));
    xbox_xiso_media_apply(-1, PATCH_OFFSET - 1024, buffer, sizeof(buffer));
    assert(!memcmp(buffer, original, sizeof(buffer)));

    image.fail = true;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    image.data[PATCH_OFFSET] = 0xeb;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    memcpy(image.data + SECTION_START + 16, image.data + PATTERN_START, 9);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    put32(image.data + XBE_START + 0x208, 0x12345678);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    put32(image.data + XBE_START + 0x400, 0);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    put32(image.data + XBE_START + 0x410, 0xffffffff);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    put32(image.data + XBE_START + 0x118, 0xffff);
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    assert(xbox_xiso_media_find_patch(read_image, &image, XBE_START + 0x200) == -1);
    fixture(&image);
    image.data[0x107ec] = 0;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    memcpy(image.data + 33 * 2048 + 14, "aaaaaaa.xbe", 11);
    image.data[33 * 2048 + 2] = 0xff;
    image.data[33 * 2048 + 3] = 0xff;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    fixture(&image);
    image.data[33 * 2048 + 12] |= 0x10;
    assert(xbox_xiso_media_find_patch(read_image, &image, IMAGE_SIZE) == -1);
    free(image.data);
    puts("PASS: XISO media adaptation, chunk boundaries, immutable source, malformed inputs and title isolation");
    if (argc == 2) {
        FILE *file = fopen(argv[1], "rb");
        uint8_t original_byte, read_byte;
        int64_t patch;
        assert(file && !fseeko(file, 0, SEEK_END));
        patch = xbox_xiso_media_find_patch(read_file, file, ftello(file));
        assert(patch >= 0 && read_file(file, patch, &original_byte, 1));
        read_byte = original_byte;
        xbox_xiso_media_apply(patch, patch, &read_byte, 1);
        assert(original_byte == 0x7d && read_byte == 0xeb);
        assert(read_file(file, patch, &read_byte, 1) && read_byte == original_byte);
        fclose(file);
        printf("PASS: mounted PGR 2 image, patch offset=0x%llx, source unchanged\n",
               (unsigned long long)patch);
    }
    return 0;
}
