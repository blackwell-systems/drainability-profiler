/**
 * bench_diagnostic.c - Benchmark diagnostic mode overhead
 *
 * Measures the additional cost of per-allocation tracking.
 * Expected: ~50ns per operation in diagnostic mode.
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#define ITERATIONS 1000000  /* 1M iterations (less than production bench) */
#define GRANULE_ID 42

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("===========================================\n");
    printf("  libdrainprof Diagnostic Mode Benchmark\n");
    printf("===========================================\n\n");

    /* Create diagnostic profiler without callback (buffered reports) */
    drainprof_config config;
    drainprof_config_default(&config);
    config.mode = DRAINPROF_DIAGNOSTIC;

    drainprof *prof = drainprof_create_with_config(&config);
    if (!prof) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        return 1;
    }

    /* Open a single granule */
    if (drainprof_granule_open(prof, GRANULE_ID) != 0) {
        fprintf(stderr, "ERROR: Failed to open granule\n");
        return 1;
    }

    printf("Iterations: %d\n", ITERATIONS);
    printf("Granule ID: %lu\n\n", (unsigned long)GRANULE_ID);

    /* ====================================================================== */
    /*  Benchmark: alloc_register_located (with source location)              */
    /* ====================================================================== */

    uint64_t start = get_nanos();

    for (uint64_t i = 0; i < ITERATIONS; i++) {
        DRAINPROF_ALLOC_REGISTER(prof, GRANULE_ID, i, 256);
    }

    uint64_t end = get_nanos();
    uint64_t elapsed_ns = end - start;
    double ns_per_op = (double)elapsed_ns / (double)ITERATIONS;

    printf("alloc_register_located (with __FILE__/__LINE__):\n");
    printf("  Total time: %.3f ms\n", elapsed_ns / 1e6);
    printf("  Per operation: %.2f ns\n", ns_per_op);
    printf("  Operations/sec: %.0f K/s\n\n", ITERATIONS / (elapsed_ns / 1e9) / 1e3);

    if (ns_per_op < 50.0) {
        printf("  PASS: < 50ns target met!\n\n");
    } else if (ns_per_op < 100.0) {
        printf("  ACCEPTABLE: < 100ns (diagnostic overhead expected)\n\n");
    } else {
        printf("  WARNING: > 100ns (may need optimization)\n\n");
    }

    /* ====================================================================== */
    /*  Benchmark: alloc_deregister                                           */
    /* ====================================================================== */

    start = get_nanos();

    for (uint64_t i = 0; i < ITERATIONS; i++) {
        drainprof_alloc_deregister(prof, GRANULE_ID, i);
    }

    end = get_nanos();
    elapsed_ns = end - start;
    ns_per_op = (double)elapsed_ns / (double)ITERATIONS;

    printf("alloc_deregister:\n");
    printf("  Total time: %.3f ms\n", elapsed_ns / 1e6);
    printf("  Per operation: %.2f ns\n", ns_per_op);
    printf("  Operations/sec: %.0f K/s\n\n", ITERATIONS / (elapsed_ns / 1e9) / 1e3);

    if (ns_per_op < 50.0) {
        printf("  PASS: < 50ns target met!\n\n");
    } else if (ns_per_op < 100.0) {
        printf("  ACCEPTABLE: < 100ns (diagnostic overhead expected)\n\n");
    } else {
        printf("  WARNING: > 100ns (may need optimization)\n\n");
    }

    /* ====================================================================== */
    /*  Verify Correctness                                                    */
    /* ====================================================================== */

    /* All allocs registered and deregistered, so close should be drainable */
    int drainable = drainprof_granule_close(prof, GRANULE_ID);

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("Correctness Check:\n");
    printf("  Total allocs: %" PRIu64 "\n", snap.total_allocs);
    printf("  Total deallocs: %" PRIu64 "\n", snap.total_deallocs);
    printf("  Granule drainable: %s\n", drainable ? "yes" : "no");
    printf("  DSR: %.3f\n\n", snap.dsr);

    if (drainable == 1 && snap.dsr == 1.0) {
        printf("  Correctness verified\n\n");
    } else {
        printf("  Correctness check failed!\n\n");
        drainprof_destroy(prof);
        return 1;
    }

    /* No pinning reports should be buffered (granule was drainable) */
    drainprof_pinning_report *reports[10];
    uint32_t report_count = drainprof_drain_reports(prof, reports, 10);
    printf("Buffered reports: %u\n", report_count);

    if (report_count == 0) {
        printf("  (Expected: 0 since granule was drainable)\n\n");
    }

    printf("===========================================\n");
    printf("  Benchmark Complete\n");
    printf("===========================================\n\n");

    printf("Summary:\n");
    printf("  Diagnostic mode overhead: ~%.1fx slower than production (< 2ns)\n",
           ns_per_op / 2.0);
    printf("  Use diagnostic mode during investigation, not in production.\n");

    drainprof_destroy(prof);
    return 0;
}
