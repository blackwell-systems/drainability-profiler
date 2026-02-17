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

## [0.2.0] - 2026-02-16

### Added - M2: Diagnostic Mode with Summary Formatter

**Diagnostic Mode Implementation:**
- Per-allocation tracking with source location capture (`__FILE__`, `__LINE__`)
- Pinning reports generated when granules close with live allocations
- Two diagnostic workflows: real-time callbacks and buffered reports
- Diagnostic slot array with per-granule allocation record tracking
- Dynamic allocation arrays (64 records pre-allocated, doubles when full)
- Optimized deregister with `last_freed_index` hint (O(1) average case)

**API Additions:**
- `drainprof_alloc_register_located()` - Register allocation with source location
- `drainprof_drain_reports()` - Retrieve buffered pinning reports
- `drainprof_pinning_report_free()` - Free a pinning report
- `drainprof_diagnostic_summary_compute()` - Aggregate reports by allocation site
- `drainprof_diagnostic_summary_free()` - Free diagnostic summary

**New Types:**
- `drainprof_alloc_site` - Source location (file, line, column)
- `drainprof_pinning_alloc` - Single allocation that pinned a granule
- `drainprof_pinning_report` - Complete report for a pinned granule
- `drainprof_pin_callback` - Callback function type for real-time notifications
- `drainprof_summary_site_entry` - Aggregated statistics per allocation site
- `drainprof_diagnostic_summary` - Summary of all pinning reports by site

**Configuration Options:**
- `config.mode = DRAINPROF_DIAGNOSTIC` - Enable diagnostic mode
- `config.on_pinning` - Optional callback for real-time pinning notifications
- `config.callback_user_data` - User data passed to callback
- `config.max_buffered_reports` - Maximum buffered reports (default: 256)
- Buffered mode when `config.on_pinning = NULL`

**Diagnostic Features:**
- Captures allocation timestamps using `clock_gettime(CLOCK_MONOTONIC)`
- Circular buffer for buffered reports (fixed-size, no unbounded growth)
- Summary formatter groups pinning allocations by source location
- Identifies most problematic allocation sites across multiple granules
- Per-site statistics: granules pinned, total allocations, total bytes

**Performance:**
- **Diagnostic mode overhead:** ~25ns per operation (10x slower than production)
- `alloc_register_located`: 24.68ns (within 50ns target)
- `alloc_deregister`: 20.50ns (within 50ns target)
- Acceptable overhead for root-cause investigation
- Mutex-protected per-granule state (not lock-free like production)

**Testing:**
- Added 8 diagnostic mode tests (total: 15 tests = 8 diagnostic + 7 production)
- New tests:
  - `diagnostic_create` - Basic diagnostic profiler creation
  - `per_alloc_tracking` - Per-allocation record tracking
  - `diagnostic_drainable` - Drainable granule in diagnostic mode
  - `counter_consistency` - Verify diagnostic and production modes produce identical DSR
  - `pinning_report` - Buffered report generation and validation
  - `callback_mode` - Real-time callback notifications
  - `psweep_diagnostic` - Theorem 3 validation with report validation
  - `diagnostic_summary` - Summary aggregation by allocation site
- All 15 tests pass with exact DSR validation

**Examples:**
- `examples/diagnostic.c` - Real-time callback mode demonstration
- `examples/diagnostic_summary.c` - Buffered reports with summary formatter
- Shows HTTP server scenario with session/request lifetime mismatches
- Demonstrates root-cause analysis workflow

**Benchmarks:**
- `bench/bench_diagnostic.c` - Diagnostic mode performance measurement
- 1M iterations (vs 10M for production, due to higher overhead)
- Validates correctness after benchmarking
- Confirms ~10x overhead vs production mode

**Documentation:**
- Updated README with diagnostic mode usage section
- Added diagnostic workflows (callback vs buffered)
- Added summary formatter API documentation
- Integration examples updated with diagnostic mode usage
- Performance section now includes diagnostic mode benchmarks

### Added - Concurrency Contract Documentation

**API Contract:**
- Documented precondition for `drainprof_granule_close()`:
  - No concurrent `alloc_register`/`alloc_deregister` calls for the same `granule_id`
  - All allocation activity must complete before closing granule
- Added "IMPORTANT PRECONDITION" block in API header documentation
- Explains why the precondition is necessary (prevents race condition)

**Implementation Comments:**
- Detailed race condition explanation in `src/slot_array.c`
- Shows how concurrent register could be lost without the precondition
- Explains why the store sequence (live_count → granule_id → occupied) is safe
- Added corresponding comment in `src/diagnostic.c` close path

**User Documentation:**
- New "Concurrency Contract" section in README before integration examples
- Describes the race condition that would occur without the contract
- Shows how epoch-based, arena, and slab allocators naturally satisfy it
- Inline comment added to basic usage example

**Rationale:**
- Contract aligns with existing allocator semantics
- Epoch allocators close old epochs after advancing
- Arena allocators close when last reference is dropped
- Slab allocators close when returning to pool
- Not a new constraint, but explicit documentation of correct usage

### Changed
- Updated test count in documentation from 7 to 15
- Enhanced API documentation with diagnostic mode details
- Improved README structure with diagnostic workflows section

### Performance Benchmarks

**Production Mode (Apple Silicon):**
```
Operation          Latency      Throughput
─────────────────────────────────────────────
alloc_register     1.97 ns      508 M/s
alloc_deregister   1.77 ns      565 M/s
```
**Target:** < 10ns - **PASS: Exceeded by 5x**

**Diagnostic Mode (Apple Silicon):**
```
Operation                  Latency      Throughput
─────────────────────────────────────────────────
alloc_register_located     24.68 ns     40.5 M/s
alloc_deregister           20.50 ns     48.8 M/s
```
**Target:** < 50ns - **PASS: Within budget**

**Overhead Analysis:**
- Diagnostic mode is ~10x slower than production mode
- Due to: per-allocation tracking, mutex operations, metadata storage
- Acceptable for diagnostic investigation (not production monitoring)

## [Unreleased]

### Planned
- M3: Rust bindings
- M4: Prometheus export

---

## Version History

- **0.2.0** (2026-02-16) - M2: Diagnostic mode with summary formatter and concurrency contract documentation
- **0.1.0** (2026-02-16) - M1: Initial release with production mode and slot array
