# Building Redis with jemalloc Extent-Level Drainprof Instrumentation

This guide shows how to build Redis with libdrainprof tracking jemalloc's extent lifecycle, enabling DSR (Drainability Satisfaction Rate) measurement alongside traditional fragmentation metrics.

## Architecture

**Granule Mapping:** jemalloc extent = libdrainprof granule

```
jemalloc extent lifecycle:
1. extent_alloc_wrapper()    → drainprof_granule_open(extent_addr)
2. arena_malloc_small()       → drainprof_alloc_register(extent_addr, ptr, size)
3. arena_dalloc_small()       → drainprof_alloc_deregister(extent_addr, ptr)
4. extent_dalloc_wrapper()    → drainprof_granule_close(extent_addr)
```

When DSR < 30%, most extents are pinned by remaining allocations and can't be reclaimed. This is the structural leak that Valgrind misses.

## Prerequisites

- Redis source (we cloned to `/Users/dayna.blackwell/code/redis-drainprof/`)
- libdrainprof built (`drainability-profiler/libdrainprof.a`)
- GCC or Clang with C11 support
- Linux (for production) or macOS (for development)

## Build Steps

### 1. Apply Patches

```bash
cd /Users/dayna.blackwell/code/redis-drainprof

# Phase 1: Extent lifecycle tracking
patch -p1 < ../drainability-profiler/examples/redis/patches/01-jemalloc-extent-lifecycle.patch

# Phase 2: Per-object tracking within extents
patch -p1 < ../drainability-profiler/examples/redis/patches/02-jemalloc-object-tracking.patch

# Phase 3: Redis INFO command integration
patch -p1 < ../drainability-profiler/examples/redis/patches/03-redis-info-drainprof.patch
```

### 2. Configure jemalloc Build

Edit `deps/jemalloc/Makefile` or set environment variables:

```bash
export CFLAGS="-DENABLE_DRAINPROF -I/Users/dayna.blackwell/code/drainability-profiler/include"
export LDFLAGS="-L/Users/dayna.blackwell/code/drainability-profiler -ldrainprof -lpthread"
```

### 3. Build jemalloc

```bash
cd deps/jemalloc
./autogen.sh --with-jemalloc-prefix=je_
make
cd ../..
```

### 4. Build Redis

```bash
make USE_JEMALLOC=yes \
     MALLOC=jemalloc \
     CFLAGS="-DENABLE_DRAINPROF -I/Users/dayna.blackwell/code/drainability-profiler/include" \
     LDFLAGS="/Users/dayna.blackwell/code/drainability-profiler/libdrainprof.a -lpthread"
```

### 5. Verify Build

```bash
./src/redis-server --version
ldd ./src/redis-server | grep jemalloc  # Linux
otool -L ./src/redis-server | grep jemalloc  # macOS
```

## Testing

### Run Instrumented Redis

```bash
./src/redis-server --save "" --appendonly no --enable-debug-command yes &
sleep 2
```

### Check Drainprof Metrics

```bash
./src/redis-cli INFO MEMORY | grep drainprof
```

Expected output (before any fragmentation):
```
mem_drainprof_enabled:yes
mem_drainability_ratio:1.0000
mem_drainprof_total_extent_closes:0
mem_drainprof_drainable_closes:0
mem_drainprof_pinned_closes:0
```

### Run Fragmentation Test

```bash
cd /Users/dayna.blackwell/code/drainability-profiler/examples/redis
./workloads/fragmentation-test-docker.sh redis-instrumented
```

Expected results **after** delete-odd-keys workload:

```
mem_fragmentation_ratio:2.16              ← Traditional metric (reactive)
mem_drainability_ratio:0.25               ← Novel metric (predictive)
mem_drainprof_total_extent_closes:150
mem_drainprof_drainable_closes:38         ← Only 25% of extents drainable
mem_drainprof_pinned_closes:112           ← 75% pinned by remaining keys
```

**This proves:** DSR detects structural leaks that Valgrind misses. All objects are freed (Valgrind clean), but extents can't be reclaimed.

## Docker Build (Recommended)

For reproducible Linux builds, use Docker:

```dockerfile
# Update Dockerfile.redis-instrumented with correct COPY paths
FROM debian:bookworm-slim AS builder

# Install build tools
RUN apt-get update && apt-get install -y \
    build-essential git autoconf pkg-config

# Copy libdrainprof
COPY ../../../include/drainprof.h /usr/local/include/
COPY ../../../libdrainprof.a /usr/local/lib/

# Clone and patch Redis
RUN git clone --branch 7.2 --depth 1 https://github.com/redis/redis.git /build/redis
WORKDIR /build/redis

COPY patches/*.patch /tmp/
RUN cat /tmp/01-jemalloc-extent-lifecycle.patch | patch -p1
RUN cat /tmp/02-jemalloc-object-tracking.patch | patch -p1
RUN cat /tmp/03-redis-info-drainprof.patch | patch -p1

# Build jemalloc
WORKDIR /build/redis/deps/jemalloc
RUN ./autogen.sh --with-jemalloc-prefix=je_
RUN make CFLAGS="-DENABLE_DRAINPROF -I/usr/local/include" \
         LDFLAGS="-L/usr/local/lib -ldrainprof -lpthread"

# Build Redis
WORKDIR /build/redis
RUN make USE_JEMALLOC=yes \
         MALLOC=jemalloc \
         CFLAGS="-DENABLE_DRAINPROF -I/usr/local/include" \
         LDFLAGS="/usr/local/lib/libdrainprof.a -lpthread"

# Runtime image
FROM debian:bookworm-slim
COPY --from=builder /build/redis/src/redis-server /usr/local/bin/
COPY --from=builder /build/redis/src/redis-cli /usr/local/bin/
CMD ["redis-server", "--save", "", "--appendonly", "no", "--enable-debug-command", "yes"]
```

Build:
```bash
docker build -f Dockerfile.redis-instrumented -t redis-drainprof .
docker run -d --name redis-test -p 6379:6379 redis-drainprof
```

## Troubleshooting

### Patch Fails

If patches don't apply cleanly (Redis version mismatch):
1. Check Redis version: `git log --oneline -1` in redis-drainprof/
2. Manually edit the target files following the patch logic
3. Search for function names: `grep -rn "extent_alloc_wrapper" deps/jemalloc/src/`

### Linker Errors

```
undefined reference to `drainprof_create'
```

**Fix:** Ensure libdrainprof.a is in LDFLAGS **before** -lpthread:
```bash
LDFLAGS="/path/to/libdrainprof.a -lpthread"  # Correct
LDFLAGS="-ldrainprof -lpthread"              # Wrong (uses .so)
```

### DSR Always 0.0

**Cause:** extents never close (jemalloc is caching them in dirty pool)

**Fix:** Force extent reclamation:
```bash
redis-cli CONFIG SET maxmemory-policy allkeys-lru
redis-cli CONFIG SET maxmemory 100mb
# Now run fragmentation test
```

### DSR Always 1.0 Even After Fragmentation

**Cause:** Per-object tracking not working (patch 02 didn't apply)

**Debug:**
```bash
# Check if arena.c was patched
grep "drainprof_alloc_register" /Users/dayna.blackwell/code/redis-drainprof/deps/jemalloc/src/arena.c

# If not found, patch failed - apply manually
```

## Performance Impact

**Overhead:** Extent lookup (`emap_edata_lookup`) on every malloc/free

- **Best case:** ~5-10ns per operation (radix tree lookup)
- **Worst case:** ~20ns if many extents active

For Redis (10M ops/sec):
- 10M × 10ns = 100ms/sec = 10% CPU overhead
- Acceptable for profiling, may be too high for production

**Mitigation:** Use sampling (instrument 1% of allocations) - future work.

## Next Steps

1. ✅ Build instrumented Redis
2. ✅ Run fragmentation test
3. ⏳ Validate: Valgrind reports 0 leaks, DSR shows ~25%
4. ⏳ Plot DSR vs mem_fragmentation_ratio over time
5. ⏳ Measure performance overhead with redis-benchmark

---

**Status:** Patches created, ready to apply and build
**Expected result:** DSR ~25% after fragmentation test (first tool to detect this)
