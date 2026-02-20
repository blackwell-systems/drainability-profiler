# Redis + libdrainprof Integration

Demonstrates drainability profiling on Redis to measure structural memory fragmentation.

## Overview

This integration instruments Redis 7.2 with libdrainprof to track allocation drainability in real-time. It measures the **Drainability Satisfaction Rate (DSR)** — the percentage of memory slabs that are fully empty and can be reclaimed after objects are freed.

## What It Measures

Structural memory fragmentation occurs when freed objects remain scattered across slabs, preventing the allocator from reclaiming memory even though the application has freed it. This instrumentation tracks:

- **DSR (mem_drainability_ratio)**: Percentage of slabs that are empty and reclaimable
- **Total allocations vs deallocations**: Validates symmetric accounting
- **Slab occupancy**: Live objects per slab, identifying fragmentation patterns

## Quick Start

```bash
# Clone Redis
git clone https://github.com/redis/redis.git redis-drainprof
cd redis-drainprof
git checkout 7.2

# Apply instrumentation patch
git am < /path/to/drainability-profiler/examples/redis/patches/0001-*.patch

# Build with drainprof (requires libdrainprof in ../drainability-profiler)
./build_with_drainprof.sh

# Run Redis
./src/redis-server --port 6380 --enable-debug-command yes
```

## Testing Fragmentation

```bash
# Populate 100K keys
redis-cli -p 6380 DEBUG POPULATE 100000 key 1000

# Check baseline DSR
redis-cli -p 6380 INFO MEMORY | grep mem_drainability_ratio

# Delete 50% (odd keys) to create fragmentation
for i in $(seq 1 2 100000); do echo "DEL key:$i"; done | redis-cli -p 6380 --pipe

# Check DSR after fragmentation
redis-cli -p 6380 INFO MEMORY | grep mem_drainability_ratio
```

**Expected Result:** DSR remains 0% after deleting 50% of keys, demonstrating that the remaining keys are scattered across all slabs, preventing any from becoming reclaimable.

## Metrics Exposed

All metrics available via `INFO MEMORY`:

```
mem_drainability_ratio:0.0222              # DSR: 2.22% of slabs are drainable
mem_drainprof_total_extents:45             # Total slabs tracked
mem_drainprof_drainable_extents:1          # Slabs with 0 live objects
mem_drainprof_pinned_extents:44            # Slabs with >0 live objects
mem_drainprof_total_allocs:74572           # Total allocations registered
mem_drainprof_total_deallocs:60668         # Total deallocations registered
mem_drainprof_malloc_fastpath_calls:74567  # Allocations via malloc fastpath
mem_drainprof_free_fastpath_calls:60666    # Deallocations via free fastpath
```

## Reference Implementation

Working fork with instrumentation: https://github.com/blackwell-systems/redis-drainprof

## How It Works

The instrumentation uses symmetric fastpath hooks:

1. **Allocation tracking**: `imalloc_fastpath()` registers each allocation via `drainprof_alloc_register()`
2. **Deallocation tracking**: `free_fastpath()` deregisters via `drainprof_alloc_deregister()`
3. **Lazy slab registration**: Slabs are registered on first allocation via `drainprof_granule_open()`
4. **Sweep-based occupancy**: Drainprof periodically surveys slab occupancy to compute DSR

This approach provides complete coverage of Redis's allocation/deallocation paths while maintaining symmetric accounting (allocations ≈ deallocations).

## Validation Results

Testing with 100K keys (1KB values each):

- **Baseline (100K keys)**: 256 slabs, 0% DSR, ~403K live objects
- **After 50% deletion**: 256 slabs, **still 0% DSR**, ~208K live objects
- **Freed**: ~195K objects (48% reduction in live data)
- **Reclaimed**: 0 slabs (0% improvement in drainability)

This demonstrates genuine structural fragmentation: the remaining keys are scattered uniformly across all slabs, preventing any from becoming fully empty and reclaimable.

## Build Requirements

- libdrainprof installed at `../drainability-profiler`
- jemalloc 5.3.0 (included in Redis deps)
- ENABLE_DRAINPROF flag passed to jemalloc configure

The `build_with_drainprof.sh` script handles all build dependencies.
