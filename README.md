# libdrainprof

[![Blackwell Systems™](https://raw.githubusercontent.com/blackwell-systems/blackwell-docs-theme/main/badge-trademark.svg)](https://github.com/blackwell-systems)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.18653776.svg)](https://doi.org/10.5281/zenodo.18653776)

**Part of the [Drainability Project](https://github.com/blackwell-systems/drainability)** – Theory, measurement tools, and validation for structural memory leaks

A lightweight C library for detecting structural memory leaks in coarse-grained allocators by measuring drainability at runtime. Helps you answer: **"Why does my service leak memory when Valgrind says it doesn't?"**

## Try It in 60 Seconds

```bash
git clone https://github.com/blackwell-systems/drainability-profiler
cd drainability-profiler

# Quick test (works everywhere)
make all && make test

# Show what Valgrind misses (Docker - Linux container)
docker build -f Dockerfile.demo -t drainprof-demo .
docker run --rm drainprof-demo ./compare.sh
```

**What you'll see:**
```
=========================================
  VALGRIND SAYS:
=========================================
==1== HEAP SUMMARY:
==1==     in use at exit: 0 bytes in 0 blocks
==1==   total heap usage: 167 allocs, 167 frees, 166,480 bytes allocated
==1==
==1== All heap blocks were freed -- no leaks are possible

=========================================
  LIBDRAINPROF SAYS:
=========================================
Requests:         2850
Sessions created: 285
Sessions freed:   285  ← All objects freed (Valgrind confirms)
Active:           0

Epochs closed:    20
Drainable:        1 (5.0%)   ← Only 1 epoch can reclaim memory
Pinned:           19          ← 19 epochs blocked by session lifetimes

Conclusion: Structural leak detected!
  - DSR = 5.0% (many epochs pinned by sessions)
  - All objects freed (Valgrind would report zero leaks) ✓
  - But epochs can't be reclaimed until sessions timeout

=========================================
  VERDICT
=========================================
✓ Valgrind:     0 bytes leaked
✗ libdrainprof: DSR=5%, structural leak detected
```

Same binary. All objects freed. Valgrind clean. But 95% of epochs are pinned and non-drainable. **This is what traditional leak detectors miss.**

**Full terminal outputs:** See complete 30-second runs with DSR progression in [`examples/temporal-slab/`](examples/temporal-slab/):
- `output-broken.txt` / `output-broken-valgrind.txt` - Watch DSR drop from 100% → 5%
- `output-fixed.txt` / `output-fixed-valgrind.txt` - Watch DSR stay at 90%

## Why This Matters

Many allocators use coarse-grained reclamation (slabs, arenas, epochs) that can only return memory when completely empty. A single long-lived allocation pins the entire granule, even if 99% of objects are freed. Valgrind reports "no leaks" because objects are eventually freed, but the allocator can't reclaim memory until lifetimes align. In production, this manifests as RSS growth that takes days to appear and is invisible to traditional tools. libdrainprof detects it in CI with <2ns overhead.

## Quick Integration

```c
#include <drainprof.h>

drainprof *prof = drainprof_create();

// Instrument your allocator
drainprof_granule_open(prof, granule_id);
drainprof_alloc_register(prof, granule_id, alloc_id, size);    // < 2ns
drainprof_alloc_deregister(prof, granule_id, alloc_id);        // < 2ns
int drainable = drainprof_granule_close(prof, granule_id);

// Read metrics
drainprof_snapshot_t snap;
drainprof_snapshot(prof, &snap);
printf("DSR: %.1f%%\n", snap.dsr * 100.0);  // 0-100%
```

See [docs/API.md](docs/API.md) for full API reference and [docs/INTEGRATION.md](docs/INTEGRATION.md) for integration patterns.

## What is Drainability?

**Drainability** measures whether allocator granules (slabs/arenas/epochs) can reclaim memory at their natural boundaries. A structural leak occurs when all individual objects are freed (Valgrind clean), but granules remain pinned by lifetime mismatches. **DSR = drainable_closes / total_closes** quantifies this: 100% means perfect, <50% indicates severe structural leaks.

Full explanation: [Research paper](https://doi.org/10.5281/zenodo.18653776)

## Features

- **Lightweight:** <2ns per allocation (production mode), ~25ns (diagnostic mode)
- **Thread-safe:** Lock-free atomic operations
- **Two modes:** Production (always-on metrics) and Diagnostic (per-allocation tracking with source locations)
- **Real integration:** Works with [temporal-slab allocator](https://github.com/blackwell-systems/temporal-slab), validated in CI

## Real-World Validation

**Redis 7.2 + jemalloc instrumentation:** Demonstrated structural fragmentation on production workload. After populating 100K keys and deleting 50% (freeing 195K objects), **0% of slabs became reclaimable** — all 256 slabs remained pinned by scattered surviving allocations. See [`examples/redis/`](examples/redis/) for patches and reproduction steps, or clone the [instrumented fork](https://github.com/blackwell-systems/redis-drainprof).

## Installation

```bash
make              # Build libdrainprof.a
make test         # Run test suite (15 tests pass)
sudo make install # Install to /usr/local
```

Requires: C11 compiler, POSIX threads, Linux (RSS tracking) or macOS (development/testing)

---

## Documentation

- [API Reference](docs/API.md) - Complete function documentation
- [Integration Guide](docs/INTEGRATION.md) - Patterns for slab/arena/epoch allocators
- [Examples](examples/) - Working code for basic, diagnostic, and temporal-slab integration
- [Research Paper](https://doi.org/10.5281/zenodo.18653776) - Drainability theory and theorem proofs

## Performance

**Production mode (Apple Silicon):**
- `alloc_register`: 1.97ns (508M ops/sec)
- `alloc_deregister`: 1.77ns (565M ops/sec)

**Diagnostic mode (Apple Silicon):**
- `alloc_register_located`: 24.68ns (40.5M ops/sec)
- `alloc_deregister`: 20.50ns (48.8M ops/sec)

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for benchmark details.

## Citation

```bibtex
@techreport{blackwell2026drainability,
  title   = {Drainability: When Coarse-Grained Memory Reclamation
             Produces Bounded Retention},
  author  = {Blackwell, Dayna},
  year    = {2026},
  doi     = {10.5281/zenodo.18653776},
  license = {CC-BY-4.0}
}
```

## License

MIT License - See LICENSE file for details

---

**Status:** M2 Complete | **Build:** Passing | **Tests:** 15/15 | **Performance:** 1.97ns/op
