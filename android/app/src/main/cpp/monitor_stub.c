#include "qemu/osdep.h"
#include "monitor/monitor-internal.h"
#include "hw/core/cpu.h"
#include "exec/target_page.h"
#include "monitor/hmp-target.h"
#include "qobject/qdict.h"
#include "system/runstate.h"
#include "hw/xbox/mcpx/apu/apu_debug.h"

static void android_apu_watch_word(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    CPUState *cpu = mon_get_cpu(mon);
    hwaddr page, physical;

    if (runstate_is_running()) {
        monitor_printf(mon, "APU watch rejected: stop the guest first\n");
        return;
    }
    if (!cpu || addr > UINT32_MAX || (addr & 3)) {
        monitor_printf(mon, "APU watch rejected: invalid aligned i386 word\n");
        return;
    }
    page = cpu_get_phys_page_debug(cpu, addr & TARGET_PAGE_MASK);
    if (page == (hwaddr)-1) {
        monitor_printf(mon, "APU watch rejected: unmapped guest address\n");
        return;
    }
    physical = page + (addr & ~TARGET_PAGE_MASK);
    if (!mcpx_apu_debug_watch_word(physical)) {
        monitor_printf(mon, "APU watch rejected: target is outside main RAM\n");
        return;
    }
    monitor_printf(mon, "APU watch armed gpa=0x%" HWADDR_PRIx " quota=16\n",
                   physical);
}

static void android_apu_unwatch_word(Monitor *mon, const QDict *qdict)
{
    (void)qdict;
    mcpx_apu_debug_unwatch_word();
    monitor_printf(mon, "APU watch disabled\n");
}

HMPCommand hmp_cmds[] = {
    {
        .name = "xemu-apu-watch",
        .args_type = "addr:l",
        .params = "guest-virtual-word",
        .help = "trace at most 16 APU DMA writes to one aligned RAM word",
        .cmd = android_apu_watch_word,
    },
    {
        .name = "xemu-apu-unwatch",
        .args_type = "",
        .params = "",
        .help = "disable the APU RAM writer diagnostic",
        .cmd = android_apu_unwatch_word,
    },
    { NULL }
};

int hmp_compare_cmd(const char *name, const char *list)
{
    /* This Android table has exact names and no aliases. */
    return strcmp(name, list) == 0;
}

int get_monitor_def(Monitor *mon, int64_t *pval, const char *name)
{
    (void)mon;
    (void)pval;
    (void)name;
    return -1;
}

void monitor_register_hmp_info_hrt(const char *name,
                                   HumanReadableText *(*handler)(Error **errp))
{
    (void)name;
    (void)handler;
}

void monitor_register_hmp(const char *name, bool info,
                          void (*cmd)(Monitor *mon, const QDict *qdict))
{
    (void)name;
    (void)info;
    (void)cmd;
}
