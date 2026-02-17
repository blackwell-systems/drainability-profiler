/**
 * counters.c - Atomic counter operations
 */

#include "drainprof_internal.h"

void drainprof_counters_init(drainprof_counters *c) {
    atomic_init(&c->total_closes, 0);
    atomic_init(&c->drainable_closes, 0);
    atomic_init(&c->pinned_closes, 0);
    atomic_init(&c->open_granules, 0);
    atomic_init(&c->peak_open_granules, 0);
    atomic_init(&c->total_allocs, 0);
    atomic_init(&c->total_deallocs, 0);
}

void drainprof_counters_reset(drainprof_counters *c) {
    atomic_store(&c->total_closes, 0);
    atomic_store(&c->drainable_closes, 0);
    atomic_store(&c->pinned_closes, 0);
    atomic_store(&c->open_granules, 0);
    atomic_store(&c->peak_open_granules, 0);
    atomic_store(&c->total_allocs, 0);
    atomic_store(&c->total_deallocs, 0);
}
