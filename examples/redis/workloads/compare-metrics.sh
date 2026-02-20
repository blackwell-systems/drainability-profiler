#!/bin/bash
# Compare Redis's mem_fragmentation_ratio with libdrainprof's DSR
#
# Run this after fragmentation-test.sh on instrumented Redis to see:
# - Traditional metric: mem_fragmentation_ratio (reactive)
# - New metric: DSR per size class (predictive)
#
# Hypothesis: DSR drops before fragmentation ratio spikes

set -euo pipefail

REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"

echo "========================================="
echo "  Metric Comparison"
echo "========================================="
echo ""

# Traditional Redis metrics
echo "Traditional metrics (INFO MEMORY):"
echo "-----------------------------------"
FRAG_RATIO=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r')
USED_MEM=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^used_memory_human:" | cut -d: -f2 | tr -d '\r')
RSS_MEM=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^used_memory_rss_human:" | cut -d: -f2 | tr -d '\r')

echo "  mem_fragmentation_ratio: $FRAG_RATIO"
echo "  used_memory:             $USED_MEM"
echo "  used_memory_rss:         $RSS_MEM"
echo ""

# libdrainprof metrics (once instrumented)
echo "libdrainprof metrics (DSR per size class):"
echo "-------------------------------------------"

# TODO: Once instrumentation is complete, these will come from INFO MEMORY
# For now, show expected format:
echo "  [Not yet instrumented - expected format:]"
echo "  drainprof_dsr:        0.25  (overall DSR)"
echo "  drainprof_small_dsr:  0.20  (< 256B - where fragmentation hits)"
echo "  drainprof_medium_dsr: 0.45  (256B-1KB)"
echo "  drainprof_large_dsr:  0.80  (1KB-4KB)"
echo "  drainprof_huge_dsr:   1.00  (>= 4KB - likely fully drainable)"
echo ""

echo "========================================="
echo "  Analysis"
echo "========================================="
echo "After delete-odd-keys workload:"
echo ""

FRAG_HIGH=$(echo "$FRAG_RATIO > 1.5" | bc -l 2>/dev/null || echo "0")
if [ "$FRAG_HIGH" = "1" ]; then
  echo "✗ Fragmentation ratio = $FRAG_RATIO (HIGH)"
  echo "  Redis knows it's fragmented"
  echo ""
  echo "Expected libdrainprof DSR:"
  echo "  ✗ Small object DSR ≈ 20-30% (LOW)"
  echo "    → Most small-object slabs pinned by remaining keys"
  echo "    → Predicts the fragmentation we're seeing"
  echo ""
  echo "Operator action:"
  echo "  - MEMORY PURGE (if Redis 4.0+)"
  echo "  - Restart during maintenance window"
  echo "  - Change key distribution strategy"
else
  echo "✓ Fragmentation ratio = $FRAG_RATIO (OK)"
  echo "  Expected DSR: > 60% across all size classes"
fi

echo ""
echo "========================================="
echo "  Next Steps"
echo "========================================="
echo "1. Implement zmalloc instrumentation patch"
echo "2. Build Redis with Dockerfile.redis-instrumented"
echo "3. Re-run this comparison on instrumented build"
echo "4. Validate: low DSR → high fragmentation ratio"
