/* Differential byte equality, including inaccessible allocation tails. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "hw/xbox/nv2a/pgraph/vk/uniform-value-compare.h"

static unsigned checks;

static void compare(const void *a, const void *b, size_t size)
{
    bool reference = size == 0 || !memcmp(a, b, size);
    assert(nv2a_vk_uniform_values_equal(a, b, size) == reference);
    checks++;
}

int main(void)
{
    uint8_t a[96], b[96];
    for (unsigned alignment_a = 0; alignment_a < 32; alignment_a++) {
        for (unsigned alignment_b = 0; alignment_b < 32; alignment_b++) {
            for (size_t size = 0; size <= 32; size++) {
                for (unsigned i = 0; i < size; i++) {
                    a[alignment_a + i] = (uint8_t)(i * 73 + size);
                }
                memcpy(b + alignment_b, a + alignment_a, size);
                compare(a + alignment_a, b + alignment_b, size);
                for (unsigned i = 0; i < size; i++) {
                    for (unsigned bit = 0; bit < 8; bit++) {
                        b[alignment_b + i] ^= 1u << bit;
                        compare(a + alignment_a, b + alignment_b, size);
                        b[alignment_b + i] ^= 1u << bit;
                    }
                }
            }
        }
    }

    long page = sysconf(_SC_PAGESIZE);
    assert(page > 3072);
    uint8_t *guard[2];
    for (unsigned i = 0; i < 2; i++) {
        guard[i] = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(guard[i] != MAP_FAILED);
        assert(!mprotect(guard[i] + page, page, PROT_NONE));
    }
    const size_t lengths[] = {0, 1, 3, 4, 7, 8, 11, 12, 15, 16, 17, 32,
                              64, 256, 3072};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size_t size = lengths[i];
        uint8_t *x = guard[0] + page - size;
        uint8_t *y = guard[1] + page - size;
        memset(x, 0x80, size);
        memset(y, 0x80, size);
        assert(!mprotect(guard[0], page, PROT_READ));
        compare(x, y, size);
        for (size_t changed = 0; changed < size; changed++) {
            y[changed] ^= 1;
            compare(x, y, size);
            y[changed] ^= 1;
        }
        assert(!mprotect(guard[0], page, PROT_READ | PROT_WRITE));
    }
    compare(NULL, NULL, 0);
    for (unsigned i = 0; i < 2; i++) {
        assert(!munmap(guard[i], page * 2));
    }
    printf("PASS uniform raw-value equality: %u comparisons; all byte/bit "
           "changes, independent unaligned pointers, protected tails and "
           "read-only source; no game FPS claim\n", checks);
    return 0;
}
