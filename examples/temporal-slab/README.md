# Temporal-Slab Integration Example

This directory contains integration validation tests demonstrating how libdrainprof integrates with the [temporal-slab allocator](https://github.com/blackwell-systems/temporal-slab), a production-quality epoch-based memory allocator.

## What is Temporal-Slab?

Temporal-slab is an epoch-based allocator that groups allocations by time. Each "epoch" is a temporal granule that can be closed and reclaimed when no longer needed. This makes it an ideal use case for drainability profiling - we want to know if epochs can actually be reclaimed or if long-lived allocations pin them indefinitely.

## Integration Architecture

The integration adds four instrumentation points to temporal-slab using conditional compilation:

```c
#ifdef ENABLE_DRAINPROF
#include <drainprof.h>
extern drainprof* g_profiler;
#endif

// In epoch_advance() - opening a new epoch
#ifdef ENABLE_DRAINPROF
if (g_profiler) {
    drainprof_granule_open(g_profiler, new_epoch_id);
}
#endif

// In alloc_obj_epoch() - registering an allocation
#ifdef ENABLE_DRAINPROF
if (g_profiler) {
    DRAINPROF_ALLOC_REGISTER(g_profiler, epoch_id, (uintptr_t)ptr, size);
}
#endif

// In free_obj() - deregistering an allocation
#ifdef ENABLE_DRAINPROF
if (g_profiler) {
    drainprof_alloc_deregister(g_profiler, epoch_id, (uintptr_t)ptr);
}
#endif

// In epoch_close() - closing an epoch
#ifdef ENABLE_DRAINPROF
if (g_profiler) {
    drainprof_granule_close(g_profiler, epoch_id);
}
#endif
```

**Key Design Principles:**

1. **Zero overhead when disabled:** All profiling code is compiled out via `#ifdef` guards
2. **Minimal API surface:** Only 4 calls across the entire allocator lifecycle
3. **Lock-free hot path:** `alloc_register` and `alloc_deregister` are < 2ns
4. **Optional integration:** Allocator builds and runs without profiler dependency

## Validation Tests

### Phase 1.2: P-Sweep Validation (`psweep_validation.c`)

**Purpose:** Validates Theorem 3 from the drainability paper: **DSR = 1.0 - p**

This test runs workloads with controlled violation probabilities and verifies that the measured DSR matches the theoretical prediction:

```bash
# In CI (Ubuntu):
make ENABLE_DRAINPROF=1 DRAINPROF_PATH=../../drainability-profiler
./psweep_validation
```

**Test Matrix:**

| Violation Rate (p) | Expected DSR | Observed DSR | Status |
|--------------------|--------------|--------------|--------|
| 0.00               | 1.000        | 1.000        | ✓ PASS |
| 0.01               | 0.990        | 1.000        | ✓ PASS |
| 0.05               | 0.950        | 0.980        | ✓ PASS |
| 0.10               | 0.900        | 0.950        | ✓ PASS |
| 0.25               | 0.750        | 0.770        | ✓ PASS |
| 0.50               | 0.500        | 0.540        | ✓ PASS |
| 1.00               | 0.000        | 0.000        | ✓ PASS |

**Key Insight:** With 1 allocation per epoch, leaking an allocation with probability p means the epoch cannot be drained. DSR = fraction of drainable epochs = 1.0 - p.

### Phase 1.3: Diagnostic Mode Validation (`diagnostic_validation.c`)

**Purpose:** Validates that diagnostic mode correctly identifies allocation sites causing structural leaks

This test runs a p=0.25 workload and verifies that the diagnostic summary:
- Tracks allocation sites (file:line)
- Reports pinning counts matching the expected violation rate
- Provides actionable information for leak investigation

```bash
# In CI (Ubuntu):
make ENABLE_DRAINPROF=1 DRAINPROF_PATH=../../drainability-profiler
./diagnostic_validation
```

**Sample Output:**

```
=== Diagnostic Summary ===
Allocation sites tracked: 1
✓ PASS: Allocation sites tracked

Allocation sites:
  Site 0:
    Location: slab_lib.c:1829
    Total allocs: 23
    Total bytes: 2944
    Pinning count: 23
    Expected violations: 25 (observed 23, error 2)
    ✓ PASS: Pinning count matches expected violations
    ✓ All tracked allocations from this site caused pinning

=== Summary ===
VALIDATION SUCCESS: Diagnostic mode correctly identifies allocation sites
```

**Key Insight:** The diagnostic mode pinpoints the exact source location (`slab_lib.c:1829`) where allocations are pinning epochs. In a real scenario, developers would:
1. Run workload in production mode, observe low DSR
2. Re-run with diagnostic mode enabled to identify problematic allocation sites
3. Refactor code to prevent long-lived allocations from pinning epochs

## Running Tests Locally

**Note:** Temporal-slab uses `pthread_mutex_timedlock` which is Linux-only. Tests run automatically in GitHub Actions CI on Ubuntu.

To run locally on Linux:

```bash
# Build libdrainprof
cd drainability-profiler
make clean && make

# Build temporal-slab with profiler enabled
cd ../temporal-slab/src
make clean
make ENABLE_DRAINPROF=1 DRAINPROF_PATH=../../drainability-profiler \
     slab_lib.o epoch_domain.o slab_stats.o

# Build and run p-sweep validation
cd ../../drainability-profiler/examples/temporal-slab
gcc -O2 -std=c11 -pthread -Wall -Wextra -pedantic \
    -I../../include -I../../../temporal-slab/include \
    -DENABLE_RSS_RECLAMATION=1 -DENABLE_DRAINPROF \
    psweep_validation.c \
    ../../../temporal-slab/src/slab_lib.o \
    ../../../temporal-slab/src/epoch_domain.o \
    ../../../temporal-slab/src/slab_stats.o \
    ../../libdrainprof.a \
    -o psweep_validation
./psweep_validation

# Build and run diagnostic validation
gcc -O2 -std=c11 -pthread -Wall -Wextra -pedantic \
    -I../../include -I../../../temporal-slab/include \
    -DENABLE_RSS_RECLAMATION=1 -DENABLE_DRAINPROF \
    diagnostic_validation.c \
    ../../../temporal-slab/src/slab_lib.o \
    ../../../temporal-slab/src/epoch_domain.o \
    ../../../temporal-slab/src/slab_stats.o \
    ../../libdrainprof.a \
    -o diagnostic_validation
./diagnostic_validation
```

## Continuous Integration

Both tests run automatically on every push via GitHub Actions (`.github/workflows/tslab-integration.yml`):

```yaml
- name: Build temporal-slab with profiler enabled
  run: |
    cd temporal-slab/src
    make clean
    make ENABLE_DRAINPROF=1 DRAINPROF_PATH=../../drainability-profiler \
         slab_lib.o epoch_domain.o slab_stats.o
```

See: [GitHub Actions workflow](../../.github/workflows/tslab-integration.yml)

## Integrating Your Own Epoch-Based Allocator

The temporal-slab integration demonstrates the general pattern for epoch-based allocators:

1. **Add conditional compilation guards** for profiler code
2. **Create global profiler instance** (or pass as context)
3. **Instrument four lifecycle events:**
   - Epoch/granule open
   - Allocation within granule
   - Deallocation within granule
   - Epoch/granule close
4. **Use macros for diagnostic mode** (`DRAINPROF_ALLOC_REGISTER`) to capture source locations
5. **Validate integration** with p-sweep and diagnostic tests

### Example: Generic Epoch Allocator

```c
#ifdef ENABLE_DRAINPROF
#include <drainprof.h>
extern drainprof* g_profiler;
#endif

typedef struct {
    uint64_t epoch_id;
    // ... your allocator fields
} epoch_t;

void epoch_advance(allocator_t* alloc) {
    epoch_t* old_epoch = alloc->current_epoch;
    epoch_t* new_epoch = create_new_epoch();

    alloc->current_epoch = new_epoch;

#ifdef ENABLE_DRAINPROF
    if (g_profiler) {
        drainprof_granule_close(g_profiler, old_epoch->epoch_id);
        drainprof_granule_open(g_profiler, new_epoch->epoch_id);
    }
#endif
}

void* epoch_alloc(epoch_t* epoch, size_t size) {
    void* ptr = internal_alloc(epoch, size);

#ifdef ENABLE_DRAINPROF
    if (g_profiler) {
        DRAINPROF_ALLOC_REGISTER(g_profiler, epoch->epoch_id,
                                 (uintptr_t)ptr, size);
    }
#endif

    return ptr;
}

void epoch_free(epoch_t* epoch, void* ptr) {
#ifdef ENABLE_DRAINPROF
    if (g_profiler) {
        drainprof_alloc_deregister(g_profiler, epoch->epoch_id,
                                    (uintptr_t)ptr);
    }
#endif

    internal_free(epoch, ptr);
}
```

### Validation Template

Create your own validation tests modeled after `psweep_validation.c` and `diagnostic_validation.c`:

1. **P-sweep test:** Run controlled workloads with known violation rates, verify DSR matches expectations
2. **Diagnostic test:** Run workload with violations, verify diagnostic mode identifies correct allocation sites

## Performance Impact

**Production Mode:**
- **Overhead:** < 2ns per allocation/deallocation
- **Memory:** ~32 bytes per open granule (slot array entry)
- **Suitable for:** Always-on production monitoring

**Diagnostic Mode:**
- **Overhead:** ~25ns per allocation/deallocation
- **Memory:** Proportional to number of pinning events
- **Suitable for:** Time-bounded investigation when DSR is low

**When disabled:** Zero overhead - all profiler code is compiled out.

## References

- **Temporal-Slab Repository:** https://github.com/blackwell-systems/temporal-slab
- **Drainability Paper:** [doi.org/10.5281/zenodo.18653776](https://doi.org/10.5281/zenodo.18653776)
- **GitHub Actions CI:** [Integration Test Workflow](../../.github/workflows/tslab-integration.yml)
- **Main README:** [../../README.md](../../README.md)

## Questions?

See the main [libdrainprof README](../../README.md) for API documentation and usage examples.

For temporal-slab specific questions, see the [temporal-slab repository](https://github.com/blackwell-systems/temporal-slab).
