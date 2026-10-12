/* Opt-in, bounded host dependency trace. Raw captures stay in app storage. */
#ifndef NV2A_LAG_TRACE_H
#define NV2A_LAG_TRACE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum NV2ALagKind {
    NV2A_LAG_FLIP, NV2A_LAG_CB_BEGIN, NV2A_LAG_CB_END,
    NV2A_LAG_ENQUEUE, NV2A_LAG_WORKER, NV2A_LAG_SUBMIT,
    NV2A_LAG_FENCE, NV2A_LAG_SLOT_WAIT, NV2A_LAG_ACK,
    NV2A_LAG_IDLE_WAIT, NV2A_LAG_COMPLETION_WAIT,
    NV2A_LAG_GPU_MAIN, NV2A_LAG_GPU_AUX, NV2A_LAG_GPU_QUERY,
    NV2A_LAG_CALIBRATION, NV2A_LAG_GPU_CAPS,
    NV2A_LAG_PFIFO_RUN, NV2A_LAG_PFIFO_IDLE,
    NV2A_LAG_MMIO_READ, NV2A_LAG_FENCE_WRITE,
    NV2A_LAG_VBLANK, NV2A_LAG_PRESENT, NV2A_LAG_MMIO_LOCK,
    NV2A_LAG_PIPELINE_FEEDBACK,
    NV2A_LAG_KIND_COUNT
} NV2ALagKind;

typedef enum NV2ALagCpuStage {
    NV2A_LAG_STAGE_draw_desc_set,
    NV2A_LAG_STAGE_draw_dispatch,
    NV2A_LAG_STAGE_draw_pipeline,
    NV2A_LAG_STAGE_draw_prim_rw,
    NV2A_LAG_STAGE_draw_setup,
    NV2A_LAG_STAGE_draw_vk_cmd,
    NV2A_LAG_STAGE_draw_vtx_attr,
    NV2A_LAG_STAGE_draw_vtx_sync,
    NV2A_LAG_STAGE_finish,
    NV2A_LAG_STAGE_finish_fence,
    NV2A_LAG_STAGE_finish_submit,
    NV2A_LAG_STAGE_flip_idle,
    NV2A_LAG_STAGE_pipe_bind_shd,
    NV2A_LAG_STAGE_pipe_bind_tex,
    NV2A_LAG_STAGE_pipe_lookup,
    NV2A_LAG_STAGE_shader_compile,
    NV2A_LAG_STAGE_surface_update,
    NV2A_LAG_STAGE_texture_upload,
    NV2A_LAG_STAGE_COUNT
} NV2ALagCpuStage;

#ifdef __cplusplus
extern "C" {
#endif
#ifdef __ANDROID__
extern bool nv2a_lag_enabled;
void nv2a_lag_init(void);
void nv2a_lag_poll(int64_t now_us);
uint64_t nv2a_lag_next_id(void);
uint64_t nv2a_lag_context(uint64_t id);
uint64_t nv2a_lag_current(void);
void nv2a_lag_event(NV2ALagKind kind, int64_t start_us, uint64_t id,
                    uint64_t link, uint64_t a, uint64_t b, uint64_t c);
void nv2a_lag_phase(unsigned phase, int64_t start_us, int64_t end_us,
                    uint64_t bytes);
void nv2a_lag_mmio_read(uint64_t addr, uint64_t value);
void nv2a_lag_cpu_span(NV2ALagCpuStage stage, int64_t start_us);
int64_t nv2a_lag_start(void);
#else
#define nv2a_lag_enabled false
static inline void nv2a_lag_init(void) { }
static inline void nv2a_lag_poll(int64_t now_us) { }
static inline uint64_t nv2a_lag_next_id(void) { return 0; }
static inline uint64_t nv2a_lag_context(uint64_t id) { return 0; }
static inline uint64_t nv2a_lag_current(void) { return 0; }
static inline void nv2a_lag_event(NV2ALagKind kind, int64_t start_us,
    uint64_t id, uint64_t link, uint64_t a, uint64_t b, uint64_t c) { }
static inline void nv2a_lag_phase(unsigned phase, int64_t start_us,
    int64_t end_us, uint64_t bytes) { }
static inline void nv2a_lag_mmio_read(uint64_t addr, uint64_t value) { }
static inline int64_t nv2a_lag_start(void) { return 0; }
static inline void nv2a_lag_cpu_span(NV2ALagCpuStage stage, int64_t start_us) { }
#endif
#ifdef __cplusplus
}
#endif

#endif
