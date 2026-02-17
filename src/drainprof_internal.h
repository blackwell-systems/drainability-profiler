/**
 * libdrainprof - Internal structures and types
 *
 * This header is NOT part of the public API.
 */

#ifndef DRAINPROF_INTERNAL_H
#define DRAINPROF_INTERNAL_H

#include "drainprof.h"
#include <stdatomic.h>
#include <pthread.h>

/* ========================================================================== */
/*  Atomic Counters                                                           */
/* ========================================================================== */

/** Fixed-size, lock-free global counters. No heap allocation. */
typedef struct drainprof_counters {
    _Atomic uint64_t total_closes;       /**< Total granule close events */
    _Atomic uint64_t drainable_closes;   /**< Closes where all allocs freed */
    _Atomic uint64_t pinned_closes;      /**< Closes with live allocs */
    _Atomic uint64_t open_granules;      /**< Currently open granules */
    _Atomic uint64_t peak_open_granules; /**< High-water mark */
    _Atomic uint64_t total_allocs;       /**< Total alloc_register calls */
    _Atomic uint64_t total_deallocs;     /**< Total alloc_deregister calls */
} drainprof_counters;

/** Initialize counters to zero. */
void drainprof_counters_init(drainprof_counters *c);

/** Reset all counters to zero. */
void drainprof_counters_reset(drainprof_counters *c);

/* ========================================================================== */
/*  Slot Array (Production Mode Storage)                                     */
/* ========================================================================== */

/** Pre-allocated granule slot.
 *  Each slot tracks one open granule.
 *  Index = granule_id % capacity. */
typedef struct drainprof_granule_slot {
    /** Live allocations in this granule. */
    _Atomic uint32_t live_count;

    /** Occupancy flag: 1 if slot is in use, 0 if free.
     *  Prevents collisions when granule_id % capacity maps multiple IDs
     *  to the same slot. */
    _Atomic uint32_t occupied;

    /** The granule ID currently occupying this slot.
     *  Only valid when occupied == 1. */
    _Atomic uint64_t granule_id;
} drainprof_granule_slot;

/** Pre-allocated slot array for granule tracking.
 *  Lock-free access. Requires dense granule IDs. */
typedef struct drainprof_slot_array {
    drainprof_granule_slot *slots;
    uint32_t capacity;
} drainprof_slot_array;

/** Create a slot array with the given capacity.
 *  Returns NULL on allocation failure. */
drainprof_slot_array *drainprof_slot_array_create(uint32_t capacity);

/** Destroy a slot array and free resources. */
void drainprof_slot_array_destroy(drainprof_slot_array *arr);

/** Open a granule in the slot array.
 *  Returns 0 on success, -1 if slot is occupied (collision). */
int drainprof_slot_array_open(
    drainprof_slot_array *arr,
    drainprof_granule_id id
);

/** Close a granule in the slot array.
 *  Returns the live count at close time, or UINT32_MAX on error. */
uint32_t drainprof_slot_array_close(
    drainprof_slot_array *arr,
    drainprof_granule_id id
);

/** Increment live count for a granule.
 *  Returns 0 on success, -1 if granule not found. */
int drainprof_slot_array_alloc(
    drainprof_slot_array *arr,
    drainprof_granule_id id
);

/** Decrement live count for a granule.
 *  Returns 0 on success, -1 if granule not found. */
int drainprof_slot_array_dealloc(
    drainprof_slot_array *arr,
    drainprof_granule_id id
);

/* ========================================================================== */
/*  Diagnostic Mode Structures                                                */
/* ========================================================================== */

/** Per-allocation record (diagnostic mode).
 *  Stored from alloc_register until granule_close. */
typedef struct drainprof_alloc_record {
    drainprof_alloc_id alloc_id;
    drainprof_granule_id granule_id;
    struct timespec alloc_time;
    drainprof_alloc_site alloc_site;
    size_t size;
    struct timespec free_time;   /**< Zero if still live */
    bool freed;                  /**< true if freed, false if still live */
} drainprof_alloc_record;

/** Per-granule diagnostic state.
 *  Allocated on granule_open, freed at granule_close. */
typedef struct drainprof_granule_diag {
    drainprof_granule_id granule_id;
    _Atomic uint32_t live_count;
    struct timespec open_time;
    drainprof_alloc_record *allocs;   /**< Dynamic array */
    uint32_t alloc_count;
    uint32_t alloc_capacity;
    uint32_t last_freed_index;        /**< Hint for mark_freed scan optimization */
    pthread_mutex_t alloc_lock;       /**< Protects allocs array */
} drainprof_granule_diag;

/** Diagnostic slot in slot array (diagnostic mode).
 *  Unlike production mode, stores full diagnostic struct. */
typedef struct drainprof_diag_slot {
    _Atomic uint32_t occupied;
    drainprof_granule_diag *diag;  /**< NULL if slot is free */
} drainprof_diag_slot;

/** Diagnostic slot array.
 *  Each slot can hold a drainprof_granule_diag structure. */
typedef struct drainprof_slot_array_diag {
    drainprof_diag_slot *slots;
    uint32_t capacity;
} drainprof_slot_array_diag;

/** Circular buffer for buffered pinning reports. */
typedef struct drainprof_report_buffer {
    drainprof_pinning_report **reports;  /**< Array of report pointers */
    uint32_t capacity;
    uint32_t count;                      /**< Current number of reports */
    uint32_t head;                       /**< Next write position */
    uint32_t tail;                       /**< Next read position */
    pthread_mutex_t lock;                /**< Protects buffer access */
} drainprof_report_buffer;

/* Diagnostic slot array operations */
drainprof_slot_array_diag *drainprof_slot_array_diag_create(uint32_t capacity);
void drainprof_slot_array_diag_destroy(drainprof_slot_array_diag *arr);

int drainprof_slot_array_diag_open(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id,
    struct timespec *open_time
);

drainprof_granule_diag *drainprof_slot_array_diag_get(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id
);

int drainprof_slot_array_diag_close(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id,
    drainprof_granule_diag **out_diag
);

/* Diagnostic granule operations */
drainprof_granule_diag *drainprof_granule_diag_create(
    drainprof_granule_id id,
    struct timespec *open_time
);

void drainprof_granule_diag_destroy(drainprof_granule_diag *diag);

int drainprof_granule_diag_add_alloc(
    drainprof_granule_diag *diag,
    drainprof_alloc_id alloc_id,
    size_t size,
    const char *file,
    uint32_t line,
    struct timespec *alloc_time
);

int drainprof_granule_diag_mark_freed(
    drainprof_granule_diag *diag,
    drainprof_alloc_id alloc_id,
    struct timespec *free_time
);

drainprof_pinning_report *drainprof_granule_diag_generate_report(
    drainprof_granule_diag *diag,
    struct timespec *close_time
);

/* Report buffer operations */
drainprof_report_buffer *drainprof_report_buffer_create(uint32_t capacity);
void drainprof_report_buffer_destroy(drainprof_report_buffer *buf);
void drainprof_report_buffer_push(
    drainprof_report_buffer *buf,
    drainprof_pinning_report *report
);
uint32_t drainprof_report_buffer_drain(
    drainprof_report_buffer *buf,
    drainprof_pinning_report **out_reports,
    uint32_t max_reports
);

/* ========================================================================== */
/*  Main Profiler Structure                                                   */
/* ========================================================================== */

/** Opaque profiler structure. */
struct drainprof {
    drainprof_config config;
    drainprof_counters counters;

    /** Storage backend (mode and storage type determine which is allocated). */
    union {
        /* Production mode */
        drainprof_slot_array *slot_array;
        void *hash_map;  /* Reserved for future hash map implementation */

        /* Diagnostic mode */
        drainprof_slot_array_diag *slot_array_diag;
        void *hash_map_diag;  /* Reserved for future hash map implementation */
    } storage;

    /** Report buffer (diagnostic mode only, when callback is NULL). */
    drainprof_report_buffer *report_buffer;
};

#endif /* DRAINPROF_INTERNAL_H */
