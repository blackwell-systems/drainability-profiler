# drainability-profiler: Architecture & API Design

**Crate:** `drainability-profiler`
**Author:** Dayna Blackwell
**Status:** Design Document (Pre-Implementation)
**Paper Reference:** Blackwell, D. (2026). *Drainability: When Coarse-Grained Memory Reclamation Produces Bounded Retention.* doi:10.5281/zenodo.18653776

---

## 1. Purpose

A standalone Rust crate that detects structural leaks in any coarse-grained, non-relocating allocator. The profiler measures the drainability satisfaction rate -- the fraction of granules that are fully drained at their reclaim boundary -- and, in diagnostic mode, identifies the specific allocations responsible for pinning.

No such tool currently exists. Conventional leak detectors (Valgrind, ASan) operate at the object level and cannot detect granule-level retention failures. This crate closes that diagnostic gap.

---

## 2. Design Principles

**Allocator-agnostic.** The profiler defines a trait that any allocator can implement. It does not depend on bumpalo, jemalloc, or any specific allocator's internals.

**Two-mode operation.** Production mode adds near-zero overhead (one comparison per granule close). Diagnostic mode adds per-allocation tracking for root-cause analysis. The modes use the same API; only the profiler configuration differs.

**Zero allocations in the hot path (production mode).** Production mode uses fixed-size atomic counters. No heap allocation occurs on granule open, allocation registration, or granule close.

**Non-invasive.** The profiler is an observer. It does not modify allocator behavior, routing decisions, or reclamation policy. It measures and reports.

---

## 3. Core Concepts

### 3.1 Terminology Mapping

The profiler uses the paper's terminology. Integrations map allocator-specific names to these concepts:

| Profiler Concept | Paper Term | bumpalo | jemalloc | Crossbeam EBR |
|---|---|---|---|---|
| `GranuleId` | Granule g | Arena / Chunk | Slab | Epoch |
| `granule_open` | t_open(g) | Arena::new | Slab creation | Epoch advance |
| `granule_close` | t_reclaim(g) | Arena::reset/drop | Slab reclaim attempt | Epoch quiescence |
| `alloc_register` | ρ(a) = g | alloc_layout | Object allocation | Retire / defer |
| `alloc_deregister` | t_free(a) | (manual or drop) | Object deallocation | Reclamation |

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

```rust
/// Opaque granule identifier. Assigned by the allocator.
/// Must be unique among currently-open granules.
/// May be reused after granule_close returns.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct GranuleId(pub u64);

/// Opaque allocation identifier. Assigned by the allocator.
/// Must be unique within a granule.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct AllocId(pub u64);
```

### 4.2 Production Mode State

```rust
use std::sync::atomic::{AtomicU64, Ordering};

/// Fixed-size, lock-free counters. No heap allocation.
pub struct ProductionCounters {
    /// Total granule close events observed.
    total_closes: AtomicU64,

    /// Granule closes where all allocations were freed (drainable).
    drainable_closes: AtomicU64,

    /// Granule closes where at least one allocation was live (pinned).
    pinned_closes: AtomicU64,

    /// Running count of currently-open granules.
    open_granules: AtomicU64,

    /// High-water mark of simultaneously open granules.
    peak_open_granules: AtomicU64,

    /// Total allocations registered across all granules.
    total_allocs: AtomicU64,

    /// Total allocations deregistered (freed) across all granules.
    total_deallocs: AtomicU64,
}
```

**Per-granule tracking (production mode).** Each open granule needs exactly one value: the count of live allocations. This is stored in a concurrent map, keyed by `GranuleId`, holding an `AtomicU32` live count. On `alloc_register`, the count is incremented. On `alloc_deregister`, it is decremented. On `granule_close`, the count is read: if zero, the granule is drainable; if nonzero, it is pinned. The entry is then removed.

```rust
use std::collections::HashMap;
use std::sync::RwLock;

/// Per-granule live allocation count.
/// Only exists for currently-open granules.
struct GranuleLiveCount {
    live: AtomicU32,
}

/// Map from GranuleId to live count.
/// Entries exist only for open granules.
/// RwLock is taken as read for alloc/dealloc (concurrent),
/// write only for granule_open/close (infrequent).
type GranuleMap = RwLock<HashMap<GranuleId, GranuleLiveCount>>;
```

**Overhead estimate (production mode):** 12 bytes per open granule (8-byte key + 4-byte atomic count), plus 56 bytes for the global counters. For a system with 100 simultaneously open granules, total overhead is ~1.3 KB.

### 4.3 Diagnostic Mode State

Diagnostic mode extends production mode with per-allocation metadata:

```rust
/// Recorded at alloc_register time. Stored until granule_close.
#[derive(Debug, Clone)]
pub struct AllocRecord {
    /// Allocation identifier (assigned by allocator).
    pub alloc_id: AllocId,

    /// Granule this allocation was routed to.
    pub granule_id: GranuleId,

    /// Timestamp of allocation.
    pub alloc_time: Instant,

    /// Source location of the allocation call.
    /// Captured via caller_location or passed explicitly.
    pub alloc_site: AllocationSite,

    /// Size in bytes of this allocation.
    pub size: usize,

    /// Set on dealloc. None if still live at observation time.
    pub free_time: Option<Instant>,
}

/// Source location of an allocation.
#[derive(Debug, Clone)]
pub struct AllocationSite {
    pub file: &'static str,
    pub line: u32,
    pub column: u32,
}
```

**Per-granule diagnostic state:**

```rust
/// Extended granule tracking for diagnostic mode.
struct GranuleDiagnostic {
    /// Base live count (same as production mode).
    live: AtomicU32,

    /// Timestamp when this granule was opened.
    open_time: Instant,

    /// All allocations routed to this granule.
    /// Only populated in diagnostic mode.
    allocs: RwLock<Vec<AllocRecord>>,
}
```

### 4.4 Pinning Report

Generated at `granule_close` when the granule is pinned (diagnostic mode only):

```rust
/// Report generated for a single non-drainable granule close.
#[derive(Debug, Clone)]
pub struct PinningReport {
    /// The granule that was pinned.
    pub granule_id: GranuleId,

    /// When the granule was opened.
    pub open_time: Instant,

    /// When the granule was closed (reclaim boundary).
    pub close_time: Instant,

    /// Allocations that were still live at close time.
    pub pinning_allocations: Vec<PinningAllocation>,

    /// Total allocations that were routed to this granule.
    pub total_allocations: u32,

    /// Allocations that completed before close.
    pub drained_allocations: u32,
}

/// A single allocation that pinned a granule.
#[derive(Debug, Clone)]
pub struct PinningAllocation {
    pub alloc_id: AllocId,

    /// Source location of the allocation.
    pub alloc_site: AllocationSite,

    /// When the allocation was made.
    pub alloc_time: Instant,

    /// Size of the allocation in bytes.
    pub size: usize,

    /// How long past the reclaim boundary this allocation extends.
    /// This is the "lifetime delta" from the paper.
    /// Computed later when the allocation is eventually freed.
    /// None if the allocation is still live at report time.
    pub lifetime_delta: Option<Duration>,
}
```

### 4.5 Snapshot

Point-in-time view of profiler state, returned by `snapshot()`:

```rust
/// Immutable snapshot of profiler state at a point in time.
#[derive(Debug, Clone)]
pub struct ProfileSnapshot {
    /// When this snapshot was taken.
    pub timestamp: Instant,

    /// Total granule closes observed.
    pub total_closes: u64,

    /// Granule closes that were drainable.
    pub drainable_closes: u64,

    /// Granule closes that were pinned.
    pub pinned_closes: u64,

    /// Drainability satisfaction rate: drainable / total.
    pub dsr: f64,

    /// Currently open granules.
    pub open_granules: u64,

    /// Peak simultaneously open granules.
    pub peak_open_granules: u64,

    /// Total allocations observed.
    pub total_allocs: u64,

    /// Total deallocations observed.
    pub total_deallocs: u64,
}
```

---

## 5. API

### 5.1 Configuration

```rust
/// Profiler operating mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProfileMode {
    /// Minimal overhead. Tracks one counter per open granule.
    /// No allocation metadata recorded.
    Production,

    /// Full tracking. Records allocation site, timestamp, and size
    /// for every allocation. Generates PinningReports on violation.
    Diagnostic,
}

/// Profiler configuration.
#[derive(Debug, Clone)]
pub struct ProfilerConfig {
    /// Operating mode.
    pub mode: ProfileMode,

    /// Optional callback invoked on each pinned granule close.
    /// Diagnostic mode only.
    /// If None, pinning reports are buffered internally.
    pub on_pinning: Option<fn(PinningReport)>,

    /// Maximum pinning reports to buffer before oldest are dropped.
    /// Only used when on_pinning is None. Default: 10_000.
    pub max_buffered_reports: usize,

    /// Enable periodic DSR logging to stderr.
    /// Logs every N granule closes. 0 = disabled.
    pub log_interval: u64,
}

impl Default for ProfilerConfig {
    fn default() -> Self {
        Self {
            mode: ProfileMode::Production,
            on_pinning: None,
            max_buffered_reports: 10_000,
            log_interval: 0,
        }
    }
}
```

### 5.2 The Profiler

```rust
/// The drainability profiler.
///
/// Thread-safe. Designed to be created once and shared via Arc
/// or stored as a static.
///
/// # Usage
///
/// ```rust
/// let profiler = DrainabilityProfiler::new(ProfilerConfig::default());
///
/// // Allocator calls these at the appropriate points:
/// profiler.granule_open(granule_id);
/// profiler.alloc_register(granule_id, alloc_id, size);
/// profiler.alloc_deregister(granule_id, alloc_id);
/// profiler.granule_close(granule_id);
///
/// // Query at any time:
/// let snap = profiler.snapshot();
/// println!("DSR: {:.2}%", snap.dsr * 100.0);
/// ```
pub struct DrainabilityProfiler {
    config: ProfilerConfig,
    counters: ProductionCounters,
    // Internal state varies by mode
    // ...
}

impl DrainabilityProfiler {
    /// Create a new profiler with the given configuration.
    pub fn new(config: ProfilerConfig) -> Self;

    // --- Granule lifecycle ---

    /// Notify the profiler that a granule has been opened.
    /// Must be called before any alloc_register for this granule.
    ///
    /// Production mode: O(1) amortized (hash map insert).
    /// Diagnostic mode: O(1) amortized + timestamp capture.
    pub fn granule_open(&self, id: GranuleId);

    /// Notify the profiler that a granule's reclaim boundary has been reached.
    /// Returns true if the granule was drainable, false if pinned.
    ///
    /// Production mode: O(1) (atomic read + hash map remove).
    /// Diagnostic mode: O(n) where n = live allocations in granule
    ///     (must scan to build PinningReport).
    ///
    /// Panics (debug) if id was not previously opened.
    pub fn granule_close(&self, id: GranuleId) -> bool;

    // --- Allocation tracking ---

    /// Register an allocation routed to a granule.
    ///
    /// Production mode: O(1) (atomic increment).
    /// Diagnostic mode: O(1) amortized (atomic increment + vec push).
    pub fn alloc_register(&self, granule_id: GranuleId, alloc_id: AllocId, size: usize);

    /// Register an allocation with source location (diagnostic mode).
    /// In production mode, the site parameter is ignored.
    ///
    /// This is the preferred method when caller_location is available.
    #[track_caller]
    pub fn alloc_register_tracked(
        &self,
        granule_id: GranuleId,
        alloc_id: AllocId,
        size: usize,
    );

    /// Deregister an allocation (freed).
    ///
    /// Production mode: O(1) (atomic decrement).
    /// Diagnostic mode: O(1) amortized (atomic decrement + timestamp record).
    ///
    /// Panics (debug) if alloc_id was not registered in granule_id.
    pub fn alloc_deregister(&self, granule_id: GranuleId, alloc_id: AllocId);

    // --- Queries ---

    /// Take a snapshot of current profiler state.
    /// Lock-free in production mode (atomic reads only).
    pub fn snapshot(&self) -> ProfileSnapshot;

    /// Drain buffered pinning reports (diagnostic mode).
    /// Returns an empty vec in production mode.
    pub fn drain_reports(&self) -> Vec<PinningReport>;

    /// Reset all counters and clear all state.
    pub fn reset(&self);
}
```

### 5.3 Allocator Integration Trait

```rust
/// Trait for allocators that support drainability profiling.
///
/// Implementors call into a DrainabilityProfiler at the appropriate
/// lifecycle points. This trait defines the mapping between the
/// allocator's concepts and the profiler's granule/allocation model.
///
/// The trait is optional -- allocators can also call the profiler
/// directly without implementing this trait.
pub trait DrainabilityInstrumented {
    /// Attach a profiler to this allocator.
    fn attach_profiler(&mut self, profiler: Arc<DrainabilityProfiler>);

    /// Detach the profiler, returning it.
    fn detach_profiler(&mut self) -> Option<Arc<DrainabilityProfiler>>;

    /// Whether a profiler is currently attached.
    fn is_profiled(&self) -> bool;
}
```

---

## 6. Integration Patterns

### 6.1 Wrapper Integration (Non-Invasive)

For allocators you don't own, wrap the allocator and intercept lifecycle events:

```rust
use drainability_profiler::*;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

/// Profiled wrapper around bumpalo::Bump.
pub struct ProfiledBump {
    inner: bumpalo::Bump,
    profiler: Arc<DrainabilityProfiler>,
    granule_id: GranuleId,
    next_alloc_id: AtomicU64,
}

impl ProfiledBump {
    pub fn new(profiler: Arc<DrainabilityProfiler>, granule_id: GranuleId) -> Self {
        profiler.granule_open(granule_id);
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
            self.granule_id,
            alloc_id,
            std::mem::size_of::<T>(),
        );
        self.inner.alloc(val)
    }
}

impl Drop for ProfiledBump {
    fn drop(&mut self) {
        // Arena drop = granule close.
        // All allocations in this arena are implicitly freed.
        let was_drainable = self.profiler.granule_close(self.granule_id);
        if !was_drainable {
            // Optional: log warning
        }
    }
}
```

### 6.2 Epoch-Based Integration

For epoch-based reclamation systems (Crossbeam-style):

```rust
/// Profiled epoch manager.
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

        // Close old epoch (reclaim boundary reached).
        self.profiler.granule_close(old);

        // Open new epoch.
        self.profiler.granule_open(new);
    }

    pub fn retire<T>(&self, ptr: *mut T) {
        let epoch = GranuleId(
            self.current_epoch.load(Ordering::SeqCst)
        );
        let alloc_id = AllocId(ptr as u64);
        self.profiler.alloc_register(
            epoch,
            alloc_id,
            std::mem::size_of::<T>(),
        );
    }

    pub fn reclaim(&self, epoch: GranuleId, alloc_id: AllocId) {
        self.profiler.alloc_deregister(epoch, alloc_id);
    }
}
```

### 6.3 Global Allocator Integration

For integration with `#[global_allocator]`, the profiler can be wrapped behind a `GlobalAlloc` implementation that intercepts `alloc` and `dealloc`. This requires external granule boundary signaling since `GlobalAlloc` has no concept of granules:

```rust
use std::alloc::{GlobalAlloc, Layout};

pub struct ProfiledGlobalAlloc<A: GlobalAlloc> {
    inner: A,
    profiler: Arc<DrainabilityProfiler>,
}

impl<A: GlobalAlloc> ProfiledGlobalAlloc<A> {
    /// Signal that the current scope constitutes a granule boundary.
    /// Called by application code, not by the allocator.
    pub fn mark_granule_boundary(&self, closing: GranuleId, opening: GranuleId) {
        self.profiler.granule_close(closing);
        self.profiler.granule_open(opening);
    }
}
```

---

## 7. Output & Reporting

### 7.1 Metrics Export

```rust
/// Metrics suitable for export to Prometheus, StatsD, or similar.
#[derive(Debug, Clone)]
pub struct ProfileMetrics {
    /// Drainability satisfaction rate (0.0 to 1.0).
    pub dsr: f64,

    /// Granule closes per second (windowed).
    pub close_rate: f64,

    /// Pinned closes per second (windowed).
    pub pin_rate: f64,

    /// Currently open granules.
    pub open_granules: u64,

    /// Estimated retained bytes due to pinning.
    /// Only available in diagnostic mode.
    pub estimated_retained_bytes: Option<u64>,
}

impl DrainabilityProfiler {
    /// Export current metrics for monitoring integration.
    pub fn metrics(&self) -> ProfileMetrics;

    /// Format metrics as Prometheus exposition format.
    pub fn prometheus_exposition(&self) -> String;
}
```

Example Prometheus output:

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

### 7.2 Diagnostic Report Format

For diagnostic mode, pinning reports can be aggregated into a human-readable summary:

```rust
impl DrainabilityProfiler {
    /// Generate a summary report of pinning violations.
    /// Groups by allocation site, sorted by frequency.
    ///
    /// Example output:
    ///
    /// ```text
    /// Drainability Violation Report
    /// =============================
    /// DSR: 87.3% (12.7% of granules pinned)
    /// Total closes: 48,291 | Drainable: 42,164 | Pinned: 6,127
    ///
    /// Top Pinning Sites:
    ///
    ///   1. src/conn_pool.rs:142 — 4,891 violations (79.8%)
    ///      Avg lifetime delta: 34.2s (granule lifetime: 12ms)
    ///      Avg pinned bytes: 2,048
    ///      Classification: FUNDAMENTAL MISMATCH
    ///
    ///   2. src/cache.rs:89 — 1,104 violations (18.0%)
    ///      Avg lifetime delta: 450ms (granule lifetime: 12ms)
    ///      Avg pinned bytes: 512
    ///      Classification: NEAR MISS
    ///
    ///   3. src/handler.rs:201 — 132 violations (2.2%)
    ///      Avg lifetime delta: 2ms (granule lifetime: 12ms)
    ///      Avg pinned bytes: 64
    ///      Classification: NEAR MISS
    /// ```
    pub fn diagnostic_summary(&self) -> String;
}
```

### 7.3 Violation Classification

The lifetime delta determines the classification:

```rust
/// Classification of a drainability violation by severity.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ViolationClass {
    /// Lifetime delta < 10% of granule lifetime.
    /// May be addressable by adjusting granule boundaries.
    NearMiss,

    /// Lifetime delta between 10% and 10x granule lifetime.
    /// Significant mismatch, likely needs routing change.
    Moderate,

    /// Lifetime delta > 10x granule lifetime.
    /// Fundamental routing error. Different lifetime class
    /// in wrong granule. Requires architectural fix.
    FundamentalMismatch,
}

impl ViolationClass {
    pub fn classify(lifetime_delta: Duration, granule_lifetime: Duration) -> Self {
        let ratio = lifetime_delta.as_secs_f64()
            / granule_lifetime.as_secs_f64();
        if ratio < 0.1 {
            Self::NearMiss
        } else if ratio < 10.0 {
            Self::Moderate
        } else {
            Self::FundamentalMismatch
        }
    }
}
```

---

## 8. Performance Budget

### 8.1 Production Mode Targets

| Operation | Target | Mechanism |
|---|---|---|
| `granule_open` | < 100 ns | Hash map insert (write lock, amortized) |
| `granule_close` | < 100 ns | Atomic read + hash map remove (write lock) |
| `alloc_register` | < 30 ns | Atomic increment (read lock) |
| `alloc_deregister` | < 30 ns | Atomic decrement (read lock) |
| `snapshot` | < 50 ns | Seven atomic loads, no locks |
| Memory per open granule | 12 bytes | Key (8) + atomic count (4) |
| Global overhead | 56 bytes | Seven AtomicU64 counters |

The critical insight: `alloc_register` and `alloc_deregister` are the hot path (called per allocation). In production mode, these are a single atomic increment/decrement behind a read lock. The read lock is uncontended in the common case because `granule_open`/`granule_close` are infrequent relative to allocations.

### 8.2 Diagnostic Mode Overhead

| Operation | Overhead | Mechanism |
|---|---|---|
| `alloc_register` | ~200 ns | Atomic + timestamp + vec push + caller location |
| `alloc_deregister` | ~100 ns | Atomic + timestamp + linear scan for alloc_id |
| `granule_close` (pinned) | O(n) | Scan live allocations, build PinningReport |
| Memory per allocation | ~80 bytes | AllocRecord struct |

Diagnostic mode is not intended for always-on production use. It is enabled during investigation, similar to running under `perf` or `strace`.

### 8.3 Lock Contention Analysis

The `RwLock<HashMap>` for granule tracking is the main contention point. The access pattern is favorable: `alloc_register` and `alloc_deregister` take the read lock (concurrent access to the map, then atomic operations on the per-granule counter). Only `granule_open` and `granule_close` take the write lock, and these are orders of magnitude less frequent than allocations.

**Alternative considered:** `DashMap` (concurrent hash map) would eliminate the read/write lock distinction but adds a dependency. A lock-free approach using a pre-allocated array indexed by `GranuleId` is possible if granule IDs are dense and bounded, which they are for most allocators (number of simultaneously open granules is small). This array-based approach would reduce `alloc_register`/`alloc_deregister` to a single atomic operation with no locking.

```rust
/// Array-based granule tracking for minimal-overhead production mode.
/// Requires that GranuleId values are dense in [0, MAX_OPEN_GRANULES).
pub struct ArrayGranuleMap {
    /// Fixed-size array. Index = GranuleId.0 % capacity.
    slots: Box<[AtomicU32]>,
    capacity: usize,
}
```

---

## 9. Crate Structure

```
drainability-profiler/
├── Cargo.toml
├── README.md
├── src/
│   ├── lib.rs              # Public API re-exports
│   ├── profiler.rs          # DrainabilityProfiler implementation
│   ├── config.rs            # ProfilerConfig, ProfileMode
│   ├── types.rs             # GranuleId, AllocId, AllocRecord, etc.
│   ├── counters.rs          # ProductionCounters (atomic counters)
│   ├── granule_map.rs       # GranuleMap (HashMap + RwLock)
│   ├── granule_array.rs     # ArrayGranuleMap (lock-free alternative)
│   ├── report.rs            # PinningReport, ViolationClass, diagnostics
│   ├── metrics.rs           # ProfileMetrics, Prometheus export
│   └── snapshot.rs          # ProfileSnapshot
├── examples/
│   ├── basic_usage.rs       # Minimal example
│   ├── epoch_profiling.rs   # Epoch-based allocator integration
│   └── prometheus.rs        # Metrics export example
├── benches/
│   ├── production_mode.rs   # Benchmark hot-path overhead
│   └── diagnostic_mode.rs   # Benchmark diagnostic overhead
└── integrations/            # Optional: thin integration crates
    └── bumpalo/
        ├── Cargo.toml
        └── src/lib.rs       # ProfiledBump wrapper
```

### 9.1 Feature Flags

```toml
[features]
default = []

# Enable diagnostic mode support.
# Without this, only production mode is compiled.
# Reduces binary size and eliminates diagnostic-only code paths.
diagnostic = []

# Enable Prometheus metrics export.
prometheus = []

# Enable serde serialization for reports and snapshots.
serde = ["dep:serde"]
```

---

## 10. Usage Examples

### 10.1 Production Monitoring

```rust
use drainability_profiler::*;
use std::sync::Arc;

fn main() {
    let profiler = Arc::new(DrainabilityProfiler::new(ProfilerConfig {
        mode: ProfileMode::Production,
        log_interval: 10_000, // Log DSR every 10K closes
        ..Default::default()
    }));

    // Pass profiler to allocator integration...
    // In a metrics endpoint:
    let snap = profiler.snapshot();
    if snap.dsr < 0.95 {
        eprintln!(
            "WARNING: DSR dropped to {:.1}% ({} pinned / {} total)",
            snap.dsr * 100.0,
            snap.pinned_closes,
            snap.total_closes,
        );
    }
}
```

### 10.2 Diagnostic Investigation

```rust
use drainability_profiler::*;
use std::sync::Arc;

fn main() {
    let profiler = Arc::new(DrainabilityProfiler::new(ProfilerConfig {
        mode: ProfileMode::Diagnostic,
        on_pinning: Some(|report| {
            for alloc in &report.pinning_allocations {
                eprintln!(
                    "PINNING: {}:{} allocated {} bytes, \
                     still live at granule close",
                    alloc.alloc_site.file,
                    alloc.alloc_site.line,
                    alloc.size,
                );
            }
        }),
        ..Default::default()
    }));

    // Run workload...

    // After investigation:
    println!("{}", profiler.diagnostic_summary());
}
```

### 10.3 Integration Test (Validating the Paper)

```rust
#[test]
fn theorem3_p_sweep() {
    for p in [0.0, 0.01, 0.05, 0.10, 0.25, 0.50, 1.0] {
        let profiler = DrainabilityProfiler::new(
            ProfilerConfig::default()
        );

        let mut rng = rand::thread_rng();
        let epochs = 100_000u64;

        for epoch in 0..epochs {
            let gid = GranuleId(epoch);
            profiler.granule_open(gid);

            // Request allocation (always freed before close)
            let req = AllocId(epoch * 2);
            profiler.alloc_register(gid, req, 128);
            profiler.alloc_deregister(gid, req);

            // Session allocation (never freed) with probability p
            if rng.gen::<f64>() < p {
                let session = AllocId(epoch * 2 + 1);
                profiler.alloc_register(gid, session, 256);
                // Not deregistered -- pins the granule
            }

            profiler.granule_close(gid);
        }

        let snap = profiler.snapshot();

        if p == 0.0 {
            assert_eq!(snap.pinned_closes, 0);
            assert!((snap.dsr - 1.0).abs() < 1e-9);
        } else {
            let observed_p = snap.pinned_closes as f64
                / snap.total_closes as f64;
            let tolerance = if p < 0.05 { 0.02 } else { p * 0.1 };
            assert!(
                (observed_p - p).abs() < tolerance,
                "p={}: expected ~{}, got {}",
                p, p, observed_p
            );
        }
    }
}
```

---

## 11. Open Questions

**1. Granule ID assignment.** Should the profiler assign granule IDs, or should the allocator? Current design: allocator assigns, since it knows its own numbering scheme. Risk: collisions if the allocator reuses IDs for concurrently open granules.

**2. `alloc_deregister` without granule ID.** Some allocators (e.g., `free(ptr)`) don't know which granule an allocation belongs to at dealloc time. Options: (a) require the allocator to track the mapping, (b) maintain a reverse map (AllocId -> GranuleId) in the profiler at the cost of memory and lookup time, (c) only support allocators that know the granule at dealloc time.

**3. Sampling.** For extremely high-throughput allocators (millions of allocs/sec), even atomic increments may be measurable. A sampling mode that instruments 1-in-N allocations could reduce overhead further at the cost of statistical precision.

**4. Async/multi-threaded granule lifecycles.** Some allocators open a granule on one thread and close it on another. The current API supports this (GranuleId is passed explicitly), but the locking strategy may need refinement for high-contention scenarios.

**5. Lifetime delta computation.** The lifetime delta for a pinning allocation is only known when the allocation is eventually freed, which may be long after the PinningReport is generated. Options: (a) update reports retroactively, (b) track pending deltas separately, (c) report "still live" and let the user re-query.

---

## 12. Milestones

**M1: Core profiler (production mode).** GranuleId/AllocId types, ProductionCounters, GranuleMap, granule_open/close, alloc_register/deregister, snapshot. No diagnostics. Benchmark suite proving < 30ns per alloc_register.

**M2: Diagnostic mode.** AllocRecord, PinningReport, ViolationClass, diagnostic_summary. Feature-gated behind `diagnostic`.

**M3: bumpalo integration.** ProfiledBump wrapper as a separate crate (`drainability-profiler-bumpalo`). Example and test demonstrating structural leak detection.

**M4: Metrics export.** Prometheus exposition format. Optional feature flag.

**M5: Blog post and announcement.** "Detecting memory leaks Valgrind can't find" -- walk through using the profiler to catch a structural leak in a real-looking example. Link to paper.