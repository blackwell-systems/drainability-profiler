/*
 * psweep_validation.c
 *
 * Validates drainability profiler integration with temporal-slab allocator.
 *
 * Tests Theorem 3 from the paper: DSR = 1.0 - p
 * where p is the violation probability (routing allocations to wrong epoch).
 *
 * Build:
 *   gcc -O2 -std=c11 -pthread -Wall -Wextra -pedantic \
 *     -I../../include -I../../../temporal-slab/include \
 *     -DENABLE_RSS_RECLAMATION=1 -DENABLE_DRAINPROF \
 *     psweep_validation.c \
 *     ../../../temporal-slab/src/slab_lib.o \
 *     ../../../temporal-slab/src/epoch_domain.o \
 *     ../../../temporal-slab/src/slab_stats.o \
 *     -L../.. -ldrainprof \
 *     -o psweep_validation
 */

#include "../../../temporal-slab/include/slab_alloc.h"
#include "../../../temporal-slab/include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern drainprof* g_profiler;

#define NUM_EPOCHS 100
#define ALLOCS_PER_EPOCH 50

typedef struct {
    void* ptr;
    SlabHandle handle;
    uint32_t target_epoch;    /* Where allocation was routed */
    uint32_t intended_epoch;  /* Where it should have been routed */
} Allocation;

/* Mixed-routing workload: allocations that should be freed but aren't */
void run_mixed_routing(SlabAllocator* alloc, double p) {
    Allocation allocs[NUM_EPOCHS * ALLOCS_PER_EPOCH];
    int alloc_count = 0;
    int violation_count = 0;

    srand(42);  /* Deterministic seed */

    for (int epoch_idx = 0; epoch_idx < NUM_EPOCHS; epoch_idx++) {
        /* Advance to new epoch */
        epoch_advance(alloc);
        EpochId current = epoch_current(alloc);

        /* Allocate objects for this epoch */
        for (int i = 0; i < ALLOCS_PER_EPOCH; i++) {
            SlabHandle handle;
            void* ptr = alloc_obj_epoch(alloc, 128, current, &handle);

            if (ptr) {
                allocs[alloc_count].ptr = ptr;
                allocs[alloc_count].handle = handle;
                allocs[alloc_count].target_epoch = current;
                allocs[alloc_count].intended_epoch = current;
                alloc_count++;
            }
        }

        /* Close the oldest epoch (epoch that just aged out) */
        if (epoch_idx >= 8) {
            EpochId old_epoch = (current - 8 + 16) % 16;

            /* Free allocations from this epoch, but with probability p, skip freeing
             * (simulates leaked references that prevent object from being freed) */
            for (int i = 0; i < alloc_count; i++) {
                if (allocs[i].intended_epoch == old_epoch && allocs[i].ptr) {
                    double r = (double)rand() / RAND_MAX;
                    if (r >= p) {
                        /* Normal case: free the allocation */
                        free_obj(alloc, allocs[i].handle);
                        allocs[i].ptr = NULL;
                    } else {
                        /* Violation: leak the allocation (don't free) */
                        violation_count++;
                    }
                }
            }

            /* Now close the epoch - leaked allocations should pin it */
            epoch_close(alloc, old_epoch);
        }
    }

    /* Clean up remaining allocations (leaked ones) */
    for (int i = 0; i < alloc_count; i++) {
        if (allocs[i].ptr) {
            free_obj(alloc, allocs[i].handle);
        }
    }

    printf("  Total violations created: %d (expected ~%.0f)\n",
           violation_count, p * 92 * ALLOCS_PER_EPOCH);
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
            .slot_capacity = 32,
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

        printf("  Created profiler at %p\n", (void*)g_profiler);

        /* Create allocator */
        SlabAllocator* alloc = slab_allocator_create();
        if (!alloc) {
            fprintf(stderr, "Failed to create allocator\n");
            drainprof_destroy(g_profiler);
            return 1;
        }

        /* Run workload */
        run_mixed_routing(alloc, p);

        printf("  After workload, g_profiler = %p\n", (void*)g_profiler);

        /* Read profiler metrics */
        drainprof_snapshot_t snapshot;
        drainprof_snapshot(g_profiler, &snapshot);

        printf("  Profiler state: allocs=%lu deallocs=%lu opens=%lu\n",
               snapshot.total_allocs, snapshot.total_deallocs, snapshot.open_granules);

        double actual_dsr = snapshot.dsr;
        double error = fabs(actual_dsr - expected_dsr);

        printf("p=%.2f: DSR=%.3f (expected %.3f, error %.3f) ",
               p, actual_dsr, expected_dsr, error);

        /* Allow 5% error margin for statistical variance */
        if (error < 0.05) {
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

    printf("=== Results ===\n");
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
