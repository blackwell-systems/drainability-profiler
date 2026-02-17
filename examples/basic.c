/**
 * basic.c - Minimal example of libdrainprof usage
 *
 * Demonstrates:
 * - Creating a profiler
 * - Tracking granule lifecycle
 * - Registering allocations
 * - Reading DSR metrics
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

int main(void) {
    printf("====================================\n");
    printf("  libdrainprof Basic Example\n");
    printf("====================================\n\n");

    /* Create profiler with default config */
    drainprof *prof = drainprof_create();
    if (!prof) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        return 1;
    }

    printf("Simulating 10 granules (5 drainable, 5 pinned)...\n\n");

    /* Simulate 10 granule lifecycles */
    for (uint64_t i = 0; i < 10; i++) {
        /* Open granule */
        if (drainprof_granule_open(prof, i) != 0) {
            fprintf(stderr, "ERROR: Failed to open granule %" PRIu64 "\n", i);
            continue;
        }

        /* Allocate a request object (always freed) */
        drainprof_alloc_register(prof, i, i * 100, 128);
        drainprof_alloc_deregister(prof, i, i * 100);

        /* For odd granules, allocate a session object (never freed) */
        if (i % 2 == 1) {
            DRAINPROF_ALLOC_REGISTER(prof, i, i * 100 + 1, 256);
            /* Not deregistered - this pins the granule */
        }

        /* Close granule */
        int drainable = drainprof_granule_close(prof, i);
        printf("  Granule %2" PRIu64 ": %s\n", i,
               drainable ? "drainable" : "PINNED");
    }

    /* Get snapshot */
    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("\n====================================\n");
    printf("  Profiler Metrics\n");
    printf("====================================\n\n");
    printf("Total closes:     %" PRIu64 "\n", snap.total_closes);
    printf("Drainable closes: %" PRIu64 "\n", snap.drainable_closes);
    printf("Pinned closes:    %" PRIu64 "\n", snap.pinned_closes);
    printf("DSR:              %.1f%%\n", snap.dsr * 100.0);
    printf("\n");
    printf("Total allocs:     %" PRIu64 "\n", snap.total_allocs);
    printf("Total deallocs:   %" PRIu64 "\n", snap.total_deallocs);
    printf("Peak open:        %" PRIu64 "\n", snap.peak_open_granules);
    printf("\n");

    if (snap.dsr < 0.9) {
        printf("WARNING: Low DSR detected!\n");
        printf("   This indicates structural leaks.\n");
        printf("   Some allocations outlive their granule's reclaim boundary.\n\n");
    } else {
        printf("DSR looks healthy (>90%%)\n\n");
    }

    printf("====================================\n");
    printf("  Example Complete\n");
    printf("====================================\n");

    drainprof_destroy(prof);
    return 0;
}
