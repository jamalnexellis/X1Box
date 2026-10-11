/* Per-destination host mirror tracking; callers fence GPU readers before copy. */
#ifndef NV2A_VK_VERTEX_PROPAGATION_H
#define NV2A_VK_VERTEX_PROPAGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct VkVertexPending {
    uint8_t *pages;
    size_t capacity, page_size, page_count;
    size_t first_page, end_page;
} VkVertexPending;

typedef struct VkVertexPropagation {
    size_t copied_bytes, bounding_bytes, runs;
    size_t first_byte, end_byte;
} VkVertexPropagation;

static inline bool vk_vertex_pending_init(VkVertexPending *pending,
                                         uint8_t *pages, size_t capacity,
                                         size_t page_size)
{
    if (!pending || !pages || !capacity || !page_size) {
        return false;
    }
    size_t count = capacity / page_size + (capacity % page_size != 0);
    *pending = (VkVertexPending){
        .pages = pages, .capacity = capacity, .page_size = page_size,
        .page_count = count, .first_page = count,
    };
    memset(pages, 0, count);
    return true;
}

static inline bool vk_vertex_pending_mark(VkVertexPending *pending,
                                         size_t offset, size_t size)
{
    if (!pending || !pending->pages || offset > pending->capacity ||
        size > pending->capacity - offset) {
        return false;
    }
    if (!size) {
        return true;
    }
    size_t first = offset / pending->page_size;
    /* The subtraction form also covers a partial final page without wrapping. */
    size_t end = (offset + size - 1) / pending->page_size + 1;
    memset(pending->pages + first, 1, end - first);
    if (first < pending->first_page) {
        pending->first_page = first;
    }
    if (end > pending->end_page) {
        pending->end_page = end;
    }
    return true;
}

static inline size_t vk_vertex_pending_page_end(const VkVertexPending *pending,
                                               size_t end_page)
{
    return end_page == pending->page_count ? pending->capacity :
        end_page * pending->page_size;
}

static inline bool vk_vertex_pending_copy(VkVertexPending *pending,
                                         void *destination, const void *source,
                                         bool sparse,
                                         VkVertexPropagation *result)
{
    if (!pending || !pending->pages || !result) {
        return false;
    }
    *result = (VkVertexPropagation){ .first_byte = pending->capacity };
    if (pending->first_page >= pending->end_page) {
        return true;
    }
    if (!destination || !source) {
        return false;
    }
    size_t first = pending->first_page, end = pending->end_page;
    size_t first_byte = first * pending->page_size;
    size_t end_byte = vk_vertex_pending_page_end(pending, end);
    if (destination != source) {
        result->first_byte = first_byte;
        result->end_byte = end_byte;
        result->bounding_bytes = end_byte - first_byte;
        if (!sparse) {
            memcpy((uint8_t *)destination + first_byte,
                   (const uint8_t *)source + first_byte,
                   end_byte - first_byte);
            result->copied_bytes = end_byte - first_byte;
            result->runs = 1;
        } else {
            size_t page = first;
            while (page < end) {
                if (!pending->pages[page]) {
                    page++;
                    continue;
                }
                size_t run_first = page++;
                while (page < end && pending->pages[page]) {
                    page++;
                }
                size_t run_byte = run_first * pending->page_size;
                size_t length = vk_vertex_pending_page_end(pending, page) -
                    run_byte;
                memcpy((uint8_t *)destination + run_byte,
                       (const uint8_t *)source + run_byte, length);
                result->copied_bytes += length;
                result->runs++;
            }
        }
    }
    memset(pending->pages + first, 0, end - first);
    pending->first_page = pending->page_count;
    pending->end_page = 0;
    return true;
}

#endif
