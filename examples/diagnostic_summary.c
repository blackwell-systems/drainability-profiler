/**
 * diagnostic_summary.c - Diagnostic summary formatter example
 *
 * Demonstrates:
 * - Using diagnostic mode without callbacks (buffered reports)
 * - Computing aggregated summary of pinning allocations by source location
 * - Identifying the most problematic allocation sites
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

int main(void) {
    printf("====================================\n");
    printf("  Diagnostic Summary Example\n");
    printf("====================================\n\n");

    /* Configure diagnostic mode WITHOUT callback (buffer reports) */
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;
    config.on_pinning = NULL;  /* NULL = buffer reports instead of callback */
    config.verbose = false;    /* Don't log individual pinned closes */

    drainprof *prof = drainprof_create_with_config(&config);
    if (!prof) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        return 1;
    }

    printf("Simulating HTTP server with mixed session/request lifetimes...\n\n");

    /* Simulate 20 request epochs */
    for (uint64_t epoch = 0; epoch < 20; epoch++) {
        drainprof_granule_open(prof, epoch);

        /* Every request allocates a request buffer (short-lived) */
        DRAINPROF_ALLOC_REGISTER(prof, epoch, epoch * 1000, 4096);
        drainprof_alloc_deregister(prof, epoch, epoch * 1000);

        /* 30% of requests establish a long-lived session */
        if (epoch % 3 == 0) {
            /* Session allocation - NOT freed (pins epoch) */
            DRAINPROF_ALLOC_REGISTER(prof, epoch, epoch * 1000 + 1, 2048);
        }

        /* 20% of requests leak a response buffer (bug!) */
        if (epoch % 5 == 0) {
            /* Response buffer leak - NOT freed (pins epoch) */
            DRAINPROF_ALLOC_REGISTER(prof, epoch, epoch * 1000 + 2, 8192);
        }

        drainprof_granule_close(prof, epoch);
    }

    /* Get overall metrics */
    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("\n====================================\n");
    printf("  Overall Metrics\n");
    printf("====================================\n\n");
    printf("Total epochs:     %u\n", 20);
    printf("Total closes:     %" PRIu64 "\n", snap.total_closes);
    printf("Drainable closes: %" PRIu64 "\n", snap.drainable_closes);
    printf("Pinned closes:    %" PRIu64 "\n", snap.pinned_closes);
    printf("DSR:              %.1f%%\n\n", snap.dsr * 100.0);

    /* Compute diagnostic summary */
    printf("====================================\n");
    printf("  Diagnostic Summary\n");
    printf("====================================\n\n");

    drainprof_diagnostic_summary *summary = drainprof_diagnostic_summary_compute(prof);
    if (!summary) {
        fprintf(stderr, "ERROR: Failed to compute summary\n");
        drainprof_destroy(prof);
        return 1;
    }

    printf("Reports analyzed:      %u\n", summary->reports_analyzed);
    printf("Total pinning allocs:  %u\n", summary->total_pinning_allocs);
    printf("Unique sites:          %u\n\n", summary->site_count);

    printf("Pinning allocations grouped by source location:\n");
    printf("---------------------------------------------------------------\n");
    printf("%-30s %10s %10s %12s\n", "Site", "Granules", "Allocs", "Bytes");
    printf("---------------------------------------------------------------\n");

    for (uint32_t i = 0; i < summary->site_count; i++) {
        drainprof_summary_site_entry *site = &summary->sites[i];

        /* Format site location */
        char site_str[64];
        if (site->site.file) {
            snprintf(site_str, sizeof(site_str), "%s:%u",
                     site->site.file, site->site.line);
        } else {
            snprintf(site_str, sizeof(site_str), "<unknown>");
        }

        printf("%-30s %10u %10u %12zu\n",
               site_str,
               site->pinning_count,
               site->total_allocs,
               site->total_bytes);
    }
    printf("---------------------------------------------------------------\n\n");

    /* Identify most problematic site (pins most granules) */
    if (summary->site_count > 0) {
        uint32_t worst_idx = 0;
        for (uint32_t i = 1; i < summary->site_count; i++) {
            if (summary->sites[i].pinning_count > summary->sites[worst_idx].pinning_count) {
                worst_idx = i;
            }
        }

        drainprof_summary_site_entry *worst = &summary->sites[worst_idx];
        printf("Most problematic site: %s:%u\n",
               worst->site.file ? worst->site.file : "<unknown>",
               worst->site.line);
        printf("  Pins %u granules (%.1f%% of all pinned)\n",
               worst->pinning_count,
               100.0 * worst->pinning_count / summary->reports_analyzed);
        printf("  Total: %u allocations, %zu bytes\n\n",
               worst->total_allocs, worst->total_bytes);
    }

    printf("====================================\n");
    printf("  Recommendations\n");
    printf("====================================\n\n");

    if (snap.dsr < 0.8) {
        printf("LOW DSR: Structural leaks detected.\n\n");
        printf("Root cause analysis (from summary):\n");
        for (uint32_t i = 0; i < summary->site_count; i++) {
            drainprof_summary_site_entry *site = &summary->sites[i];
            if (site->pinning_count > 2) {
                printf("  - %s:%u pins %u epochs\n",
                       site->site.file ? site->site.file : "<unknown>",
                       site->site.line,
                       site->pinning_count);
            }
        }
        printf("\nRecommendations:\n");
        printf("  1. Review allocation sites above for lifetime mismatches\n");
        printf("  2. Consider separate allocators for session vs request data\n");
        printf("  3. Fix any identified memory leaks\n");
    } else {
        printf("DSR is acceptable (>80%%).\n");
        printf("Monitor over time for degradation.\n");
    }

    /* Cleanup */
    drainprof_diagnostic_summary_free(summary);
    drainprof_destroy(prof);

    printf("\n====================================\n");
    printf("  Summary Example Complete\n");
    printf("====================================\n");

    return 0;
}
