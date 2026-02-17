/**
 * diagnostic.c - Diagnostic mode example
 *
 * Demonstrates:
 * - Creating a profiler in diagnostic mode
 * - Using pinning report callbacks
 * - Identifying which allocations pin granules
 * - Reading detailed source locations
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

/* Callback invoked when a granule is pinned */
void on_granule_pinned(const drainprof_pinning_report *report, void *user_data) {
    (void)user_data;

    fprintf(stderr, "\n[DIAGNOSTIC] Granule %" PRIu64 " PINNED:\n",
            report->granule_id);
    fprintf(stderr, "  Total allocations: %u\n", report->total_allocs);
    fprintf(stderr, "  Drained: %u\n", report->drained_allocs);
    fprintf(stderr, "  Pinning: %u\n", report->pinning_count);
    fprintf(stderr, "\n  Pinning allocations:\n");

    for (uint32_t i = 0; i < report->pinning_count; i++) {
        const drainprof_pinning_alloc *pa = &report->pinning_allocs[i];
        fprintf(stderr, "    [%u] alloc_id=%" PRIu64 " size=%zu",
                i, pa->alloc_id, pa->size);

        if (pa->alloc_site.file) {
            fprintf(stderr, " at %s:%u", pa->alloc_site.file, pa->alloc_site.line);
        }

        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\n");
}

int main(void) {
    printf("====================================\n");
    printf("  Diagnostic Mode Example\n");
    printf("====================================\n\n");

    /* Configure diagnostic mode with callback */
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;
    config.on_pinning = on_granule_pinned;
    config.verbose = true;

    drainprof *prof = drainprof_create_with_config(&config);
    if (!prof) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        return 1;
    }

    printf("Simulating allocator with mixed lifetimes...\n\n");

    /* Simulate 5 granules */
    for (uint64_t gid = 0; gid < 5; gid++) {
        drainprof_granule_open(prof, gid);

        /* Short-lived request allocation (always freed) */
        DRAINPROF_ALLOC_REGISTER(prof, gid, gid * 100, 128);
        drainprof_alloc_deregister(prof, gid, gid * 100);

        /* For odd granules, add a long-lived session allocation */
        if (gid % 2 == 1) {
            DRAINPROF_ALLOC_REGISTER(prof, gid, gid * 100 + 1, 1024);
            /* Not freed - this will pin the granule */
            printf("  [INFO] Granule %" PRIu64 ": allocated session object (not freed)\n", gid);
        }

        /* Close granule */
        int drainable = drainprof_granule_close(prof, gid);
        if (drainable) {
            printf("  Granule %" PRIu64 ": DRAINABLE\n", gid);
        }
        /* Pinned granules will trigger the callback above */
    }

    /* Get final metrics */
    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("\n====================================\n");
    printf("  Final Metrics\n");
    printf("====================================\n\n");
    printf("Total closes:     %" PRIu64 "\n", snap.total_closes);
    printf("Drainable closes: %" PRIu64 "\n", snap.drainable_closes);
    printf("Pinned closes:    %" PRIu64 "\n", snap.pinned_closes);
    printf("DSR:              %.1f%%\n\n", snap.dsr * 100.0);

    if (snap.dsr < 0.9) {
        printf("Analysis: Low DSR indicates structural leaks.\n");
        printf("Session objects are pinning request-scoped granules.\n");
        printf("Consider: Separate allocators for different lifetime classes.\n\n");
    }

    printf("====================================\n");
    printf("  Diagnostic Example Complete\n");
    printf("====================================\n");

    drainprof_destroy(prof);
    return 0;
}
