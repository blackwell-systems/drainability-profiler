#!/bin/bash
# Reproduce Redis memory fragmentation using populate-delete pattern
#
# This workload demonstrates the structural leak that libdrainprof detects:
# 1. Create 1M small keys (~100MB total)
# 2. Delete every other key (50% freed)
# 3. jemalloc can't reclaim slabs (remaining keys pin them)
# 4. mem_fragmentation_ratio spikes to 1.8-2.0+

set -euo pipefail

REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"

echo "========================================="
echo "  Redis Fragmentation Test"
echo "========================================="
echo "Host: $REDIS_HOST:$REDIS_PORT"
echo "Workload: 1M keys, delete odd keys"
echo ""

# Wait for Redis to be ready
echo "Waiting for Redis..."
until redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" ping 2>/dev/null | grep -q PONG; do
  sleep 1
done
echo "✓ Redis ready"
echo ""

# Capture baseline metrics
echo "========================================="
echo "  Baseline (Empty Database)"
echo "========================================="
redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Phase 1: Populate with 1M keys
echo "========================================="
echo "  Phase 1: Creating 1M keys"
echo "========================================="
redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" DEBUG POPULATE 1000000 key 100
echo "✓ Created 1,000,000 keys (100 bytes each)"
echo ""

redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Phase 2: Delete odd keys (50% freed)
echo "========================================="
echo "  Phase 2: Deleting odd keys (50%)"
echo "========================================="
echo "Running deletion script..."

# Use Lua for fast deletion (pipeline would be slower)
redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" --eval - 0 <<'LUA'
local deleted = 0
for i = 1, 1000000, 2 do
  local key = string.format("key:%012d", i)
  if redis.call('DEL', key) == 1 then
    deleted = deleted + 1
  end

  -- Progress indicator every 100k deletions
  if deleted % 100000 == 0 then
    redis.log(redis.LOG_NOTICE, string.format("Deleted %d keys", deleted))
  end
end
return deleted
LUA

echo "✓ Deleted 500,000 keys (50% freed)"
echo ""

# Phase 3: Show fragmentation
echo "========================================="
echo "  Phase 3: Fragmentation Result"
echo "========================================="
redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep -E "(used_memory:|used_memory_rss:|mem_fragmentation_ratio:)"
echo ""

# Extract and display key metrics
USED_MEM=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^used_memory:" | cut -d: -f2 | tr -d '\r')
RSS_MEM=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^used_memory_rss:" | cut -d: -f2 | tr -d '\r')
FRAG_RATIO=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" INFO MEMORY | grep "^mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r')

echo "========================================="
echo "  Verdict"
echo "========================================="
echo "Used memory:      $USED_MEM bytes (Redis's view)"
echo "RSS memory:       $RSS_MEM bytes (actual RAM)"
echo "Fragmentation:    ${FRAG_RATIO}x"
echo ""

# Interpret results
FRAG_CHECK=$(echo "$FRAG_RATIO > 1.5" | bc -l 2>/dev/null || echo "0")
if [ "$FRAG_CHECK" = "1" ]; then
  echo "✗ Fragmentation detected (ratio > 1.5)"
  echo "  - 50% of objects freed ✓"
  echo "  - jemalloc can't reclaim slabs ✗"
  echo "  - RSS stays high (structural leak) ✗"
else
  echo "✓ Fragmentation below threshold"
  echo "  (May need larger workload or different Redis version)"
fi

echo ""
echo "Next step: Run instrumented Redis and compare DSR"
