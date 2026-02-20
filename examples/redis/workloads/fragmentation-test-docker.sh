#!/bin/bash
# Docker-friendly version of fragmentation test
# Runs redis-cli commands via docker exec

set -euo pipefail

CONTAINER_NAME="${1:-redis-baseline}"

echo "========================================="
echo "  Redis Fragmentation Test (Docker)"
echo "========================================="
echo "Container: $CONTAINER_NAME"
echo "Workload: 1M keys, delete odd keys"
echo ""

# Function to run redis-cli in container
rcli() {
  docker exec "$CONTAINER_NAME" redis-cli "$@"
}

# Wait for Redis
echo "Waiting for Redis..."
until rcli ping 2>/dev/null | grep -q PONG; do
  sleep 1
done
echo "✓ Redis ready"
echo ""

# Baseline
echo "========================================="
echo "  Baseline (Empty Database)"
echo "========================================="
rcli INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Phase 1: Populate
echo "========================================="
echo "  Phase 1: Creating 1M keys"
echo "========================================="
echo "This takes ~10-15 seconds..."
rcli DEBUG POPULATE 1000000 key 100
echo "✓ Created 1,000,000 keys (100 bytes each)"
echo ""

rcli INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Phase 2: Delete odd keys
echo "========================================="
echo "  Phase 2: Deleting odd keys (50%)"
echo "========================================="
echo "Running deletion script (this takes ~20-30 seconds)..."

# Copy Lua script into container and execute
SCRIPT_DIR="$(dirname "${BASH_SOURCE[0]}")"
docker cp "$SCRIPT_DIR/delete-lua.lua" "$CONTAINER_NAME:/tmp/delete.lua"
DELETED=$(docker exec "$CONTAINER_NAME" redis-cli --eval /tmp/delete.lua)

echo "✓ Deleted $DELETED keys (50% freed)"
echo ""

# Phase 3: Show fragmentation
echo "========================================="
echo "  Phase 3: Fragmentation Result"
echo "========================================="
rcli INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Extract metrics
USED_MEM=$(rcli INFO MEMORY | grep "^used_memory:" | cut -d: -f2 | tr -d '\r')
RSS_MEM=$(rcli INFO MEMORY | grep "^used_memory_rss:" | cut -d: -f2 | tr -d '\r')
FRAG_RATIO=$(rcli INFO MEMORY | grep "^mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r')

echo "========================================="
echo "  Verdict"
echo "========================================="
echo "Used memory:      $USED_MEM bytes (Redis's view)"
echo "RSS memory:       $RSS_MEM bytes (actual RAM)"
echo "Fragmentation:    ${FRAG_RATIO}x"
echo ""

# Interpret
FRAG_CHECK=$(echo "$FRAG_RATIO > 1.5" | bc -l 2>/dev/null || echo "0")
if [ "$FRAG_CHECK" = "1" ]; then
  echo "✗ Fragmentation detected (ratio > 1.5)"
  echo "  - 50% of objects freed ✓"
  echo "  - jemalloc can't reclaim slabs ✗"
  echo "  - RSS stays high (structural leak) ✗"
  echo ""
  echo "This is exactly what libdrainprof should detect!"
else
  echo "✓ Fragmentation below threshold (ratio = $FRAG_RATIO)"
  echo "  (May need larger workload)"
fi

echo ""
echo "========================================="
echo "  Next Steps"
echo "========================================="
echo "1. Implement zmalloc instrumentation patch"
echo "2. Build instrumented Redis with Dockerfile"
echo "3. Re-run this test and compare DSR"
echo ""
echo "Expected: DSR ~20-30% when fragmentation ratio = $FRAG_RATIO"
