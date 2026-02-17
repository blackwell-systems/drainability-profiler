# libdrainprof

A lightweight C library for detecting structural memory leaks in coarse-grained allocators by measuring drainability at runtime. Helps you answer: **"Why does my service leak memory when Valgrind says it doesn't?"**

## What is Drainability?

**Drainability** is a structural property of memory allocators that determines whether allocated granules (slabs, arenas, epochs, regions) can be reclaimed at their natural reclaim boundaries, even when all individual objects have been freed.

### The Problem: Structural Leaks

Traditional memory leak detectors (Valgrind, ASan) only detect **unreachable objects**—memory that was allocated but never freed. They miss **structural leaks**: situations where:

- All objects are properly freed
- But granules can't be reclaimed because one long-lived allocation pins the entire granule

**Example:** A slab allocator with 1000 slots. If 999 objects are freed but 1 remains, the entire slab (and its backing memory) cannot be reclaimed—even though 99.9% of objects are freed.

### The Metric: DSR (Drainability Satisfaction Rate)

**DSR = drainable_closes / total_closes**

- **1.0 (100%)**: Perfect drainability - all granules reclaimed when closed
- **0.5 (50%)**: Half of granules are pinned by lingering allocations
- **0.0 (0%)**: Every granule has pinned allocations

## Features

- **Lightweight:** < 2ns per allocation in production mode
- **Thread-safe:** Lock-free atomic operations
- **Real-time metrics:** Track DSR, allocation patterns, peak granule usage
- **Zero-overhead when disabled:** Compiled out via preprocessor macros
- **Two modes:** Production (always-on) and Diagnostic (deep investigation)

**Current Status:** M2 Complete (Production + Diagnostic modes, slot array storage)

## Quick Start

### Build the Library

```bash
make all          # Build static + shared libraries
make test         # Run test suite (7 tests)
make bench        # Benchmark hot path performance
make examples     # Build example programs
```

### Basic Usage

```c
#include <drainprof.h>

// Create profiler
drainprof *prof = drainprof_create();

// Track granule lifecycle
drainprof_granule_open(prof, granule_id);

// Register allocations
drainprof_alloc_register(prof, granule_id, alloc_id, size);
drainprof_alloc_deregister(prof, granule_id, alloc_id);

// Close granule and check drainability
// IMPORTANT: Ensure no concurrent alloc_register/deregister calls
// for this granule_id are in flight when calling close
int drainable = drainprof_granule_close(prof, granule_id);
// drainable = 1 if no live allocations, 0 if pinned

// Read metrics
drainprof_snapshot_t snap;
drainprof_snapshot(prof, &snap);
printf("DSR: %.1f%%\n", snap.dsr * 100.0);

// Cleanup
drainprof_destroy(prof);
```

See `examples/basic.c` for a complete working example.

### Diagnostic Mode Usage

When production monitoring shows low DSR, enable diagnostic mode to identify which allocations are pinning granules:

```c
drainprof_config config;
drainprof_config_default(&config);
config.mode = DRAINPROF_DIAGNOSTIC;
config.on_pinning = my_callback;  /* Or NULL to buffer reports */

drainprof *prof = drainprof_create_with_config(&config);

// Use DRAINPROF_ALLOC_REGISTER macro to capture source locations
DRAINPROF_ALLOC_REGISTER(prof, granule_id, alloc_id, size);

// When a pinned granule closes, callback receives detailed report
```

**Pinning Report Contents:**
- Granule ID and timestamps (open/close)
- List of all allocations that pinned the granule
- Source locations (__FILE__:__LINE__ for each allocation)
- Allocation sizes and timestamps

**Diagnostic Workflows:**

1. **Real-time callback:** Set `config.on_pinning` to a callback function that receives reports as granules are pinned (see `examples/diagnostic.c`)

2. **Buffered reports:** Set `config.on_pinning = NULL` to buffer reports, then:
   - Drain individual reports with `drainprof_drain_reports()` for detailed analysis
   - Compute aggregated summary with `drainprof_diagnostic_summary_compute()` to identify the most problematic allocation sites (see `examples/diagnostic_summary.c`)

## Installation

### Prerequisites

- C11-compatible compiler (gcc, clang)
- POSIX threads support
- Make

### Build Static Library

```bash
make
# Creates: libdrainprof.a
```

### Build Shared Library

```bash
make libdrainprof.so
```

### Install System-Wide

```bash
sudo make install
# Installs to /usr/local/lib and /usr/local/include
```

## API Reference

### Initialization

```c
drainprof *drainprof_create(void);
```
Creates a profiler with default configuration (production mode, slot array, 1024 capacity).

```c
drainprof *drainprof_create_with_config(const drainprof_config *config);
```
Creates a profiler with custom configuration.

```c
void drainprof_destroy(drainprof *prof);
```
Destroys profiler and frees resources.

### Granule Lifecycle

```c
int drainprof_granule_open(drainprof *prof, drainprof_granule_id id);
```
Opens a granule for tracking. Returns 0 on success, -1 on collision (slot array full).

```c
int drainprof_granule_close(drainprof *prof, drainprof_granule_id id);
```
Closes a granule. Returns 1 if drainable (no live allocations), 0 if pinned.

### Allocation Tracking

```c
int drainprof_alloc_register(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size
);
```
Registers an allocation in a granule. **Hot path:** < 2ns overhead.

```c
int drainprof_alloc_deregister(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id
);
```
Deregisters an allocation (object freed). **Hot path:** < 2ns overhead.

```c
#define DRAINPROF_ALLOC_REGISTER(prof, gid, aid, sz)
```
Macro version that captures `__FILE__` and `__LINE__` for diagnostic mode.

### Metrics

```c
void drainprof_snapshot(const drainprof *prof, drainprof_snapshot_t *out);
```
Takes a consistent snapshot of all metrics. Lock-free, safe to call anytime.

**Snapshot fields:**
```c
typedef struct {
    uint64_t total_closes;          // Total granules closed
    uint64_t drainable_closes;      // Granules reclaimed (no live allocs)
    uint64_t pinned_closes;         // Granules pinned (live allocs remain)
    uint64_t open_granules;         // Currently open granules
    uint64_t peak_open_granules;    // Peak simultaneous open granules
    uint64_t total_allocs;          // Total allocations registered
    uint64_t total_deallocs;        // Total deallocations registered
    double dsr;                     // Drainability Satisfaction Rate [0.0, 1.0]
} drainprof_snapshot_t;
```

### Configuration

```c
typedef struct {
    drainprof_mode mode;               // PRODUCTION or DIAGNOSTIC
    drainprof_storage storage;         // SLOT_ARRAY or HASH_MAP
    uint32_t slot_capacity;            // For SLOT_ARRAY: max granules
    uint64_t log_interval;             // Log DSR every N closes (0=off)
    drainprof_pin_callback on_pinning; // Diagnostic: callback on pin
    void *callback_user_data;          // User data for callback
    uint32_t max_buffered_reports;     // Diagnostic: max buffered reports
    bool verbose;                      // Log pinned closes to stderr
} drainprof_config;

drainprof_config drainprof_config_default(void);
```

### Diagnostic Mode API

```c
// Callback type for pinning events
typedef void (*drainprof_pin_callback)(
    const drainprof_pinning_report *report,
    void *user_data
);

// Drain buffered reports (when callback is NULL)
uint32_t drainprof_drain_reports(
    drainprof *prof,
    drainprof_pinning_report **reports,
    uint32_t max_reports
);

// Free a pinning report
void drainprof_pinning_report_free(drainprof_pinning_report *report);
```

**Pinning Report Structure:**
```c
typedef struct {
    drainprof_granule_id granule_id;
    struct timespec open_time;
    struct timespec close_time;
    drainprof_pinning_alloc *pinning_allocs;  // Array of pinning allocs
    uint32_t pinning_count;      // Number of allocations pinning this granule
    uint32_t total_allocs;       // Total allocations in granule
    uint32_t drained_allocs;     // Allocations freed before close
} drainprof_pinning_report;

typedef struct {
    drainprof_alloc_id alloc_id;
    drainprof_alloc_site alloc_site;  // file, line, column
    struct timespec alloc_time;
    size_t size;
    double lifetime_delta_sec;  // -1.0 if still live
} drainprof_pinning_alloc;
```

**Diagnostic Summary (Aggregation):**

When investigating structural leaks, it's useful to aggregate pinning reports by allocation site to identify the most problematic source locations:

```c
// Compute summary from buffered reports (diagnostic mode only)
// Aggregates all buffered reports by allocation site (file:line)
drainprof_diagnostic_summary *drainprof_diagnostic_summary_compute(drainprof *prof);

// Free a diagnostic summary
void drainprof_diagnostic_summary_free(drainprof_diagnostic_summary *summary);
```

**Summary Structures:**
```c
typedef struct {
    drainprof_alloc_site site;   // Source location (file, line, column)
    uint32_t pinning_count;       // Number of granules this site pinned
    uint32_t total_allocs;        // Total allocations from this site
    size_t total_bytes;           // Total bytes allocated from this site
} drainprof_summary_site_entry;

typedef struct {
    drainprof_summary_site_entry *sites;  // Array of unique sites
    uint32_t site_count;                  // Number of unique sites
    uint32_t total_pinning_allocs;        // Sum of all pinning allocations
    uint32_t reports_analyzed;            // Number of reports included
} drainprof_diagnostic_summary;
```

**Usage:**
```c
// After running workload in diagnostic mode without callback...
drainprof_diagnostic_summary *summary = drainprof_diagnostic_summary_compute(prof);

printf("Pinning allocations by source location:\n");
for (uint32_t i = 0; i < summary->site_count; i++) {
    drainprof_summary_site_entry *site = &summary->sites[i];
    printf("  %s:%u - pins %u granules (%u allocs, %zu bytes)\n",
           site->site.file, site->site.line,
           site->pinning_count, site->total_allocs, site->total_bytes);
}

drainprof_diagnostic_summary_free(summary);
```

See `examples/diagnostic_summary.c` for a complete example.

## Performance

### Production Mode

**Benchmark Results (Apple Silicon):**

| Operation           | Latency     | Throughput |
|---------------------|-------------|------------|
| `alloc_register`    | **1.97 ns** | 508 M/s    |
| `alloc_deregister`  | **1.77 ns** | 565 M/s    |

**Target:** < 10ns per operation - **PASS: Exceeded by 5x**

### Diagnostic Mode

**Benchmark Results (Apple Silicon):**

| Operation                  | Latency      | Throughput |
|----------------------------|--------------|------------|
| `alloc_register_located`   | **24.68 ns** | 40.5 M/s   |
| `alloc_deregister`         | **20.50 ns** | 48.8 M/s   |

**Target:** < 50ns per operation - **PASS: Within budget**

**Overhead:** ~10x slower than production mode due to per-allocation tracking, mutex operations, and metadata storage. Acceptable for diagnostic investigation.

### Why So Fast?

- **Lock-free design:** Single atomic increment/decrement per operation
- **Pre-allocated slots:** No malloc in hot path
- **Cache-friendly:** Contiguous slot array layout
- **Minimal indirection:** Direct granule_id → slot index mapping

## Testing

### Run Test Suite

```bash
make test
```

**Test Coverage:**
- Basic creation/destruction
- Single drainable granule
- Single pinned granule
- Multiple allocations per granule
- Sequential granule tracking (100 granules)
- Peak open granule tracking
- **Theorem 3 validation** (P-sweep with p=0.0, 0.1, 0.5, 1.0)

### Theorem 3 Validation

The library validates the drainability theorem from the paper:

**R(t) ≥ p·m(t)** where:
- R(t) = retained granules
- m(t) = closed granules
- p = violation probability

Test results confirm **DSR = 1.0 - p** exactly for all p-values.

## Integration Examples

### Concurrency Contract

**IMPORTANT:** When calling `drainprof_granule_close()`, you must ensure that no concurrent `alloc_register` or `alloc_deregister` calls for that `granule_id` are in flight. All allocation activity for the granule must have completed before closing it.

This contract matches typical allocator usage patterns:

- **Epoch-based allocators**: Close an epoch only after advancing past it. New allocations go to the new epoch, so no concurrent activity on the old epoch.
- **Arena allocators**: Close an arena only after all users have released it. The arena is destroyed when the last reference is dropped.
- **Slab allocators**: Close a slab only when returning it to the pool. The allocator stops routing allocations to that slab before closing it.

**Why this matters:** Without this precondition, there's a race where `granule_close` reads the live count while a concurrent `alloc_register` increments it, then `close` zeros the live count, silently losing the concurrent allocation. The precondition eliminates this race by requiring the caller to quiesce allocation activity first.

The examples below demonstrate how different allocator types naturally satisfy this contract.

### Slab Allocator

```c
void *slab_alloc(slab_t *slab) {
    void *ptr = internal_slab_alloc(slab);
    drainprof_alloc_register(g_prof, slab->id, (uintptr_t)ptr, slab->obj_size);
    return ptr;
}

void slab_free(slab_t *slab, void *ptr) {
    drainprof_alloc_deregister(g_prof, slab->id, (uintptr_t)ptr);
    internal_slab_free(slab, ptr);
}
```

### Arena Allocator

```c
arena_t *arena_create(void) {
    arena_t *arena = malloc(sizeof(arena_t));
    arena->prof_id = next_arena_id++;
    drainprof_granule_open(g_prof, arena->prof_id);
    return arena;
}

void arena_destroy(arena_t *arena) {
    int drainable = drainprof_granule_close(g_prof, arena->prof_id);
    if (!drainable) {
        fprintf(stderr, "Warning: Arena %llu destroyed with live allocations\n",
                arena->prof_id);
    }
    free(arena);
}
```

### Epoch-Based Reclamation

```c
void epoch_advance(epoch_system_t *sys) {
    uint64_t old_epoch = sys->current_epoch;
    sys->current_epoch++;

    drainprof_granule_close(g_prof, old_epoch);
    drainprof_granule_open(g_prof, sys->current_epoch);
}
```

## Interpreting Results

### Understanding DSR

**DSR (Drainability Satisfaction Rate)** measures what fraction of closed granules were successfully drained (all allocations freed by close time). A DSR of 1.0 means perfect drainability; 0.0 means every granule had pinned allocations.

**Important:** The "acceptable" DSR depends on your workload:
- **Granule close rate:** Services closing 1000 granules/sec are more sensitive to retention than those closing 1/sec
- **Service lifetime:** Even DSR=0.99 (1% pinned) can accumulate significant retention over days/weeks
- **Memory pressure:** Systems with tight memory budgets need higher DSR than those with headroom

### Example Interpretations

```
DSR: 0.95 (95%)
```
5% of granules are pinned. If you close 100 granules/sec, that's 5 pinned granules/sec. Multiply by service uptime to estimate total retained granules.

```
DSR: 0.50 (50%)
```
Half of all granules are pinned. Strong signal of structural leaks. Investigate:
- Are long-lived allocations (sessions, connections) mixed with short-lived ones (requests)?
- Are allocations outliving their natural granule boundary?
- Would lifetime-aware allocation policies help?

```
DSR: 0.10 (10%)
```
Severe structural leaks. Most granules cannot be reclaimed:
1. Enable verbose mode: `config.verbose = true`
2. Review pinned granule logs to identify leak sources
3. Consider redesigning allocator granule boundaries
4. May need separate allocators for different lifetime classes

**Baseline:** Measure DSR on your workload over time. Look for trends, not absolute thresholds. A drop from 0.95 to 0.85 may indicate a newly introduced leak, even if 0.85 seems "acceptable" in isolation.

## Architecture

**Storage Modes:**

| Mode       | Status      | Use Case |
|------------|-------------|----------|
| SLOT_ARRAY | M1 Complete | Dense granule IDs (0-1023) |
| HASH_MAP   | Future      | Sparse granule IDs (any uint64) |

**Profiler Modes:**

| Mode        | Status      | Overhead | Features |
|-------------|-------------|----------|----------|
| PRODUCTION  | M1 Complete | < 2ns    | DSR, counters, peak tracking |
| DIAGNOSTIC  | M2 Complete | ~25ns    | Per-allocation tracking, source locations, pinning reports |

**Concurrency Model:**
- Lock-free slot array using C11 atomics
- Safe for concurrent access from multiple threads
- No global locks on hot path

## Roadmap

- **M1:** Core C library, production mode, slot array [COMPLETE]
- **M2:** Diagnostic mode with allocation tracking [COMPLETE]
- **M3:** Rust bindings [PLANNED]
- **M4:** Prometheus export [PLANNED]
- **M5:** Blog post and announcement [PLANNED]

## Contributing

This project is part of the drainability research framework. See `drainability_profiler_architecture.md` for detailed design decisions.

## Related Projects

- **drainability-framework**: Research paper and LaTeX source
- **drainability-profiler**: This library (C implementation)

## Citation

If you use libdrainprof in research, please cite:

```bibtex
@techreport{blackwell2026drainability,
  title   = {Drainability: When Coarse-Grained Memory Reclamation
             Produces Bounded Retention},
  author  = {Blackwell, Dayna},
  year    = {2026},
  doi     = {10.5281/zenodo.18653776},
  note    = {Technical Report},
  license = {CC-BY-4.0}
}
```

## License

MIT License - See LICENSE file for details

## Author

Dayna Blackwell
[Paper: doi.org/10.5281/zenodo.18653776]

---

**Status:** M1 Complete | **Build:** Passing | **Coverage:** 7/7 tests | **Performance:** 1.97ns/op
