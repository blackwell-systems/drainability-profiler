# libdrainprof API Reference

Complete API documentation for libdrainprof. For quick integration, see the main [README](../README.md).

## Initialization

### `drainprof_create()`
```c
drainprof *drainprof_create(void);
```
Creates a profiler with default configuration (production mode, slot array, 1024 capacity).

**Returns:** Profiler instance, or NULL on failure

### `drainprof_create_with_config()`
```c
drainprof *drainprof_create_with_config(const drainprof_config *config);
```
Creates a profiler with custom configuration.

**Parameters:**
- `config`: Configuration structure (see Configuration section)

**Returns:** Profiler instance, or NULL on failure

### `drainprof_destroy()`
```c
void drainprof_destroy(drainprof *prof);
```
Destroys profiler and frees all resources.

**Parameters:**
- `prof`: Profiler instance to destroy

## Granule Lifecycle

### `drainprof_granule_open()`
```c
int drainprof_granule_open(drainprof *prof, drainprof_granule_id id);
```
Opens a granule for tracking. Call this when creating a new slab/arena/epoch.

**Parameters:**
- `prof`: Profiler instance
- `id`: Granule identifier (must be < slot_capacity for slot array mode)

**Returns:**
- `0` on success
- `-1` on collision (slot array full or granule already open)

### `drainprof_granule_close()`
```c
int drainprof_granule_close(drainprof *prof, drainprof_granule_id id);
```
Closes a granule and determines if it's drainable.

**IMPORTANT:** Ensure no concurrent `alloc_register` or `alloc_deregister` calls for this `granule_id` are in flight. All allocation activity must complete before closing.

**Parameters:**
- `prof`: Profiler instance
- `id`: Granule identifier

**Returns:**
- `1` if drainable (no live allocations remaining)
- `0` if pinned (live allocations present)

## Allocation Tracking

### `drainprof_alloc_register()`
```c
int drainprof_alloc_register(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size
);
```
Registers an allocation in a granule. **Hot path:** <2ns overhead.

**Parameters:**
- `prof`: Profiler instance
- `granule_id`: Granule containing this allocation
- `alloc_id`: Unique identifier for this allocation (typically pointer address)
- `size`: Allocation size in bytes

**Returns:** `0` on success, `-1` on failure

### `drainprof_alloc_deregister()`
```c
int drainprof_alloc_deregister(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id
);
```
Deregisters an allocation (object freed). **Hot path:** <2ns overhead.

**Parameters:**
- `prof`: Profiler instance
- `granule_id`: Granule containing this allocation
- `alloc_id`: Allocation identifier (same as passed to register)

**Returns:** `0` on success, `-1` on failure

### `DRAINPROF_ALLOC_REGISTER()` (Macro)
```c
#define DRAINPROF_ALLOC_REGISTER(prof, gid, aid, sz)
```
Macro version that captures `__FILE__` and `__LINE__` for diagnostic mode. Expands to regular `alloc_register` in production mode.

**Usage:**
```c
DRAINPROF_ALLOC_REGISTER(prof, granule_id, alloc_id, size);
```

## Metrics

### `drainprof_snapshot()`
```c
void drainprof_snapshot(const drainprof *prof, drainprof_snapshot_t *out);
```
Takes a consistent snapshot of all metrics. Lock-free, safe to call anytime.

**Parameters:**
- `prof`: Profiler instance
- `out`: Output buffer for snapshot

**Snapshot Structure:**
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

**Key Metric:**
- `dsr`: **Drainability Satisfaction Rate** = drainable_closes / total_closes
  - 1.0 (100%): Perfect drainability
  - 0.5 (50%): Half of granules pinned
  - 0.0 (0%): All granules pinned

## Configuration

### `drainprof_config`
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
```

### `drainprof_config_default()`
```c
drainprof_config drainprof_config_default(void);
```
Returns default configuration:
- Mode: `DRAINPROF_PRODUCTION`
- Storage: `DRAINPROF_SLOT_ARRAY`
- Slot capacity: 1024
- Log interval: 0 (disabled)
- Callback: NULL
- Max buffered reports: 1000
- Verbose: false

## Diagnostic Mode

When production metrics show low DSR, enable diagnostic mode to identify which allocations are pinning granules.

### `drainprof_pin_callback`
```c
typedef void (*drainprof_pin_callback)(
    const drainprof_pinning_report *report,
    void *user_data
);
```
Callback invoked when a granule closes with live allocations (pinned).

**Parameters:**
- `report`: Detailed report about the pinned granule (must be freed with `drainprof_pinning_report_free`)
- `user_data`: User-provided context from `config.callback_user_data`

### `drainprof_drain_reports()`
```c
uint32_t drainprof_drain_reports(
    drainprof *prof,
    drainprof_pinning_report **reports,
    uint32_t max_reports
);
```
Retrieves buffered pinning reports (when `on_pinning` callback is NULL).

**Parameters:**
- `prof`: Profiler instance
- `reports`: Output array for report pointers (caller must free each report)
- `max_reports`: Maximum reports to retrieve

**Returns:** Number of reports actually retrieved

### `drainprof_pinning_report_free()`
```c
void drainprof_pinning_report_free(drainprof_pinning_report *report);
```
Frees a pinning report returned by callback or `drain_reports`.

### Pinning Report Structure

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

### Diagnostic Summary

Aggregate pinning reports by allocation site to identify problematic source locations:

```c
drainprof_diagnostic_summary *drainprof_diagnostic_summary_compute(drainprof *prof);
```
Computes summary from all buffered reports (diagnostic mode only). Aggregates by allocation site (file:line).

**Returns:** Summary structure (caller must free with `drainprof_diagnostic_summary_free`)

```c
void drainprof_diagnostic_summary_free(drainprof_diagnostic_summary *summary);
```
Frees a diagnostic summary.

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

**Example Usage:**
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

See `examples/diagnostic_summary.c` for complete working example.

## Thread Safety

All functions are thread-safe:
- `alloc_register` / `alloc_deregister`: Lock-free atomic operations
- `granule_open` / `granule_close`: Atomic state transitions
- `snapshot`: Lock-free snapshot with eventual consistency

**Concurrency constraint:** `granule_close` requires quiescence - no concurrent allocation activity for that granule.

## Performance Characteristics

### Production Mode
- **`alloc_register`**: <2ns (lock-free atomic increment)
- **`alloc_deregister`**: <2ns (lock-free atomic decrement)
- **`granule_close`**: ~50ns (atomic read + compare)
- **`snapshot`**: ~100ns (read 8 atomic counters)

### Diagnostic Mode
- **`alloc_register_located`**: ~25ns (mutex + metadata capture)
- **`alloc_deregister`**: ~20ns (atomic decrement, metadata already stored)
- **`granule_close`**: ~5-10µs (mutex + report generation)

## Examples

See `examples/` directory:
- `basic.c`: Production mode with DSR tracking
- `diagnostic.c`: Diagnostic mode with real-time callbacks
- `diagnostic_summary.c`: Buffered reports with site aggregation
- `temporal-slab/`: Real integration with epoch-based allocator
