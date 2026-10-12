/* Exercise the actual Android recorder with concurrent generated events.
 * Link profile.c with section GC; the only timer dependency is this test clock.
 * The invoking harness preserves/restores the two diagnostic properties. */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/stall-profile.h"
#include <sys/system_properties.h>
#include <pthread.h>
#include <sys/stat.h>

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    assert(type == QEMU_CLOCK_REALTIME);
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void *producer(void *opaque)
{
    uintptr_t worker = (uintptr_t)opaque;
    for (unsigned i = 0; i < 1000; ++i) {
        uint64_t id = nv2a_lag_next_id();
        nv2a_lag_context(id);
        int64_t now = nv2a_lag_start();
        nv2a_lag_event(NV2A_LAG_CB_BEGIN, now, id, 0, worker, i, 0);
        nv2a_lag_phase(NV2A_STALL_TEXTURE_DECODE, now - 60, now, 128);
        nv2a_lag_cpu_span(NV2A_LAG_STAGE_draw_pipeline, now - 60);
        nv2a_lag_event(NV2A_LAG_CB_END, now, id, 0, worker, i, 0);
    }
    return NULL;
}

static void verify_export(const char *directory, unsigned token,
                           unsigned expected_events)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/lag-%d-%u.csv", directory, getpid(), token);
    bool complete = false;
    unsigned records = 0;
    for (unsigned attempt = 0; attempt < 500 && !complete; ++attempt) {
        FILE *file = fopen(path, "r");
        if (file) {
            char line[512];
            while (fgets(line, sizeof(line), file)) {
                unsigned long long lost;
                if (sscanf(line, "# complete records=%u dropped=%llu", &records, &lost) == 2) {
                    assert(lost == 0);
                    complete = true;
                }
            }
            assert(fclose(file) == 0);
        }
        if (!complete) { usleep(10000); }
    }
    assert(complete && records == expected_events);
    printf("RECORDER_CAPTURE token=%u records=%u path=%s\n", token, records, path);
    /* Footer precedes fclose and exporting flag; allow its thread to finish. */
    usleep(30000);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !strncmp(argv[1], "/data/local/tmp/x1box-lag44-", 26));
    assert(mkdir(argv[1], 0700) == 0); /* Fresh directory, never overwrite. */
    assert(setenv("XEMU_LAG_TRACE_DIR", argv[1], 1) == 0);
    assert(__system_property_set("debug.xemu.lag.trace", "0") == 0);
    assert(__system_property_set("debug.xemu.lag.capture", "0") == 0);
    nv2a_lag_init();
    assert(!nv2a_lag_enabled && nv2a_lag_next_id() == 0);
    assert(__system_property_set("debug.xemu.lag.trace", "1") == 0);
    nv2a_lag_init();
    assert(nv2a_lag_enabled);
    assert(__system_property_set("debug.xemu.lag.capture", "1") == 0);
    nv2a_lag_poll(nv2a_lag_start());
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) { assert(pthread_create(&threads[i], NULL, producer, (void *)i) == 0); }
    for (unsigned i = 0; i < 4; ++i) { assert(pthread_join(threads[i], NULL) == 0); }
    assert(__system_property_set("debug.xemu.lag.capture", "0") == 0);
    usleep(110000);
    nv2a_lag_poll(nv2a_lag_start());
    verify_export(argv[1], 1, 16001);
    assert(__system_property_set("debug.xemu.lag.capture", "2") == 0);
    usleep(110000);
    nv2a_lag_poll(nv2a_lag_start());
    nv2a_lag_event(NV2A_LAG_SUBMIT, nv2a_lag_start(), 5000, 0, 0, 0, 0);
    assert(__system_property_set("debug.xemu.lag.capture", "0") == 0);
    usleep(110000);
    nv2a_lag_poll(nv2a_lag_start());
    verify_export(argv[1], 2, 2);
    puts("PASS actual recorder: disabled mode, 4 concurrent producers, independent captures, complete exports");
    return 0;
}
