/* Exact RGB565 pixels for the production GPU copy and repeated destination
 * writes across queued command buffers. Uses generated patterns only. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/cubemap-copy-vk.h"
#include "hw/xbox/nv2a/pgraph/vk/surface-copy-vk.h"
#include "hw/xbox/nv2a/pgraph/vk/depth-copy-vk.h"
#include "hw/xbox/nv2a/pgraph/vk/depth-stencil-copy-vk.h"
#include "hw/xbox/nv2a/pgraph/vk/depth-copy.h"
#include "hw/xbox/nv2a/pgraph/vk/surface-depth-view-vk.h"

#define CHECK(call) do { VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) { \
        fprintf(stderr, "%s: %d\n", #call, result_); abort(); \
    } } while (0)
static VkPhysicalDevice physical;
static VkDevice device;
static unsigned validation_errors;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)severity; (void)type; (void)user;
    fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    validation_errors++;
    return VK_FALSE;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    abort();
}

static VkDeviceMemory make_buffer(VkDeviceSize size, VkBuffer *buffer)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(device, &info, NULL, buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &memory));
    CHECK(vkBindBufferMemory(device, *buffer, memory, 0));
    return memory;
}

static VkDeviceMemory make_image(uint32_t width, uint32_t height,
                                uint32_t layers, VkFormat format, VkImage *image)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | ((format == VK_FORMAT_D16_UNORM ||
                    format == VK_FORMAT_D24_UNORM_S8_UINT ||
                    format == VK_FORMAT_D32_SFLOAT_S8_UINT) ?
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateImage(device, &info, NULL, image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *image, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, 0),
    };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &memory));
    CHECK(vkBindImageMemory(device, *image, memory, 0));
    return memory;
}

static void transition_aspect(VkCommandBuffer cmd, VkImage image, uint32_t layers,
    VkImageLayout old_layout, VkImageLayout new_layout, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { aspect, 0, 1, 0, layers },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void transition(VkCommandBuffer cmd, VkImage image, uint32_t layers,
                       VkImageLayout old_layout, VkImageLayout new_layout)
{
    transition_aspect(cmd, image, layers, old_layout, new_layout, VK_IMAGE_ASPECT_COLOR_BIT);
}

static void test_cube(VkQueue queue, VkCommandPool pool, uint32_t width)
{
    size_t face_bytes = (size_t)width * width * 2, cube_bytes = face_bytes * 6;
    VkBuffer input, output;
    VkDeviceMemory input_memory = make_buffer(cube_bytes + face_bytes, &input);
    VkDeviceMemory output_memory = make_buffer(cube_bytes * 2, &output);
    unsigned char *input_data, *output_data;
    CHECK(vkMapMemory(device, input_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&input_data));
    CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&output_data));
    for (size_t i = 0; i < cube_bytes + face_bytes; i++) {
        input_data[i] = (unsigned char)(i * 73 + (i >> 8) * 19 + (i / face_bytes) * 41);
    }
    memset(output_data, 0xa5, cube_bytes * 2);
    VkImage sources[6], cube;
    VkImageLayout layouts[6] = {
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDeviceMemory source_memory[6], cube_memory;
    for (unsigned i = 0; i < 6; i++) {
        source_memory[i] = make_image(width, width, 1, VK_FORMAT_R5G6B5_UNORM_PACK16,
                                      &sources[i]);
    }
    cube_memory = make_image(width, width, 6, VK_FORMAT_R5G6B5_UNORM_PACK16, &cube);
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    VkCommandBuffer cmds[2];
    CHECK(vkAllocateCommandBuffers(device, &alloc, cmds));
    for (unsigned pass = 0; pass < 2; pass++) {
        VkCommandBuffer cmd = cmds[pass];
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        if (!pass) {
            VkBufferMemoryBarrier host = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = input, .size = VK_WHOLE_SIZE,
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        }
        /* The second queued buffer changes only face five. The first result
         * must retain its old pixels; the second must contain the update. */
        for (unsigned i = pass ? 5 : 0; i < 6; i++) {
            transition(cmd, sources[i], 1, pass ? layouts[i] :
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region = {
                .bufferOffset = pass ? cube_bytes : face_bytes * i,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { width, width, 1 },
            };
            vkCmdCopyBufferToImage(cmd, input, sources[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(cmd, sources[i], 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       layouts[i]);
        }
        vk_cube_copy_record(cmd, sources, layouts, cube,
            pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                   VK_IMAGE_LAYOUT_UNDEFINED, width, width);
        transition(cmd, cube, 6, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy regions[6];
        for (unsigned i = 0; i < 6; i++) {
            regions[i] = (VkBufferImageCopy) {
                .bufferOffset = cube_bytes * pass + face_bytes * i,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1 },
                .imageExtent = { width, width, 1 },
            };
        }
        vkCmdCopyImageToBuffer(cmd, cube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              output, 6, regions);
        transition(cmd, cube, 6, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = output, .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
    }
    /* Submit separately without waiting between frames. */
    for (unsigned i = 0; i < 2; i++) {
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmds[i],
        };
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    }
    CHECK(vkQueueWaitIdle(queue));
    assert(memcmp(output_data, input_data, cube_bytes) == 0);
    assert(memcmp(output_data + cube_bytes, input_data, face_bytes * 5) == 0);
    assert(memcmp(output_data + cube_bytes + face_bytes * 5,
                  input_data + cube_bytes, face_bytes) == 0);
    vkFreeCommandBuffers(device, pool, 2, cmds);
    for (unsigned i = 0; i < 6; i++) {
        vkDestroyImage(device, sources[i], NULL);
        vkFreeMemory(device, source_memory[i], NULL);
    }
    vkDestroyImage(device, cube, NULL);
    vkFreeMemory(device, cube_memory, NULL);
    vkUnmapMemory(device, input_memory);
    vkUnmapMemory(device, output_memory);
    vkDestroyBuffer(device, input, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, input_memory, NULL);
    vkFreeMemory(device, output_memory, NULL);
    printf("PASS RGB565 %ux%u six faces, queued replacement of face five\n", width, width);
}

static void test_surface(VkQueue queue, VkCommandPool pool, uint32_t width,
                         uint32_t height, VkFormat source_format,
                         VkFormat destination_format, unsigned bytes_per_pixel)
{
    bool depth = source_format == VK_FORMAT_D16_UNORM;
    assert(depth ? destination_format == VK_FORMAT_R16_UNORM :
           vk_surface_copy_formats_compatible(source_format, destination_format));
    const unsigned passes = 3;
    size_t bytes = (size_t)width * height * bytes_per_pixel;
    VkBuffer input, output;
    VkDeviceMemory input_memory = make_buffer(bytes * passes, &input);
    VkDeviceMemory output_memory = make_buffer(bytes * passes, &output);
    VkBuffer scratch = VK_NULL_HANDLE;
    VkDeviceMemory scratch_memory = depth ? make_buffer(bytes, &scratch) : VK_NULL_HANDLE;
    unsigned char *input_data, *output_data;
    CHECK(vkMapMemory(device, input_memory, 0, VK_WHOLE_SIZE, 0, (void **)&input_data));
    CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0, (void **)&output_data));
    for (size_t i = 0; i < bytes * passes; i++) {
        input_data[i] = (unsigned char)(i * 73 + (i >> 8) * 19 + (i / bytes) * 41);
    }
    memset(output_data, 0xa5, bytes * passes);
    VkImage source, destination;
    VkDeviceMemory source_memory = make_image(width, height, 1, source_format, &source);
    VkDeviceMemory destination_memory = make_image(width, height, 1, destination_format, &destination);
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = passes,
    };
    VkCommandBuffer cmds[3];
    CHECK(vkAllocateCommandBuffers(device, &alloc, cmds));
    VkImageLayout layouts[3] = { VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    if (depth) {
        layouts[1] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        layouts[2] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    for (unsigned pass = 0; pass < passes; pass++) {
        VkCommandBuffer cmd = cmds[pass];
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = input, .offset = bytes * pass, .size = bytes,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 1, &host, 0, NULL);
        transition_aspect(cmd, source, 1, pass ? layouts[pass - 1] : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region = {
            .bufferOffset = bytes * pass,
            .imageSubresource = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { width, height, 1 },
        };
        vkCmdCopyBufferToImage(cmd, input, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition_aspect(cmd, source, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            layouts[pass], depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
        if (depth) {
            vk_depth_copy_record(cmd, source, layouts[pass], destination,
                pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                scratch, width, height);
        } else {
            vk_surface_copy_record(cmd, source, layouts[pass], destination,
                pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                width, height);
        }
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        transition(cmd, destination, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              output, 1, &region);
        transition(cmd, destination, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkBufferMemoryBarrier readback = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = output, .offset = bytes * pass, .size = bytes,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &readback, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
    }
    /* Enqueue all versions without a host wait between them. Previous reads
     * must preserve their version when the same destination is refreshed. */
    for (unsigned pass = 0; pass < passes; pass++) {
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmds[pass] };
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    }
    CHECK(vkQueueWaitIdle(queue));
    assert(memcmp(output_data, input_data, bytes * passes) == 0);
    vkFreeCommandBuffers(device, pool, passes, cmds);
    vkDestroyImage(device, source, NULL); vkDestroyImage(device, destination, NULL);
    vkFreeMemory(device, source_memory, NULL); vkFreeMemory(device, destination_memory, NULL);
    vkUnmapMemory(device, input_memory); vkUnmapMemory(device, output_memory);
    vkDestroyBuffer(device, input, NULL); vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, input_memory, NULL); vkFreeMemory(device, output_memory, NULL);
    if (depth) {
        vkDestroyBuffer(device, scratch, NULL); vkFreeMemory(device, scratch_memory, NULL);
    }
    printf("PASS %s color 2D source_fmt=%d destination_fmt=%d %ux%u, "
           "three queued versions and prior reads\n",
           depth ? "D16/R16 scratch" : (source_format == destination_format ? "native" : "byte-preserving"),
           source_format, destination_format, width, height);
}

#include "test-depth-stencil-vk.h"
#include "test-vertex-upload-vk.h"

int main(int argc, char **argv)
{
    bool validate = getenv("XEMU_TEST_VALIDATE") != NULL;
    VkValidationFeatureEnableEXT feature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &feature,
    };
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME };
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = validate ? &validation : NULL,
        .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = validate ? 2 : 0, .ppEnabledExtensionNames = extensions,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&info, NULL, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                "vkCreateDebugUtilsMessengerEXT");
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        assert(create_messenger && destroy_messenger);
        VkDebugUtilsMessengerCreateInfoEXT debug = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = debug_callback,
        };
        CHECK(create_messenger(instance, &debug, NULL, &messenger));
    }
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("GPU: %s\n", props.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    assert(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i; break;
        }
    }
    free(families);
    assert(family != UINT32_MAX);
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
    };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    test_vertex_upload(queue, pool);
    test_cube(queue, pool, 16);
    test_cube(queue, pool, 256);
    test_cube(queue, pool, 512); /* 2x reflections */
    test_cube(queue, pool, 768); /* 3x, non-power-of-two host allocation */
    test_surface(queue, pool, 16, 16, VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM, 2);
    test_surface(queue, pool, 1024, 1024, VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM, 2);
    test_surface(queue, pool, 3072, 3072, VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM, 2);
    test_surface(queue, pool, 256, 256, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1);
    test_surface(queue, pool, 768, 768, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1);
    assert(!vk_surface_copy_formats_compatible(VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM));
    VkFormat formats[] = { VK_FORMAT_R5G6B5_UNORM_PACK16,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 3; j++) {
            assert(vk_surface_copy_formats_compatible(formats[i], formats[j]) ==
                   (i == j || (i != 0 && j != 0)));
        }
    }
    VkFormat rejected[] = { VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8_UNORM,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_D32_SFLOAT };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        for (unsigned j = 0; j < 3; j++) {
            assert(!vk_surface_copy_formats_compatible(rejected[i], formats[j]));
            assert(!vk_surface_copy_formats_compatible(formats[j], rejected[i]));
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        unsigned bytes = i ? 4 : 2;
        test_surface(queue, pool, 16, 16, formats[i], formats[i], bytes);
        test_surface(queue, pool, 640, 480, formats[i], formats[i], bytes);
        test_surface(queue, pool, 1920, 1440, formats[i], formats[i], bytes);
    }
    for (unsigned i = 1; i < 3; i++) {
        VkFormat destination = formats[i == 1 ? 2 : 1];
        test_surface(queue, pool, 16, 16, formats[i], destination, 4);
        test_surface(queue, pool, 640, 480, formats[i], destination, 4);
        test_surface(queue, pool, 1280, 480, formats[i], destination, 4);
        test_surface(queue, pool, 2560, 960, formats[i], destination, 4);
        test_surface(queue, pool, 3840, 1440, formats[i], destination, 4);
    }
    if (argc >= 2) {
        test_stencil(queue, pool, argv[1], 16, 16, VK_FORMAT_R8G8B8A8_UNORM);
        test_stencil(queue, pool, argv[1], 1280, 480, VK_FORMAT_R8G8B8A8_UNORM);
        test_stencil(queue, pool, argv[1], 3840, 1440, VK_FORMAT_B8G8R8A8_UNORM);
        test_stencil(queue, pool, argv[1], 3072, 3072, VK_FORMAT_R8G8B8A8_UNORM);
    }
    if (argc >= 3) {
        test_depth_view(queue, pool, argv[2], VK_FORMAT_D24_UNORM_S8_UINT);
        test_depth_view(queue, pool, argv[2], VK_FORMAT_D32_SFLOAT_S8_UINT);
    }
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    if (messenger) {
        destroy_messenger(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);
    assert(validation_errors == 0);
    puts("Production cubemap and 2D surface copy pixel and synchronization fixture passed");
    return 0;
}
