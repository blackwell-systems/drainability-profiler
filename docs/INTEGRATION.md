# Integration Guide

How to integrate libdrainprof with different allocator types. For API details, see [API.md](API.md).

## Concurrency Contract

**IMPORTANT:** When calling `drainprof_granule_close()`, you must ensure that no concurrent `alloc_register` or `alloc_deregister` calls for that `granule_id` are in flight. All allocation activity for the granule must have completed before closing it.

### Why This Matters

Without this precondition, there's a race:
1. Thread A calls `granule_close`, reads live_count = 0
2. Thread B calls `alloc_register`, increments live_count = 1
3. Thread A marks granule drainable, zeros live_count
4. Allocation from Thread B is silently lost

The precondition eliminates this race by requiring allocation activity to quiesce before closing.

### How Allocators Satisfy This

Different allocator types naturally satisfy this contract:

- **Epoch-based allocators**: Close epoch only after advancing. New allocations go to new epoch, no concurrent activity on old epoch.
- **Arena allocators**: Close arena only after all users release it. Arena destroyed when last reference drops.
- **Slab allocators**: Close slab only when returning to pool. Allocator stops routing allocations before closing.

## Real-World Integration: temporal-slab

See [`examples/temporal-slab/`](../examples/temporal-slab/) for complete production-ready integration with the [temporal-slab allocator](https://github.com/blackwell-systems/temporal-slab).

**Key features:**
- Conditional compilation via `#ifdef ENABLE_DRAINPROF`
- Only 4 instrumentation points in entire allocator
- Zero overhead when disabled (compiled out)
- CI validation with p-sweep tests

**Instrumentation points:**
```c
// 1. Epoch advance
void epoch_advance(SlabAllocator* alloc) {
    EpochId old = atomic_fetch_add(&alloc->current_epoch, 1);
#ifdef ENABLE_DRAINPROF
    drainprof_granule_open(g_profiler, old + 1);
#endif
}

// 2. Epoch close
void epoch_close(SlabAllocator* alloc, EpochId epoch_id) {
    set_epoch_state(alloc, epoch_id, EPOCH_CLOSING);
#ifdef ENABLE_DRAINPROF
    drainprof_granule_close(g_profiler, epoch_id);
#endif
}

// 3. Allocation
void* alloc_obj_epoch(SlabAllocator* alloc, uint32_t size,
                      EpochId epoch, SlabHandle* out_handle) {
    void* ptr = internal_alloc(alloc, size, epoch, out_handle);
#ifdef ENABLE_DRAINPROF
    drainprof_alloc_register(g_profiler, epoch, *out_handle, size);
#endif
    return ptr;
}

// 4. Free
bool free_obj(SlabAllocator* alloc, SlabHandle handle) {
    EpochId epoch = slab_get_epoch(handle);
#ifdef ENABLE_DRAINPROF
    drainprof_alloc_deregister(g_profiler, epoch, handle);
#endif
    return internal_free(alloc, handle);
}
```

## Generic Patterns

### Slab Allocator

```c
// Global profiler (or per-allocator instance)
static drainprof *g_prof;

void slab_init(slab_allocator_t *alloc) {
    g_prof = drainprof_create();
    // ... initialize slab allocator ...
}

void *slab_alloc(slab_t *slab) {
    void *ptr = internal_slab_alloc(slab);
    if (ptr) {
        drainprof_alloc_register(g_prof, slab->id, (uintptr_t)ptr, slab->obj_size);
    }
    return ptr;
}

void slab_free(slab_t *slab, void *ptr) {
    drainprof_alloc_deregister(g_prof, slab->id, (uintptr_t)ptr);
    internal_slab_free(slab, ptr);
}

void slab_return_to_pool(slab_t *slab) {
    // Slab is no longer accepting allocations
    int drainable = drainprof_granule_close(g_prof, slab->id);
    if (!drainable) {
        fprintf(stderr, "Warning: Slab %llu returned with live allocations\n", slab->id);
    }
    // Return slab to pool or free it
}
```

### Arena Allocator

```c
typedef struct arena {
    void *base;
    size_t size;
    size_t used;
    drainprof_granule_id prof_id;
} arena_t;

arena_t *arena_create(size_t size) {
    arena_t *arena = malloc(sizeof(arena_t));
    arena->base = malloc(size);
    arena->size = size;
    arena->used = 0;
    arena->prof_id = next_arena_id++;

    drainprof_granule_open(g_prof, arena->prof_id);
    return arena;
}

void *arena_alloc(arena_t *arena, size_t size) {
    if (arena->used + size > arena->size) return NULL;

    void *ptr = (char*)arena->base + arena->used;
    arena->used += size;

    drainprof_alloc_register(g_prof, arena->prof_id, (uintptr_t)ptr, size);
    return ptr;
}

void arena_destroy(arena_t *arena) {
    // Arena no longer accepts allocations
    int drainable = drainprof_granule_close(g_prof, arena->prof_id);
    if (!drainable) {
        fprintf(stderr, "Warning: Arena %llu destroyed with live allocations\n",
                arena->prof_id);
    }

    free(arena->base);
    free(arena);
}
```

**Note:** This example doesn't implement `arena_free()` (arena allocators typically don't support individual frees). If your arena does support frees, add `drainprof_alloc_deregister()` there.

### Epoch-Based Reclamation (RCU-style)

```c
typedef struct epoch_system {
    uint64_t current_epoch;
    // ... other fields ...
} epoch_system_t;

void epoch_init(epoch_system_t *sys) {
    sys->current_epoch = 0;
    drainprof_granule_open(g_prof, sys->current_epoch);
}

void *epoch_alloc(epoch_system_t *sys, size_t size) {
    void *ptr = malloc(size);  // Or your allocator
    drainprof_alloc_register(g_prof, sys->current_epoch, (uintptr_t)ptr, size);
    return ptr;
}

void epoch_free_deferred(epoch_system_t *sys, void *ptr, size_t size) {
    // Mark for deferred free in current epoch
    drainprof_alloc_deregister(g_prof, sys->current_epoch, (uintptr_t)ptr);
    add_to_deferred_list(sys->current_epoch, ptr);
}

void epoch_advance(epoch_system_t *sys) {
    uint64_t old_epoch = sys->current_epoch;
    sys->current_epoch++;

    // Close old epoch (after quiescence period)
    int drainable = drainprof_granule_close(g_prof, old_epoch);

    // Open new epoch
    drainprof_granule_open(g_prof, sys->current_epoch);

    // Process deferred frees from old epoch
    process_deferred_frees(old_epoch);
}
```

## Diagnostic Workflow

When production DSR drops below threshold:

### 1. Enable Diagnostic Mode

```c
drainprof_config config = drainprof_config_default();
config.mode = DRAINPROF_DIAGNOSTIC;
config.on_pinning = NULL;  // Buffer reports
config.max_buffered_reports = 10000;

drainprof *prof = drainprof_create_with_config(&config);
```

### 2. Use Macro for Source Locations

```c
// Replace:
drainprof_alloc_register(prof, gid, aid, size);

// With:
DRAINPROF_ALLOC_REGISTER(prof, gid, aid, size);
```

This captures `__FILE__` and `__LINE__` for each allocation.

### 3. Run Workload

Run your service under diagnostic mode. Reports are buffered automatically when `on_pinning` is NULL.

### 4. Analyze Results

**Option A: Aggregate by site**
```c
drainprof_diagnostic_summary *summary = drainprof_diagnostic_summary_compute(prof);

printf("Top pinning allocation sites:\n");
for (uint32_t i = 0; i < summary->site_count; i++) {
    printf("  %s:%u - pins %u granules\n",
           summary->sites[i].site.file,
           summary->sites[i].site.line,
           summary->sites[i].pinning_count);
}

drainprof_diagnostic_summary_free(summary);
```

**Option B: Examine individual reports**
```c
drainprof_pinning_report *reports[100];
uint32_t count = drainprof_drain_reports(prof, reports, 100);

for (uint32_t i = 0; i < count; i++) {
    printf("Granule %llu: %u allocations pinned\n",
           reports[i]->granule_id, reports[i]->pinning_count);

    for (uint32_t j = 0; j < reports[i]->pinning_count; j++) {
        drainprof_pinning_alloc *alloc = &reports[i]->pinning_allocs[j];
        printf("  %s:%u - %zu bytes\n",
               alloc->alloc_site.file, alloc->alloc_site.line, alloc->size);
    }

    drainprof_pinning_report_free(reports[i]);
}
```

### 5. Fix the Leak

Common fixes:
- **Separate allocators:** Use different allocators for different lifetime classes (e.g., sessions vs requests)
- **Adjust granule boundaries:** Align granule lifetimes with natural allocation patterns
- **Lifetime-aware routing:** Route long-lived allocations to persistent arenas

## Conditional Compilation

Recommended pattern for zero overhead when disabled:

```c
#ifdef ENABLE_DRAINPROF
  #include <drainprof.h>
  extern drainprof *g_profiler;
  #define DRAINPROF_GRANULE_OPEN(gid) \
      drainprof_granule_open(g_profiler, gid)
  #define DRAINPROF_ALLOC_REGISTER(gid, aid, sz) \
      drainprof_alloc_register(g_profiler, gid, aid, sz)
  // ... other macros ...
#else
  #define DRAINPROF_GRANULE_OPEN(gid) ((void)0)
  #define DRAINPROF_ALLOC_REGISTER(gid, aid, sz) ((void)0)
  // ... other macros ...
#endif
```

Then in your code:
```c
void epoch_advance(allocator_t *alloc) {
    DRAINPROF_GRANULE_OPEN(alloc->current_epoch);  // Compiled out if disabled
    // ... rest of logic ...
}
```

See `temporal-slab/include/slab_alloc.h` for complete example.

## Interpreting DSR Values

**DSR (Drainability Satisfaction Rate)** = drainable_closes / total_closes

### What Different Values Mean

**DSR ≥ 0.95 (95%+):**
Excellent drainability. ≤5% of granules are pinned. Monitor for trends over time.

**DSR 0.80-0.95 (80-95%):**
Acceptable for many workloads. 5-20% of granules pinned. Watch for downward trends that indicate newly introduced leaks.

**DSR 0.50-0.80 (50-80%):**
Moderate structural leaks. Significant memory retention over time. Enable diagnostic mode to identify sources.

**DSR < 0.50 (<50%):**
Severe structural leaks. Majority of granules cannot reclaim memory. Likely needs architectural fix (separate allocators for different lifetime classes).

### Context Matters

The "acceptable" DSR depends on:
- **Granule close rate:** 1000 closes/sec × 5% pinned = 50 pinned granules/sec
- **Service lifetime:** Even 1% pinned accumulates over days/weeks
- **Memory pressure:** Tight budgets need higher DSR

**Baseline approach:** Measure DSR on your production workload. Look for changes over time, not absolute thresholds. A drop from 95% → 85% may indicate a new leak, even if 85% seems reasonable.

## Example: Session Allocation Bug

This is the bug demonstrated in `rss_demo.c`:

**Problem (Broken Mode):**
```c
// Sessions allocated in request epochs (mixed lifetimes)
void handle_request(server_t *srv) {
    EpochId epoch = epoch_current(srv->alloc);

    void *request_buf = alloc_from_epoch(srv->alloc, 128, epoch);
    void *session = alloc_from_epoch(srv->alloc, 256, epoch);  // BUG

    // Request completes quickly, session lives 60 seconds
    free(request_buf);
    // session freed 60 seconds later

    epoch_close(srv->alloc, epoch);  // Can't reclaim - session still live
}
```

**Result:** DSR = 5% (19/20 epochs pinned by sessions)

**Fix (Fixed Mode):**
```c
// Sessions in separate allocator (isolated lifetimes)
server_t *srv = malloc(sizeof(server_t));
srv->request_alloc = slab_allocator_create();  // For requests
srv->session_alloc = slab_allocator_create();  // For sessions

void handle_request(server_t *srv) {
    EpochId req_epoch = epoch_current(srv->request_alloc);
    EpochId sess_epoch = epoch_current(srv->session_alloc);  // Different allocator

    void *request_buf = alloc_from_epoch(srv->request_alloc, 128, req_epoch);
    void *session = alloc_from_epoch(srv->session_alloc, 256, sess_epoch);  // FIX

    free(request_buf);
    epoch_close(srv->request_alloc, req_epoch);  // Can reclaim immediately!

    // session freed 60 seconds later from session_alloc
}
```

**Result:** DSR = 90% (18/20 request epochs drainable)

**Lesson:** Separate allocators for separate lifetime classes. Request epochs drain immediately, session allocator stays pinned - but only affects session memory, not request memory.

## Integration Checklist

- [ ] Identify granules in your allocator (slabs, arenas, epochs, regions)
- [ ] Add `drainprof_granule_open()` when granule created
- [ ] Add `drainprof_granule_close()` when granule retired
- [ ] Add `drainprof_alloc_register()` in allocation path
- [ ] Add `drainprof_alloc_deregister()` in free path
- [ ] Verify concurrency contract: no concurrent alloc activity during `granule_close`
- [ ] Add conditional compilation (`#ifdef ENABLE_DRAINPROF`) for zero overhead when disabled
- [ ] Monitor DSR in production, set up alerts for drops below baseline
- [ ] Create diagnostic build for investigating low DSR
- [ ] Validate with p-sweep test (see `examples/temporal-slab/psweep_validation.c`)

## Validation Testing

After integration, validate DSR measurements with p-sweep test:

```c
// Simulate controlled violation probability
for (double p = 0.0; p <= 1.0; p += 0.1) {
    run_workload(p);  // p = probability an allocation violates drainability

    drainprof_snapshot_t snap;
    drainprof_snapshot(prof, &snap);

    // Verify: DSR should equal (1.0 - p)
    assert(fabs(snap.dsr - (1.0 - p)) < 0.01);
}
```

See `examples/temporal-slab/psweep_validation.c` for complete working example that runs in CI.

## Common Mistakes

### Mistake 1: Closing Before Quiescence
```c
// WRONG: Concurrent allocation possible
epoch_advance(alloc);
drainprof_granule_close(prof, old_epoch);  // Race!
```

```c
// RIGHT: Close after advancing (no more allocations to old epoch)
uint64_t old_epoch = alloc->current_epoch;
alloc->current_epoch++;  // Advance first
drainprof_granule_close(prof, old_epoch);  // Now safe
```

### Mistake 2: Using Granule ID After Close
```c
// WRONG: Using granule_id after closing
drainprof_granule_close(prof, granule_id);
drainprof_alloc_register(prof, granule_id, ...);  // Undefined behavior
```

### Mistake 3: Missing Deregister
```c
// WRONG: Forgot to deregister on free
void my_free(void *ptr) {
    internal_free(ptr);  // Missing drainprof_alloc_deregister!
}
```

All allocations must be paired with deregistrations, or granules will appear pinned.

### Mistake 4: Wrong Granule ID
```c
// WRONG: Using allocation address as granule ID
drainprof_alloc_register(prof, (uintptr_t)ptr, ...);  // ptr is alloc_id, not granule_id!
```

```c
// RIGHT: Use slab/arena/epoch ID as granule_id
drainprof_alloc_register(prof, slab->granule_id, (uintptr_t)ptr, size);
```

## Additional Resources

- [API.md](API.md) - Complete API reference
- [examples/basic.c](../examples/basic.c) - Minimal working example
- [examples/diagnostic.c](../examples/diagnostic.c) - Diagnostic mode with callbacks
- [examples/temporal-slab/](../examples/temporal-slab/) - Production integration
- [Research paper](https://doi.org/10.5281/zenodo.18653776) - Theory and proofs
