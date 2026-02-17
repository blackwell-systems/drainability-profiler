/**
 * bench_hot_path.c - Benchmark alloc_register/deregister hot path
 *
 * Target: < 10ns per operation in production mode with slot array
 */

#include "../include/drainprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#define ITERATIONS 10000000  /* 10M iterations */
#define GRANULE_ID 42

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("===========================================\n");
    printf("  libdrainprof Hot Path Benchmark\n");
    printf("===========================================\n\n");

    /* Create profiler */
    drainprof *prof = drainprof_create();
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
    /*  Benchmark: alloc_register                                             */
    /* ====================================================================== */

    uint64_t start = get_nanos();

    for (uint64_t i = 0; i < ITERATIONS; i++) {
        drainprof_alloc_register(prof, GRANULE_ID, i, 256);
    }

    uint64_t end = get_nanos();
    uint64_t elapsed_ns = end - start;
    double ns_per_op = (double)elapsed_ns / (double)ITERATIONS;

    printf("alloc_register:\n");
    printf("  Total time: %.3f ms\n", elapsed_ns / 1e6);
    printf("  Per operation: %.2f ns\n", ns_per_op);
    printf("  Operations/sec: %.0f M/s\n\n", ITERATIONS / (elapsed_ns / 1e9) / 1e6);

    if (ns_per_op < 10.0) {
        printf("PASS: < 10ns target met!\n\n");
    } else if (ns_per_op < 15.0) {
        printf("  ~ ACCEPTABLE: < 15ns (slightly above target)\n\n");
    } else {
        printf("  ✗ FAIL: > 15ns (needs optimization)\n\n");
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
    printf("  Operations/sec: %.0f M/s\n\n", ITERATIONS / (elapsed_ns / 1e9) / 1e6);

    if (ns_per_op < 10.0) {
        printf("PASS: < 10ns target met!\n\n");
    } else if (ns_per_op < 15.0) {
        printf("  ~ ACCEPTABLE: < 15ns (slightly above target)\n\n");
    } else {
        printf("  ✗ FAIL: > 15ns (needs optimization)\n\n");
    }

    /* ====================================================================== */
    /*  Verify Correctness                                                    */
    /* ====================================================================== */

    /* All allocs were registered and deregistered, so close should be drainable */
    int drainable = drainprof_granule_close(prof, GRANULE_ID);

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("Correctness Check:\n");
    printf("  Total allocs: %" PRIu64 "\n", snap.total_allocs);
    printf("  Total deallocs: %" PRIu64 "\n", snap.total_deallocs);
    printf("  Granule drainable: %s\n", drainable ? "yes" : "no");
    printf("  DSR: %.3f\n\n", snap.dsr);

    if (drainable == 1 && snap.dsr == 1.0) {
        printf("Correctness verified\n\n");
    } else {
        printf("  ✗ Correctness check failed!\n\n");
        drainprof_destroy(prof);
        return 1;
    }

    printf("===========================================\n");
    printf("  Benchmark Complete\n");
    printf("===========================================\n");

    drainprof_destroy(prof);
    return 0;
}
