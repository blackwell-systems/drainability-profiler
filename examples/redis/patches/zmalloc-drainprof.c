/*
 * Drainability profiling instrumentation for Redis zmalloc.c
 *
 * This code instruments Redis's memory allocation wrapper to track
 * drainability by size class. Add this to zmalloc.c in Redis source.
 *
 * Size class granules:
 *   0: < 256B (small - strings, list nodes, hash entries)
 *   1: 256B - 1KB (medium)
 *   2: 1KB - 4KB (large)
 *   3: >= 4KB (huge)
 */

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "drainprof.h"

/* ============================================
 * Drainprof state
 * ============================================ */

static drainprof *g_drainprof = NULL;
static pthread_once_t g_drainprof_init_once = PTHREAD_ONCE_INIT;

#define SIZE_CLASS_SMALL  0  /* < 256B */
#define SIZE_CLASS_MEDIUM 1  /* 256B - 1KB */
#define SIZE_CLASS_LARGE  2  /* 1KB - 4KB */
#define SIZE_CLASS_HUGE   3  /* >= 4KB */

/* ============================================
 * Helper functions
 * ============================================ */

static inline uint64_t size_to_granule(size_t size) {
    if (size < 256) return SIZE_CLASS_SMALL;
    if (size < 1024) return SIZE_CLASS_MEDIUM;
    if (size < 4096) return SIZE_CLASS_LARGE;
    return SIZE_CLASS_HUGE;
}

static void drainprof_initialize(void) {
    g_drainprof = drainprof_create();
    if (g_drainprof) {
        /* Open all size class granules (never closed) */
        drainprof_granule_open(g_drainprof, SIZE_CLASS_SMALL);
        drainprof_granule_open(g_drainprof, SIZE_CLASS_MEDIUM);
        drainprof_granule_open(g_drainprof, SIZE_CLASS_LARGE);
        drainprof_granule_open(g_drainprof, SIZE_CLASS_HUGE);
    }
}

static inline void ensure_drainprof_initialized(void) {
    pthread_once(&g_drainprof_init_once, drainprof_initialize);
}

/* ============================================
 * Instrumented allocation functions
 * ============================================
 *
 * These replace the existing zmalloc/zfree implementations.
 * The key challenge: zfree(ptr) doesn't know the original size.
 *
 * Solution: Use jemalloc's je_sallocx() to query allocated size.
 * This is fast (~5ns) and supported by jemalloc 3.0+.
 */

#ifdef USE_JEMALLOC

/* Forward declare jemalloc functions */
extern void *je_malloc(size_t size);
extern void *je_calloc(size_t count, size_t size);
extern void *je_realloc(void *ptr, size_t size);
extern void je_free(void *ptr);
extern size_t je_sallocx(const void *ptr, int flags);

void *zmalloc(size_t size) {
    void *ptr = je_malloc(size);

    if (ptr) {
        /* Update Redis's internal memory accounting */
        update_zmalloc_stat_alloc(je_sallocx(ptr, 0));

        /* Track with drainprof */
        ensure_drainprof_initialized();
        if (g_drainprof) {
            uint64_t granule_id = size_to_granule(size);
            drainprof_alloc_register(g_drainprof, granule_id, (uint64_t)ptr, size);
        }
    }

    return ptr;
}

void *zcalloc(size_t size) {
    void *ptr = je_calloc(1, size);

    if (ptr) {
        update_zmalloc_stat_alloc(je_sallocx(ptr, 0));

        ensure_drainprof_initialized();
        if (g_drainprof) {
            uint64_t granule_id = size_to_granule(size);
            drainprof_alloc_register(g_drainprof, granule_id, (uint64_t)ptr, size);
        }
    }

    return ptr;
}

void *zrealloc(void *ptr, size_t size) {
    size_t old_size = 0;
    uint64_t old_granule = 0;

    if (ptr) {
        old_size = je_sallocx(ptr, 0);
        old_granule = size_to_granule(old_size);
    }

    void *newptr = je_realloc(ptr, size);

    if (newptr) {
        size_t new_size = je_sallocx(newptr, 0);

        /* Update Redis accounting */
        update_zmalloc_stat_free(old_size);
        update_zmalloc_stat_alloc(new_size);

        /* Update drainprof */
        ensure_drainprof_initialized();
        if (g_drainprof) {
            /* Deregister old allocation */
            if (ptr) {
                drainprof_alloc_deregister(g_drainprof, old_granule, (uint64_t)ptr);
            }

            /* Register new allocation */
            uint64_t new_granule = size_to_granule(size);
            drainprof_alloc_register(g_drainprof, new_granule, (uint64_t)newptr, size);
        }
    }

    return newptr;
}

void zfree(void *ptr) {
    if (ptr == NULL) return;

    /* Query allocation size from jemalloc */
    size_t size = je_sallocx(ptr, 0);

    /* Update drainprof before freeing */
    ensure_drainprof_initialized();
    if (g_drainprof) {
        uint64_t granule_id = size_to_granule(size);
        drainprof_alloc_deregister(g_drainprof, granule_id, (uint64_t)ptr);
    }

    /* Update Redis accounting and free */
    update_zmalloc_stat_free(size);
    je_free(ptr);
}

#else /* !USE_JEMALLOC */

/*
 * For libc malloc, we don't have je_sallocx().
 * Options:
 * 1. Maintain shadow hashmap (adds overhead)
 * 2. Don't track with drainprof on non-jemalloc systems
 *
 * For now, we only support jemalloc instrumentation.
 */

#warning "libdrainprof instrumentation only works with jemalloc"

/* Fallback: original zmalloc implementations without instrumentation */
void *zmalloc(size_t size) {
    void *ptr = malloc(size + PREFIX_SIZE);
    /* ... original zmalloc logic ... */
    return ptr;
}

void zfree(void *ptr) {
    /* ... original zfree logic ... */
    free(ptr);
}

#endif /* USE_JEMALLOC */

/* ============================================
 * INFO command integration
 * ============================================
 *
 * Add these metrics to the MEMORY section in server.c's INFO handler:
 */

/*
void drainprofCommand(client *c) {
    if (g_drainprof == NULL) {
        addReplyError(c, "drainprof not initialized (requires jemalloc)");
        return;
    }

    drainprof_snapshot_t snap;
    drainprof_snapshot(g_drainprof, &snap);

    sds info = sdsempty();

    info = sdscatprintf(info,
        "# Drainability Profiling\r\n"
        "drainprof_enabled:yes\r\n"
        "drainprof_dsr:%.4f\r\n"
        "drainprof_allocations:%llu\r\n"
        "drainprof_deallocations:%llu\r\n"
        "drainprof_active:%llu\r\n",
        snap.dsr,
        (unsigned long long)snap.total_allocations,
        (unsigned long long)snap.total_deallocations,
        (unsigned long long)snap.active_allocations
    );

    // Per-size-class metrics
    // Note: Requires extended drainprof API to query per-granule stats
    // For now, overall DSR shows the fragmentation state

    addReplyBulkSds(c, info);
}
*/

/* ============================================
 * Integration notes
 * ============================================
 *
 * To apply this patch:
 *
 * 1. Add to Redis Makefile:
 *    LDFLAGS += -ldrainprof -lpthread
 *    CFLAGS += -I/usr/local/include -DENABLE_DRAINPROF
 *
 * 2. In zmalloc.c, replace zmalloc/zfree/zcalloc/zrealloc with above
 *
 * 3. In server.c, add drainprof metrics to INFO MEMORY:
 *    if (g_drainprof) {
 *        drainprof_snapshot_t snap;
 *        drainprof_snapshot(g_drainprof, &snap);
 *        info = sdscatprintf(info,
 *            "mem_drainability_ratio:%.4f\r\n",
 *            snap.dsr);
 *    }
 *
 * 4. Build Redis:
 *    make BUILD_TLS=yes
 *
 * Expected behavior after running fragmentation test:
 * - mem_fragmentation_ratio: 2.16 (traditional metric)
 * - mem_drainability_ratio: 0.25 (DSR ~25%, predicts fragmentation)
 */
