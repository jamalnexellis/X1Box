/* Opt-in host timing for first-use graphics stalls. No guest data is logged. */
#ifndef NV2A_STALL_PROFILE_H
#define NV2A_STALL_PROFILE_H

#include "qemu/timer.h"

typedef enum NV2AStallPhase {
    NV2A_STALL_TEXTURE_HASH,
    NV2A_STALL_TEXTURE_DECODE,
    NV2A_STALL_TEXTURE_STAGE,
    NV2A_STALL_TEXTURE_RECORD,
    NV2A_STALL_TEXTURE_IMAGE,
    NV2A_STALL_TEXTURE_REUSE_WAIT,
    NV2A_STALL_TEXTURE_CAPACITY_WAIT,
    NV2A_STALL_SHADER_LOAD,
    NV2A_STALL_SHADER_COMPILE,
    NV2A_STALL_SHADER_SAVE,
    NV2A_STALL_PIPELINE_CREATE,
    NV2A_STALL_PIPELINE_SAVE,
    NV2A_STALL_FLUSH_WAIT,
    NV2A_STALL_FINISH_SUBMIT,
    NV2A_STALL_FINISH_WAIT,
    NV2A_STALL_PHASE_COUNT
} NV2AStallPhase;

#ifdef __ANDROID__
extern bool nv2a_stall_profile_enabled;
void nv2a_stall_profile_init(void);
void nv2a_stall_record(NV2AStallPhase phase, int64_t start_us, uint64_t bytes);
void nv2a_stall_frame(int64_t now_us);
static inline int64_t nv2a_stall_start(void)
{
    return unlikely(nv2a_stall_profile_enabled) ?
        qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
}
#else
static inline void nv2a_stall_profile_init(void) { }
static inline int64_t nv2a_stall_start(void) { return 0; }
static inline void nv2a_stall_record(NV2AStallPhase phase, int64_t start_us,
                                   uint64_t bytes) { }
static inline void nv2a_stall_frame(int64_t now_us) { }
#endif

#endif
