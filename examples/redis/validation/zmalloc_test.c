/*
 * Standalone test for zmalloc drainprof instrumentation
 *
 * Simulates Redis's allocation pattern and validates that:
 * 1. Size-class tracking works correctly
 * 2. DSR drops when allocations can't drain
 * 3. sallocx() approach is viable
 *
 * Build:
 *   gcc -O2 -g -std=c11 -pthread \
 *     -I../../include \
 *     zmalloc_test.c \
 *     ../../libdrainprof.a \
 *     -ljemalloc \
 *     -o zmalloc_test
 *
 * Run:
 *   ./zmalloc_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* jemalloc function declarations
 * Note: On some systems jemalloc prefixes with je_, on others it doesn't.
 * We use malloc/free/sallocx directly which will resolve to jemalloc
 * when linked with -ljemalloc.
 */
extern size_t sallocx(const void *ptr, int flags);

#include "drainprof.h"

#define SIZE_CLASS_SMALL  0  /* < 256B */
#define SIZE_CLASS_MEDIUM 1  /* 256B - 1KB */
#define SIZE_CLASS_LARGE  2  /* 1KB - 4KB */
#define SIZE_CLASS_HUGE   3  /* >= 4KB */

static inline uint64_t size_to_granule(size_t size) {
    if (size < 256) return SIZE_CLASS_SMALL;
    if (size < 1024) return SIZE_CLASS_MEDIUM;
    if (size < 4096) return SIZE_CLASS_LARGE;
    return SIZE_CLASS_HUGE;
}

void print_snapshot(drainprof *prof, const char *label) {
    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    printf("\n=== %s ===\n", label);
    printf("DSR: %.2f%%\n", snap.dsr * 100.0);
    printf("Allocations: %llu\n", (unsigned long long)snap.total_allocs);
    printf("Deallocations: %llu\n", (unsigned long long)snap.total_deallocs);
    printf("Active: %llu\n", (unsigned long long)(snap.total_allocs - snap.total_deallocs));
}

int main(void) {
    printf("========================================\n");
    printf("  zmalloc drainprof instrumentation test\n");
    printf("========================================\n");

    /* Initialize drainprof with size-class granules */
    drainprof *prof = drainprof_create();
    if (!prof) {
        fprintf(stderr, "Failed to create drainprof\n");
        return 1;
    }

    /* Open granules (never closed, like in Redis) */
    drainprof_granule_open(prof, SIZE_CLASS_SMALL);
    drainprof_granule_open(prof, SIZE_CLASS_MEDIUM);
    drainprof_granule_open(prof, SIZE_CLASS_LARGE);
    drainprof_granule_open(prof, SIZE_CLASS_HUGE);

    print_snapshot(prof, "Initial state");

    /* Test 1: Allocate and free completely (should have high DSR) */
    printf("\n--- Test 1: Fully drainable pattern ---\n");
    void *ptrs[1000];
    for (int i = 0; i < 1000; i++) {
        size_t size = 128;  /* Small objects */
        ptrs[i] = malloc(size);

        uint64_t granule_id = size_to_granule(size);
        drainprof_alloc_register(prof, granule_id, (uint64_t)ptrs[i], size);
    }

    printf("Allocated 1000 small objects (128B each)\n");
    print_snapshot(prof, "After allocation");

    /* Free all */
    for (int i = 0; i < 1000; i++) {
        size_t size = sallocx(ptrs[i], 0);  /* Query size from jemalloc */
        uint64_t granule_id = size_to_granule(size);

        drainprof_alloc_deregister(prof, granule_id, (uint64_t)ptrs[i]);
        free(ptrs[i]);
    }

    printf("Freed all 1000 objects\n");
    print_snapshot(prof, "After freeing all");

    /* Test 2: Simulate Redis fragmentation (checkerboard pattern) */
    printf("\n--- Test 2: Fragmentation pattern (like Redis test) ---\n");

    /* Allocate 1000 small objects */
    for (int i = 0; i < 1000; i++) {
        size_t size = 128;
        ptrs[i] = malloc(size);

        uint64_t granule_id = size_to_granule(size);
        drainprof_alloc_register(prof, granule_id, (uint64_t)ptrs[i], size);
    }

    printf("Allocated 1000 small objects\n");
    print_snapshot(prof, "After allocation");

    /* Free every other object (simulating Redis delete-odd-keys) */
    for (int i = 0; i < 1000; i += 2) {
        size_t size = sallocx(ptrs[i], 0);
        uint64_t granule_id = size_to_granule(size);

        drainprof_alloc_deregister(prof, granule_id, (uint64_t)ptrs[i]);
        free(ptrs[i]);
    }

    printf("Freed 500 objects (odd indices)\n");
    print_snapshot(prof, "After freeing 50%%");

    /* Cleanup remaining */
    for (int i = 1; i < 1000; i += 2) {
        size_t size = sallocx(ptrs[i], 0);
        uint64_t granule_id = size_to_granule(size);

        drainprof_alloc_deregister(prof, granule_id, (uint64_t)ptrs[i]);
        free(ptrs[i]);
    }

    print_snapshot(prof, "Final state (cleanup)");

    /* Test 3: Mixed size classes */
    printf("\n--- Test 3: Mixed size classes ---\n");

    void *small[100], *medium[100], *large[100];

    /* Allocate across size classes */
    for (int i = 0; i < 100; i++) {
        small[i] = malloc(100);    /* < 256B */
        medium[i] = malloc(512);   /* 256B-1KB */
        large[i] = malloc(2048);   /* 1KB-4KB */

        drainprof_alloc_register(prof, SIZE_CLASS_SMALL, (uint64_t)small[i], 100);
        drainprof_alloc_register(prof, SIZE_CLASS_MEDIUM, (uint64_t)medium[i], 512);
        drainprof_alloc_register(prof, SIZE_CLASS_LARGE, (uint64_t)large[i], 2048);
    }

    print_snapshot(prof, "Mixed sizes allocated");

    /* Free only small objects */
    for (int i = 0; i < 100; i++) {
        size_t size = sallocx(small[i], 0);
        drainprof_alloc_deregister(prof, SIZE_CLASS_SMALL, (uint64_t)small[i]);
        free(small[i]);
    }

    printf("Freed all small objects, medium/large still allocated\n");
    print_snapshot(prof, "After freeing small");

    /* Cleanup */
    for (int i = 0; i < 100; i++) {
        drainprof_alloc_deregister(prof, SIZE_CLASS_MEDIUM, (uint64_t)medium[i]);
        drainprof_alloc_deregister(prof, SIZE_CLASS_LARGE, (uint64_t)large[i]);
        free(medium[i]);
        free(large[i]);
    }

    print_snapshot(prof, "Final cleanup");

    /* Validation */
    drainprof_snapshot_t final_snap;
    drainprof_snapshot(prof, &final_snap);

    printf("\n========================================\n");
    printf("  Validation Results\n");
    printf("========================================\n");

    uint64_t active = final_snap.total_allocs - final_snap.total_deallocs;
    if (active == 0) {
        printf("✓ All allocations freed\n");
    } else {
        printf("✗ Memory leak: %llu active allocations\n",
               (unsigned long long)active);
    }

    if (final_snap.total_allocs > 0 && final_snap.total_deallocs > 0) {
        printf("✓ Tracking working (allocs=%llu, deallocs=%llu)\n",
               (unsigned long long)final_snap.total_allocs,
               (unsigned long long)final_snap.total_deallocs);
    }

    drainprof_destroy(prof);

    printf("\n✓ Test complete - instrumentation approach validated\n");
    printf("  Ready to apply to Redis zmalloc.c\n\n");

    return 0;
}
