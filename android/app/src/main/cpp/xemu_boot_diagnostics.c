/*
 * Android startup diagnostics. SPDX-License-Identifier: GPL-2.0-or-later
 * Sample registers on the vCPU thread; never read live guest CPU state from
 * the SDL or Java threads. Keep at most one queued sample if the CPU stalls.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "cpu.h"
#include <android/log.h>

extern bool xemu_android_is_debug_logging_enabled(void);

static QEMUTimer *boot_timer;
static int64_t boot_started_ms;
static bool boot_sample_pending;

static void boot_sample_cpu(CPUState *cpu, run_on_cpu_data unused)
{
    CPUX86State *env = &X86_CPU(cpu)->env;

    __android_log_print(ANDROID_LOG_INFO, "xemu-boot",
        "CPU pc=0x%08" PRIx64 " eip=0x%08" PRIx64
        " cs=0x%04x cr3=0x%08" PRIx64 " esp=0x%08" PRIx64
        " eax=0x%08" PRIx64 " halted=%d interrupt=0x%x",
        (uint64_t)(env->eip + env->segs[R_CS].base), (uint64_t)env->eip,
        env->segs[R_CS].selector, (uint64_t)env->cr[3],
        (uint64_t)env->regs[R_ESP], (uint64_t)env->regs[R_EAX],
        cpu->halted, cpu->interrupt_request);
    qatomic_set(&boot_sample_pending, false);
}

static void boot_sample_tick(void *unused)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (xemu_android_is_debug_logging_enabled() && runstate_is_running()) {
        __android_log_print(ANDROID_LOG_INFO, "xemu-boot",
            "uptime_ms=%" PRId64 " runstate=%d cpu_sample_pending=%d",
            now - boot_started_ms, runstate_get(),
            qatomic_read(&boot_sample_pending));
        if (first_cpu && !qatomic_read(&boot_sample_pending)) {
            qatomic_set(&boot_sample_pending, true);
            async_run_on_cpu(first_cpu, boot_sample_cpu, RUN_ON_CPU_NULL);
        }
    }
    timer_mod(boot_timer, now + 5000);
}

void xemu_android_boot_diagnostics_start(void)
{
    if (boot_timer || !xemu_android_is_debug_logging_enabled()) {
        return;
    }
    boot_started_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    boot_timer = timer_new_ms(QEMU_CLOCK_REALTIME, boot_sample_tick, NULL);
    timer_mod(boot_timer, boot_started_ms + 5000);
    __android_log_print(ANDROID_LOG_INFO, "xemu-boot",
        "Startup diagnostics enabled; vCPU sample interval=5000 ms");
}
