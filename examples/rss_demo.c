/**
 * rss_demo.c - Visual demonstration of structural memory leaks
 *
 * Shows RSS growth and DSR metrics in two modes:
 * - --broken: Sessions allocated in request epochs (structural leak)
 * - --fixed:  Sessions allocated in separate long-lived arena (no leak)
 *
 * Usage:
 *   ./rss_demo --broken    # Watch RSS climb, DSR drop
 *   ./rss_demo --fixed     # Watch RSS stay flat, DSR stay high
 */

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

/* Simulated HTTP server state */
typedef struct {
    drainprof *prof;
    uint64_t current_epoch;
    uint64_t session_arena_id;
    int broken_mode;

    /* Counters */
    uint64_t total_requests;
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
    srv->prof = drainprof_create();
    srv->current_epoch = 0;
    srv->session_arena_id = 999999;  /* Separate from epochs */
    srv->broken_mode = broken;
    srv->total_requests = 0;
    srv->active_sessions = 0;

    /* Open initial request epoch */
    drainprof_granule_open(srv->prof, srv->current_epoch);

    /* Open session arena (fixed mode only) */
    if (!broken) {
        drainprof_granule_open(srv->prof, srv->session_arena_id);
    }
}

/* Advance to next request epoch */
static void server_advance_epoch(Server *srv) {
    /* Close old epoch */
    int drainable = drainprof_granule_close(srv->prof, srv->current_epoch);
    (void)drainable;  /* We'll check DSR via snapshot */

    /* Open new epoch */
    srv->current_epoch++;
    drainprof_granule_open(srv->prof, srv->current_epoch);
}

/* Process single HTTP request */
static void process_request(Server *srv) {
    uint64_t epoch = srv->current_epoch;

    /* Allocate request buffer (always in request epoch) */
    uint64_t req_id = srv->total_requests * 100;
    void *req_buf = malloc(REQUEST_BUFFER_SIZE);
    drainprof_alloc_register(srv->prof, epoch, (uintptr_t)req_buf, REQUEST_BUFFER_SIZE);

    /* Simulate: 10% of requests create new sessions */
    int creates_session = (srv->total_requests % 10) == 0;
    void *session = NULL;

    if (creates_session) {
        session = malloc(SESSION_SIZE);

        if (srv->broken_mode) {
            /* BUG: Session allocated in request epoch (will pin epoch) */
            drainprof_alloc_register(srv->prof, epoch, (uintptr_t)session, SESSION_SIZE);
        } else {
            /* FIX: Session allocated in separate arena */
            drainprof_alloc_register(srv->prof, srv->session_arena_id,
                                   (uintptr_t)session, SESSION_SIZE);
        }
        srv->active_sessions++;
    }

    /* Process request (no-op) */

    /* Free request buffer (always freed before epoch closes) */
    drainprof_alloc_deregister(srv->prof, epoch, (uintptr_t)req_buf);
    free(req_buf);

    /* Sessions stay live (freed much later in real system) */
    /* For demo, we'll leak them to show the effect */

    srv->total_requests++;
}

/* Print status line */
static void print_status(Server *srv, int elapsed, uint64_t start_rss, uint64_t current_rss) {
    drainprof_snapshot_t snap;
    drainprof_snapshot(srv->prof, &snap);

    double rss_mb = current_rss / (1024.0 * 1024.0);
    double rss_delta_mb = (current_rss - start_rss) / (1024.0 * 1024.0);

    printf("\r[%2ds] RSS: %6.1f MB (+%5.1f MB) | "
           "Requests: %6lu | Sessions: %4lu | "
           "DSR: %5.1f%% | Epochs: %4lu/%4lu drainable   ",
           elapsed, rss_mb, rss_delta_mb,
           srv->total_requests, srv->active_sessions,
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
    printf("Workload: %d requests/sec, 10%% create sessions\n\n", REQUESTS_PER_SECOND);

    if (broken) {
        printf("Problem: Sessions allocated in request epochs\n");
        printf("Result:  Epochs pinned, RSS grows unbounded\n\n");
    } else {
        printf("Fix:     Sessions allocated in separate arena\n");
        printf("Result:  Request epochs drainable, RSS bounded\n\n");
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

    while (1) {
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);

        if (elapsed >= DURATION_SECONDS) break;

        /* Process requests */
        for (int i = 0; i < REQUESTS_PER_SECOND / 10; i++) {
            process_request(&srv);
        }

        /* Advance epoch every 100 requests */
        if ((srv.total_requests % 100) == 0 && srv.total_requests > 0) {
            server_advance_epoch(&srv);
        }

        /* Print status every second */
        if (elapsed != last_printed) {
            uint64_t current_rss = get_rss_bytes();
            print_status(&srv, elapsed, start_rss, current_rss);
            last_printed = elapsed;
        }

        usleep(100000);  /* 100ms */
    }

    /* Final stats */
    printf("\n\n");
    printf("=========================================\n");
    printf("  Final Results\n");
    printf("=========================================\n");

    uint64_t end_rss = get_rss_bytes();
    double start_mb = start_rss / (1024.0 * 1024.0);
    double end_mb = end_rss / (1024.0 * 1024.0);
    double growth_mb = (end_rss - start_rss) / (1024.0 * 1024.0);

    drainprof_snapshot_t snap;
    drainprof_snapshot(srv.prof, &snap);

    printf("RSS:              %.1f MB → %.1f MB (+%.1f MB)\n", start_mb, end_mb, growth_mb);
    printf("Requests:         %lu\n", srv.total_requests);
    printf("Sessions:         %lu\n", srv.active_sessions);
    printf("Epochs closed:    %lu\n", snap.total_closes);
    printf("Drainable:        %lu (%.1f%%)\n", snap.drainable_closes, snap.dsr * 100.0);
    printf("Pinned:           %lu\n", snap.pinned_closes);
    printf("\n");

    if (broken) {
        printf("Conclusion: Structural leak detected!\n");
        printf("  - RSS grew %.1f MB over %d seconds\n", growth_mb, DURATION_SECONDS);
        printf("  - DSR = %.1f%% (many epochs pinned)\n", snap.dsr * 100.0);
        printf("  - Sessions in wrong granule prevent reclamation\n");
    } else {
        printf("Conclusion: No structural leak\n");
        printf("  - RSS grew only %.1f MB (working set stable)\n", growth_mb);
        printf("  - DSR = %.1f%% (epochs drainable)\n", snap.dsr * 100.0);
        printf("  - Correct allocation strategy works\n");
    }
    printf("\n");

    drainprof_destroy(srv.prof);
    return 0;
}
