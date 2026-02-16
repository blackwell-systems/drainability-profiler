# drainability-profiler: Architecture & API Design

**Library:** `libdrainprof` (C core) + `drainability-profiler` (Rust bindings)
**Author:** Dayna Blackwell
**Status:** Design Document (Pre-Implementation)
**Paper Reference:** Blackwell, D. (2026). *Drainability: When Coarse-Grained Memory Reclamation Produces Bounded Retention.* doi:10.5281/zenodo.18653776

---

## 1. Purpose

A C library that detects structural leaks in any coarse-grained, non-relocating allocator, with idiomatic Rust bindings published to crates.io. The profiler measures the drainability satisfaction rate -- the fraction of granules that are fully drained at their reclaim boundary -- and, in diagnostic mode, identifies the specific allocations responsible for pinning.

No such tool currently exists. Conventional leak detectors (Valgrind, ASan) operate at the object level and cannot detect granule-level retention failures. This library closes that diagnostic gap.

**Language strategy.** The core is C because the experimental allocator that produced the paper's results is C, and the most immediate integration target is that allocator. A thin Rust crate wraps the C API for the Rust ecosystem (bumpalo, Crossbeam), where coarse-grained allocation patterns are most explicit and drainability violations bite hardest in practice.

---

## 2. Design Principles

**Allocator-agnostic.** The profiler defines a callback interface that any allocator can call into. It does not depend on any specific allocator's internals.

**Two-mode operation.** Production mode adds near-zero overhead (one atomic operation per allocation, one comparison per granule close). Diagnostic mode adds per-allocation tracking for root-cause analysis. The modes use the same API; only the profiler configuration differs.

**Zero allocations in the hot path (production mode).** Production mode uses fixed-size atomic counters and a pre-allocated granule slot array. No heap allocation occurs on granule open, allocation registration, or granule close in the steady state.

**Non-invasive.** The profiler is an observer. It does not modify allocator behavior, routing decisions, or reclamation policy. It measures and reports.

**Minimal dependencies.** The C core depends only on `<stdatomic.h>`, `<stdint.h>`, `<stdlib.h>`, `<string.h>`, `<time.h>`, and `<pthread.h>`. No external libraries.

---

## 3. Core Concepts

### 3.1 Terminology Mapping

The profiler uses the paper's terminology. Integrations map allocator-specific names to these concepts:

| Profiler Concept | Paper Term | tslab (author) | bumpalo | jemalloc | Crossbeam EBR |
|---|---|---|---|---|---|
| `granule_id` | Granule g | Epoch | Arena / Chunk | Slab | Epoch |
| `drainprof_granule_open` | t_open(g) | epoch_open | Arena::new | Slab creation | Epoch advance |
| `drainprof_granule_close` | t_reclaim(g) | epoch_close | Arena::reset/drop | Slab reclaim attempt | Epoch quiescence |
| `drainprof_alloc_register` | ρ(a) = g | tslab_alloc | alloc_layout | Object allocation | Retire / defer |
| `drainprof_alloc_deregister` | t_free(a) | tslab_free | (manual or drop) | Object deallocation | Reclamation |

### 3.2 What the Profiler Measures

At each granule close, the profiler answers one question: **are there any live allocations in this granule?**

- If no: the granule is **drainable**. Counter incremented.
- If yes: the granule is **pinned**. Counter incremented. In diagnostic mode, the pinning allocations are recorded.

The **drainability satisfaction rate** is:

    DSR = drainable_closes / total_closes

This is the runtime analog of the paper's drainability condition, measured continuously.

---

## 4. Data Structures

### 4.1 Identifiers

```c
#include <stdint.h>

/* Opaque granule identifier. Assigned by the allocator.
 * Must be unique among currently-open granules.
 * May be reused after drainprof_granule_close returns. */
typedef uint64_t drainprof_granule_id;

/* Opaque allocation identifier. Assigned by the allocator.
 * Must be unique within a granule. */
typedef uint64_t drainprof_alloc_id;
```

### 4.2 Production Mode State

```c
#include <stdatomic.h>

/* Fixed-size, lock-free global counters. No heap allocation. */
typedef struct drainprof_counters {
    _Atomic uint64_t total_closes;       /* Total granule close events */
    _Atomic uint64_t drainable_closes;   /* Closes where all allocs were freed */
    _Atomic uint64_t pinned_closes;      /* Closes with at least one live alloc */
    _Atomic uint64_t open_granules;      /* Currently open granules */
    _Atomic uint64_t peak_open_granules; /* High-water mark */
    _Atomic uint64_t total_allocs;       /* Total alloc_register calls */
    _Atomic uint64_t total_deallocs;     /* Total alloc_deregister calls */
} drainprof_counters;
```

**Per-granule tracking (production mode).** Each open granule needs exactly one value: the count of live allocations. Two implementation strategies are provided:

**Strategy A: Pre-allocated slot array (default).** For allocators with bounded, dense granule IDs (typical of epoch-based systems), a fixed-size array indexed by `granule_id % capacity` gives lock-free O(1) access with no heap allocation after initialization.

```c
/* Pre-allocated granule slot array.
 * Index = granule_id % capacity.
 * Requires that the number of simultaneously open granules
 * is bounded and known at init time. */
typedef struct drainprof_granule_slot {
    _Atomic uint32_t live_count;  /* Live allocations in this granule */
    _Atomic uint32_t occupied;    /* 1 if slot is in use, 0 if free */
} drainprof_granule_slot;

typedef struct drainprof_slot_array {
    drainprof_granule_slot *slots;
    uint32_t capacity;
} drainprof_slot_array;
```

**Strategy B: Hash map with rwlock.** For allocators with sparse or unpredictable granule IDs. Uses `pthread_rwlock_t` so `alloc_register`/`alloc_deregister` take the read lock (concurrent) while `granule_open`/`granule_close` take the write lock (infrequent).

```c
#include <pthread.h>

typedef struct drainprof_granule_entry {
    drainprof_granule_id id;
    _Atomic uint32_t live_count;
    struct drainprof_granule_entry *next;  /* Chaining for collisions */
} drainprof_granule_entry;

typedef struct drainprof_granule_map {
    drainprof_granule_entry **buckets;
    uint32_t bucket_count;
    pthread_rwlock_t lock;
} drainprof_granule_map;
```

**Overhead estimate (production mode):** Slot array: 8 bytes per slot (4-byte atomic count + 4-byte occupied flag), plus 56 bytes for global counters. For 256 slots (typical), total overhead is 2.1 KB. Hash map: ~24 bytes per open granule (8-byte key + 4-byte count + 4-byte occupied + 8-byte next pointer), plus bucket array.

### 4.3 Diagnostic Mode State

Diagnostic mode extends production mode with per-allocation metadata:

```c
#include <time.h>

/* Source location of an allocation call.
 * Captured via __FILE__, __LINE__ macros at the call site. */
typedef struct drainprof_alloc_site {
    const char *file;   /* __FILE__ (static string, not owned) */
    uint32_t line;      /* __LINE__ */
    uint32_t column;    /* 0 if unavailable */
} drainprof_alloc_site;

/* Per-allocation record. Stored from alloc_register until granule_close. */
typedef struct drainprof_alloc_record {
    drainprof_alloc_id alloc_id;
    drainprof_granule_id granule_id;
    struct timespec alloc_time;
    drainprof_alloc_site alloc_site;
    size_t size;
    struct timespec free_time;   /* Zero if still live */
    int freed;                   /* 1 if freed, 0 if still live */
} drainprof_alloc_record;
```

**Per-granule diagnostic state:**

```c
/* Extended granule tracking for diagnostic mode.
 * Allocated per granule_open, freed at granule_close. */
typedef struct drainprof_granule_diag {
    _Atomic uint32_t live_count;
    struct timespec open_time;
    drainprof_alloc_record *allocs;   /* Dynamic array */
    uint32_t alloc_count;
    uint32_t alloc_capacity;
    pthread_mutex_t alloc_lock;       /* Protects allocs array */
} drainprof_granule_diag;
```

### 4.4 Pinning Report

Generated at `granule_close` when the granule is pinned (diagnostic mode only):

```c
/* A single allocation that pinned a granule. */
typedef struct drainprof_pinning_alloc {
    drainprof_alloc_id alloc_id;
    drainprof_alloc_site alloc_site;
    struct timespec alloc_time;
    size_t size;
    /* Lifetime delta: how long past the reclaim boundary this
     * allocation extends. -1 if still live at report time.
     * Computed later if/when the allocation is eventually freed. */
    double lifetime_delta_sec;
} drainprof_pinning_alloc;

/* Report generated for a single non-drainable granule close. */
typedef struct drainprof_pinning_report {
    drainprof_granule_id granule_id;
    struct timespec open_time;
    struct timespec close_time;
    drainprof_pinning_alloc *pinning_allocs;
    uint32_t pinning_count;
    uint32_t total_allocs;
    uint32_t drained_allocs;
} drainprof_pinning_report;
```

### 4.5 Snapshot

Point-in-time view of profiler state:

```c
/* Immutable snapshot of profiler state at a point in time. */
typedef struct drainprof_snapshot {
    struct timespec timestamp;
    uint64_t total_closes;
    uint64_t drainable_closes;
    uint64_t pinned_closes;
    double dsr;                  /* drainable / total */
    uint64_t open_granules;
    uint64_t peak_open_granules;
    uint64_t total_allocs;
    uint64_t total_deallocs;
} drainprof_snapshot;
```

---

## 5. C API

### 5.1 Configuration

```c
/* Profiler operating mode. */
typedef enum drainprof_mode {
    DRAINPROF_PRODUCTION = 0,   /* Minimal overhead, counters only */
    DRAINPROF_DIAGNOSTIC = 1    /* Full tracking, pinning reports */
} drainprof_mode;

/* Granule tracking strategy. */
typedef enum drainprof_storage {
    DRAINPROF_SLOT_ARRAY = 0,   /* Pre-allocated array, lock-free */
    DRAINPROF_HASH_MAP = 1      /* Dynamic hash map with rwlock */
} drainprof_storage;

/* Callback for pinning events (diagnostic mode). */
typedef void (*drainprof_pin_callback)(
    const drainprof_pinning_report *report,
    void *user_data
);

/* Profiler configuration. */
typedef struct drainprof_config {
    drainprof_mode mode;
    drainprof_storage storage;

    /* Slot array capacity. Only used when storage = SLOT_ARRAY.
     * Must be >= max simultaneously open granules.
     * Default: 256. */
    uint32_t slot_capacity;

    /* Hash map bucket count. Only used when storage = HASH_MAP.
     * Default: 64. */
    uint32_t bucket_count;

    /* Callback invoked on each pinned granule close.
     * Diagnostic mode only. If NULL, reports are buffered. */
    drainprof_pin_callback on_pinning;
    void *callback_user_data;

    /* Max buffered pinning reports before oldest dropped.
     * Only used when on_pinning is NULL. Default: 10000. */
    uint32_t max_buffered_reports;

    /* Log DSR to stderr every N granule closes. 0 = disabled. */
    uint64_t log_interval;
} drainprof_config;
```

### 5.2 Lifecycle

```c
/* Opaque profiler handle. */
typedef struct drainprof drainprof;

/* Create a profiler with default configuration.
 * Returns NULL on allocation failure. */
drainprof *drainprof_create(void);

/* Create a profiler with explicit configuration.
 * The config struct is copied; caller may free it after this call.
 * Returns NULL on allocation failure. */
drainprof *drainprof_create_with_config(const drainprof_config *config);

/* Destroy a profiler and free all resources.
 * All outstanding granules are implicitly closed.
 * Safe to call with NULL (no-op). */
void drainprof_destroy(drainprof *prof);

/* Fill config with default values.
 * Convenience for callers who want to modify only some fields. */
void drainprof_config_default(drainprof_config *config);
```

### 5.3 Granule Lifecycle

```c
/* Notify the profiler that a granule has been opened.
 * Must be called before any alloc_register for this granule.
 *
 * Production mode (slot array): O(1), lock-free.
 * Production mode (hash map): O(1) amortized, write lock.
 * Diagnostic mode: adds timestamp capture + diagnostic struct alloc.
 *
 * Returns 0 on success, -1 on error (e.g., slot array full). */
int drainprof_granule_open(drainprof *prof, drainprof_granule_id id);

/* Notify the profiler that a granule's reclaim boundary has been reached.
 * Returns 1 if the granule was drainable, 0 if pinned, -1 on error.
 *
 * Production mode (slot array): O(1), lock-free.
 * Production mode (hash map): O(1) amortized, write lock.
 * Diagnostic mode (pinned): O(n) where n = live allocs in granule. */
int drainprof_granule_close(drainprof *prof, drainprof_granule_id id);
```

### 5.4 Allocation Tracking

```c
/* Register an allocation routed to a granule.
 *
 * Production mode (slot array): O(1), lock-free (atomic increment).
 * Production mode (hash map): O(1), read lock + atomic increment.
 * Diagnostic mode: O(1) amortized, adds record to per-granule array.
 *
 * Returns 0 on success, -1 on error. */
int drainprof_alloc_register(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size
);

/* Register an allocation with source location (diagnostic mode).
 * In production mode, file/line are ignored.
 *
 * Use the DRAINPROF_ALLOC_REGISTER macro for automatic
 * __FILE__ / __LINE__ capture. */
int drainprof_alloc_register_located(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size,
    const char *file,
    uint32_t line
);

/* Convenience macro: captures __FILE__ and __LINE__ automatically. */
#define DRAINPROF_ALLOC_REGISTER(prof, gid, aid, sz) \
    drainprof_alloc_register_located((prof), (gid), (aid), (sz), \
                                     __FILE__, __LINE__)

/* Deregister an allocation (freed).
 *
 * Production mode (slot array): O(1), lock-free (atomic decrement).
 * Production mode (hash map): O(1), read lock + atomic decrement.
 * Diagnostic mode: O(1) amortized, marks record as freed.
 *
 * Returns 0 on success, -1 on error. */
int drainprof_alloc_deregister(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id
);
```

### 5.5 Queries

```c
/* Take a snapshot of current profiler state.
 * Lock-free in production mode with slot array (atomic reads only).
 * Snapshot is written to *out. */
void drainprof_snapshot(const drainprof *prof, drainprof_snapshot *out);

/* Drain buffered pinning reports (diagnostic mode).
 * Copies up to max_reports into the reports array.
 * Returns the number of reports copied.
 * Returns 0 in production mode. */
uint32_t drainprof_drain_reports(
    drainprof *prof,
    drainprof_pinning_report *reports,
    uint32_t max_reports
);

/* Free the internal allocations of a pinning report
 * (the pinning_allocs array). Call after processing each report
 * returned by drainprof_drain_reports. */
void drainprof_report_free(drainprof_pinning_report *report);

/* Reset all counters and clear all state. */
void drainprof_reset(drainprof *prof);
```

### 5.6 Reporting

```c
/* Generate a human-readable diagnostic summary.
 * Caller must free the returned string with free().
 * Returns NULL in production mode or on allocation failure.
 *
 * Example output:
 *
 *   Drainability Violation Report
 *   =============================
 *   DSR: 87.3% (12.7% of granules pinned)
 *   Total closes: 48,291 | Drainable: 42,164 | Pinned: 6,127
 *
 *   Top Pinning Sites:
 *
 *     1. src/conn_pool.c:142 -- 4,891 violations (79.8%)
 *        Avg lifetime delta: 34.2s (granule lifetime: 12ms)
 *        Avg pinned bytes: 2,048
 *        Classification: FUNDAMENTAL MISMATCH
 *
 *     2. src/cache.c:89 -- 1,104 violations (18.0%)
 *        Avg lifetime delta: 450ms (granule lifetime: 12ms)
 *        Avg pinned bytes: 512
 *        Classification: NEAR MISS
 */
char *drainprof_diagnostic_summary(const drainprof *prof);

/* Write metrics in Prometheus exposition format to a buffer.
 * Returns bytes written, or required size if buf is too small.
 * Pass NULL/0 to query required size. */
int drainprof_prometheus(const drainprof *prof, char *buf, size_t buf_size);
```

### 5.7 Violation Classification

```c
typedef enum drainprof_violation_class {
    /* Lifetime delta < 10% of granule lifetime.
     * May be addressable by adjusting granule boundaries. */
    DRAINPROF_NEAR_MISS = 0,

    /* Lifetime delta between 10% and 10x granule lifetime.
     * Significant mismatch, likely needs routing change. */
    DRAINPROF_MODERATE = 1,

    /* Lifetime delta > 10x granule lifetime.
     * Fundamental routing error. Requires architectural fix. */
    DRAINPROF_FUNDAMENTAL_MISMATCH = 2
} drainprof_violation_class;

/* Classify a violation by severity.
 * lifetime_delta_sec and granule_lifetime_sec in seconds. */
drainprof_violation_class drainprof_classify(
    double lifetime_delta_sec,
    double granule_lifetime_sec
);
```

---

## 6. Rust Bindings

The Rust crate wraps `libdrainprof` via FFI, providing a safe, idiomatic API. Published as two crates:

- `drainability-profiler-sys`: Raw FFI bindings generated by `bindgen`. Links to `libdrainprof`.
- `drainability-profiler`: Safe wrapper with Rust types, `Drop` semantics, and `Send + Sync` implementation.

### 6.1 Safe Wrapper API

```rust
use std::sync::Arc;

/// Safe wrapper around the C profiler.
/// Thread-safe (Send + Sync). Reference-counted via Arc internally.
pub struct DrainabilityProfiler {
    inner: *mut drainprof,  // Owned pointer
}

// SAFETY: The C library is thread-safe (atomic ops + rwlock).
unsafe impl Send for DrainabilityProfiler {}
unsafe impl Sync for DrainabilityProfiler {}

impl DrainabilityProfiler {
    pub fn new(config: ProfilerConfig) -> Result<Self, Error>;

    pub fn granule_open(&self, id: GranuleId) -> Result<(), Error>;
    pub fn granule_close(&self, id: GranuleId) -> Result<bool, Error>;

    pub fn alloc_register(
        &self, granule: GranuleId, alloc: AllocId, size: usize
    ) -> Result<(), Error>;

    #[track_caller]
    pub fn alloc_register_tracked(
        &self, granule: GranuleId, alloc: AllocId, size: usize
    ) -> Result<(), Error>;

    pub fn alloc_deregister(
        &self, granule: GranuleId, alloc: AllocId
    ) -> Result<(), Error>;

    pub fn snapshot(&self) -> ProfileSnapshot;
    pub fn drain_reports(&self) -> Vec<PinningReport>;
    pub fn diagnostic_summary(&self) -> Option<String>;
    pub fn reset(&self);
}

impl Drop for DrainabilityProfiler {
    fn drop(&mut self) {
        unsafe { drainprof_destroy(self.inner); }
    }
}
```

### 6.2 Allocator Integration Trait

```rust
/// Optional trait for allocators that support drainability profiling.
pub trait DrainabilityInstrumented {
    fn attach_profiler(&mut self, profiler: Arc<DrainabilityProfiler>);
    fn detach_profiler(&mut self) -> Option<Arc<DrainabilityProfiler>>;
    fn is_profiled(&self) -> bool;
}
```

### 6.3 Build Integration

The `build.rs` in `drainability-profiler-sys` compiles `libdrainprof` from source using the `cc` crate, then generates bindings with `bindgen`. This means `cargo build` compiles the C library automatically -- no separate build step required for Rust users.

```rust
// bindings/rust/drainability-profiler-sys/build.rs
fn main() {
    cc::Build::new()
        .files(glob::glob("../../../src/*.c").unwrap()
            .filter_map(Result::ok))
        .include("../../../include")
        .flag("-std=c11")
        .flag("-pthread")
        .compile("drainprof");

    let bindings = bindgen::Builder::default()
        .header("../../../include/drainprof.h")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = std::path::PathBuf::from(
        std::env::var("OUT_DIR").unwrap()
    );
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings");
}
```

---

## 7. Integration Patterns

### 7.1 Direct C Integration (Author's Allocator)

The primary integration path. The allocator calls `libdrainprof` directly:

```c
#include "drainprof.h"

static drainprof *g_profiler = NULL;

void tslab_init(void) {
    drainprof_config cfg;
    drainprof_config_default(&cfg);
    cfg.mode = DRAINPROF_PRODUCTION;
    cfg.storage = DRAINPROF_SLOT_ARRAY;
    cfg.slot_capacity = 256;
    g_profiler = drainprof_create_with_config(&cfg);
}

void epoch_open(uint64_t epoch_id) {
    /* ... existing epoch open logic ... */
    if (g_profiler)
        drainprof_granule_open(g_profiler, epoch_id);
}

void *tslab_alloc(uint64_t epoch_id, size_t size) {
    void *ptr = /* ... existing alloc logic ... */;
    if (g_profiler)
        DRAINPROF_ALLOC_REGISTER(g_profiler, epoch_id,
                                 (uint64_t)ptr, size);
    return ptr;
}

void tslab_free(uint64_t epoch_id, void *ptr) {
    /* ... existing free logic ... */
    if (g_profiler)
        drainprof_alloc_deregister(g_profiler, epoch_id,
                                   (uint64_t)ptr);
}

void epoch_close(uint64_t epoch_id) {
    if (g_profiler) {
        int drainable = drainprof_granule_close(g_profiler, epoch_id);
        /* drainable == 1: clean close
         * drainable == 0: structural leak detected */
    }
    /* ... existing epoch close logic ... */
}

void tslab_report(void) {
    if (!g_profiler) return;
    drainprof_snapshot snap;
    drainprof_snapshot(g_profiler, &snap);
    fprintf(stderr, "DSR: %.1f%% (%lu pinned / %lu total)\n",
            snap.dsr * 100.0, snap.pinned_closes, snap.total_closes);
}
```

### 7.2 Rust Wrapper Integration (bumpalo)

```rust
use drainability_profiler::*;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

pub struct ProfiledBump {
    inner: bumpalo::Bump,
    profiler: Arc<DrainabilityProfiler>,
    granule_id: GranuleId,
    next_alloc_id: AtomicU64,
}

impl ProfiledBump {
    pub fn new(
        profiler: Arc<DrainabilityProfiler>,
        granule_id: GranuleId,
    ) -> Self {
        profiler.granule_open(granule_id).unwrap();
        Self {
            inner: bumpalo::Bump::new(),
            profiler,
            granule_id,
            next_alloc_id: AtomicU64::new(0),
        }
    }

    pub fn alloc<T>(&self, val: T) -> &T {
        let alloc_id = AllocId(
            self.next_alloc_id.fetch_add(1, Ordering::Relaxed)
        );
        self.profiler.alloc_register(
            self.granule_id, alloc_id, std::mem::size_of::<T>(),
        ).unwrap();
        self.inner.alloc(val)
    }
}

impl Drop for ProfiledBump {
    fn drop(&mut self) {
        let _ = self.profiler.granule_close(self.granule_id);
    }
}
```

### 7.3 Epoch-Based Integration (Crossbeam-style, Rust)

```rust
pub struct ProfiledEpochManager {
    profiler: Arc<DrainabilityProfiler>,
    current_epoch: AtomicU64,
}

impl ProfiledEpochManager {
    pub fn advance_epoch(&self) {
        let old = GranuleId(
            self.current_epoch.fetch_add(1, Ordering::SeqCst)
        );
        let new = GranuleId(old.0 + 1);
        let _ = self.profiler.granule_close(old);
        self.profiler.granule_open(new).unwrap();
    }

    pub fn retire<T>(&self, ptr: *mut T) {
        let epoch = GranuleId(
            self.current_epoch.load(Ordering::SeqCst)
        );
        let alloc_id = AllocId(ptr as u64);
        self.profiler.alloc_register(
            epoch, alloc_id, std::mem::size_of::<T>(),
        ).unwrap();
    }

    pub fn reclaim(&self, epoch: GranuleId, alloc_id: AllocId) {
        let _ = self.profiler.alloc_deregister(epoch, alloc_id);
    }
}
```

---

## 8. Output & Reporting

### 8.1 Prometheus Metrics

Example output from `drainprof_prometheus`:

```
# HELP drainability_satisfaction_rate Fraction of granules drainable at close
# TYPE drainability_satisfaction_rate gauge
drainability_satisfaction_rate 0.95

# HELP drainability_total_closes Total granule close events
# TYPE drainability_total_closes counter
drainability_total_closes 482910

# HELP drainability_pinned_closes Granule closes with live allocations
# TYPE drainability_pinned_closes counter
drainability_pinned_closes 24145

# HELP drainability_open_granules Currently open granules
# TYPE drainability_open_granules gauge
drainability_open_granules 8
```

### 8.2 Violation Classification

The lifetime delta (how long past the reclaim boundary an allocation lives) determines the classification:

| Class | Condition | Interpretation | Remediation |
|---|---|---|---|
| NEAR_MISS | delta < 10% of granule lifetime | Barely outlives granule | Adjust granule boundaries |
| MODERATE | 10% to 10x | Significant mismatch | Consider routing change |
| FUNDAMENTAL_MISMATCH | > 10x | Wrong lifetime class entirely | Architectural routing fix |

---

## 9. Performance Budget

### 9.1 Production Mode Targets (Slot Array)

| Operation | Target | Mechanism |
|---|---|---|
| `drainprof_granule_open` | < 20 ns | Atomic store to slot |
| `drainprof_granule_close` | < 20 ns | Atomic load + atomic store to clear slot |
| `drainprof_alloc_register` | < 10 ns | Atomic increment, no lock |
| `drainprof_alloc_deregister` | < 10 ns | Atomic decrement, no lock |
| `drainprof_snapshot` | < 50 ns | Seven atomic loads |
| Memory per granule slot | 8 bytes | atomic count (4) + occupied flag (4) |
| Global overhead | 56 bytes | Seven atomic uint64 counters |
| Total (256 slots) | ~2.1 KB | Fixed, no growth |

The slot array is the critical design choice. With epoch-based allocators, granule IDs are dense sequential integers. `granule_id % capacity` gives a direct array index. No hashing, no locking, no pointer chasing on the hot path. The hot path (`alloc_register` / `alloc_deregister`) is a single `atomic_fetch_add` / `atomic_fetch_sub` on a known memory location.

### 9.2 Production Mode Targets (Hash Map)

| Operation | Target | Mechanism |
|---|---|---|
| `drainprof_granule_open` | < 100 ns | Write lock + insert |
| `drainprof_granule_close` | < 100 ns | Write lock + remove |
| `drainprof_alloc_register` | < 30 ns | Read lock + atomic increment |
| `drainprof_alloc_deregister` | < 30 ns | Read lock + atomic decrement |

### 9.3 Diagnostic Mode Overhead

| Operation | Overhead | Mechanism |
|---|---|---|
| `alloc_register` | ~200 ns | Atomic + clock_gettime + array push + location |
| `alloc_deregister` | ~100 ns | Atomic + clock_gettime + linear scan for alloc_id |
| `granule_close` (pinned) | O(n) | Scan live allocations, build pinning report |
| Memory per allocation | ~80 bytes | drainprof_alloc_record struct |

Diagnostic mode is not intended for always-on production use. It is enabled during investigation, similar to running under `perf` or `strace`.

---

## 10. Build System & Project Structure

```
libdrainprof/
├── Makefile
├── LICENSE                      (MIT)
├── README.md
├── include/
│   └── drainprof.h              # Public API header
├── src/
│   ├── drainprof.c              # Core profiler implementation
│   ├── counters.c               # Atomic counter operations
│   ├── slot_array.c             # Pre-allocated slot array
│   ├── hash_map.c               # Hash map with rwlock
│   ├── diagnostic.c             # Diagnostic mode, pinning reports
│   ├── report.c                 # Summary generation, classification
│   └── prometheus.c             # Metrics export
├── test/
│   ├── test_production.c        # Production mode tests
│   ├── test_diagnostic.c        # Diagnostic mode tests
│   ├── test_slot_array.c        # Slot array edge cases
│   ├── test_concurrent.c        # Multi-threaded stress tests
│   └── test_psweep.c            # Theorem 3 validation via profiler
├── bench/
│   ├── bench_hot_path.c         # alloc_register / deregister latency
│   └── bench_contention.c       # Multi-threaded throughput
├── examples/
│   ├── basic.c                  # Minimal usage example
│   └── epoch_integration.c      # Epoch-based allocator example
└── bindings/
    └── rust/
        ├── drainability-profiler-sys/
        │   ├── Cargo.toml
        │   ├── build.rs          # Compile libdrainprof, run bindgen
        │   └── src/lib.rs        # Raw FFI bindings
        └── drainability-profiler/
            ├── Cargo.toml
            ├── src/
            │   ├── lib.rs        # Public re-exports
            │   ├── profiler.rs   # Safe wrapper
            │   ├── types.rs      # Rust types (GranuleId, etc.)
            │   ├── config.rs     # ProfilerConfig
            │   ├── report.rs     # PinningReport, ViolationClass
            │   └── snapshot.rs   # ProfileSnapshot
            └── examples/
                ├── basic_usage.rs
                ├── bumpalo_integration.rs
                └── prometheus.rs
```

### 10.1 Build Targets

```makefile
CC ?= gcc
CFLAGS = -O2 -std=c11 -pthread -Iinclude -Wall -Wextra -Wpedantic

# Static library (primary artifact)
libdrainprof.a: src/*.c
	$(CC) $(CFLAGS) -c src/*.c
	ar rcs $@ *.o
	rm -f *.o

# Shared library
libdrainprof.so: src/*.c
	$(CC) $(CFLAGS) -shared -fPIC src/*.c -o $@

# Tests
test: libdrainprof.a
	$(CC) $(CFLAGS) test/*.c -L. -ldrainprof -o run_tests
	./run_tests

# Benchmarks
bench: libdrainprof.a
	$(CC) $(CFLAGS) bench/*.c -L. -ldrainprof -o run_bench
	./run_bench

clean:
	rm -f *.o *.a *.so run_tests run_bench
```

---

## 11. Open Questions

**1. `alloc_deregister` without granule ID.** Some allocators (e.g., `free(ptr)` in general-purpose allocators) don't know which granule an allocation belongs to at dealloc time. Decision deferred: M1-M3 scope to allocators where the granule is explicit (arenas, epochs). General-purpose allocator support would require a reverse map (alloc_id -> granule_id) maintained by the profiler, adding ~16 bytes per live allocation. Added as a future milestone.

**2. Sampling.** For allocators exceeding ~10M allocs/sec, even atomic operations may be measurable. A `sample_rate` config field that instruments 1-in-N allocations reduces overhead at the cost of statistical precision. Deferred until benchmarks on M1 show whether it's needed.

**3. Lifetime delta computation.** The lifetime delta for a pinning allocation is only known when it is eventually freed, which may be long after the pinning report is generated. M2 will report `lifetime_delta_sec = -1` (still live) at granule close and not attempt retroactive updates. Users re-query or infer from allocation site context.

**4. Thread-local optimization.** For very high-throughput allocators, a thread-local counter buffer that periodically flushes to the global counters could reduce atomic contention. Deferred until `bench_contention` shows it's necessary.

---

## 12. Milestones

**M1: C core (production mode).** `drainprof.h`, slot array implementation, atomic counters, `granule_open`/`close`, `alloc_register`/`deregister`, `snapshot`. Test suite. Benchmark suite proving < 10ns per `alloc_register` on the slot array path. Integrate with author's C allocator and reproduce p-sweep validation through the profiler.

**M2: Diagnostic mode.** `alloc_record`, pinning reports, `diagnostic_summary`, violation classification. Feature-complete C library. Test suite for diagnostic mode. `test_psweep.c` validating Theorem 3 via profiler instrumentation.

**M3: Rust bindings.** `drainability-profiler-sys` (bindgen + cc build) and `drainability-profiler` (safe wrapper). Published to crates.io. bumpalo integration example.

**M4: Prometheus export.** `drainprof_prometheus` function. Optional Rust feature flag.

**M5: Blog post and announcement.** "Detecting memory leaks Valgrind can't find" -- walk through using the profiler to catch a structural leak. Demonstrate with author's C allocator. Link to paper and crate.