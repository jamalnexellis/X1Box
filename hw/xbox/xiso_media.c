/*
 * PGR 2 XISO startup compatibility. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unprepared backups retain XAPI's DVD-X2 authentication check. A trimmed
 * XISO lacks the physical DVD authentication data, so that check fails and
 * the title returns to the dashboard before displaying its first frame.
 * Apply extract-xiso's media-enable instruction adaptation to read buffers
 * only: https://github.com/XboxDev/extract-xiso/blob/master/extract-xiso.c
 * Restrict detection to PGR 2's default.xbe and its executable sections.
 * Never change the user's image, certificate, section digests, or BIOS.
 */
#include "hw/xbox/xiso_media.h"
#include <stdlib.h>
#include <string.h>

#define XISO_SECTOR_SIZE 2048u
#define XISO_HEADER_OFFSET 0x10000u
#define MAX_ROOT_SIZE (64u * 1024u)
#define MAX_XBE_SIZE (32u * 1024u * 1024u)
#define SCAN_SIZE (64u * 1024u)
#define PGR2_TITLE_ID 0x4d53004bu

static const uint8_t media_check[] = {
    0xe8, 0xca, 0xfd, 0xff, 0xff, 0x85, 0xc0, 0x7d, 0x09
};

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned int ascii_lower(unsigned int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int compare_default_name(const uint8_t *name, size_t length)
{
    const char target[] = "default.xbe";
    size_t i;

    for (i = 0; i < length && i < sizeof(target) - 1; i++) {
        int difference = (int)ascii_lower(target[i]) -
                         (int)ascii_lower(name[i]);
        if (difference) {
            return difference;
        }
    }
    return (int)(sizeof(target) - 1) - (int)length;
}

static bool read_checked(XisoMediaRead read, void *opaque, uint64_t image_size,
                         uint64_t offset, void *buffer, size_t length)
{
    return offset <= image_size && length <= image_size - offset &&
           read(opaque, offset, buffer, length);
}

int64_t xbox_xiso_media_find_patch(XisoMediaRead read, void *opaque,
                                  uint64_t image_size)
{
    uint8_t header[XISO_SECTOR_SIZE], node[14 + 255], xbe_header[0x178];
    uint8_t certificate[12], section[56];
    uint64_t root_offset, xbe_offset, certificate_offset, section_table;
    uint32_t root_size, xbe_size = 0, base, section_count;
    uint32_t node_offset = 0;
    int64_t result = -1;
    unsigned int matches = 0;
    uint8_t *scan;

    if (!read || !read_checked(read, opaque, image_size, XISO_HEADER_OFFSET,
                              header, sizeof(header)) ||
        memcmp(header, "MICROSOFT*XBOX*MEDIA", 20) ||
        memcmp(header + sizeof(header) - 20, "MICROSOFT*XBOX*MEDIA", 20)) {
        return -1;
    }
    root_offset = (uint64_t)u32le(header + 20) * XISO_SECTOR_SIZE;
    root_size = u32le(header + 24);
    if (root_size < 14 || root_size > MAX_ROOT_SIZE ||
        root_offset > image_size || root_size > image_size - root_offset) {
        return -1;
    }

    /* XDVDFS directory entries form a case-insensitive binary search tree. */
    xbe_offset = 0;
    for (unsigned int depth = 0; depth < 64; depth++) {
        uint32_t next;
        int comparison;

        if (node_offset > root_size - 14 ||
            !read_checked(read, opaque, image_size, root_offset + node_offset,
                          node, 14) ||
            node[13] > root_size - node_offset - 14 ||
            !read_checked(read, opaque, image_size,
                          root_offset + node_offset + 14, node + 14, node[13])) {
            return -1;
        }
        comparison = compare_default_name(node + 14, node[13]);
        if (!comparison) {
            if (node[12] & 0x10) {
                return -1;
            }
            xbe_offset = (uint64_t)u32le(node + 4) * XISO_SECTOR_SIZE;
            xbe_size = u32le(node + 8);
            break;
        }
        const uint8_t *child = node + (comparison < 0 ? 0 : 2);
        next = ((uint32_t)child[0] | (uint32_t)child[1] << 8) * 4;
        if (!next || next == node_offset) {
            return -1;
        }
        node_offset = next;
    }
    if (xbe_size < sizeof(xbe_header) || xbe_size > MAX_XBE_SIZE ||
        xbe_offset > image_size || xbe_size > image_size - xbe_offset ||
        !read_checked(read, opaque, image_size, xbe_offset,
                      xbe_header, sizeof(xbe_header)) ||
        memcmp(xbe_header, "XBEH", 4)) {
        return -1;
    }
    base = u32le(xbe_header + 0x104);
    if (u32le(xbe_header + 0x118) < base ||
        u32le(xbe_header + 0x120) < base) {
        return -1;
    }
    certificate_offset = (uint64_t)u32le(xbe_header + 0x118) - base;
    section_table = (uint64_t)u32le(xbe_header + 0x120) - base;
    section_count = u32le(xbe_header + 0x11c);
    if (certificate_offset > xbe_size - sizeof(certificate) ||
        !section_count || section_count > 256 || section_table > xbe_size ||
        (uint64_t)section_count * sizeof(section) > xbe_size - section_table ||
        !read_checked(read, opaque, image_size, xbe_offset + certificate_offset,
                      certificate, sizeof(certificate)) ||
        u32le(certificate) < sizeof(certificate) ||
        u32le(certificate + 8) != PGR2_TITLE_ID) {
        return -1;
    }

    scan = malloc(SCAN_SIZE + sizeof(media_check) - 1);
    if (!scan) {
        return -1;
    }
    for (uint32_t index = 0; index < section_count; index++) {
        uint32_t raw_offset, raw_size, done = 0;
        size_t carry = 0;

        if (!read_checked(read, opaque, image_size,
                          xbe_offset + section_table + index * sizeof(section),
                          section, sizeof(section))) {
            goto invalid;
        }
        raw_offset = u32le(section + 12);
        raw_size = u32le(section + 16);
        if (raw_offset > xbe_size || raw_size > xbe_size - raw_offset) {
            goto invalid;
        }
        if (!(u32le(section) & 4)) {
            continue;
        }
        while (done < raw_size) {
            size_t chunk = raw_size - done;
            size_t total;
            if (chunk > SCAN_SIZE) {
                chunk = SCAN_SIZE;
            }
            if (!read_checked(read, opaque, image_size,
                              xbe_offset + raw_offset + done,
                              scan + carry, chunk)) {
                goto invalid;
            }
            total = carry + chunk;
            for (size_t i = 0; i + sizeof(media_check) <= total; i++) {
                if (!memcmp(scan + i, media_check, sizeof(media_check))) {
                    if (++matches != 1) {
                        goto invalid;
                    }
                    result = (int64_t)(xbe_offset + raw_offset + done - carry +
                                       i + 7);
                }
            }
            done += chunk;
            carry = total < sizeof(media_check) - 1 ? total :
                    sizeof(media_check) - 1;
            memmove(scan, scan + total - carry, carry);
        }
    }
    free(scan);
    return result;

invalid:
    free(scan);
    return -1;
}

void xbox_xiso_media_apply(int64_t patch_offset, uint64_t read_offset,
                          uint8_t *buffer, size_t length)
{
    if (patch_offset >= 0 && (uint64_t)patch_offset >= read_offset &&
        (uint64_t)patch_offset - read_offset < length) {
        size_t index = (uint64_t)patch_offset - read_offset;
        if (buffer[index] == 0x7d) {
            buffer[index] = 0xeb;
        }
    }
}
