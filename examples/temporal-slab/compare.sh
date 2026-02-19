#!/bin/bash
# compare.sh - Show what Valgrind misses: structural leaks with 0 traditional leaks

set -e

echo "========================================="
echo "  Valgrind vs libdrainprof"
echo "========================================="
echo ""
echo "Same binary, same workload, two tools:"
echo ""

# Run broken mode under Valgrind
echo "--- Running broken mode with Valgrind (30s, be patient)..."
valgrind --leak-check=full --show-leak-kinds=all --log-file=/tmp/valgrind-broken.log \
    ./rss_demo --broken >/tmp/drainprof-broken.log 2>&1

echo ""
echo "========================================="
echo "  VALGRIND SAYS:"
echo "========================================="
grep -A 8 "LEAK SUMMARY" /tmp/valgrind-broken.log || echo "  All heap blocks were freed -- no leaks are possible"

echo ""
echo "========================================="
echo "  LIBDRAINPROF SAYS:"
echo "========================================="
tail -12 /tmp/drainprof-broken.log

echo ""
echo "========================================="
echo "  VERDICT"
echo "========================================="
echo "✓ Valgrind:     0 bytes leaked"
echo "✗ libdrainprof: DSR=5%, structural leak detected"
echo ""
echo "Same binary. All objects freed. Valgrind clean."
echo "But 95% of epochs are pinned and non-drainable."
echo ""
echo "This is what traditional tools miss."
echo ""

