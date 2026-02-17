/*
 * psweep_validation.c
 *
 * Validates drainability profiler integration with temporal-slab allocator.
 *
 * Tests Theorem 3 from the paper: DSR = 1.0 - p
 * where p is the violation probability (leak rate at epoch boundary).
 */

#include "../../../temporal-slab/include/slab_alloc.h"
#include "../../../temporal-slab/include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern drainprof* g_profiler;

#define NUM_REQUESTS 100
#define ALLOCS_PER_REQUEST 1

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

    printf("  Total leaks: %d / %d allocations (%.1f%%)\n",
           total_leaked, NUM_REQUESTS * ALLOCS_PER_REQUEST,
           100.0 * total_leaked / (NUM_REQUESTS * ALLOCS_PER_REQUEST));
}

int main() {
    printf("=== Temporal-Slab Integration Validation ===\n");
    printf("Testing Theorem 3: DSR = 1.0 - p\n\n");

    double p_values[] = {0.0, 0.01, 0.05, 0.10, 0.25, 0.50, 1.0};
    int num_tests = sizeof(p_values) / sizeof(p_values[0]);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < num_tests; i++) {
        double p = p_values[i];
        double expected_dsr = 1.0 - p;

        /* Create profiler */
        drainprof_config config = {
            .mode = DRAINPROF_PRODUCTION,
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
        run_workload(alloc, p);

        /* Read profiler metrics */
        drainprof_snapshot_t snapshot;
        drainprof_snapshot(g_profiler, &snapshot);

        double actual_dsr = snapshot.dsr;
        double error = fabs(actual_dsr - expected_dsr);

        printf("  DSR: %.3f (expected %.3f, error %.3f) ",
               actual_dsr, expected_dsr, error);

        /* Allow 10% error margin for statistical variance */
        if (error < 0.10) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }

        printf("  Metrics: closes=%lu drainable=%lu pinned=%lu\n",
               snapshot.total_closes,
               snapshot.drainable_closes,
               snapshot.pinned_closes);

        /* Cleanup */
        slab_allocator_free(alloc);
        drainprof_destroy(g_profiler);
        g_profiler = NULL;

        printf("\n");
    }

    printf("=== Summary ===\n");
    printf("Passed: %d/%d\n", passed, num_tests);
    printf("Failed: %d/%d\n", failed, num_tests);

    if (failed == 0) {
        printf("\nVALIDATION SUCCESS: Profiler correctly measures temporal-slab drainability\n");
        return 0;
    } else {
        printf("\nVALIDATION FAILURE: DSR measurements do not match theoretical predictions\n");
        return 1;
    }
}
