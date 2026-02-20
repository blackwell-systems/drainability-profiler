/**
 * slot_array.c - Lock-free slot array for granule tracking
 *
 * Performance-critical code. All hot-path operations (alloc/dealloc)
 * are lock-free atomic operations.
 */

#include "drainprof_internal.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*  Hash Function for Granule IDs                                             */
/* ========================================================================== */

/* MurmurHash3 finalizer - distributes bits evenly for page-aligned addresses.
 * Critical for slot arrays where slab addresses have many trailing zeros. */
static inline uint32_t hash_granule_id(uint64_t id) {
    id ^= id >> 33;
    id *= 0xff51afd7ed558ccdULL;
    id ^= id >> 33;
    id *= 0xc4ceb9fe1a85ec53ULL;
    id ^= id >> 33;
    return (uint32_t)id;
}

/* ========================================================================== */
/*  Slot Lookup with Linear Probing                                           */
/* ========================================================================== */

/* Find the slot containing the given granule ID.
 * Returns slot pointer on success, NULL if not found. */
static inline drainprof_granule_slot *find_slot(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    uint32_t hash = hash_granule_id(id);
    uint32_t slot_idx = hash % arr->capacity;

    /* Linear probe to find the granule */
    for (uint32_t probe = 0; probe < arr->capacity; probe++) {
        uint32_t idx = (slot_idx + probe) % arr->capacity;
        drainprof_granule_slot *slot = &arr->slots[idx];

        uint32_t occupied = atomic_load(&slot->occupied);
        if (!occupied) {
            /* Hit an empty slot - granule not found */
            return NULL;
        }

        uint64_t stored_id = atomic_load(&slot->granule_id);
        if (stored_id == id) {
            /* Found it */
            return slot;
        }
        /* Wrong granule - continue probing */
    }

    /* Searched entire array - not found */
    return NULL;
}

/* ========================================================================== */
/*  Slot Array Creation/Destruction                                           */
/* ========================================================================== */

drainprof_slot_array *drainprof_slot_array_create(uint32_t capacity) {
    if (capacity == 0) {
        return NULL;
    }

    drainprof_slot_array *arr = malloc(sizeof(drainprof_slot_array));
    if (!arr) {
        return NULL;
    }

    arr->slots = calloc(capacity, sizeof(drainprof_granule_slot));
    if (!arr->slots) {
        free(arr);
        return NULL;
    }

    arr->capacity = capacity;

    /* Initialize all slots to zero (calloc does this, but be explicit) */
    for (uint32_t i = 0; i < capacity; i++) {
        atomic_init(&arr->slots[i].live_count, 0);
        atomic_init(&arr->slots[i].occupied, 0);
        atomic_init(&arr->slots[i].granule_id, 0);
    }

    return arr;
}

void drainprof_slot_array_destroy(drainprof_slot_array *arr) {
    if (!arr) {
        return;
    }
    free(arr->slots);
    free(arr);
}

/* ========================================================================== */
/*  Granule Lifecycle Operations                                              */
/* ========================================================================== */

int drainprof_slot_array_open(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    uint32_t hash = hash_granule_id(id);
    uint32_t slot_idx = hash % arr->capacity;

    /* Linear probing: try up to capacity slots to find an empty one */
    for (uint32_t probe = 0; probe < arr->capacity; probe++) {
        uint32_t idx = (slot_idx + probe) % arr->capacity;
        drainprof_granule_slot *slot = &arr->slots[idx];

        /* Check if this slot already holds our granule (idempotent open) */
        uint32_t occupied = atomic_load(&slot->occupied);
        if (occupied) {
            uint64_t stored_id = atomic_load(&slot->granule_id);
            if (stored_id == id) {
                /* Granule already registered - idempotent success */
                return 0;
            }
            /* Slot occupied by different granule - continue probing */
            continue;
        }

        /* Try to claim this empty slot using CAS on occupied flag */
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong(&slot->occupied, &expected, 1)) {
            /* We own the slot now. Initialize it. */
            atomic_store(&slot->granule_id, id);
            atomic_store(&slot->live_count, 0);
            return 0;
        }
        /* CAS failed - another thread claimed it. Continue probing. */
    }

    /* Slot array is full */
    return -1;
}

uint32_t drainprof_slot_array_close(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    drainprof_granule_slot *slot = find_slot(arr, id);
    if (!slot) {
        /* Granule not found */
        return UINT32_MAX;
    }

    /* Read the final live count */
    uint32_t live_count = atomic_load(&slot->live_count);

    /* Clear the slot for reuse.
     *
     * CORRECTNESS NOTE: This store sequence is safe because the API contract
     * requires that no concurrent alloc_register/deregister calls for this
     * granule_id are in flight when close is called. The caller must ensure
     * all allocation activity has quiesced before closing.
     *
     * Without this precondition, there would be a race:
     *   Thread A: reads live_count = 3
     *   Thread B: alloc_register does atomic_fetch_add, making it 4
     *   Thread A: atomic_store(&live_count, 0) clobbers Thread B's increment
     *
     * The precondition eliminates this race. Epoch-based allocators satisfy
     * this naturally (close epoch only after advancing past it). Arena and
     * slab allocators close only when all users have released the granule.
     *
     * Clear order: live_count → granule_id → occupied (release slot last). */
    atomic_store(&slot->live_count, 0);
    atomic_store(&slot->granule_id, 0);
    atomic_store(&slot->occupied, 0);  /* Release the slot */

    return live_count;
}

/* ========================================================================== */
/*  Hot Path: Allocation Tracking                                             */
/* ========================================================================== */

int drainprof_slot_array_alloc(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    drainprof_granule_slot *slot = find_slot(arr, id);
    if (!slot) {
        return -1;  /* Granule not found */
    }

    /* HOT PATH: Single atomic increment, no locks */
    atomic_fetch_add(&slot->live_count, 1);
    return 0;
}

int drainprof_slot_array_dealloc(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    drainprof_granule_slot *slot = find_slot(arr, id);
    if (!slot) {
        return -1;  /* Granule not found */
    }

    /* HOT PATH: Single atomic decrement, no locks */
    atomic_fetch_sub(&slot->live_count, 1);
    return 0;
}
