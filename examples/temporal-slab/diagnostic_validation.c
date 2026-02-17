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

    /* Create profiler in diagnostic mode */
    drainprof_config config = {
        .mode = DRAINPROF_DIAGNOSTIC,
        .storage = DRAINPROF_SLOT_ARRAY,
        .slot_capacity = 128,
        .bucket_count = 0,
        .log_interval = 0,
        .on_pinning = NULL,
        .callback_user_data = NULL,
        .max_buffered_reports = 0
    };

    g_profiler = drainprof_create_with_config(&config);
    if (!g_profiler) {
        fprintf(stderr, "Failed to create profiler\n");
        return 1;
    }

    /* Create allocator */
    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        fprintf(stderr, "Failed to create allocator\n");
        drainprof_destroy(g_profiler);
        return 1;
    }

    /* Run workload */
    printf("Running workload...\n");
    run_workload(alloc, VIOLATION_PROBABILITY);

    /* Read basic profiler metrics */
    drainprof_snapshot_t snapshot;
    drainprof_snapshot(g_profiler, &snapshot);

    printf("\n=== Basic Metrics ===\n");
    printf("Total closes: %lu\n", snapshot.total_closes);
    printf("Drainable closes: %lu\n", snapshot.drainable_closes);
    printf("Pinned closes: %lu\n", snapshot.pinned_closes);
    printf("DSR: %.3f\n", snapshot.dsr);

    /* Generate diagnostic summary */
    printf("\n=== Diagnostic Summary ===\n");
    drainprof_diagnostic_summary* summary = drainprof_diagnostic_summary_compute(g_profiler);
    if (!summary) {
        fprintf(stderr, "Failed to generate diagnostic summary\n");
        slab_allocator_free(alloc);
        drainprof_destroy(g_profiler);
        return 1;
    }

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

            if (entry->total_allocs > 0) {
                double pinning_rate = (double)entry->pinning_count / entry->total_allocs;
                printf("    Pinning rate: %.3f\n", pinning_rate);

                /* Expected: pinning_rate ≈ VIOLATION_PROBABILITY (within 15% margin) */
                double expected = VIOLATION_PROBABILITY;
                double error = fabs(pinning_rate - expected);
                double margin = 0.15;  /* Allow 15% error for statistical variance */

                printf("    Expected rate: %.3f (error: %.3f)\n", expected, error);

                if (error > margin) {
                    printf("    ✗ FAIL: Pinning rate does not match expected violation rate\n");
                    validation_passed = 0;
                } else {
                    printf("    ✓ PASS: Pinning rate matches expected violation rate\n");
                }

                /* With 1 allocation per epoch, pinning_count should equal number
                   of granules pinned (each leak pins its epoch) */
                uint32_t expected_pinned = (uint32_t)(VIOLATION_PROBABILITY * NUM_REQUESTS);
                uint32_t error_count = entry->pinning_count > expected_pinned ?
                    entry->pinning_count - expected_pinned :
                    expected_pinned - entry->pinning_count;

                if (error_count > (uint32_t)(0.15 * NUM_REQUESTS)) {
                    printf("    ✗ FAIL: Pinning count (%u) far from expected (%u)\n",
                           entry->pinning_count, expected_pinned);
                    validation_passed = 0;
                } else {
                    printf("    ✓ PASS: Pinning count reasonable (%u vs %u expected)\n",
                           entry->pinning_count, expected_pinned);
                }
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
