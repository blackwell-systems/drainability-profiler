/**
 * test_diagnostic.c - Diagnostic mode tests
 *
 * Tests per-allocation tracking, pinning reports, and diagnostic metadata.
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
/*  Test: Basic Diagnostic Creation                                           */
/* ========================================================================== */

int test_diagnostic_create(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prof = drainprof_create_with_config(&config);
    TEST_ASSERT(prof != NULL, "diagnostic profiler creation should succeed");

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    TEST_ASSERT(snap.total_closes == 0, "Initial total_closes should be 0");
    TEST_ASSERT(snap.drainable_closes == 0, "Initial drainable_closes should be 0");
    TEST_ASSERT(snap.pinned_closes == 0, "Initial pinned_closes should be 0");

    drainprof_destroy(prof);
    TEST_PASS("diagnostic_create");
}

/* ========================================================================== */
/*  Test: Per-Allocation Tracking                                             */
/* ========================================================================== */

int test_per_alloc_tracking(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prof = drainprof_create_with_config(&config);
    TEST_ASSERT(prof != NULL, "profiler creation failed");

    /* Open granule */
    int result = drainprof_granule_open(prof, 1);
    TEST_ASSERT(result == 0, "granule_open should succeed");

    /* Allocate 5 objects */
    for (int i = 0; i < 5; i++) {
        result = DRAINPROF_ALLOC_REGISTER(prof, 1, 100 + i, 256);
        TEST_ASSERT(result == 0, "alloc_register should succeed");
    }

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_allocs == 5, "Should have 5 total allocations");

    /* Free 3 objects */
    for (int i = 0; i < 3; i++) {
        result = drainprof_alloc_deregister(prof, 1, 100 + i);
        TEST_ASSERT(result == 0, "alloc_deregister should succeed");
    }

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.total_deallocs == 3, "Should have 3 total deallocations");

    /* Close granule - should be pinned (2 still live) */
    int drainable = drainprof_granule_close(prof, 1);
    TEST_ASSERT(drainable == 0, "Granule should be pinned");

    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.pinned_closes == 1, "Should have 1 pinned close");
    TEST_ASSERT(snap.drainable_closes == 0, "Should have 0 drainable closes");

    drainprof_destroy(prof);
    TEST_PASS("per_alloc_tracking");
}

/* ========================================================================== */
/*  Test: Diagnostic Drainable Granule                                        */
/* ========================================================================== */

int test_diagnostic_drainable(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prof = drainprof_create_with_config(&config);

    /* Open granule */
    drainprof_granule_open(prof, 1);

    /* Allocate and free immediately */
    DRAINPROF_ALLOC_REGISTER(prof, 1, 100, 128);
    drainprof_alloc_deregister(prof, 1, 100);

    /* Close - should be drainable */
    int drainable = drainprof_granule_close(prof, 1);
    TEST_ASSERT(drainable == 1, "Granule should be drainable");

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);
    TEST_ASSERT(snap.drainable_closes == 1, "Should have 1 drainable close");
    TEST_ASSERT(snap.pinned_closes == 0, "Should have 0 pinned closes");
    TEST_ASSERT(snap.dsr == 1.0, "DSR should be 1.0");

    drainprof_destroy(prof);
    TEST_PASS("diagnostic_drainable");
}

/* ========================================================================== */
/*  Test: Counter Consistency                                                 */
/* ========================================================================== */

int test_counter_consistency(void) {
    printf("\n=== Counter Consistency Test (Diagnostic vs Production) ===\n\n");

    /* Create two profilers: one production, one diagnostic */
    drainprof_config prod_config;
    drainprof_config_default(&prod_config);
    prod_config.mode = DRAINPROF_PRODUCTION;

    drainprof_config diag_config;
    drainprof_config_default(&diag_config);
    diag_config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prod_prof = drainprof_create_with_config(&prod_config);
    drainprof *diag_prof = drainprof_create_with_config(&diag_config);

    /* Run identical workload on both */
    for (uint64_t gid = 0; gid < 10; gid++) {
        drainprof_granule_open(prod_prof, gid);
        drainprof_granule_open(diag_prof, gid);

        /* Allocate 10 objects per granule */
        for (int i = 0; i < 10; i++) {
            drainprof_alloc_register(prod_prof, gid, gid * 100 + i, 128);
            drainprof_alloc_register(diag_prof, gid, gid * 100 + i, 128);
        }

        /* Free half */
        for (int i = 0; i < 5; i++) {
            drainprof_alloc_deregister(prod_prof, gid, gid * 100 + i);
            drainprof_alloc_deregister(diag_prof, gid, gid * 100 + i);
        }

        drainprof_granule_close(prod_prof, gid);
        drainprof_granule_close(diag_prof, gid);
    }

    /* Compare snapshots */
    drainprof_snapshot_t prod_snap, diag_snap;
    drainprof_snapshot(prod_prof, &prod_snap);
    drainprof_snapshot(diag_prof, &diag_snap);

    printf("  Production mode:\n");
    printf("    Total closes: %" PRIu64 "\n", prod_snap.total_closes);
    printf("    Pinned closes: %" PRIu64 "\n", prod_snap.pinned_closes);
    printf("    DSR: %.3f\n", prod_snap.dsr);

    printf("  Diagnostic mode:\n");
    printf("    Total closes: %" PRIu64 "\n", diag_snap.total_closes);
    printf("    Pinned closes: %" PRIu64 "\n", diag_snap.pinned_closes);
    printf("    DSR: %.3f\n\n", diag_snap.dsr);

    /* Verify consistency */
    TEST_ASSERT(prod_snap.total_closes == diag_snap.total_closes,
                "Total closes should match");
    TEST_ASSERT(prod_snap.drainable_closes == diag_snap.drainable_closes,
                "Drainable closes should match");
    TEST_ASSERT(prod_snap.pinned_closes == diag_snap.pinned_closes,
                "Pinned closes should match");
    TEST_ASSERT(prod_snap.total_allocs == diag_snap.total_allocs,
                "Total allocs should match");
    TEST_ASSERT(prod_snap.total_deallocs == diag_snap.total_deallocs,
                "Total deallocs should match");
    TEST_ASSERT(prod_snap.dsr == diag_snap.dsr,
                "DSR should match");

    drainprof_destroy(prod_prof);
    drainprof_destroy(diag_prof);

    printf("  Counters consistent across modes\n\n");
    TEST_PASS("counter_consistency");
}

/* ========================================================================== */
/*  Test: Pinning Report Generation                                           */
/* ========================================================================== */

int test_pinning_report(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;
    /* No callback - use buffering */

    drainprof *prof = drainprof_create_with_config(&config);
    TEST_ASSERT(prof != NULL, "profiler creation failed");

    /* Open granule */
    drainprof_granule_open(prof, 42);

    /* Allocate 10 objects with known IDs */
    for (int i = 0; i < 10; i++) {
        DRAINPROF_ALLOC_REGISTER(prof, 42, 1000 + i, 128);
    }

    /* Free objects at even indices */
    for (int i = 0; i < 10; i += 2) {
        drainprof_alloc_deregister(prof, 42, 1000 + i);
    }

    /* Close granule - should be pinned (5 live allocations remain) */
    int drainable = drainprof_granule_close(prof, 42);
    TEST_ASSERT(drainable == 0, "Granule should be pinned");

    /* Drain reports */
    drainprof_pinning_report *reports[10];
    uint32_t count = drainprof_drain_reports(prof, reports, 10);
    TEST_ASSERT(count == 1, "Should have exactly 1 pinning report");

    drainprof_pinning_report *report = reports[0];
    TEST_ASSERT(report != NULL, "Report should not be NULL");
    TEST_ASSERT(report->granule_id == 42, "Report granule_id should be 42");
    TEST_ASSERT(report->total_allocs == 10, "Report should show 10 total allocs");
    TEST_ASSERT(report->drained_allocs == 5, "Report should show 5 drained allocs");
    TEST_ASSERT(report->pinning_count == 5, "Report should show 5 pinning allocs");

    /* Verify pinning allocations are the odd-indexed ones */
    bool found[5] = {false, false, false, false, false};
    for (uint32_t i = 0; i < report->pinning_count; i++) {
        drainprof_alloc_id aid = report->pinning_allocs[i].alloc_id;
        /* Odd indices: 1001, 1003, 1005, 1007, 1009 */
        if (aid == 1001) found[0] = true;
        if (aid == 1003) found[1] = true;
        if (aid == 1005) found[2] = true;
        if (aid == 1007) found[3] = true;
        if (aid == 1009) found[4] = true;
    }

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(found[i], "Missing expected pinning allocation");
    }

    /* Cleanup */
    drainprof_pinning_report_free(report);
    drainprof_destroy(prof);

    TEST_PASS("pinning_report");
}

/* ========================================================================== */
/*  Test: Pinning Report Callback                                             */
/* ========================================================================== */

static int callback_invoked = 0;
static uint32_t callback_pinning_count = 0;

void test_callback(const drainprof_pinning_report *report, void *user_data) {
    int *counter = (int *)user_data;
    (*counter)++;
    callback_invoked = 1;
    callback_pinning_count = report->pinning_count;
}

int test_callback_mode(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    int callback_count = 0;
    config.on_pinning = test_callback;
    config.callback_user_data = &callback_count;

    drainprof *prof = drainprof_create_with_config(&config);

    /* Open and pin a granule */
    drainprof_granule_open(prof, 1);
    DRAINPROF_ALLOC_REGISTER(prof, 1, 100, 256);  /* Not freed - pins granule */

    callback_invoked = 0;
    callback_pinning_count = 0;
    int drainable = drainprof_granule_close(prof, 1);

    TEST_ASSERT(drainable == 0, "Granule should be pinned");
    TEST_ASSERT(callback_invoked == 1, "Callback should have been invoked");
    TEST_ASSERT(callback_count == 1, "Callback count should be 1");
    TEST_ASSERT(callback_pinning_count == 1, "Should have 1 pinning allocation");

    /* When callback is used, reports should NOT be buffered */
    drainprof_pinning_report *reports[10];
    uint32_t count = drainprof_drain_reports(prof, reports, 10);
    TEST_ASSERT(count == 0, "Should have 0 buffered reports when callback is used");

    drainprof_destroy(prof);
    TEST_PASS("callback_mode");
}

/* ========================================================================== */
/*  Test: P-Sweep with Report Validation                                      */
/* ========================================================================== */

int test_psweep_diagnostic(void) {
    printf("\n=== Diagnostic P-Sweep (Theorem 3 with Report Validation) ===\n\n");

    double p = 0.1;  /* Pin 10% of epochs */
    uint64_t epochs = 100;

    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prof = drainprof_create_with_config(&config);
    TEST_ASSERT(prof != NULL, "profiler creation failed");

    /* Track which epochs should be pinned */
    bool expected_pinned[100] = {false};

    for (uint64_t epoch = 0; epoch < epochs; epoch++) {
        drainprof_granule_open(prof, epoch);

        /* Request allocation (always freed) */
        drainprof_alloc_register(prof, epoch, epoch * 2, 128);
        drainprof_alloc_deregister(prof, epoch, epoch * 2);

        /* Session allocation (never freed) with deterministic probability p */
        if (epoch % (uint64_t)(1.0 / p) == 0) {
            DRAINPROF_ALLOC_REGISTER(prof, epoch, epoch * 2 + 1, 256);
            expected_pinned[epoch] = true;
            /* Not deregistered - pins the granule */
        }

        drainprof_granule_close(prof, epoch);
    }

    /* Verify DSR matches expected */
    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    double expected_dsr = 1.0 - p;
    double dsr_error = snap.dsr - expected_dsr;

    printf("  p=%.1f: DSR=%.3f (expected %.3f, error %.3f)\n",
           p, snap.dsr, expected_dsr, dsr_error);
    printf("  Pinned: %" PRIu64 "/%" PRIu64 "\n\n",
           snap.pinned_closes, snap.total_closes);

    TEST_ASSERT(dsr_error > -0.05 && dsr_error < 0.05,
                "DSR should be within 5% of expected");

    /* Drain all pinning reports */
    drainprof_pinning_report *reports[100];
    uint32_t report_count = drainprof_drain_reports(prof, reports, 100);

    printf("  Generated %u pinning reports\n", report_count);
    TEST_ASSERT(report_count == snap.pinned_closes,
                "Report count should match pinned_closes");

    /* Verify each report corresponds to an expected pinned epoch */
    for (uint32_t i = 0; i < report_count; i++) {
        drainprof_pinning_report *report = reports[i];
        uint64_t gid = report->granule_id;

        TEST_ASSERT(gid < epochs, "Granule ID should be in range");
        TEST_ASSERT(expected_pinned[gid], "Unexpected pinned granule");

        /* Each pinned epoch should have exactly 1 pinning allocation */
        TEST_ASSERT(report->pinning_count == 1,
                    "Each pinned epoch should have 1 pinning alloc");
        TEST_ASSERT(report->total_allocs == 2,
                    "Each epoch should have 2 total allocs");
        TEST_ASSERT(report->drained_allocs == 1,
                    "Each pinned epoch should have 1 drained alloc");

        /* Verify the pinning allocation ID */
        drainprof_alloc_id expected_aid = gid * 2 + 1;
        TEST_ASSERT(report->pinning_allocs[0].alloc_id == expected_aid,
                    "Pinning allocation ID should match expected");

        drainprof_pinning_report_free(report);
    }

    printf("  All pinning reports validated\n\n");

    drainprof_destroy(prof);
    TEST_PASS("psweep_diagnostic (Theorem 3 with reports)");
}

/**
 * Test: diagnostic_summary
 *
 * Verifies that drainprof_diagnostic_summary_compute() correctly aggregates
 * pinning reports by allocation site (file:line).
 */
int test_diagnostic_summary(void) {
    printf("\n=== Test: diagnostic_summary ===\n\n");

    /* Create diagnostic profiler without callback (buffered reports) */
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;
    config.on_pinning = NULL;  /* Buffer reports, don't callback */

    drainprof *prof = drainprof_create_with_config(&config);
    TEST_ASSERT(prof != NULL, "Profiler should be created");

    printf("  Simulating allocations from 3 different source locations...\n");

    /* Granule 0: Pinned by allocation from line 100 */
    drainprof_granule_open(prof, 0);
    drainprof_alloc_register_located(prof, 0, 1000, 256, "source.c", 100);
    /* Not freed - pins granule 0 */
    drainprof_granule_close(prof, 0);

    /* Granule 1: Pinned by allocation from line 200 */
    drainprof_granule_open(prof, 1);
    drainprof_alloc_register_located(prof, 1, 1001, 512, "source.c", 200);
    /* Not freed - pins granule 1 */
    drainprof_granule_close(prof, 1);

    /* Granule 2: Pinned by TWO allocations from line 100 */
    drainprof_granule_open(prof, 2);
    drainprof_alloc_register_located(prof, 2, 1002, 128, "source.c", 100);
    drainprof_alloc_register_located(prof, 2, 1003, 128, "source.c", 100);
    /* Both not freed - pins granule 2 */
    drainprof_granule_close(prof, 2);

    /* Granule 3: Pinned by allocation from line 300 */
    drainprof_granule_open(prof, 3);
    drainprof_alloc_register_located(prof, 3, 1004, 1024, "other.c", 300);
    /* Not freed - pins granule 3 */
    drainprof_granule_close(prof, 3);

    /* Granule 4: Drainable (all allocations freed) - no report generated */
    drainprof_granule_open(prof, 4);
    drainprof_alloc_register_located(prof, 4, 1005, 64, "source.c", 100);
    drainprof_alloc_deregister(prof, 4, 1005);
    drainprof_granule_close(prof, 4);

    printf("  Granules 0-3 pinned (4 reports), Granule 4 drainable\n\n");

    /* Compute summary */
    drainprof_diagnostic_summary *summary = drainprof_diagnostic_summary_compute(prof);
    TEST_ASSERT(summary != NULL, "Summary should be computed");

    printf("  Summary computed:\n");
    printf("    Reports analyzed: %u\n", summary->reports_analyzed);
    printf("    Unique sites: %u\n", summary->site_count);
    printf("    Total pinning allocs: %u\n\n", summary->total_pinning_allocs);

    /* Verify summary statistics */
    TEST_ASSERT(summary->reports_analyzed == 4,
                "Should analyze 4 pinning reports (granules 0-3)");
    TEST_ASSERT(summary->site_count == 3,
                "Should have 3 unique sites (source.c:100, source.c:200, other.c:300)");
    TEST_ASSERT(summary->total_pinning_allocs == 5,
                "Should have 5 total pinning allocations");

    /* Find each site and verify counts */
    int found_100 = 0, found_200 = 0, found_300 = 0;

    for (uint32_t i = 0; i < summary->site_count; i++) {
        drainprof_summary_site_entry *site = &summary->sites[i];

        if (site->site.line == 100) {
            found_100 = 1;
            printf("  Site: source.c:100\n");
            printf("    Pinning count: %u (granules pinned)\n", site->pinning_count);
            printf("    Total allocs: %u\n", site->total_allocs);
            printf("    Total bytes: %zu\n\n", site->total_bytes);

            /* source.c:100 appears in granule 0 (1 alloc) and granule 2 (2 allocs) */
            TEST_ASSERT(site->pinning_count == 2,
                        "source.c:100 should pin 2 granules");
            TEST_ASSERT(site->total_allocs == 3,
                        "source.c:100 should have 3 allocations");
            TEST_ASSERT(site->total_bytes == 256 + 128 + 128,
                        "source.c:100 should have 512 bytes total");
        } else if (site->site.line == 200) {
            found_200 = 1;
            printf("  Site: source.c:200\n");
            printf("    Pinning count: %u (granules pinned)\n", site->pinning_count);
            printf("    Total allocs: %u\n", site->total_allocs);
            printf("    Total bytes: %zu\n\n", site->total_bytes);

            /* source.c:200 appears in granule 1 (1 alloc) */
            TEST_ASSERT(site->pinning_count == 1,
                        "source.c:200 should pin 1 granule");
            TEST_ASSERT(site->total_allocs == 1,
                        "source.c:200 should have 1 allocation");
            TEST_ASSERT(site->total_bytes == 512,
                        "source.c:200 should have 512 bytes total");
        } else if (site->site.line == 300) {
            found_300 = 1;
            printf("  Site: other.c:300\n");
            printf("    Pinning count: %u (granules pinned)\n", site->pinning_count);
            printf("    Total allocs: %u\n", site->total_allocs);
            printf("    Total bytes: %zu\n\n", site->total_bytes);

            /* other.c:300 appears in granule 3 (1 alloc) */
            TEST_ASSERT(site->pinning_count == 1,
                        "other.c:300 should pin 1 granule");
            TEST_ASSERT(site->total_allocs == 1,
                        "other.c:300 should have 1 allocation");
            TEST_ASSERT(site->total_bytes == 1024,
                        "other.c:300 should have 1024 bytes total");
        }
    }

    TEST_ASSERT(found_100, "Should find source.c:100 in summary");
    TEST_ASSERT(found_200, "Should find source.c:200 in summary");
    TEST_ASSERT(found_300, "Should find other.c:300 in summary");

    printf("  All sites validated\n\n");

    /* Cleanup */
    drainprof_diagnostic_summary_free(summary);
    drainprof_destroy(prof);
    TEST_PASS("diagnostic_summary (aggregation by site)");
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
        {"diagnostic_create", test_diagnostic_create},
        {"per_alloc_tracking", test_per_alloc_tracking},
        {"diagnostic_drainable", test_diagnostic_drainable},
        {"counter_consistency", test_counter_consistency},
        {"pinning_report", test_pinning_report},
        {"callback_mode", test_callback_mode},
        {"psweep_diagnostic", test_psweep_diagnostic},
        {"diagnostic_summary", test_diagnostic_summary},
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;

    printf("Running libdrainprof diagnostic mode tests...\n\n");

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
