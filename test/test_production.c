/**
 * test_production.c - Basic production mode tests
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    printf("PASS: %s\n", name); \
    return 0; \
} while (0)

/* ========================================================================== */
/*  Test: Basic Creation and Destruction                                      */
/* ========================================================================== */

int test_create_destroy(void) {
    drainprof *prof = drainprof_create();
    TEST_ASSERT(prof != NULL, "drainprof_create should succeed");

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    TEST_ASSERT(snap.total_closes == 0, "Initial total_closes should be 0");
    TEST_ASSERT(snap.drainable_closes == 0, "Initial drainable_closes should be 0");
    TEST_ASSERT(snap.pinned_closes == 0, "Initial pinned_closes should be 0");
    TEST_ASSERT(snap.open_granules == 0, "Initial open_granules should be 0");
    TEST_ASSERT(snap.dsr == 0.0, "Initial DSR should be 0.0");

    drainprof_destroy(prof);
    TEST_PASS("create_destroy");
}

/* ========================================================================== */
/*  Test: Single Drainable Granule                                            */
/* ========================================================================== */

int test_single_drainable(void) {
    drainprof *prof = drainprof_create();
    TEST_ASSERT(prof != NULL, "profiler creation failed");

    /* Open granule */
    int result = drainprof_granule_open(prof, 1);
    TEST_ASSERT(result == 0, "granule_open should succeed");

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.open_granules == 1, "Should have 1 open granule");

    /* Allocate and free an object */
    result = drainprof_alloc_register(prof, 1, 100, 256);
    TEST_ASSERT(result == 0, "alloc_register should succeed");

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_allocs == 1, "Should have 1 total allocation");

    result = drainprof_alloc_deregister(prof, 1, 100);
    TEST_ASSERT(result == 0, "alloc_deregister should succeed");

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_deallocs == 1, "Should have 1 total deallocation");

    /* Close granule - should be drainable */
    int drainable = drainprof_granule_close(prof, 1);
    TEST_ASSERT(drainable == 1, "Granule should be drainable");

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_closes == 1, "Should have 1 total close");
    TEST_ASSERT(snap.drainable_closes == 1, "Should have 1 drainable close");
    TEST_ASSERT(snap.pinned_closes == 0, "Should have 0 pinned closes");
    TEST_ASSERT(snap.dsr == 1.0, "DSR should be 1.0");
    TEST_ASSERT(snap.open_granules == 0, "Should have 0 open granules");

    drainprof_destroy(prof);
    TEST_PASS("single_drainable");
}

/* ========================================================================== */
/*  Test: Single Pinned Granule                                               */
/* ========================================================================== */

int test_single_pinned(void) {
    drainprof *prof = drainprof_create();
    TEST_ASSERT(prof != NULL, "profiler creation failed");

    /* Open granule */
    drainprof_granule_open(prof, 1);

    /* Allocate but DON'T free */
    drainprof_alloc_register(prof, 1, 100, 256);

    /* Close granule - should be pinned */
    int drainable = drainprof_granule_close(prof, 1);
    TEST_ASSERT(drainable == 0, "Granule should be pinned");

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_closes == 1, "Should have 1 total close");
    TEST_ASSERT(snap.drainable_closes == 0, "Should have 0 drainable closes");
    TEST_ASSERT(snap.pinned_closes == 1, "Should have 1 pinned close");
    TEST_ASSERT(snap.dsr == 0.0, "DSR should be 0.0");

    drainprof_destroy(prof);
    TEST_PASS("single_pinned");
}

/* ========================================================================== */
/*  Test: Multiple Allocations in One Granule                                 */
/* ========================================================================== */

int test_multiple_allocs(void) {
    drainprof *prof = drainprof_create();

    drainprof_granule_open(prof, 1);

    /* Allocate 10 objects */
    for (int i = 0; i < 10; i++) {
        drainprof_alloc_register(prof, 1, 100 + i, 128);
    }

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_allocs == 10, "Should have 10 allocations");

    /* Free 5 objects */
    for (int i = 0; i < 5; i++) {
        drainprof_alloc_deregister(prof, 1, 100 + i);
    }

    /* Close - should be pinned (5 still live) */
    int drainable = drainprof_granule_close(prof, 1);
    TEST_ASSERT(drainable == 0, "Granule should be pinned");

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.pinned_closes == 1, "Should have 1 pinned close");

    drainprof_destroy(prof);
    TEST_PASS("multiple_allocs");
}

/* ========================================================================== */
/*  Test: Sequential Granules (Dense IDs)                                     */
/* ========================================================================== */

int test_sequential_granules(void) {
    drainprof *prof = drainprof_create();

    /* Open and close 100 granules sequentially */
    for (uint64_t i = 0; i < 100; i++) {
        drainprof_granule_open(prof, i);

        /* Allocate and free immediately */
        drainprof_alloc_register(prof, i, i * 10, 256);
        drainprof_alloc_deregister(prof, i, i * 10);

        int drainable = drainprof_granule_close(prof, i);
        TEST_ASSERT(drainable == 1, "All granules should be drainable");
    }

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_closes == 100, "Should have 100 closes");
    TEST_ASSERT(snap.drainable_closes == 100, "All should be drainable");
    TEST_ASSERT(snap.dsr == 1.0, "DSR should be 1.0");

    drainprof_destroy(prof);
    TEST_PASS("sequential_granules");
}

/* ========================================================================== */
/*  Test: Peak Open Granules Tracking                                         */
/* ========================================================================== */

int test_peak_tracking(void) {
    drainprof *prof = drainprof_create();

    /* Open 5 granules simultaneously */
    for (uint64_t i = 0; i < 5; i++) {
        drainprof_granule_open(prof, i);
    }

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.open_granules == 5, "Should have 5 open granules");
    TEST_ASSERT(snap.peak_open_granules == 5, "Peak should be 5");

    /* Close 3 */
    for (uint64_t i = 0; i < 3; i++) {
        drainprof_granule_close(prof, i);
    }

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.open_granules == 2, "Should have 2 open granules");
    TEST_ASSERT(snap.peak_open_granules == 5, "Peak should still be 5");

    /* Open 3 more (total 5 again) */
    for (uint64_t i = 10; i < 13; i++) {
        drainprof_granule_open(prof, i);
    }

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.open_granules == 5, "Should have 5 open granules again");
    TEST_ASSERT(snap.peak_open_granules == 5, "Peak should still be 5");

    drainprof_destroy(prof);
    TEST_PASS("peak_tracking");
}

/* ========================================================================== */
/*  Test: P-Sweep Validation (Theorem 3)                                      */
/* ========================================================================== */

int test_psweep(void) {
    printf("\n=== Theorem 3 Validation (Mini P-Sweep) ===\n\n");

    double p_values[] = {0.0, 0.1, 0.5, 1.0};
    int num_p = sizeof(p_values) / sizeof(p_values[0]);

    for (int i = 0; i < num_p; i++) {
        double p = p_values[i];
        drainprof *prof = drainprof_create();

        uint64_t epochs = 1000;

        /* Simulate p-sweep: pin epoch with probability p */
        for (uint64_t epoch = 0; epoch < epochs; epoch++) {
            drainprof_granule_open(prof, epoch);

            /* Request allocation (always freed) */
            drainprof_alloc_register(prof, epoch, epoch * 2, 128);
            drainprof_alloc_deregister(prof, epoch, epoch * 2);

            /* Session allocation (never freed) with probability p */
            /* For simplicity, use deterministic: pin every (1/p)-th epoch */
            if (p > 0.0 && (epoch % (int)(1.0 / p)) == 0) {
                drainprof_alloc_register(prof, epoch, epoch * 2 + 1, 256);
                /* Not deregistered - pins the granule */
            }

            drainprof_granule_close(prof, epoch);
        }

        drainprof_snapshot_t snap;
        drainprof_snapshot(prof, &snap);

        double expected_dsr = 1.0 - p;
        double dsr_error = snap.dsr - expected_dsr;

        printf("  p=%.1f: DSR=%.3f (expected %.3f, error %.3f), "
               "pinned=%" PRIu64 "/%" PRIu64 "\n",
               p, snap.dsr, expected_dsr, dsr_error,
               snap.pinned_closes, snap.total_closes);

        /* Tolerance for small sample size */
        TEST_ASSERT(dsr_error > -0.05 && dsr_error < 0.05,
                    "DSR should be within 5% of expected");

        drainprof_destroy(prof);
    }

    printf("\n");
    TEST_PASS("psweep (Theorem 3 validation)");
}

/* ========================================================================== */
/*  Main Test Runner                                                          */
/* ========================================================================== */

typedef int (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} test_case;

int main(void) {
    test_case tests[] = {
        {"create_destroy", test_create_destroy},
        {"single_drainable", test_single_drainable},
        {"single_pinned", test_single_pinned},
        {"multiple_allocs", test_multiple_allocs},
        {"sequential_granules", test_sequential_granules},
        {"peak_tracking", test_peak_tracking},
        {"psweep", test_psweep},
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;

    printf("Running libdrainprof tests...\n\n");

    for (int i = 0; i < num_tests; i++) {
        printf("Test %d/%d: %s\n", i + 1, num_tests, tests[i].name);
        int result = tests[i].fn();
        if (result == 0) {
            passed++;
        } else {
            failed++;
            fprintf(stderr, "FAILED: %s\n\n", tests[i].name);
        }
    }

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");

    return failed > 0 ? 1 : 0;
}
