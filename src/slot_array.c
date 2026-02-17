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
    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_granule_slot *slot = &arr->slots[slot_idx];

    /* Try to claim the slot using CAS on occupied flag */
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong(&slot->occupied, &expected, 1)) {
        /* Slot is occupied by another granule - collision */
        return -1;
    }

    /* We own the slot now. Initialize it. */
    atomic_store(&slot->granule_id, id);
    atomic_store(&slot->live_count, 0);

    return 0;
}

uint32_t drainprof_slot_array_close(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_granule_slot *slot = &arr->slots[slot_idx];

    /* Verify this slot belongs to the requested granule */
    uint64_t stored_id = atomic_load(&slot->granule_id);
    uint32_t occupied = atomic_load(&slot->occupied);

    if (!occupied || stored_id != id) {
        /* Granule not found in expected slot */
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
    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_granule_slot *slot = &arr->slots[slot_idx];

    /* Verify slot ownership (optional in production, helps catch bugs) */
#ifndef NDEBUG
    uint64_t stored_id = atomic_load(&slot->granule_id);
    uint32_t occupied = atomic_load(&slot->occupied);
    if (!occupied || stored_id != id) {
        return -1;  /* Granule not in expected slot */
    }
#endif

    /* HOT PATH: Single atomic increment, no locks */
    atomic_fetch_add(&slot->live_count, 1);
    return 0;
}

int drainprof_slot_array_dealloc(
    drainprof_slot_array *arr,
    drainprof_granule_id id
) {
    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_granule_slot *slot = &arr->slots[slot_idx];

    /* Verify slot ownership (debug only) */
#ifndef NDEBUG
    uint64_t stored_id = atomic_load(&slot->granule_id);
    uint32_t occupied = atomic_load(&slot->occupied);
    if (!occupied || stored_id != id) {
        return -1;
    }
#endif

    /* HOT PATH: Single atomic decrement, no locks */
    atomic_fetch_sub(&slot->live_count, 1);
    return 0;
}
