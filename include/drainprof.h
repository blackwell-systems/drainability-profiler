/**
 * libdrainprof - Drainability Profiler for Coarse-Grained Allocators
 *
 * Detects structural leaks by measuring the drainability satisfaction rate:
 * the fraction of granules that are fully drained at their reclaim boundary.
 *
 * Paper: Blackwell, D. (2026). Drainability: When Coarse-Grained Memory
 *        Reclamation Produces Bounded Retention. doi:10.5281/zenodo.18653776
 *
 * License: MIT
 */

#ifndef DRAINPROF_H
#define DRAINPROF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*  Version                                                                   */
/* ========================================================================== */

#define DRAINPROF_VERSION_MAJOR 0
#define DRAINPROF_VERSION_MINOR 1
#define DRAINPROF_VERSION_PATCH 0

/* ========================================================================== */
/*  Types                                                                     */
/* ========================================================================== */

/** Opaque granule identifier. Assigned by the allocator.
 *  Must be unique among currently-open granules.
 *  May be reused after drainprof_granule_close returns. */
typedef uint64_t drainprof_granule_id;

/** Opaque allocation identifier. Assigned by the allocator.
 *  Must be unique within a granule. */
typedef uint64_t drainprof_alloc_id;

/** Profiler operating mode. */
typedef enum drainprof_mode {
    DRAINPROF_PRODUCTION = 0,   /**< Minimal overhead, counters only */
    DRAINPROF_DIAGNOSTIC = 1    /**< Full tracking, pinning reports */
} drainprof_mode;

/** Granule tracking strategy. */
typedef enum drainprof_storage {
    DRAINPROF_SLOT_ARRAY = 0,   /**< Pre-allocated array, lock-free */
    DRAINPROF_HASH_MAP = 1      /**< Dynamic hash map with rwlock */
} drainprof_storage;

/** Immutable snapshot of profiler state at a point in time. */
typedef struct drainprof_snapshot_t_s {
    struct timespec timestamp;
    uint64_t total_closes;
    uint64_t drainable_closes;
    uint64_t pinned_closes;
    double dsr;                  /**< drainable / total */
    uint64_t open_granules;
    uint64_t peak_open_granules;
    uint64_t total_allocs;
    uint64_t total_deallocs;
} drainprof_snapshot_t;

/* ========================================================================== */
/*  Diagnostic Mode Types (DRAINPROF_DIAGNOSTIC only)                        */
/* ========================================================================== */

/** Source location of an allocation call.
 *  Captured via __FILE__, __LINE__ macros at the call site. */
typedef struct drainprof_alloc_site {
    const char *file;   /**< __FILE__ (static string, not owned) */
    uint32_t line;      /**< __LINE__ */
    uint32_t column;    /**< 0 if unavailable */
} drainprof_alloc_site;

/** A single allocation that pinned a granule. */
typedef struct drainprof_pinning_alloc {
    drainprof_alloc_id alloc_id;
    drainprof_alloc_site alloc_site;
    struct timespec alloc_time;
    size_t size;
    /** Lifetime delta: how long past the reclaim boundary this
     *  allocation extends. -1.0 if still live at report time. */
    double lifetime_delta_sec;
} drainprof_pinning_alloc;

/** Report generated for a single non-drainable granule close.
 *  Generated in DRAINPROF_DIAGNOSTIC mode only. */
typedef struct drainprof_pinning_report {
    drainprof_granule_id granule_id;
    struct timespec open_time;
    struct timespec close_time;
    drainprof_pinning_alloc *pinning_allocs;
    uint32_t pinning_count;
    uint32_t total_allocs;
    uint32_t drained_allocs;
} drainprof_pinning_report;

/** Callback for pinning events (diagnostic mode).
 *  Invoked synchronously during drainprof_granule_close when a granule
 *  is pinned. The report pointer is valid only during the callback.
 *  user_data is the value passed to drainprof_config.callback_user_data. */
typedef void (*drainprof_pin_callback)(
    const drainprof_pinning_report *report,
    void *user_data
);

/** Free a pinning report obtained from drainprof_drain_reports.
 *  Safe to call with NULL (no-op). */
void drainprof_pinning_report_free(drainprof_pinning_report *report);

/** Allocation site entry in diagnostic summary.
 *  Represents aggregated statistics for a unique allocation site (file:line). */
typedef struct drainprof_summary_site_entry {
    drainprof_alloc_site site;   /* Source location (file, line, column) */
    uint32_t pinning_count;       /* Number of times this site pinned a granule */
    uint32_t total_allocs;        /* Total allocations from this site */
    size_t total_bytes;           /* Total bytes allocated from this site */
} drainprof_summary_site_entry;

/** Diagnostic summary aggregating all buffered pinning reports.
 *  Groups allocations by source location (file:line) and provides
 *  per-site statistics across all buffered reports. */
typedef struct drainprof_diagnostic_summary {
    drainprof_summary_site_entry *sites;  /* Array of unique allocation sites */
    uint32_t site_count;                  /* Number of unique sites */
    uint32_t total_pinning_allocs;        /* Sum of all pinning allocations */
    uint32_t reports_analyzed;            /* Number of reports included */
} drainprof_diagnostic_summary;

/* ========================================================================== */
/*  Configuration                                                             */
/* ========================================================================== */

/** Profiler configuration. */
typedef struct drainprof_config {
    drainprof_mode mode;
    drainprof_storage storage;

    /** Slot array capacity. Only used when storage = SLOT_ARRAY.
     *  Must be >= max simultaneously open granules.
     *  Default: 1024. */
    uint32_t slot_capacity;

    /** Hash map bucket count. Only used when storage = HASH_MAP.
     *  Default: 64. */
    uint32_t bucket_count;

    /** Log DSR to stderr every N granule closes. 0 = disabled.
     *  Default: 0 (disabled). */
    uint64_t log_interval;

    /** Callback invoked on each pinned granule close.
     *  Diagnostic mode only. If NULL, reports are buffered.
     *  Default: NULL. */
    drainprof_pin_callback on_pinning;

    /** User data passed to on_pinning callback.
     *  Default: NULL. */
    void *callback_user_data;

    /** Max buffered pinning reports before oldest dropped.
     *  Only used when on_pinning is NULL in diagnostic mode.
     *  Default: 1000. */
    uint32_t max_buffered_reports;

    /** Log pinned closes to stderr (diagnostic mode).
     *  Default: false. */
    bool verbose;
} drainprof_config;

/** Fill config with default values.
 *  Convenience for callers who want to modify only some fields. */
void drainprof_config_default(drainprof_config *config);

/* ========================================================================== */
/*  Lifecycle                                                                 */
/* ========================================================================== */

/** Opaque profiler handle. */
typedef struct drainprof drainprof;

/** Create a profiler with default configuration.
 *  Returns NULL on allocation failure. */
drainprof *drainprof_create(void);

/** Create a profiler with explicit configuration.
 *  The config struct is copied; caller may free it after this call.
 *  Returns NULL on allocation failure. */
drainprof *drainprof_create_with_config(const drainprof_config *config);

/** Destroy a profiler and free all resources.
 *  All outstanding granules are implicitly closed.
 *  Safe to call with NULL (no-op). */
void drainprof_destroy(drainprof *prof);

/* ========================================================================== */
/*  Granule Lifecycle                                                         */
/* ========================================================================== */

/** Notify the profiler that a granule has been opened.
 *  Must be called before any alloc_register for this granule.
 *
 *  Production mode (slot array): O(1), lock-free.
 *  Production mode (hash map): O(1) amortized, write lock.
 *  Diagnostic mode: adds timestamp capture + diagnostic struct alloc.
 *
 *  Returns 0 on success, -1 on error (e.g., slot array full). */
int drainprof_granule_open(drainprof *prof, drainprof_granule_id id);

/** Notify the profiler that a granule's reclaim boundary has been reached.
 *  Returns 1 if the granule was drainable, 0 if pinned, -1 on error.
 *
 *  IMPORTANT PRECONDITION: The caller MUST ensure that no concurrent
 *  alloc_register or alloc_deregister calls for this granule_id are in
 *  flight when granule_close is called. All allocation activity for this
 *  granule must have completed before closing it.
 *
 *  This contract matches typical allocator usage patterns:
 *  - Epoch-based allocators: Close epoch only after advancing past it
 *  - Arena allocators: Close arena only after all users have released it
 *  - Slab allocators: Close slab only when returning to pool
 *
 *  Production mode (slot array): O(1), lock-free.
 *  Production mode (hash map): O(1) amortized, write lock.
 *  Diagnostic mode (pinned): O(n) where n = live allocs in granule. */
int drainprof_granule_close(drainprof *prof, drainprof_granule_id id);

/* ========================================================================== */
/*  Allocation Tracking                                                       */
/* ========================================================================== */

/** Register an allocation routed to a granule.
 *
 *  Production mode (slot array): O(1), lock-free (atomic increment).
 *  Production mode (hash map): O(1), read lock + atomic increment.
 *  Diagnostic mode: O(1) amortized, adds record to per-granule array.
 *
 *  Returns 0 on success, -1 on error. */
int drainprof_alloc_register(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size
);

/** Register an allocation with source location (diagnostic mode).
 *  In production mode, file/line are ignored.
 *
 *  Use the DRAINPROF_ALLOC_REGISTER macro for automatic
 *  __FILE__ / __LINE__ capture. */
int drainprof_alloc_register_located(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size,
    const char *file,
    uint32_t line
);

/** Convenience macro: captures __FILE__ and __LINE__ automatically. */
#define DRAINPROF_ALLOC_REGISTER(prof, gid, aid, sz) \
    drainprof_alloc_register_located((prof), (gid), (aid), (sz), \
                                     __FILE__, __LINE__)

/** Deregister an allocation (freed).
 *
 *  Production mode (slot array): O(1), lock-free (atomic decrement).
 *  Production mode (hash map): O(1), read lock + atomic decrement.
 *  Diagnostic mode: O(1) amortized, marks record as freed.
 *
 *  Returns 0 on success, -1 on error. */
int drainprof_alloc_deregister(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id
);

/* ========================================================================== */
/*  Queries                                                                   */
/* ========================================================================== */

/** Take a snapshot of current profiler state.
 *  Lock-free in production mode with slot array (atomic reads only).
 *  Snapshot is written to *out. */
void drainprof_snapshot(const drainprof *prof, drainprof_snapshot_t *out);

/** Reset all counters and clear all state. */
void drainprof_reset(drainprof *prof);

/* ========================================================================== */
/*  Diagnostic Mode Queries                                                   */
/* ========================================================================== */

/** Drain buffered pinning reports (diagnostic mode only).
 *  Retrieves up to max_reports from the internal buffer.
 *  Reports are removed from the buffer and ownership transferred to caller.
 *  Caller must free each report with drainprof_pinning_report_free().
 *
 *  reports: Array to receive report pointers (allocated by caller).
 *  max_reports: Maximum number of reports to retrieve.
 *
 *  Returns: Number of reports actually retrieved (may be less than max_reports). */
uint32_t drainprof_drain_reports(
    drainprof *prof,
    drainprof_pinning_report **reports,
    uint32_t max_reports
);

/** Compute diagnostic summary from buffered reports (diagnostic mode only).
 *  Aggregates all currently buffered pinning reports by allocation site.
 *  Does NOT consume the reports (they remain in buffer).
 *  Returns NULL if no reports are available, profiler is not in diagnostic mode,
 *  or on error.
 *  Caller must free the summary with drainprof_diagnostic_summary_free(). */
drainprof_diagnostic_summary *drainprof_diagnostic_summary_compute(drainprof *prof);

/** Free a diagnostic summary. Safe to call with NULL (no-op). */
void drainprof_diagnostic_summary_free(drainprof_diagnostic_summary *summary);

#ifdef __cplusplus
}
#endif

#endif /* DRAINPROF_H */
