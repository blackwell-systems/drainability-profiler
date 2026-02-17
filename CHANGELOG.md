# Changelog

All notable changes to libdrainprof will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-02-16

### Added - M1: Core Library Foundation

**Core Implementation:**
- Initial release of libdrainprof C library
- Production mode profiler with lock-free slot array storage
- Thread-safe atomic operations using C11 `stdatomic.h`
- Zero-copy snapshot API for reading metrics

**API Functions:**
- `drainprof_create()` - Create profiler with default config
- `drainprof_create_with_config()` - Create with custom configuration
- `drainprof_destroy()` - Cleanup and free resources
- `drainprof_granule_open()` - Begin tracking a granule
- `drainprof_granule_close()` - End tracking and check drainability
- `drainprof_alloc_register()` - Register allocation in granule
- `drainprof_alloc_deregister()` - Deregister allocation (free)
- `drainprof_alloc_register_located()` - Register with source location
- `drainprof_snapshot()` - Read current metrics atomically
- `drainprof_reset()` - Reset all counters
- `DRAINPROF_ALLOC_REGISTER()` - Macro capturing `__FILE__` and `__LINE__`

**Metrics Tracked:**
- Total closes (granules closed)
- Drainable closes (granules with no live allocations)
- Pinned closes (granules with remaining allocations)
- DSR (Drainability Satisfaction Rate)
- Total allocations registered
- Total deallocations registered
- Current open granules
- Peak simultaneous open granules

**Storage Backend:**
- Slot array with configurable capacity (default: 1024)
- Direct index mapping: `slot_index = granule_id % capacity`
- CAS-based slot occupation to prevent collisions
- Lock-free granule tracking using atomic counters

**Performance:**
- **Hot path overhead:** < 2ns per operation (1.77-1.97ns measured)
- 5x faster than the 10ns target
- Lock-free atomic increment/decrement only
- Zero malloc() in hot path
- Thread-safe without global locks

**Testing:**
- 7 comprehensive test cases covering:
  - Basic creation and destruction
  - Single drainable granule lifecycle
  - Single pinned granule (allocation not freed)
  - Multiple allocations per granule
  - Sequential granule tracking (100 granules)
  - Peak open granule tracking
  - **Theorem 3 validation:** P-sweep with p ∈ {0.0, 0.1, 0.5, 1.0}
- All tests pass with exact DSR = 1.0 - p validation

**Build System:**
- Makefile with targets for: `all`, `test`, `bench`, `examples`, `clean`, `install`
- Static library (`libdrainprof.a`) and shared library (`libdrainprof.so`)
- Debug builds with `-O0 -g` for tests
- Optimized builds with `-O2` for library and benchmarks
- Full compiler warning enforcement: `-Wall -Wextra -Wpedantic`
- Zero warnings on clean build

**Examples:**
- `examples/basic.c` - Minimal usage demonstration
- Simulates 10 granules with mixed drainable/pinned scenarios
- Shows DSR interpretation and warning thresholds

**Benchmarks:**
- `bench/bench_hot_path.c` - Measures allocation/deallocation latency
- 10M iterations per operation
- Reports ns/op and operations/second
- Validates correctness after benchmarking

**Documentation:**
- Comprehensive README.md with:
  - Drainability concept explanation
  - Quick start guide
  - Complete API reference
  - Performance benchmarks
  - Integration examples (slab, arena, epoch allocators)
  - Result interpretation guidelines
- Architecture document: `drainability_profiler_architecture.md`
- This CHANGELOG.md

**Configuration Options:**
- Mode: `DRAINPROF_PRODUCTION` (M1) or `DRAINPROF_DIAGNOSTIC` (future)
- Storage: `DRAINPROF_SLOT_ARRAY` (M1) or `DRAINPROF_HASH_MAP` (future)
- Slot array capacity: configurable via `drainprof_config`
- Verbose logging: optional stderr output for pinned closes

### Performance Benchmarks

Measured on Apple Silicon (M1/M2 class):

```
Operation          Latency      Throughput
─────────────────────────────────────────────
alloc_register     1.97 ns      508 M/s
alloc_deregister   1.77 ns      565 M/s
```

**Target:** < 10ns per operation - **PASS: Exceeded by 5x**

### Validation

Theorem 3 from the drainability paper states:

**R(t) ≥ p·m(t)** where:
- R(t) = retained (pinned) granules
- m(t) = total closed granules
- p = violation probability

Test results validate **DSR = 1.0 - p** exactly:

```
p=0.0: DSR=1.000 (expected 1.000, error 0.000)
p=0.1: DSR=0.900 (expected 0.900, error 0.000)
p=0.5: DSR=0.500 (expected 0.500, error 0.000)
p=1.0: DSR=0.000 (expected 0.000, error 0.000)
```

### Technical Details

**Slot Array Design:**
- Pre-allocated array of `drainprof_granule_slot` structures
- Each slot contains:
  - `atomic_uint32_t live_count` - Live allocations in granule
  - `atomic_uint64_t granule_id` - Granule occupying this slot
  - `atomic_bool occupied` - Slot occupation flag
- CAS loop on `occupied` flag prevents collisions
- Requirement: `capacity >= max_simultaneously_open_granules`

**Atomic Operations:**
- All counter operations use `atomic_fetch_add()` for increment
- All counter operations use `atomic_fetch_sub()` for decrement
- Slot operations use `atomic_compare_exchange_strong()` for occupation
- Snapshot uses `atomic_load()` with `memory_order_relaxed`
- No locks, no mutexes, no condition variables

**Memory Safety:**
- All pointers validated before dereference
- Slot array bounds checked via modulo arithmetic
- No dynamic allocation in hot path
- Clean destruction with proper free() ordering

### Known Limitations (M1 Scope)

- Slot array only: Dense granule IDs required (0 to capacity-1)
- Production mode only: No per-allocation tracking yet
- No hash map storage: Sparse IDs not supported
- Location tracking inactive: `__FILE__`/`__LINE__` captured but unused
- Single-threaded tests: Concurrent access not stress-tested

### Future Work

**M2: Diagnostic Mode**
- Per-allocation tracking with hash table
- Allocation site capture (file, line, timestamp)
- Detailed leak reports with allocation traces
- ~50ns overhead (vs. <2ns production mode)

**M3: Rust Bindings**
- Safe wrapper around C API
- Idiomatic Rust types and error handling
- Zero-cost abstractions
- Cargo integration

**M4: Prometheus Export**
- Metrics exporter for Prometheus monitoring
- Real-time DSR tracking in production
- Grafana dashboard templates
- Alert rules for DSR thresholds

**M5: Blog Post and Announcement**
- Technical blog post explaining drainability
- Usage examples and case studies
- Public release announcement

## [Unreleased]

### Planned
- M2: Diagnostic mode implementation
- M3: Rust bindings
- M4: Prometheus export
- M5: Blog post and announcement

---

## Version History

- **0.1.0** (2026-02-16) - M1: Initial release with production mode and slot array
