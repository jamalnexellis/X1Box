/* Keep shared-work ordering while allowing independent slot uploads first. */
#ifndef NV2A_VK_DRAW_SUBMIT_H
#define NV2A_VK_DRAW_SUBMIT_H
#include <stdbool.h>
#include <vulkan/vulkan.h>

typedef struct NV2AVkDrawSubmit {
    VkSubmitInfo infos[2];
    VkCommandBuffer commands[2];
    VkSemaphore aux_wait;
    VkPipelineStageFlags aux_stage;
    VkSemaphore main_waits[2];
    VkPipelineStageFlags main_stages[2];
    VkSemaphore main_signal;
} NV2AVkDrawSubmit;

static inline void nv2a_vk_draw_submit_init(NV2AVkDrawSubmit *s,
    VkCommandBuffer aux, VkCommandBuffer main, VkSemaphore aux_done,
    VkSemaphore chain, bool chain_wait, bool chain_signal, bool independent_aux)
{
    *s = (NV2AVkDrawSubmit){0};
    s->commands[0] = aux; s->commands[1] = main;
    s->aux_wait = chain; s->aux_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s->main_waits[0] = aux_done; s->main_waits[1] = chain;
    s->main_stages[0] = s->main_stages[1] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s->main_signal = chain;
    s->infos[0] = (VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = chain_wait && !independent_aux ? 1 : 0,
        .pWaitSemaphores = &s->aux_wait, .pWaitDstStageMask = &s->aux_stage,
        .commandBufferCount = 1, .pCommandBuffers = &s->commands[0],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s->main_waits[0],
    };
    s->infos[1] = (VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = chain_wait && independent_aux ? 2 : 1,
        .pWaitSemaphores = s->main_waits, .pWaitDstStageMask = s->main_stages,
        .commandBufferCount = 1, .pCommandBuffers = &s->commands[1],
        .signalSemaphoreCount = chain_signal ? 1 : 0,
        .pSignalSemaphores = &s->main_signal,
    };
}
#endif
