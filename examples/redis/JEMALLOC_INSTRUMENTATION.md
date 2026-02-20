# jemalloc Extent-Level Instrumentation Strategy

## Goal

Instrument Redis's bundled jemalloc to track extent/slab drainability:
- **Granule = jemalloc extent** (2MB memory region, or smaller slabs)
- Measure whether extents can be returned to the OS when all objects are freed
- Detect structural leaks where pinned extents prevent memory reclamation

## jemalloc Architecture Overview

### Extent Lifecycle

```
1. Extent allocation (from OS or dirty/muzzy pool):
   extent_alloc_wrapper() → extent_t created

2. Object allocation from extent:
   arena_malloc_small() → pulls object from slab
   arena_malloc_large() → entire extent is one object

3. Object deallocation:
   arena_dalloc_small() → returns object to slab
   arena_dalloc_large() → entire extent freed

4. Extent reclamation (when fully empty):
   extent_dalloc_wrapper() → returned to OS or recycled
```

### Key Data Structures

```c
// jemalloc/include/jemalloc/internal/extent_structs.h
struct extent_s {
    void *e_addr;           // Base address of extent
    size_t e_size;          // Size in bytes
    arena_t *e_arena;       // Owning arena
    // ... slab metadata for small allocations
};
```

### Critical Files in Redis's Bundled jemalloc

```
redis/deps/jemalloc/src/
├── arena.c             // Per-thread allocation arenas
├── extent.c            // Extent management (alloc/dealloc)
├── extent_dss.c        // DSS (sbrk) extent allocation
├── extent_mmap.c       // mmap extent allocation
├── large.c             // Large object allocation (>= 4KB)
└── tcache.c            // Thread cache for small objects
```

## Instrumentation Points

### 1. Extent Creation (extent.c)

**Function:** `extent_alloc_wrapper()`
- Called when jemalloc needs a new extent from the OS
- This is where we call `drainprof_granule_open()`

```c
extent_t *
extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena, ...) {
    extent_t *extent = extent_alloc_wrapper_impl(...);

    if (extent != NULL && g_drainprof != NULL) {
        void *addr = extent_addr_get(extent);
        drainprof_granule_open(g_drainprof, (uint64_t)addr);
    }

    return extent;
}
```

**Challenge:** There are multiple extent allocation paths:
- `extent_alloc_mmap()` - fresh mmap
- `extents_alloc()` - from dirty/muzzy pool (recycled)
- Need to instrument the common entry point

### 2. Object Allocation (arena.c)

**Function:** `arena_malloc_small()` (for objects < ~4KB)

```c
void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, size_t size, ...) {
    void *ptr = arena_malloc_small_impl(...);

    if (ptr != NULL && g_drainprof != NULL) {
        // Lookup which extent this object belongs to
        extent_t *extent = iealloc(tsdn, ptr);
        void *extent_addr = extent_addr_get(extent);

        drainprof_alloc_register(g_drainprof,
                                 (uint64_t)extent_addr,
                                 (uint64_t)ptr,
                                 size);
    }

    return ptr;
}
```

**Function:** `arena_malloc_large()` (for objects >= ~4KB)

For large objects, the extent IS the allocation, so:

```c
void *
arena_malloc_large(tsdn_t *tsdn, arena_t *arena, size_t size, ...) {
    extent_t *extent = large_palloc(...);

    if (extent != NULL && g_drainprof != NULL) {
        void *addr = extent_addr_get(extent);

        // Open and immediately register single allocation
        drainprof_granule_open(g_drainprof, (uint64_t)addr);
        drainprof_alloc_register(g_drainprof, (uint64_t)addr, (uint64_t)addr, size);
    }

    return extent != NULL ? extent_addr_get(extent) : NULL;
}
```

### 3. Object Deallocation (arena.c)

**Function:** `arena_dalloc_small()`

```c
void
arena_dalloc_small(tsdn_t *tsdn, void *ptr) {
    if (g_drainprof != NULL) {
        extent_t *extent = iealloc(tsdn, ptr);
        void *extent_addr = extent_addr_get(extent);

        drainprof_alloc_deregister(g_drainprof,
                                    (uint64_t)extent_addr,
                                    (uint64_t)ptr);
    }

    arena_dalloc_small_impl(...);
}
```

**Function:** `arena_dalloc_large()`

```c
void
arena_dalloc_large(tsdn_t *tsdn, void *ptr) {
    extent_t *extent = iealloc(tsdn, ptr);

    if (g_drainprof != NULL) {
        void *addr = extent_addr_get(extent);

        // Deregister and close (large objects own entire extent)
        drainprof_alloc_deregister(g_drainprof, (uint64_t)addr, (uint64_t)addr);
        drainprof_granule_close(g_drainprof, (uint64_t)addr);
    }

    large_dalloc_impl(...);
}
```

### 4. Extent Reclamation (extent.c)

**Function:** `extent_dalloc_wrapper()`
- Called when extent is returned to OS or moved to dirty pool
- This is where we call `drainprof_granule_close()`

```c
void
extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
    if (g_drainprof != NULL) {
        void *addr = extent_addr_get(extent);
        int drainable = drainprof_granule_close(g_drainprof, (uint64_t)addr);

        // If drainable=0, this extent was pinned by remaining allocations
        // This is a bug in our instrumentation or jemalloc's logic
        assert(drainable == 1);
    }

    extent_dalloc_wrapper_impl(tsdn, arena, extent);
}
```

**Challenge:** jemalloc doesn't always return extents to OS immediately:
- Extents go to "dirty" pool first (recently freed, cached)
- Then "muzzy" pool (partially unmapped)
- Finally returned to OS via munmap

We need to decide: do we close on entry to dirty pool, or only on actual munmap?

**Decision:** Close on entry to dirty pool. Once an extent is fully empty, it's "drainable" even if cached. This matches the semantic: "could this memory be reclaimed?"

## Performance Considerations

### 1. extent_lookup() Overhead

The biggest cost is finding which extent a pointer belongs to on every alloc/free:

```c
extent_t *extent = iealloc(tsdn, ptr);  // Expensive!
```

jemalloc's `iealloc()` uses a radix tree lookup - O(log n) where n = number of extents.

**Mitigation options:**
- **A) Accept the overhead** - Redis is I/O bound, allocator overhead is secondary
- **B) Sampling** - Only instrument 1% of allocations (loses accuracy)
- **C) Extent caching** - Thread-local cache of recently used extent addresses

**Recommendation:** Start with A, measure overhead, optimize if needed.

### 2. Atomic Contention

libdrainprof uses lock-free atomics for counters. With many threads hitting the same extent:

```c
drainprof_alloc_register(prof, extent_addr, ...);  // Atomic increment
```

This could cause cache line bouncing.

**Mitigation:** libdrainprof's production mode is already optimized for this (separate cache lines per granule).

## Implementation Plan

### Phase 1: Minimal Proof-of-Concept

**Goal:** Show that extent-level tracking works at all

1. Clone Redis with bundled jemalloc
2. Instrument only:
   - `extent_alloc_wrapper()` → open
   - `extent_dalloc_wrapper()` → close
   - Skip per-object tracking initially
3. Run fragmentation test, check that extents are being tracked
4. Verify DSR = 100% when no fragmentation (all extents close when empty)

### Phase 2: Per-Object Tracking

**Goal:** Track individual allocations within extents

1. Instrument `arena_malloc_small()` → register
2. Instrument `arena_dalloc_small()` → deregister
3. Run fragmentation test again
4. **Expected result:** DSR drops to ~20-30% when 50% of keys deleted
   - Many extents have some objects freed, but not all
   - Those extents can't be reclaimed (structural leak detected!)

### Phase 3: Complete Coverage

**Goal:** Handle all allocation types

1. Add large object tracking (`arena_malloc_large()`, `arena_dalloc_large()`)
2. Handle tcache (thread cache) allocations
3. Add INFO command integration to expose DSR

### Phase 4: Validation

**Goal:** Prove correctness and measure performance

1. Valgrind clean (0 leaks) + libdrainprof shows DSR < 30% = success
2. Benchmark overhead: run redis-benchmark with/without instrumentation
3. Compare DSR to mem_fragmentation_ratio over time
4. Plot correlation graph

## Expected Results

After running the fragmentation workload (create 1M keys, delete odd keys):

| Metric | Before Delete | After Delete | Interpretation |
|--------|--------------|--------------|----------------|
| Keys | 1,000,000 | 500,000 | 50% objects freed |
| RSS | 160MB | 183MB | Memory not reclaimed |
| mem_fragmentation_ratio | 1.14 | 2.16 | Traditional metric (reactive) |
| **DSR (libdrainprof)** | **~95%** | **~25%** | **Novel result (predictive)** |

**The key insight:** DSR drops because:
- Before: Most extents are fully utilized, when freed they drain completely
- After: Most extents have 1-2 remaining keys, pinning the entire 2MB extent
- jemalloc can't return these extents to OS (structural leak)
- This is invisible to Valgrind (all objects ARE freed) but visible to DSR

## Files to Create

```
examples/redis/
├── patches/
│   ├── jemalloc-extent-tracking.patch     # extent alloc/dealloc hooks
│   ├── jemalloc-object-tracking.patch     # per-object register/deregister
│   └── redis-info-drainprof.patch         # INFO command integration
├── docs/
│   └── JEMALLOC_INSTRUMENTATION.md        # This file
└── validation/
    └── extent_lifecycle_test.c            # Standalone test of extent tracking
```

## Next Steps

1. Clone Redis source locally
2. Locate the exact function signatures in `deps/jemalloc/src/`
3. Create `jemalloc-extent-tracking.patch` (Phase 1)
4. Build instrumented Redis in Docker
5. Run fragmentation test
6. Measure DSR

---

**Status:** Strategy documented, ready to implement
**Difficulty:** High (modifying jemalloc internals)
**Expected timeline:** 2-3 days for Phase 2 (working DSR measurement)
**Expected outcome:** DSR ~25% after fragmentation = first tool to detect this class of leak
