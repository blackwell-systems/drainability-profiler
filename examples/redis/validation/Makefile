# Build validation programs for Redis drainprof integration

CC = gcc
CFLAGS = -O2 -g -std=c11 -pthread -Wall -Wextra -pedantic
CFLAGS += -I../../../include

# jemalloc paths (adjust for your system)
JEMALLOC_PREFIX ?= $(shell brew --prefix jemalloc 2>/dev/null || echo "/usr")
CFLAGS += -I$(JEMALLOC_PREFIX)/include

LDFLAGS = -L$(JEMALLOC_PREFIX)/lib
LDLIBS = -ljemalloc

DRAINPROF_LIB = ../../../libdrainprof.a

.PHONY: all clean test

all: zmalloc_test

zmalloc_test: zmalloc_test.c $(DRAINPROF_LIB)
	$(CC) $(CFLAGS) $< $(DRAINPROF_LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: zmalloc_test
	@echo "Running zmalloc instrumentation test..."
	@./zmalloc_test

clean:
	rm -f zmalloc_test *.o

# Ensure libdrainprof is built
$(DRAINPROF_LIB):
	@echo "Building libdrainprof..."
	@cd ../../.. && $(MAKE)
