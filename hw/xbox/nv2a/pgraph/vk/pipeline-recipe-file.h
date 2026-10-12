/* Bounded cache reads, including unaligned files and interrupted appends. */
#ifndef NV2A_VK_PIPELINE_RECIPE_FILE_H
#define NV2A_VK_PIPELINE_RECIPE_FILE_H
#include "pipeline-recipe.h"

#define NV2A_VK_RECIPE_MAGIC 0x50524b56u
typedef struct NV2AVkRecipeHeader {
    uint32_t magic, abi, record_size, caps[12];
    uint8_t driver_digest[32], feature_digest[32];
} NV2AVkRecipeHeader;
typedef struct NV2AVkRecipeRecord {
    NV2AVkPipelineRecipe recipe;
    uint8_t digest[32];
} NV2AVkRecipeRecord;
typedef void (*NV2AVkRecipeDigest)(const void *, size_t, uint8_t[32]);

static inline bool nv2a_vk_recipe_file_valid(const void *bytes, size_t size,
                                            const NV2AVkRecipeHeader *expected)
{
    return bytes && expected && size >= sizeof(*expected) &&
        size <= sizeof(*expected) + NV2A_VK_RECIPE_MAX * sizeof(NV2AVkRecipeRecord) &&
        !memcmp(bytes, expected, sizeof(*expected));
}

static inline bool nv2a_vk_recipe_file_record(const void *bytes, size_t size,
    const NV2AVkRecipeHeader *expected, size_t index, NV2AVkRecipeDigest digest,
    NV2AVkRecipeRecord *out)
{
    if (!digest || !out || !nv2a_vk_recipe_file_valid(bytes, size, expected) ||
        index >= (size - sizeof(*expected)) / sizeof(*out)) return false;
    memcpy(out, (const uint8_t *)bytes + sizeof(*expected) + index * sizeof(*out), sizeof(*out));
    uint8_t actual[32];
    digest(&out->recipe, sizeof(out->recipe), actual);
    return !memcmp(actual, out->digest, 32);
}
#endif
