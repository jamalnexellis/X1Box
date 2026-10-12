/* Replay complete, previously successful pipelines into the Vulkan cache.
 * Replay objects are discarded; guest draws always use the ordinary lookup. */
#include "renderer.h"
#include "ui/xemu-settings.h"
#include "qemu/fast-hash.h"
#include <glib/gstdio.h>
#include "pipeline-recipe-file.h"

extern bool xemu_get_async_compile(void);

typedef NV2AVkRecipeHeader RecipeHeader;
typedef NV2AVkRecipeRecord RecipeRecord;

typedef struct NV2AVkPipelineRecipeCache {
    RecipeHeader header;
    GArray *records;
    char *path, *shader_dir;
    bool writable;
} NV2AVkPipelineRecipeCache;

typedef struct RecipeModule {
    VkShaderModule module;
    uint32_t stage;
} RecipeModule;

static void digest(const void *data, size_t size, uint8_t out[32])
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, data, size);
    gsize length = 32;
    g_checksum_get_digest(sum, out, &length);
    g_checksum_free(sum);
}

static char *shader_path(const NV2AVkPipelineRecipeCache *cache,
                         const uint8_t id[32])
{
    char hex[65];
    for (unsigned i = 0; i < 32; i++) g_snprintf(hex + i * 2, 3, "%02x", id[i]);
    return g_strdup_printf("%s/%s.spv", cache->shader_dir, hex);
}

static bool read_bounded(const char *path, size_t maximum, gchar **data, gsize *size)
{
    GStatBuf stat;
    *data = NULL; *size = 0;
    if (g_lstat(path, &stat) || !S_ISREG(stat.st_mode) || stat.st_size <= 0 ||
        (uint64_t)stat.st_size > maximum) return false;
    return g_file_get_contents(path, data, size, NULL) && *size <= maximum;
}

static void discard_shader_files(NV2AVkPipelineRecipeCache *cache)
{
    GDir *dir = g_dir_open(cache->shader_dir, 0, NULL);
    if (!dir) return;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (strlen(name) != 68 || strcmp(name + 64, ".spv")) continue;
        bool ours = true;
        for (unsigned i = 0; i < 64; i++) ours &= (name[i] >= '0' && name[i] <= '9') ||
                                                (name[i] >= 'a' && name[i] <= 'f');
        if (ours) {
            char *path = g_build_filename(cache->shader_dir, name, NULL);
            g_unlink(path); /* Do not follow symlinks or recurse into directories. */
            g_free(path);
        }
    }
    g_dir_close(dir);
}

static bool make_header(PGRAPHVkState *r, RecipeHeader *header)
{
    memset(header, 0, sizeof(*header));
    header->magic = NV2A_VK_RECIPE_MAGIC; header->abi = NV2A_VK_RECIPE_ABI;
    header->record_size = sizeof(RecipeRecord);
    header->caps[0] = r->dynamic_vertex_input_enabled;
    header->caps[1] = r->unoptimized_pipeline_compile;
    header->caps[2] = r->extended_dynamic_state_supported;
    header->caps[3] = r->push_descriptors_supported;
    header->caps[4] = r->use_push_constants_for_uniform_attrs;
    header->caps[5] = OPT_DYNAMIC_STATES;
    header->caps[6] = OPT_DYNAMIC_BLEND;
    header->caps[7] = OPT_BINDLESS_TEXTURES;
#if OPT_DYNAMIC_BLEND
    header->caps[8] = r->eds3_blend_supported;
#endif
#if OPT_BINDLESS_TEXTURES
    header->caps[9] = r->bindless_textures_supported;
    header->caps[10] = r->tex_push_offset;
#endif
    header->caps[11] = r->device_props.apiVersion;
    digest(&r->enabled_physical_device_features,
           sizeof(r->enabled_physical_device_features), header->feature_digest);
    char *path = g_strdup_printf("%sgpu_driver_id.bin", xemu_settings_get_base_path());
    gchar *data = NULL; gsize size = 0;
    bool ok = read_bounded(path, 32, &data, &size) && size == 32;
    if (ok) digest(data, size, header->driver_digest);
    g_free(data); g_free(path);
    return ok;
}

static bool rewrite_archive(NV2AVkPipelineRecipeCache *cache)
{
    GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, (const uint8_t *)&cache->header, sizeof(cache->header));
    g_byte_array_append(bytes, (const uint8_t *)cache->records->data,
                        cache->records->len * sizeof(RecipeRecord));
    bool ok = g_file_set_contents(cache->path, (const char *)bytes->data, bytes->len, NULL);
    g_byte_array_unref(bytes);
    return ok;
}

static bool supported_recipe(PGRAPHVkState *r, const NV2AVkPipelineRecipe *recipe)
{
    if (!nv2a_vk_recipe_valid(recipe, &r->device_props.limits) ||
        recipe->color_format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK ||
        recipe->depth_format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return false;
    uint32_t sets = 1;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) sets = 2;
    else
#endif
    if (r->push_descriptors_supported) sets = 2;
    if (recipe->set_count != sets || recipe->dynamic_input != r->dynamic_vertex_input_enabled ||
        recipe->flags != nv2a_vk_draw_pipeline_flags(r->unoptimized_pipeline_compile)) return false;
    if (recipe->stage_count == 3 && !r->enabled_physical_device_features.geometryShader) return false;
    if (recipe->depth_clamp && !r->enabled_physical_device_features.depthClamp) return false;
    if (recipe->polygon && !r->enabled_physical_device_features.fillModeNonSolid) return false;
    if (recipe->has_color || recipe->has_depth) {
        VkFormatProperties properties;
        if (recipe->has_color) {
            vkGetPhysicalDeviceFormatProperties(r->physical_device, recipe->color_format, &properties);
            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
        }
        if (recipe->has_depth) {
            vkGetPhysicalDeviceFormatProperties(r->physical_device, recipe->depth_format, &properties);
            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return false;
        }
    }
    for (uint32_t i = 0; i < recipe->input.attribute_count; i++) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(r->physical_device, recipe->input.attributes[i].format, &properties);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) return false;
    }
    return true;
}

static VkShaderModule load_module(PGRAPHVkState *r, NV2AVkPipelineRecipeCache *cache,
    GHashTable *modules, const uint8_t id[32], uint32_t stage)
{
    char *path = shader_path(cache, id);
    RecipeModule *known = g_hash_table_lookup(modules, path);
    if (known) { g_free(path); return known->stage == stage ? known->module : VK_NULL_HANDLE; }
    RecipeModule *entry = g_new0(RecipeModule, 1);
    entry->stage = stage;
    gchar *data = NULL; gsize size = 0; uint8_t actual[32];
    if (read_bounded(path, NV2A_VK_RECIPE_SHADER_MAX, &data, &size)) {
        digest(data, size, actual);
        if (!memcmp(id, actual, 32) && nv2a_vk_recipe_spirv_valid((const uint32_t *)data, size, stage)) {
            VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = size, .pCode = (const uint32_t *)data};
            if (vkCreateShaderModule(r->device, &info, NULL, &entry->module) != VK_SUCCESS)
                entry->module = VK_NULL_HANDLE;
        }
    }
    g_free(data);
    g_hash_table_insert(modules, path, entry);
    return entry->module;
}

static bool replay(PGRAPHVkState *r, NV2AVkPipelineRecipeCache *cache,
                    GHashTable *modules, const NV2AVkPipelineRecipe *recipe)
{
    if (!supported_recipe(r, recipe)) return false;
    VkShaderModule shaders[3] = {0};
    for (uint32_t i = 0; i < recipe->stage_count; i++) {
        shaders[i] = load_module(r, cache, modules, recipe->shader_ids[i], recipe->stages[i]);
        if (!shaders[i]) return false;
    }
    VkDescriptorSetLayout sets[2];
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        sets[0] = r->bindless_set_layout; sets[1] = r->ubo_set_layout;
    } else
#endif
    if (r->push_descriptors_supported) {
        sets[0] = r->push_tex_set_layout; sets[1] = r->push_ubo_set_layout;
    } else sets[0] = r->descriptor_set_layout;
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = recipe->set_count, .pSetLayouts = sets,
        .pushConstantRangeCount = recipe->push_count, .pPushConstantRanges = recipe->push_ranges};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(r->device, &layout_info, NULL, &layout) != VK_SUCCESS) return false;
    VkRenderPass pass = pgraph_vk_recipe_render_pass(r, recipe->color_format, recipe->depth_format);
    if (!pass) { vkDestroyPipelineLayout(r->device, layout, NULL); return false; }
    NV2AVkPipelineReplayInfo info;
    nv2a_vk_recipe_rebuild(recipe, shaders, layout, pass, &info);
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1, &info.pipeline, NULL, &pipeline);
    if (pipeline) vkDestroyPipeline(r->device, pipeline, NULL);
    vkDestroyPipelineLayout(r->device, layout, NULL);
    return result == VK_SUCCESS;
}

void pgraph_vk_prewarm_pipeline_recipes(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    unsigned loaded = 0, warmed = 0, failed = 0;
    int64_t start = g_get_monotonic_time();
    if (!r->pipeline_recipe_mode || !g_config.perf.cache_shaders) goto report;
#if OPT_ASYNC_COMPILE
    if (xemu_get_async_compile()) goto report; /* Do not compete with pending async startup jobs. */
#endif
    NV2AVkPipelineRecipeCache *cache = g_new0(NV2AVkPipelineRecipeCache, 1);
    if (!make_header(r, &cache->header)) { g_free(cache); goto report; }
    cache->path = g_strdup_printf("%svk_pipeline_recipes.bin", xemu_settings_get_base_path());
    cache->shader_dir = g_strdup_printf("%spipeline_spv", xemu_settings_get_base_path());
    cache->records = g_array_new(FALSE, FALSE, sizeof(RecipeRecord));
    gchar *data = NULL; gsize size = 0;
    size_t maximum = sizeof(RecipeHeader) + NV2A_VK_RECIPE_MAX * sizeof(RecipeRecord);
    if (read_bounded(cache->path, maximum, &data, &size) &&
        nv2a_vk_recipe_file_valid(data, size, &cache->header)) {
        size_t count = (size - sizeof(RecipeHeader)) / sizeof(RecipeRecord);
        for (size_t i = 0; i < count; i++) {
            RecipeRecord record;
            if (nv2a_vk_recipe_file_record(data, size, &cache->header, i, digest, &record) &&
                supported_recipe(r, &record.recipe))
                g_array_append_val(cache->records, record);
            else failed++;
        }
    } else discard_shader_files(cache);
    g_free(data);
    loaded = cache->records->len;
    /* Recover only complete, checked records after an interrupted append. */
    cache->writable = !g_mkdir_with_parents(cache->shader_dir, 0700) && rewrite_archive(cache);
    r->pipeline_recipe_cache = cache;
    if (r->pipeline_recipe_mode == 1) {
        GHashTable *modules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        for (unsigned i = 0; i < cache->records->len; i++) {
            RecipeRecord *record = &g_array_index(cache->records, RecipeRecord, i);
            if (replay(r, cache, modules, &record->recipe)) warmed++;
            else failed++;
        }
        GHashTableIter iterator; gpointer value;
        g_hash_table_iter_init(&iterator, modules);
        while (g_hash_table_iter_next(&iterator, NULL, &value)) {
            RecipeModule *module = value;
            if (module->module) vkDestroyShaderModule(r->device, module->module, NULL);
        }
        g_hash_table_destroy(modules);
    }
report:
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-pipeline-recipe",
        "mode=%u loaded=%u warmed=%u failed=%u us=%" PRId64,
        r->pipeline_recipe_mode, loaded, warmed, failed, g_get_monotonic_time() - start);
#else
    fprintf(stderr, "Pipeline recipes mode=%u loaded=%u warmed=%u failed=%u us=%" PRId64 "\n",
        r->pipeline_recipe_mode, loaded, warmed, failed, g_get_monotonic_time() - start);
#endif
}

void pgraph_vk_capture_pipeline_recipe(PGRAPHVkState *r,
    const VkGraphicsPipelineCreateInfo *info,
    const VkPipelineLayoutCreateInfo *layout, const RenderPassState *pass,
    const ShaderBinding *shader)
{
    NV2AVkPipelineRecipeCache *cache = r->pipeline_recipe_cache;
    if (!cache || !cache->writable || cache->records->len >= NV2A_VK_RECIPE_MAX) return;
    int64_t start = nv2a_stall_start();
    RecipeRecord record = {0};
    if (!nv2a_vk_recipe_capture(&record.recipe, info, layout, pass->color_format, pass->zeta_format)) return;
    const ShaderModuleInfo *modules[3];
    for (uint32_t i = 0; i < record.recipe.stage_count; i++) {
        uint32_t stage = record.recipe.stages[i];
        modules[i] = stage == VK_SHADER_STAGE_VERTEX_BIT ? shader->vsh.module_info :
                     stage == VK_SHADER_STAGE_GEOMETRY_BIT ? shader->geom.module_info : shader->psh.module_info;
        if (!modules[i] || !modules[i]->spirv) return;
        digest(modules[i]->spirv->data, modules[i]->spirv->len, record.recipe.shader_ids[i]);
    }
    if (!supported_recipe(r, &record.recipe)) return;
    digest(&record.recipe, sizeof(record.recipe), record.digest);
    for (unsigned i = 0; i < cache->records->len; i++) {
        RecipeRecord *previous = &g_array_index(cache->records, RecipeRecord, i);
        if (!memcmp(record.digest, previous->digest, 32) &&
            !memcmp(&record.recipe, &previous->recipe, sizeof(record.recipe))) return;
    }
    for (uint32_t i = 0; i < record.recipe.stage_count; i++) {
        char *path = shader_path(cache, record.recipe.shader_ids[i]);
        /* IDs are SHA-256 of complete SPIR-V, not filenames from guest state. */
        gchar *existing = NULL; gsize size = 0; uint8_t actual[32];
        bool valid = read_bounded(path, NV2A_VK_RECIPE_SHADER_MAX, &existing, &size);
        if (valid) {
            digest(existing, size, actual);
            valid = !memcmp(actual, record.recipe.shader_ids[i], 32);
        }
        g_free(existing);
        if (!valid &&
            !g_file_set_contents(path, (const char *)modules[i]->spirv->data, modules[i]->spirv->len, NULL)) {
            cache->writable = false; g_free(path); return;
        }
        g_free(path);
    }
    FILE *file = fopen(cache->path, "ab");
    bool written = false;
    if (file) {
        written = fwrite(&record, sizeof(record), 1, file) == 1;
        if (fclose(file)) written = false;
    }
    if (written) g_array_append_val(cache->records, record);
    else cache->writable = false;
    nv2a_stall_record(NV2A_STALL_PIPELINE_SAVE, start, sizeof(record));
}

void pgraph_vk_finalize_pipeline_recipes(PGRAPHVkState *r)
{
    NV2AVkPipelineRecipeCache *cache = r->pipeline_recipe_cache;
    if (!cache) return;
    g_array_free(cache->records, TRUE);
    g_free(cache->path); g_free(cache->shader_dir); g_free(cache);
    r->pipeline_recipe_cache = NULL;
}
