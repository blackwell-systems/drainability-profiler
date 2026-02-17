/*
 * diagnostic_validation.c
 *
 * Validates drainability profiler diagnostic mode with temporal-slab allocator.
 *
 * Tests that diagnostic mode correctly identifies:
 * - Allocation sites causing violations
 * - Pinning counts matching violation fraction
 * - Granule-pinned-per-site metrics
 */

#include "../../../temporal-slab/include/slab_alloc.h"
#include "../../../temporal-slab/include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern drainprof* g_profiler;

#define NUM_REQUESTS 100
#define ALLOCS_PER_REQUEST 1
#define VIOLATION_PROBABILITY 0.25

typedef struct {
    void* ptr;
    SlabHandle handle;
} Allocation;

/* Workload: process requests, with probability p leak allocations */
void run_workload(SlabAllocator* alloc, double p) {
    int total_leaked = 0;
    srand(42);  /* Deterministic seed */

    for (int req = 0; req < NUM_REQUESTS; req++) {
        /* Start new request epoch */
        epoch_advance(alloc);
        EpochId epoch = epoch_current(alloc);

        /* Allocate objects for this request */
        Allocation allocs[ALLOCS_PER_REQUEST];
        for (int i = 0; i < ALLOCS_PER_REQUEST; i++) {
            SlabHandle handle;
            void* ptr = alloc_obj_epoch(alloc, 128, epoch, &handle);
            allocs[i].ptr = ptr;
            allocs[i].handle = handle;
        }

        /* Process request (no-op in this test) */

        /* Close request: free allocations, but with probability p skip freeing */
        int leaked_this_request = 0;
        for (int i = 0; i < ALLOCS_PER_REQUEST; i++) {
            if (!allocs[i].ptr) continue;

            double r = (double)rand() / RAND_MAX;
            if (r >= p) {
                /* Normal: free the allocation */
                free_obj(alloc, allocs[i].handle);
                allocs[i].ptr = NULL;
            } else {
                /* Leak: skip freeing (simulates reference held past epoch boundary) */
                leaked_this_request++;
                total_leaked++;
            }
        }

        /* Close epoch - leaked allocations should pin it */
        epoch_close(alloc, epoch);

        /* Clean up leaked allocations after epoch closes (delayed free) */
        for (int i = 0; i < ALLOCS_PER_REQUEST; i++) {
            if (allocs[i].ptr) {
                free_obj(alloc, allocs[i].handle);
            }
        }
    }

    printf("  Total violations: %d / %d allocations (%.1f%%)\n",
           total_leaked, NUM_REQUESTS * ALLOCS_PER_REQUEST,
           100.0 * total_leaked / (NUM_REQUESTS * ALLOCS_PER_REQUEST));
}

int main() {
    printf("=== Temporal-Slab Diagnostic Mode Validation ===\n");
    printf("Testing diagnostic mode with p=%.2f violation rate\n\n", VIOLATION_PROBABILITY);
    fflush(stdout);

    /* Create profiler in diagnostic mode */
    printf("Creating profiler in diagnostic mode...\n");
    fflush(stdout);

    drainprof_config config = {
        .mode = DRAINPROF_DIAGNOSTIC,
        .storage = DRAINPROF_SLOT_ARRAY,
        .slot_capacity = 128,
        .bucket_count = 0,
        .log_interval = 0,
        .on_pinning = NULL,
        .callback_user_data = NULL,
        .max_buffered_reports = 1000
    };

    g_profiler = drainprof_create_with_config(&config);
    if (!g_profiler) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        fflush(stderr);
        return 1;
    }
    printf("Profiler created successfully\n");
    fflush(stdout);

    /* Create allocator */
    printf("Creating allocator...\n");
    fflush(stdout);

    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        fprintf(stderr, "ERROR: Failed to create allocator\n");
        fflush(stderr);
        drainprof_destroy(g_profiler);
        return 1;
    }
    printf("Allocator created successfully\n");
    fflush(stdout);

    /* Run workload */
    printf("Running workload...\n");
    fflush(stdout);
    run_workload(alloc, VIOLATION_PROBABILITY);
    printf("Workload completed\n");
    fflush(stdout);

    /* Read basic profiler metrics */
    printf("Reading profiler metrics...\n");
    fflush(stdout);
    drainprof_snapshot_t snapshot;
    drainprof_snapshot(g_profiler, &snapshot);
    printf("Snapshot retrieved\n");
    fflush(stdout);

    printf("\n=== Basic Metrics ===\n");
    printf("Total closes: %lu\n", snapshot.total_closes);
    printf("Drainable closes: %lu\n", snapshot.drainable_closes);
    printf("Pinned closes: %lu\n", snapshot.pinned_closes);
    printf("DSR: %.3f\n", snapshot.dsr);
    printf("Total allocs: %lu\n", snapshot.total_allocs);
    printf("Total deallocs: %lu\n", snapshot.total_deallocs);
    fflush(stdout);

    if (snapshot.pinned_closes == 0) {
        fprintf(stderr, "WARNING: No pinned closes detected. Diagnostic reports may be empty.\n");
        fflush(stderr);
    }

    /* Generate diagnostic summary */
    printf("\n=== Diagnostic Summary ===\n");
    fflush(stdout);

    printf("Computing diagnostic summary...\n");
    fflush(stdout);

    drainprof_diagnostic_summary* summary = drainprof_diagnostic_summary_compute(g_profiler);
    if (!summary) {
        fprintf(stderr, "ERROR: Failed to generate diagnostic summary\n");
        fprintf(stderr, "This might indicate no reports were buffered.\n");
        fflush(stderr);
        slab_allocator_free(alloc);
        drainprof_destroy(g_profiler);
        return 1;
    }

    printf("Diagnostic summary computed successfully\n");
    fflush(stdout);

    printf("Allocation sites tracked: %u\n", summary->site_count);

    /* Validate diagnostic results */
    int validation_passed = 1;

    if (summary->site_count == 0) {
        printf("✗ FAIL: No allocation sites tracked\n");
        validation_passed = 0;
    } else {
        printf("✓ PASS: Allocation sites tracked\n");

        /* Find the allocation site from run_workload */
        printf("\nAllocation sites:\n");
        for (uint32_t i = 0; i < summary->site_count; i++) {
            drainprof_summary_site_entry* entry = &summary->sites[i];
            printf("  Site %u:\n", i);
            printf("    Location: %s:%u\n", entry->site.file, entry->site.line);
            printf("    Total allocs: %u\n", entry->total_allocs);
            printf("    Total bytes: %zu\n", entry->total_bytes);
            printf("    Pinning count: %u\n", entry->pinning_count);

            /* Note: total_allocs counts allocations that appeared in pinning
               reports (live at granule close), not all allocations from this site.
               So pinning_count / total_allocs is often ~1.0 and not meaningful. */

            /* The key validation: pinning_count should match expected violations */
            uint32_t expected_violations = (uint32_t)(VIOLATION_PROBABILITY * NUM_REQUESTS);
            uint32_t error_count = entry->pinning_count > expected_violations ?
                entry->pinning_count - expected_violations :
                expected_violations - entry->pinning_count;

            printf("    Expected violations: %u (observed %u, error %u)\n",
                   expected_violations, entry->pinning_count, error_count);

            /* Allow 15% error margin for statistical variance */
            uint32_t margin = (uint32_t)(0.15 * NUM_REQUESTS);
            if (error_count > margin) {
                printf("    ✗ FAIL: Pinning count does not match expected violations\n");
                validation_passed = 0;
            } else {
                printf("    ✓ PASS: Pinning count matches expected violations\n");
            }

            /* Verify that total_allocs equals pinning_count (all tracked allocs pinned) */
            if (entry->total_allocs != entry->pinning_count) {
                printf("    ⚠ WARNING: total_allocs (%u) != pinning_count (%u)\n",
                       entry->total_allocs, entry->pinning_count);
                printf("    This may indicate allocations tracked but not pinning.\n");
            } else {
                printf("    ✓ All tracked allocations from this site caused pinning\n");
            }
        }
    }

    /* Cleanup */
    drainprof_diagnostic_summary_free(summary);
    slab_allocator_free(alloc);
    drainprof_destroy(g_profiler);
    g_profiler = NULL;

    printf("\n=== Summary ===\n");
    if (validation_passed) {
        printf("VALIDATION SUCCESS: Diagnostic mode correctly identifies allocation sites\n");
        return 0;
    } else {
        printf("VALIDATION FAILURE: Diagnostic mode did not meet expectations\n");
        return 1;
    }
}
