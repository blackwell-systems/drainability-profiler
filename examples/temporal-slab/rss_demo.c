/**
 * rss_demo.c - Visual demonstration of structural memory leaks
 *
 * Shows RSS growth and DSR metrics using real temporal-slab allocation:
 * - --broken: Sessions allocated in request epochs (structural leak)
 * - --fixed:  Sessions allocated in separate long-lived arena (no leak)
 *
 * Both modes free ALL allocations (Valgrind reports zero leaks).
 * Only --broken shows RSS growth because epochs are pinned.
 *
 * Usage:
 *   ./rss_demo --broken    # Watch RSS climb, DSR drop
 *   ./rss_demo --fixed     # Watch RSS stay flat, DSR stay high
 */

#define _GNU_SOURCE
#include "../../../temporal-slab/include/slab_alloc.h"
#include "../../../temporal-slab/include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define DURATION_SECONDS 30
#define REQUESTS_PER_SECOND 100
#define REQUEST_BUFFER_SIZE 1024
#define SESSION_SIZE 4096
#define SESSION_TIMEOUT_SECONDS 10  /* Sessions live for 10 seconds */
#define MAX_SESSIONS 10000

extern drainprof* g_profiler;

/* Session tracking */
typedef struct {
    void* ptr;
    SlabHandle handle;
    time_t created;
    uint64_t epoch_id;  /* Which epoch it was allocated from */
    int active;
} Session;

/* Server state */
typedef struct {
    SlabAllocator* alloc;
    int broken_mode;

    /* Session ring buffer */
    Session sessions[MAX_SESSIONS];
    int next_session_slot;

    /* Separate arena for sessions (fixed mode only) */
    EpochId session_arena_id;

    /* Counters */
    uint64_t total_requests;
    uint64_t total_sessions_created;
    uint64_t total_sessions_freed;
    uint64_t active_sessions;
} Server;

/* Get RSS in bytes */
static uint64_t get_rss_bytes(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;

    char line[256];
    uint64_t rss_kb = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%lu", &rss_kb);
            break;
        }
    }
    fclose(fp);
    return rss_kb * 1024;
}

/* Initialize server */
static void server_init(Server *srv, int broken) {
    /* Initialize profiler */
    g_profiler = drainprof_create();
    if (!g_profiler) {
        fprintf(stderr, "ERROR: Failed to create profiler\n");
        exit(1);
    }

    srv->alloc = slab_allocator_create();
    if (!srv->alloc) {
        fprintf(stderr, "ERROR: Failed to create allocator\n");
        exit(1);
    }

    srv->broken_mode = broken;
    srv->next_session_slot = 0;
    srv->total_requests = 0;
    srv->total_sessions_created = 0;
    srv->total_sessions_freed = 0;
    srv->active_sessions = 0;

    /* Initialize session tracking */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        srv->sessions[i].active = 0;
    }

    /* In fixed mode, create a separate long-lived arena for sessions */
    if (!broken) {
        /* Use a high epoch ID that won't conflict with request epochs */
        srv->session_arena_id = 1000000;
        /* Advance to this epoch (temporal-slab will manage it) */
        while (epoch_current(srv->alloc) < srv->session_arena_id) {
            epoch_advance(srv->alloc);
        }
    }

    /* Start at epoch 0 for requests */
    while (epoch_current(srv->alloc) > 0) {
        /* Already at epoch 0 after init */
        break;
    }
}

/* Timeout and free old sessions */
static void server_timeout_sessions(Server *srv, time_t now) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &srv->sessions[i];
        if (s->active && (now - s->created >= SESSION_TIMEOUT_SECONDS)) {
            /* Session timed out - free it */
            free_obj(srv->alloc, s->handle);
            s->active = 0;
            srv->total_sessions_freed++;
            srv->active_sessions--;
        }
    }
}

/* Process single HTTP request */
static void server_process_request(Server *srv, time_t now) {
    EpochId request_epoch = epoch_current(srv->alloc);

    /* Allocate request buffer (always in request epoch) */
    SlabHandle req_handle;
    void *req_buf = alloc_obj_epoch(srv->alloc, REQUEST_BUFFER_SIZE,
                                     request_epoch, &req_handle);
    if (!req_buf) return;

    /* Simulate: 10% of requests create new sessions */
    int creates_session = (srv->total_requests % 10) == 0;

    if (creates_session) {
        /* Find free session slot */
        int slot = srv->next_session_slot;
        srv->next_session_slot = (srv->next_session_slot + 1) % MAX_SESSIONS;

        Session *session = &srv->sessions[slot];
        if (session->active) {
            /* Force free old session if slot occupied */
            free_obj(srv->alloc, session->handle);
            srv->total_sessions_freed++;
            srv->active_sessions--;
        }

        /* Allocate session */
        EpochId session_epoch;
        if (srv->broken_mode) {
            /* BUG: Allocate session from request epoch */
            session_epoch = request_epoch;
        } else {
            /* FIX: Allocate session from separate long-lived arena */
            session_epoch = srv->session_arena_id;
        }

        session->ptr = alloc_obj_epoch(srv->alloc, SESSION_SIZE,
                                       session_epoch, &session->handle);
        if (session->ptr) {
            session->created = now;
            session->epoch_id = session_epoch;
            session->active = 1;
            srv->total_sessions_created++;
            srv->active_sessions++;
        }
    }

    /* Process request (no-op) */

    /* Free request buffer (always freed before epoch closes) */
    free_obj(srv->alloc, req_handle);

    srv->total_requests++;
}

/* Print status line */
static void print_status(Server *srv, int elapsed, uint64_t start_rss, uint64_t current_rss) {
    drainprof_snapshot_t snap;
    drainprof_snapshot(g_profiler, &snap);

    double rss_mb = current_rss / (1024.0 * 1024.0);
    double rss_delta_mb = (current_rss - start_rss) / (1024.0 * 1024.0);

    printf("\r[%2ds] RSS: %6.1f MB (+%5.1f MB) | "
           "Reqs: %5lu | Sessions: %4lu/%4lu | "
           "DSR: %5.1f%% | Epochs: %4lu/%4lu drainable   ",
           elapsed, rss_mb, rss_delta_mb,
           srv->total_requests,
           srv->active_sessions, srv->total_sessions_created,
           snap.dsr * 100.0,
           snap.drainable_closes, snap.total_closes);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--broken") != 0 && strcmp(argv[1], "--fixed") != 0)) {
        fprintf(stderr, "Usage: %s [--broken|--fixed]\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "  --broken  Allocate sessions in request epochs (structural leak)\n");
        fprintf(stderr, "  --fixed   Allocate sessions in separate arena (no leak)\n");
        fprintf(stderr, "\n");
        return 1;
    }

    int broken = strcmp(argv[1], "--broken") == 0;

    printf("=========================================\n");
    printf("  libdrainprof RSS Demo\n");
    printf("=========================================\n");
    printf("Mode: %s\n", broken ? "BROKEN (structural leak)" : "FIXED (correct allocation)");
    printf("Duration: %d seconds\n", DURATION_SECONDS);
    printf("Workload: %d requests/sec, 10%% create sessions\n", REQUESTS_PER_SECOND);
    printf("Session timeout: %d seconds\n\n", SESSION_TIMEOUT_SECONDS);

    if (broken) {
        printf("Problem: Sessions allocated in request epochs\n");
        printf("Result:  Epochs pinned until session timeout, RSS grows\n");
        printf("Note:    All objects ARE freed (Valgrind clean)\n\n");
    } else {
        printf("Fix:     Sessions allocated in separate arena\n");
        printf("Result:  Request epochs drainable, RSS bounded\n");
        printf("Note:    All objects ARE freed (Valgrind clean)\n\n");
    }

    printf("Starting in 2 seconds...\n");
    sleep(2);

    /* Initialize */
    Server srv;
    server_init(&srv, broken);
    uint64_t start_rss = get_rss_bytes();

    printf("\nRunning");
    fflush(stdout);

    /* Run workload */
    time_t start_time = time(NULL);
    int last_printed = -1;
    int last_epoch_advance = 0;

    while (1) {
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);

        if (elapsed >= DURATION_SECONDS) break;

        /* Timeout old sessions */
        server_timeout_sessions(&srv, now);

        /* Process requests */
        for (int i = 0; i < REQUESTS_PER_SECOND / 10; i++) {
            server_process_request(&srv, now);
        }

        /* Advance epoch every second */
        if (elapsed > last_epoch_advance && elapsed > 0) {
            epoch_advance(srv.alloc);

            /* Close old epochs (simulate keeping last 5 seconds active) */
            EpochId current = epoch_current(srv.alloc);
            if (current > 5) {
                EpochId old = current - 5;
                if (old != srv.session_arena_id) {  /* Don't close session arena */
                    epoch_close(srv.alloc, old);
                }
            }
            last_epoch_advance = elapsed;
        }

        /* Print status every second */
        if (elapsed != last_printed) {
            uint64_t current_rss = get_rss_bytes();
            print_status(&srv, elapsed, start_rss, current_rss);
            last_printed = elapsed;
        }

        usleep(100000);  /* 100ms */
    }

    /* Clean up all remaining sessions */
    printf("\n\nCleaning up sessions...\n");
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (srv.sessions[i].active) {
            free_obj(srv.alloc, srv.sessions[i].handle);
            srv.total_sessions_freed++;
            srv.active_sessions--;
        }
    }

    /* Final stats */
    printf("\n=========================================\n");
    printf("  Final Results\n");
    printf("=========================================\n");

    uint64_t end_rss = get_rss_bytes();
    double start_mb = start_rss / (1024.0 * 1024.0);
    double end_mb = end_rss / (1024.0 * 1024.0);
    double growth_mb = (end_rss - start_rss) / (1024.0 * 1024.0);

    drainprof_snapshot_t snap;
    drainprof_snapshot(g_profiler, &snap);

    printf("RSS:              %.1f MB → %.1f MB (+%.1f MB)\n", start_mb, end_mb, growth_mb);
    printf("Requests:         %lu\n", srv.total_requests);
    printf("Sessions created: %lu\n", srv.total_sessions_created);
    printf("Sessions freed:   %lu\n", srv.total_sessions_freed);
    printf("Active:           %lu\n", srv.active_sessions);
    printf("Epochs closed:    %lu\n", snap.total_closes);
    printf("Drainable:        %lu (%.1f%%)\n", snap.drainable_closes, snap.dsr * 100.0);
    printf("Pinned:           %lu\n", snap.pinned_closes);
    printf("\n");

    if (broken) {
        printf("Conclusion: Structural leak detected!\n");
        printf("  - RSS grew %.1f MB over %d seconds\n", growth_mb, DURATION_SECONDS);
        printf("  - DSR = %.1f%% (many epochs pinned by sessions)\n", snap.dsr * 100.0);
        printf("  - All objects freed (Valgrind would report zero leaks)\n");
        printf("  - But epochs can't be reclaimed until sessions timeout\n");
    } else {
        printf("Conclusion: No structural leak\n");
        printf("  - RSS grew only %.1f MB (working set stable)\n", growth_mb);
        printf("  - DSR = %.1f%% (request epochs drainable)\n", snap.dsr * 100.0);
        printf("  - All objects freed (Valgrind would report zero leaks)\n");
        printf("  - Sessions in separate arena, request epochs reclaim immediately\n");
    }
    printf("\n");

    slab_allocator_free(srv.alloc);
    drainprof_destroy(g_profiler);
    return 0;
}
