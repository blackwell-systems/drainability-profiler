/**
 * drainprof.c - Main profiler implementation
 */

#include "drainprof_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ========================================================================== */
/*  Configuration Defaults                                                    */
/* ========================================================================== */

void drainprof_config_default(drainprof_config *config) {
    config->mode = DRAINPROF_PRODUCTION;
    config->storage = DRAINPROF_SLOT_ARRAY;
    config->slot_capacity = 1024;
    config->bucket_count = 64;
    config->log_interval = 0;
    config->on_pinning = NULL;
    config->callback_user_data = NULL;
    config->max_buffered_reports = 1000;
    config->verbose = false;
}

/* ========================================================================== */
/*  Profiler Lifecycle                                                        */
/* ========================================================================== */

drainprof *drainprof_create(void) {
    drainprof_config config;
    drainprof_config_default(&config);
    return drainprof_create_with_config(&config);
}

drainprof *drainprof_create_with_config(const drainprof_config *config) {
    if (!config) {
        return NULL;
    }

    drainprof *prof = malloc(sizeof(drainprof));
    if (!prof) {
        return NULL;
    }

    /* Copy configuration */
    prof->config = *config;

    /* Initialize counters */
    drainprof_counters_init(&prof->counters);

    /* Initialize storage backend based on mode and storage type */
    if (config->mode == DRAINPROF_PRODUCTION) {
        /* Production mode storage */
        if (config->storage == DRAINPROF_SLOT_ARRAY) {
            prof->storage.slot_array = drainprof_slot_array_create(
                config->slot_capacity
            );
            if (!prof->storage.slot_array) {
                free(prof);
                return NULL;
            }
        } else {
            /* Hash map not yet implemented */
            prof->storage.hash_map = NULL;
            fprintf(stderr, "drainprof: HASH_MAP storage not yet implemented\n");
            free(prof);
            return NULL;
        }
        prof->report_buffer = NULL;
    } else {
        /* Diagnostic mode storage */
        if (config->storage == DRAINPROF_SLOT_ARRAY) {
            prof->storage.slot_array_diag = drainprof_slot_array_diag_create(
                config->slot_capacity
            );
            if (!prof->storage.slot_array_diag) {
                free(prof);
                return NULL;
            }
        } else {
            /* Hash map not yet implemented */
            prof->storage.hash_map_diag = NULL;
            fprintf(stderr, "drainprof: HASH_MAP storage not yet implemented\n");
            free(prof);
            return NULL;
        }

        /* Create report buffer if no callback provided */
        if (config->on_pinning == NULL) {
            prof->report_buffer = drainprof_report_buffer_create(
                config->max_buffered_reports
            );
            if (!prof->report_buffer) {
                if (config->storage == DRAINPROF_SLOT_ARRAY) {
                    drainprof_slot_array_diag_destroy(prof->storage.slot_array_diag);
                }
                free(prof);
                return NULL;
            }
        } else {
            prof->report_buffer = NULL;
        }
    }

    return prof;
}

void drainprof_destroy(drainprof *prof) {
    if (!prof) {
        return;
    }

    /* Destroy storage backend */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_slot_array_destroy(prof->storage.slot_array);
        }
    } else {
        /* Diagnostic mode */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_slot_array_diag_destroy(prof->storage.slot_array_diag);
        }
        if (prof->report_buffer) {
            drainprof_report_buffer_destroy(prof->report_buffer);
        }
    }

    free(prof);
}

/* ========================================================================== */
/*  Granule Lifecycle                                                         */
/* ========================================================================== */

int drainprof_granule_open(drainprof *prof, drainprof_granule_id id) {
    if (!prof) {
        return -1;
    }

    int result = -1;

    /* Mode-specific storage operations */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        /* Production mode: simple slot array */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            result = drainprof_slot_array_open(prof->storage.slot_array, id);
        }
    } else {
        /* Diagnostic mode: slot array with diagnostic state */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            struct timespec open_time;
            clock_gettime(CLOCK_MONOTONIC, &open_time);
            result = drainprof_slot_array_diag_open(
                prof->storage.slot_array_diag,
                id,
                &open_time
            );
        }
    }

    if (result != 0) {
        return result;
    }

    /* Update global counters (always, regardless of mode) */
    uint64_t open = atomic_fetch_add(&prof->counters.open_granules, 1) + 1;

    /* Update peak if needed */
    uint64_t peak = atomic_load(&prof->counters.peak_open_granules);
    while (open > peak) {
        if (atomic_compare_exchange_weak(
            &prof->counters.peak_open_granules, &peak, open
        )) {
            break;
        }
    }

    return 0;
}

int drainprof_granule_close(drainprof *prof, drainprof_granule_id id) {
    if (!prof) {
        return -1;
    }

    uint32_t live_count = UINT32_MAX;
    int drainable;

    /* Mode-specific close operations */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        /* Production mode: get live count from slot array */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            live_count = drainprof_slot_array_close(prof->storage.slot_array, id);
        }

        if (live_count == UINT32_MAX) {
            return -1;
        }

        drainable = (live_count == 0) ? 1 : 0;
    } else {
        /* Diagnostic mode: get diagnostic state */
        drainprof_granule_diag *diag = NULL;
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            int result = drainprof_slot_array_diag_close(
                prof->storage.slot_array_diag,
                id,
                &diag
            );
            if (result != 0 || !diag) {
                return -1;
            }
        } else {
            return -1;
        }

        live_count = atomic_load(&diag->live_count);
        drainable = (live_count == 0) ? 1 : 0;

        /* If pinned, generate and handle report */
        if (!drainable) {
            struct timespec close_time;
            clock_gettime(CLOCK_MONOTONIC, &close_time);

            drainprof_pinning_report *report =
                drainprof_granule_diag_generate_report(diag, &close_time);

            if (report) {
                /* Invoke callback or buffer */
                if (prof->config.on_pinning) {
                    prof->config.on_pinning(report, prof->config.callback_user_data);
                    drainprof_pinning_report_free(report);
                } else if (prof->report_buffer) {
                    drainprof_report_buffer_push(prof->report_buffer, report);
                } else {
                    /* No callback and no buffer - free immediately */
                    drainprof_pinning_report_free(report);
                }
            }

            /* Verbose logging */
            if (prof->config.verbose) {
                fprintf(stderr, "drainprof: Granule %" PRIu64 " pinned by %u allocations\n",
                        id, live_count);
            }
        }

        /* Clean up diagnostic state */
        drainprof_granule_diag_destroy(diag);
    }

    /* Update global counters (always, regardless of mode) */
    atomic_fetch_sub(&prof->counters.open_granules, 1);
    atomic_fetch_add(&prof->counters.total_closes, 1);

    if (drainable) {
        atomic_fetch_add(&prof->counters.drainable_closes, 1);
    } else {
        atomic_fetch_add(&prof->counters.pinned_closes, 1);
    }

    /* Optional periodic logging */
    if (prof->config.log_interval > 0) {
        uint64_t total = atomic_load(&prof->counters.total_closes);
        if (total % prof->config.log_interval == 0) {
            uint64_t drainable_count = atomic_load(&prof->counters.drainable_closes);
            uint64_t pinned_count = atomic_load(&prof->counters.pinned_closes);
            double dsr = total > 0 ? (double)drainable_count / (double)total : 0.0;
            fprintf(stderr, "drainprof: DSR=%.1f%% (%" PRIu64 " pinned / %" PRIu64 " total)\n",
                    dsr * 100.0, pinned_count, total);
        }
    }

    return drainable;
}

/* ========================================================================== */
/*  Allocation Tracking (Hot Path)                                            */
/* ========================================================================== */

int drainprof_alloc_register(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size
) {
    if (!prof) {
        return -1;
    }

    /* Update global counter (always, regardless of mode) */
    atomic_fetch_add(&prof->counters.total_allocs, 1);

    /* Mode-specific tracking */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        /* Production mode: increment live count only */
        (void)alloc_id;  /* Unused in production mode */
        (void)size;      /* Unused in production mode */

        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            return drainprof_slot_array_alloc(prof->storage.slot_array, granule_id);
        }
    } else {
        /* Diagnostic mode: add to per-granule allocation array */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_granule_diag *diag = drainprof_slot_array_diag_get(
                prof->storage.slot_array_diag,
                granule_id
            );
            if (!diag) {
                return -1;
            }

            struct timespec alloc_time;
            clock_gettime(CLOCK_MONOTONIC, &alloc_time);

            return drainprof_granule_diag_add_alloc(
                diag,
                alloc_id,
                size,
                NULL,  /* file: NULL when called without location */
                0,     /* line: 0 when called without location */
                &alloc_time
            );
        }
    }

    return -1;
}

int drainprof_alloc_register_located(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id,
    size_t size,
    const char *file,
    uint32_t line
) {
    if (!prof) {
        return -1;
    }

    /* Update global counter (always, regardless of mode) */
    atomic_fetch_add(&prof->counters.total_allocs, 1);

    /* Mode-specific tracking */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        /* Production mode: location ignored */
        (void)alloc_id;
        (void)size;
        (void)file;
        (void)line;

        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            return drainprof_slot_array_alloc(prof->storage.slot_array, granule_id);
        }
    } else {
        /* Diagnostic mode: record with source location */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_granule_diag *diag = drainprof_slot_array_diag_get(
                prof->storage.slot_array_diag,
                granule_id
            );
            if (!diag) {
                return -1;
            }

            struct timespec alloc_time;
            clock_gettime(CLOCK_MONOTONIC, &alloc_time);

            return drainprof_granule_diag_add_alloc(
                diag,
                alloc_id,
                size,
                file,
                line,
                &alloc_time
            );
        }
    }

    return -1;
}

int drainprof_alloc_deregister(
    drainprof *prof,
    drainprof_granule_id granule_id,
    drainprof_alloc_id alloc_id
) {
    if (!prof) {
        return -1;
    }

    /* Update global counter (always, regardless of mode) */
    atomic_fetch_add(&prof->counters.total_deallocs, 1);

    /* Mode-specific tracking */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        /* Production mode: decrement live count only */
        (void)alloc_id;  /* Unused in production mode */

        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            return drainprof_slot_array_dealloc(prof->storage.slot_array, granule_id);
        }
    } else {
        /* Diagnostic mode: mark allocation as freed in records */
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_granule_diag *diag = drainprof_slot_array_diag_get(
                prof->storage.slot_array_diag,
                granule_id
            );
            if (!diag) {
                return -1;
            }

            struct timespec free_time;
            clock_gettime(CLOCK_MONOTONIC, &free_time);

            return drainprof_granule_diag_mark_freed(diag, alloc_id, &free_time);
        }
    }

    return -1;
}

/* ========================================================================== */
/*  Queries                                                                   */
/* ========================================================================== */

void drainprof_snapshot(const drainprof *prof, drainprof_snapshot_t *out) {
    if (!prof || !out) {
        return;
    }

    /* Capture timestamp */
    clock_gettime(CLOCK_MONOTONIC, &out->timestamp);

    /* Read all counters (lock-free atomic loads) */
    out->total_closes = atomic_load(&prof->counters.total_closes);
    out->drainable_closes = atomic_load(&prof->counters.drainable_closes);
    out->pinned_closes = atomic_load(&prof->counters.pinned_closes);
    out->open_granules = atomic_load(&prof->counters.open_granules);
    out->peak_open_granules = atomic_load(&prof->counters.peak_open_granules);
    out->total_allocs = atomic_load(&prof->counters.total_allocs);
    out->total_deallocs = atomic_load(&prof->counters.total_deallocs);

    /* Calculate DSR */
    out->dsr = out->total_closes > 0
        ? (double)out->drainable_closes / (double)out->total_closes
        : 0.0;
}

void drainprof_reset(drainprof *prof) {
    if (!prof) {
        return;
    }

    drainprof_counters_reset(&prof->counters);

    /* Reset storage backend */
    if (prof->config.mode == DRAINPROF_PRODUCTION) {
        if (prof->config.storage == DRAINPROF_SLOT_ARRAY) {
            drainprof_slot_array *arr = prof->storage.slot_array;
            for (uint32_t i = 0; i < arr->capacity; i++) {
                atomic_store(&arr->slots[i].live_count, 0);
                atomic_store(&arr->slots[i].occupied, 0);
                atomic_store(&arr->slots[i].granule_id, 0);
            }
        }
    }
    /* Note: Diagnostic mode reset would need to clear all open granule states.
     * Not implemented in M2 - reset is primarily for testing. */
}

/* ========================================================================== */
/*  Diagnostic Mode Queries                                                   */
/* ========================================================================== */

uint32_t drainprof_drain_reports(
    drainprof *prof,
    drainprof_pinning_report **reports,
    uint32_t max_reports
) {
    if (!prof || !reports || max_reports == 0) {
        return 0;
    }

    if (prof->config.mode != DRAINPROF_DIAGNOSTIC || !prof->report_buffer) {
        return 0;
    }

    return drainprof_report_buffer_drain(prof->report_buffer, reports, max_reports);
}
