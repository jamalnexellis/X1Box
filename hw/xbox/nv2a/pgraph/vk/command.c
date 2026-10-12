/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "renderer.h"

VkResult pgraph_vk_lag_wait_for_fences(PGRAPHVkState *r, uint32_t count,
    const VkFence *fences, VkBool32 wait_all, uint64_t timeout)
{
    uint64_t target = 0;
    if (count == 1) {
        for (int i = 0; i < NUM_SUBMIT_FRAMES; ++i) {
            if (fences[0] == r->frame_fences[i]) { target = r->lag_cb_ids[i]; }
        }
        for (int i = 0; i < NUM_DISPLAY_IMAGES; ++i) {
            if (fences[0] == r->display.present_fences[i]) {
                target = r->display.lag_present_ids[i];
            }
        }
        if (fences[0] == r->aux_fence) { target = r->lag_aux_id; }
    }
    int64_t start = nv2a_lag_start();
    VkResult result = vkWaitForFences(r->device, count, fences, wait_all, timeout);
    nv2a_lag_event(NV2A_LAG_FENCE, start, nv2a_lag_current(), target,
        count, (uint64_t)(int64_t)result, wait_all);
    return result;
}


static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    extern int xemu_get_submit_frames(void);
    r->current_frame = 0;
    r->num_active_frames = xemu_get_submit_frames();
    r->command_buffer = r->command_buffers[0];
    r->aux_command_buffer = r->command_buffers[1];
    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        r->frame_submitted[i] = false;
        r->deferred_framebuffer_count[i] = 0;
    }
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_LOG("begin_single_time_commands");

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_LOG("end_single_time_commands: submit + wait");

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    pgraph_vk_render_thread_wait_idle(r);

    vkResetFences(r->device, 1, &r->aux_fence);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    r->lag_aux_id = nv2a_lag_next_id();
    int64_t lag_submit = nv2a_lag_start();
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, r->aux_fence));
    nv2a_lag_event(NV2A_LAG_SUBMIT, lag_submit, r->lag_aux_id,
                    nv2a_lag_current(), 101, 0, 0);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    VK_CHECK(pgraph_vk_wait_for_fences(r, 1, &r->aux_fence, VK_TRUE,
                             UINT64_MAX));

    VK_LOG("end_single_time_commands: done");
    r->in_aux_command_buffer = false;
}

VkCommandBuffer pgraph_vk_ensure_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_aux_command_buffer) {
        VK_LOG("ensure_nondraw_commands: begin aux CB");
        r->in_aux_command_buffer = true;
        VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));
        if (!r->lag_batch_id) { r->lag_batch_id = nv2a_lag_next_id(); }
        nv2a_lag_context(r->lag_batch_id);
        nv2a_lag_event(NV2A_LAG_CB_BEGIN, nv2a_lag_start(),
                        r->lag_batch_id, 0, r->current_frame, 1, 0);
        if (r->lag_aux_ts_pool) {
            vkCmdResetQueryPool(r->aux_command_buffer, r->lag_aux_ts_pool,
                               r->current_frame * 2, 2);
            vkCmdWriteTimestamp(r->aux_command_buffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                r->lag_aux_ts_pool, r->current_frame * 2);
            r->lag_aux_ts_recorded[r->current_frame] = true;
        }
    }

    return r->aux_command_buffer;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    create_command_pool(pg);
    create_command_buffers(pg);
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}