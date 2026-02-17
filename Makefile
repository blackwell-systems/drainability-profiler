# Makefile for libdrainprof

CC ?= gcc
CFLAGS = -O2 -std=c11 -pthread -Iinclude -Wall -Wextra -Wpedantic
CFLAGS_DEBUG = -O0 -g -std=c11 -pthread -Iinclude -Wall -Wextra -Wpedantic

# Source files
SRC = src/drainprof.c src/counters.c src/slot_array.c src/diagnostic.c
OBJ = $(SRC:.c=.o)

# Library targets
LIB_STATIC = libdrainprof.a
LIB_SHARED = libdrainprof.so

# Test and benchmark sources
TEST_SRC = $(wildcard test/test_*.c)
TEST_BIN = $(TEST_SRC:test/%.c=build/%)

BENCH_SRC = $(wildcard bench/bench_*.c)
BENCH_BIN = $(BENCH_SRC:bench/%.c=build/%)

EXAMPLE_SRC = $(wildcard examples/*.c)
EXAMPLE_BIN = $(EXAMPLE_SRC:examples/%.c=build/%)

# ============================================================================
#  Main Targets
# ============================================================================

.PHONY: all clean test bench examples install

all: $(LIB_STATIC) $(LIB_SHARED)

# Static library (primary artifact)
$(LIB_STATIC): $(OBJ)
	ar rcs $@ $^

# Shared library
$(LIB_SHARED): $(SRC)
	$(CC) $(CFLAGS) -shared -fPIC $^ -o $@

# Object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================================================
#  Tests
# ============================================================================

test: $(LIB_STATIC) $(TEST_BIN)
	@echo "Running tests..."
	@for test in $(TEST_BIN); do \
		echo "  $$test"; \
		./$$test || exit 1; \
	done
	@echo "All tests passed!"

build/test_%: test/test_%.c $(LIB_STATIC)
	@mkdir -p build
	$(CC) $(CFLAGS_DEBUG) $< -L. -ldrainprof -o $@

# ============================================================================
#  Benchmarks
# ============================================================================

bench: $(LIB_STATIC) $(BENCH_BIN)
	@echo "Running benchmarks..."
	@for bench in $(BENCH_BIN); do \
		echo "  $$bench"; \
		./$$bench; \
	done

build/bench_%: bench/bench_%.c $(LIB_STATIC)
	@mkdir -p build
	$(CC) $(CFLAGS) $< -L. -ldrainprof -o $@

# ============================================================================
#  Examples
# ============================================================================

examples: $(LIB_STATIC) $(EXAMPLE_BIN)

build/%: examples/%.c $(LIB_STATIC)
	@mkdir -p build
	$(CC) $(CFLAGS) $< -L. -ldrainprof -o $@

# ============================================================================
#  Installation
# ============================================================================

PREFIX ?= /usr/local

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib
	install -d $(PREFIX)/include
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 include/drainprof.h $(PREFIX)/include/

# ============================================================================
#  Cleanup
# ============================================================================

clean:
	rm -f $(OBJ) $(LIB_STATIC) $(LIB_SHARED)
	rm -rf build/

.PHONY: all clean test bench examples install
