/* Generated mirror histories, checked against a fully copied byte oracle. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "hw/xbox/nv2a/pgraph/vk/vertex-propagation.h"

static uint32_t random_state = 0x543210fe;
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void old_three_slot_regression(void)
{
    uint8_t mirrors[3][32] = {{0}};
    mirrors[0][1] = 11;
    memcpy(mirrors[1] + 1, mirrors[0] + 1, 1);
    mirrors[1][25] = 35;
    memcpy(mirrors[2] + 25, mirrors[1] + 25, 1);
    assert(mirrors[2][25] == 35 && mirrors[2][1] != 11);
    /* The old copy only sees slot 1's new write. Slot 2 also needs slot 0's
     * earlier write, even though slot 1 made no further change to that byte. */
}

static void generated_history(unsigned slots, bool sparse)
{
    const size_t page = 4096, capacity = 16 * page + 19, page_count = 17;
    uint8_t *oracle = calloc(1, capacity);
    uint8_t *before = malloc(capacity);
    uint8_t *mirrors[3], *flags[3];
    VkVertexPending pending[3];
    bool initialized[3] = {true, false, false};
    assert(oracle && before);
    for (unsigned i = 0; i < slots; i++) {
        mirrors[i] = calloc(1, capacity);
        flags[i] = malloc(page_count);
        assert(mirrors[i] && flags[i]);
        assert(vk_vertex_pending_init(&pending[i], flags[i], capacity, page));
    }
    unsigned current = 0;
    for (unsigned step = 0; step < 2000; step++) {
        unsigned writes = step % 7 ? next_random() % 5 : 0;
        for (unsigned write = 0; write < writes; write++) {
            size_t offset = next_random() % capacity;
            size_t length = 1 + next_random() % 8193;
            if (length > capacity - offset) {
                length = capacity - offset;
            }
            for (size_t byte = offset; byte < offset + length; byte++) {
                mirrors[current][byte] = oracle[byte] = next_random();
            }
            for (unsigned i = 0; i < slots; i++) {
                if (i != current) {
                    assert(vk_vertex_pending_mark(&pending[i], offset, length));
                }
            }
        }
        assert(memcmp(mirrors[current], oracle, capacity) == 0);
        unsigned next = (current + 1) % slots;
        /* A GPU-work flush retires readers, not their mirror histories.
         * Exercise idle rotations and repeated destination reuse as well. */
        if (!initialized[next]) {
            assert(vk_vertex_pending_mark(&pending[next], 0, capacity));
        }
        memcpy(before, mirrors[current], capacity);
        VkVertexPropagation result;
        assert(vk_vertex_pending_copy(&pending[next], mirrors[next],
                                      mirrors[current], sparse, &result));
        initialized[next] = true;
        assert(memcmp(mirrors[next], oracle, capacity) == 0);
        assert(memcmp(mirrors[current], before, capacity) == 0);
        assert(result.copied_bytes <= result.bounding_bytes);
        assert(result.end_byte <= capacity);
        assert(pending[next].first_page == page_count);
        assert(pending[next].end_page == 0);
        for (size_t i = 0; i < page_count; i++) {
            assert(flags[next][i] == 0);
        }
        current = next;
    }
    for (unsigned i = 0; i < slots; i++) {
        free(mirrors[i]);
        free(flags[i]);
    }
    free(before);
    free(oracle);
}

static void boundaries_and_sparse_gaps(void)
{
    const size_t page = 4096, capacity = 5 * page + 19;
    uint8_t *source = malloc(capacity), *destination = calloc(1, capacity);
    uint8_t flags[6];
    VkVertexPending pending;
    VkVertexPropagation result;
    assert(source && destination);
    for (size_t i = 0; i < capacity; i++) {
        source[i] = (uint8_t)(i * 71 + (i >> 9) * 17);
    }
    assert(!vk_vertex_pending_init(NULL, flags, capacity, page));
    assert(!vk_vertex_pending_init(&pending, NULL, capacity, page));
    assert(!vk_vertex_pending_init(&pending, flags, 0, page));
    assert(!vk_vertex_pending_init(&pending, flags, capacity, 0));
    assert(vk_vertex_pending_init(&pending, flags, capacity, page));
    assert(!vk_vertex_pending_mark(&pending, SIZE_MAX, 1));
    assert(!vk_vertex_pending_mark(&pending, 1, SIZE_MAX));
    assert(!vk_vertex_pending_mark(&pending, capacity, 1));
    assert(vk_vertex_pending_mark(&pending, capacity, 0));
    void *unreadable = mmap(NULL, page, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(unreadable != MAP_FAILED);
    assert(vk_vertex_pending_copy(&pending, unreadable, unreadable, true, &result));
    assert(result.copied_bytes == 0);
    /* Noncontiguous first and last pages, including a partial final page. */
    assert(vk_vertex_pending_mark(&pending, 1, 1));
    assert(vk_vertex_pending_mark(&pending, capacity - 1, 1));
    assert(!vk_vertex_pending_copy(&pending, NULL, source, true, &result));
    assert(pending.end_page == 6); /* Failed copies retain pending writes. */
    assert(vk_vertex_pending_copy(&pending, destination, source, true, &result));
    assert(result.copied_bytes == page + 19);
    assert(result.bounding_bytes == capacity && result.runs == 2);
    assert(result.first_byte == 0 && result.end_byte == capacity);
    assert(memcmp(destination, source, page) == 0);
    assert(memcmp(destination + 5 * page, source + 5 * page, 19) == 0);
    for (size_t i = page; i < 5 * page; i++) {
        assert(destination[i] == 0); /* Clean gaps are never copied. */
    }
    assert(vk_vertex_pending_mark(&pending, page - 1, 2));
    assert(vk_vertex_pending_copy(&pending, destination, source, true, &result));
    assert(result.copied_bytes == 2 * page && result.runs == 1);
    assert(vk_vertex_pending_mark(&pending, 0, capacity));
    assert(vk_vertex_pending_copy(&pending, destination, source, false, &result));
    assert(result.copied_bytes == capacity);
    assert(memcmp(destination, source, capacity) == 0);
    assert(vk_vertex_pending_mark(&pending, 0, capacity));
    assert(vk_vertex_pending_copy(&pending, unreadable, unreadable, false, &result));
    assert(result.copied_bytes == 0 && pending.end_page == 0);
    assert(munmap(unreadable, page) == 0);
    free(source);
    free(destination);
}

static void distant_pages_64mib(void)
{
    const size_t capacity = 64 * 1024 * 1024, page = 4096;
    uint8_t *source = calloc(1, capacity), *destination = calloc(1, capacity);
    uint8_t *flags = malloc(capacity / page);
    VkVertexPending pending;
    VkVertexPropagation result;
    assert(source && destination && flags);
    assert(vk_vertex_pending_init(&pending, flags, capacity, page));
    source[0] = 17;
    source[capacity - 1] = 29;
    assert(vk_vertex_pending_mark(&pending, 0, 1));
    assert(vk_vertex_pending_mark(&pending, capacity - 1, 1));
    assert(vk_vertex_pending_copy(&pending, destination, source, true, &result));
    assert(memcmp(destination, source, capacity) == 0);
    assert(result.copied_bytes == 8192 && result.bounding_bytes == capacity);
    printf("Generated distant-page case: copied=%zu bounding=%zu runs=%zu\n",
           result.copied_bytes, result.bounding_bytes, result.runs);
    free(flags);
    free(source);
    free(destination);
}

int main(void)
{
    old_three_slot_regression();
    boundaries_and_sparse_gaps();
    for (unsigned slots = 1; slots <= 3; slots++) {
        generated_history(slots, false);
        generated_history(slots, true);
    }
    distant_pages_64mib();
    puts("PASS three-slot regression, 12000 mirror rotations, tail/bounds and sparse gaps");
    return 0;
}
