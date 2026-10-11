/* Differentially exercise production assembly with scalar/batched emitters. */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/prim_rewrite.h"

void reference_prim_rewrite_init(PrimRewriteBuf *buf);
void reference_prim_rewrite_finalize(PrimRewriteBuf *buf);
PrimRewrite reference_prim_rewrite_indexed(PrimRewriteBuf *buf,
    PrimAssemblyState mode, const uint32_t *indices, unsigned int count);
PrimRewrite reference_prim_rewrite_ranges(PrimRewriteBuf *buf,
    PrimAssemblyState mode, const int32_t *starts, const int32_t *counts,
    unsigned int num_ranges);

static uint32_t random_state = 0x362728u;
static uint32_t next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void same(PrimRewrite a, PrimRewrite b)
{
    assert(a.num_indices == b.num_indices);
    assert(!!a.indices == !!b.indices);
    if (a.num_indices) {
        assert(memcmp(a.indices, b.indices,
                      (size_t)a.num_indices * sizeof(uint32_t)) == 0);
    }
}

static int64_t host_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void)
{
    PrimRewriteBuf scalar, batched;
    reference_prim_rewrite_init(&scalar);
    pgraph_prim_rewrite_init(&batched);
    uint32_t input[8193];
    unsigned int checks = 0;
    for (int primitive = PRIM_TYPE_POINTS; primitive <= PRIM_TYPE_POLYGON;
         primitive++) {
        for (int polygon = POLY_MODE_FILL; polygon <= POLY_MODE_LINE;
             polygon++) {
            for (unsigned int flags = 0; flags < 4; flags++) {
                PrimAssemblyState mode = {
                    .primitive_mode = primitive, .polygon_mode = polygon,
                    .last_provoking = !!(flags & 1),
                    .flat_shading = !!(flags & 2),
                };
                for (unsigned int trial = 0; trial < 136; trial++) {
                    unsigned int n = trial < 130 ? trial : 8193 - trial % 7;
                    for (unsigned int i = 0; i < n; i++) {
                        /* Duplicated indices, full-width values, degenerate
                         * primitives and incomplete tails are intentional. */
                        input[i] = i % 4 == 0 ? UINT32_MAX : next_random() % 23;
                    }
                    same(reference_prim_rewrite_indexed(&scalar, mode, input, n),
                         pgraph_prim_rewrite_indexed(&batched, mode, input, n));
                    int32_t starts[] = { 0, 7, 1000000, INT32_MAX - 8193 };
                    int32_t counts[] = { (int32_t)n, -1, (int32_t)(n / 2), 0 };
                    same(reference_prim_rewrite_ranges(&scalar, mode, starts,
                                                      counts, 4),
                         pgraph_prim_rewrite_ranges(&batched, mode, starts,
                                                   counts, 4));
                    counts[0] = 0;
                    counts[1] = n;
                    counts[2] = 0;
                    counts[3] = n;
                    same(reference_prim_rewrite_ranges(&scalar, mode, starts,
                                                      counts, 4),
                         pgraph_prim_rewrite_ranges(&batched, mode, starts,
                                                   counts, 4));
                    checks += 3;
                }
            }
        }
    }
    assert(checks == 48960);
    printf("Primitive index sequence checks passed: %u cases\n", checks);
    /* Timings are isolated assembly work, not a game FPS measurement. */
    PrimAssemblyState strip = { .primitive_mode = PRIM_TYPE_TRIANGLE_STRIP,
        .polygon_mode = POLY_MODE_FILL, .last_provoking = true,
        .flat_shading = true };
    uint64_t checksum = 0;
    for (unsigned int trial = 0; trial < 6; trial++) {
        bool old = trial == 0 || trial == 3 || trial == 4;
        int64_t start = host_ns();
        for (unsigned int i = 0; i < 300; i++) {
            PrimRewrite result = old ?
                reference_prim_rewrite_indexed(&scalar, strip, input, 8193) :
                pgraph_prim_rewrite_indexed(&batched, strip, input, 8193);
            checksum += result.indices[i % result.num_indices];
        }
        printf("Primitive assembly benchmark scalar=%d us=%" PRId64 "\n",
               old, (host_ns() - start) / 1000);
    }
    printf("Primitive assembly checksum=%" PRIu64 "\n", checksum);
    reference_prim_rewrite_finalize(&scalar);
    pgraph_prim_rewrite_finalize(&batched);
    return 0;
}
