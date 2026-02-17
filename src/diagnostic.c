/**
 * diagnostic.c - Diagnostic mode implementation
 *
 * Provides per-allocation tracking, pinning reports, and detailed leak analysis.
 * ~50ns overhead per allocation (vs < 2ns production mode).
 */

#include "drainprof_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/*  Report Buffer Implementation                                              */
/* ========================================================================== */

drainprof_report_buffer *drainprof_report_buffer_create(uint32_t capacity) {
    drainprof_report_buffer *buf = malloc(sizeof(*buf));
    if (!buf) {
        return NULL;
    }

    buf->reports = calloc(capacity, sizeof(drainprof_pinning_report *));
    if (!buf->reports) {
        free(buf);
        return NULL;
    }

    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
    buf->tail = 0;

    if (pthread_mutex_init(&buf->lock, NULL) != 0) {
        free(buf->reports);
        free(buf);
        return NULL;
    }

    return buf;
}

void drainprof_report_buffer_destroy(drainprof_report_buffer *buf) {
    if (!buf) {
        return;
    }

    /* Free any remaining buffered reports */
    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (buf->tail + i) % buf->capacity;
        if (buf->reports[idx]) {
            drainprof_pinning_report_free(buf->reports[idx]);
        }
    }

    pthread_mutex_destroy(&buf->lock);
    free(buf->reports);
    free(buf);
}

void drainprof_report_buffer_push(
    drainprof_report_buffer *buf,
    drainprof_pinning_report *report
) {
    if (!buf || !report) {
        return;
    }

    pthread_mutex_lock(&buf->lock);

    /* If buffer is full, drop the oldest report */
    if (buf->count == buf->capacity) {
        drainprof_pinning_report_free(buf->reports[buf->tail]);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
    }

    /* Add new report */
    buf->reports[buf->head] = report;
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count++;

    pthread_mutex_unlock(&buf->lock);
}

uint32_t drainprof_report_buffer_drain(
    drainprof_report_buffer *buf,
    drainprof_pinning_report **out_reports,
    uint32_t max_reports
) {
    if (!buf || !out_reports || max_reports == 0) {
        return 0;
    }

    pthread_mutex_lock(&buf->lock);

    uint32_t to_drain = buf->count < max_reports ? buf->count : max_reports;

    for (uint32_t i = 0; i < to_drain; i++) {
        out_reports[i] = buf->reports[buf->tail];
        buf->reports[buf->tail] = NULL;
        buf->tail = (buf->tail + 1) % buf->capacity;
    }

    buf->count -= to_drain;

    pthread_mutex_unlock(&buf->lock);

    return to_drain;
}

/* ========================================================================== */
/*  Granule Diagnostic State Implementation                                   */
/* ========================================================================== */

drainprof_granule_diag *drainprof_granule_diag_create(
    drainprof_granule_id id,
    struct timespec *open_time
) {
    drainprof_granule_diag *diag = malloc(sizeof(*diag));
    if (!diag) {
        return NULL;
    }

    diag->granule_id = id;
    atomic_init(&diag->live_count, 0);
    diag->open_time = *open_time;

    /* Pre-allocate reasonable capacity (64 records) to avoid frequent reallocs */
    diag->alloc_capacity = 64;
    diag->alloc_count = 0;
    diag->last_freed_index = 0;  /* Hint for deregister scan optimization */
    diag->allocs = calloc(diag->alloc_capacity, sizeof(drainprof_alloc_record));
    if (!diag->allocs) {
        free(diag);
        return NULL;
    }

    if (pthread_mutex_init(&diag->alloc_lock, NULL) != 0) {
        free(diag->allocs);
        free(diag);
        return NULL;
    }

    return diag;
}

void drainprof_granule_diag_destroy(drainprof_granule_diag *diag) {
    if (!diag) {
        return;
    }

    pthread_mutex_destroy(&diag->alloc_lock);
    free(diag->allocs);
    free(diag);
}

int drainprof_granule_diag_add_alloc(
    drainprof_granule_diag *diag,
    drainprof_alloc_id alloc_id,
    size_t size,
    const char *file,
    uint32_t line,
    struct timespec *alloc_time
) {
    if (!diag) {
        return -1;
    }

    pthread_mutex_lock(&diag->alloc_lock);

    /* Grow array if needed */
    if (diag->alloc_count >= diag->alloc_capacity) {
        uint32_t new_capacity = diag->alloc_capacity * 2;
        drainprof_alloc_record *new_allocs = realloc(
            diag->allocs,
            new_capacity * sizeof(drainprof_alloc_record)
        );
        if (!new_allocs) {
            pthread_mutex_unlock(&diag->alloc_lock);
            return -1;
        }
        diag->allocs = new_allocs;
        diag->alloc_capacity = new_capacity;

        /* Zero out new slots */
        memset(
            &diag->allocs[diag->alloc_count],
            0,
            (new_capacity - diag->alloc_count) * sizeof(drainprof_alloc_record)
        );
    }

    /* Add record */
    drainprof_alloc_record *rec = &diag->allocs[diag->alloc_count++];
    rec->alloc_id = alloc_id;
    rec->granule_id = diag->granule_id;
    rec->alloc_time = *alloc_time;
    rec->alloc_site.file = file;
    rec->alloc_site.line = line;
    rec->alloc_site.column = 0;
    rec->size = size;
    memset(&rec->free_time, 0, sizeof(rec->free_time));
    rec->freed = false;

    atomic_fetch_add(&diag->live_count, 1);

    pthread_mutex_unlock(&diag->alloc_lock);

    return 0;
}

int drainprof_granule_diag_mark_freed(
    drainprof_granule_diag *diag,
    drainprof_alloc_id alloc_id,
    struct timespec *free_time
) {
    if (!diag) {
        return -1;
    }

    pthread_mutex_lock(&diag->alloc_lock);

    /* Optimize for sequential frees: scan from last freed index forward.
     * Average case O(1) for epoch-based allocators where frees happen
     * in roughly allocation order. */
    uint32_t start = diag->last_freed_index;
    for (uint32_t offset = 0; offset < diag->alloc_count; offset++) {
        uint32_t i = (start + offset) % diag->alloc_count;
        if (diag->allocs[i].alloc_id == alloc_id && !diag->allocs[i].freed) {
            diag->allocs[i].freed = true;
            diag->allocs[i].free_time = *free_time;
            diag->last_freed_index = (i + 1) % diag->alloc_count;
            atomic_fetch_sub(&diag->live_count, 1);
            pthread_mutex_unlock(&diag->alloc_lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&diag->alloc_lock);
    return -1;  /* Not found */
}

drainprof_pinning_report *drainprof_granule_diag_generate_report(
    drainprof_granule_diag *diag,
    struct timespec *close_time
) {
    if (!diag) {
        return NULL;
    }

    pthread_mutex_lock(&diag->alloc_lock);

    /* Count live allocations */
    uint32_t live_count = atomic_load(&diag->live_count);
    if (live_count == 0) {
        pthread_mutex_unlock(&diag->alloc_lock);
        return NULL;  /* Not pinned, no report needed */
    }

    /* Allocate report */
    drainprof_pinning_report *report = malloc(sizeof(*report));
    if (!report) {
        pthread_mutex_unlock(&diag->alloc_lock);
        return NULL;
    }

    report->granule_id = diag->granule_id;
    report->open_time = diag->open_time;
    report->close_time = *close_time;
    report->total_allocs = diag->alloc_count;
    report->drained_allocs = diag->alloc_count - live_count;
    report->pinning_count = live_count;

    /* Allocate array for pinning allocations */
    report->pinning_allocs = malloc(live_count * sizeof(drainprof_pinning_alloc));
    if (!report->pinning_allocs) {
        free(report);
        pthread_mutex_unlock(&diag->alloc_lock);
        return NULL;
    }

    /* Copy live allocations to report */
    uint32_t pinning_idx = 0;
    for (uint32_t i = 0; i < diag->alloc_count; i++) {
        if (!diag->allocs[i].freed) {
            drainprof_pinning_alloc *pa = &report->pinning_allocs[pinning_idx++];
            pa->alloc_id = diag->allocs[i].alloc_id;
            pa->alloc_site = diag->allocs[i].alloc_site;
            pa->alloc_time = diag->allocs[i].alloc_time;
            pa->size = diag->allocs[i].size;
            pa->lifetime_delta_sec = -1.0;  /* Still live */
        }
    }

    pthread_mutex_unlock(&diag->alloc_lock);

    return report;
}

/* ========================================================================== */
/*  Diagnostic Slot Array Implementation                                      */
/* ========================================================================== */

drainprof_slot_array_diag *drainprof_slot_array_diag_create(uint32_t capacity) {
    drainprof_slot_array_diag *arr = malloc(sizeof(*arr));
    if (!arr) {
        return NULL;
    }

    arr->slots = calloc(capacity, sizeof(drainprof_diag_slot));
    if (!arr->slots) {
        free(arr);
        return NULL;
    }

    arr->capacity = capacity;

    /* Initialize atomic flags */
    for (uint32_t i = 0; i < capacity; i++) {
        atomic_init(&arr->slots[i].occupied, 0);
        arr->slots[i].diag = NULL;
    }

    return arr;
}

void drainprof_slot_array_diag_destroy(drainprof_slot_array_diag *arr) {
    if (!arr) {
        return;
    }

    /* Free any remaining open granules */
    for (uint32_t i = 0; i < arr->capacity; i++) {
        if (arr->slots[i].diag) {
            drainprof_granule_diag_destroy(arr->slots[i].diag);
        }
    }

    free(arr->slots);
    free(arr);
}

int drainprof_slot_array_diag_open(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id,
    struct timespec *open_time
) {
    if (!arr) {
        return -1;
    }

    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_diag_slot *slot = &arr->slots[slot_idx];

    /* Try to claim the slot with CAS */
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong(&slot->occupied, &expected, 1)) {
        return -1;  /* Slot collision */
    }

    /* Create diagnostic state */
    slot->diag = drainprof_granule_diag_create(id, open_time);
    if (!slot->diag) {
        atomic_store(&slot->occupied, 0);
        return -1;
    }

    return 0;
}

drainprof_granule_diag *drainprof_slot_array_diag_get(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id
) {
    if (!arr) {
        return NULL;
    }

    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_diag_slot *slot = &arr->slots[slot_idx];

    if (!atomic_load(&slot->occupied)) {
        return NULL;
    }

    if (slot->diag && slot->diag->granule_id == id) {
        return slot->diag;
    }

    return NULL;
}

int drainprof_slot_array_diag_close(
    drainprof_slot_array_diag *arr,
    drainprof_granule_id id,
    drainprof_granule_diag **out_diag
) {
    if (!arr || !out_diag) {
        return -1;
    }

    uint32_t slot_idx = (uint32_t)(id % arr->capacity);
    drainprof_diag_slot *slot = &arr->slots[slot_idx];

    if (!atomic_load(&slot->occupied)) {
        return -1;
    }

    if (!slot->diag || slot->diag->granule_id != id) {
        return -1;
    }

    /* Return diagnostic state to caller.
     *
     * CORRECTNESS NOTE: Safe to extract diag struct because the API contract
     * requires no concurrent alloc_register/deregister calls for this granule_id
     * are in flight when close is called. Without this precondition, a concurrent
     * alloc_register could be appending to diag->allocs while we extract it. */
    *out_diag = slot->diag;
    slot->diag = NULL;

    /* Release slot */
    atomic_store(&slot->occupied, 0);

    return 0;
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

void drainprof_pinning_report_free(drainprof_pinning_report *report) {
    if (!report) {
        return;
    }

    free(report->pinning_allocs);
    free(report);
}

/* ========================================================================== */
/*  Diagnostic Summary (Aggregation)                                          */
/* ========================================================================== */

/* Helper: Find or create site entry in summary array */
static drainprof_summary_site_entry *find_or_create_site(
    drainprof_summary_site_entry **sites,
    uint32_t *count,
    uint32_t *capacity,
    const drainprof_alloc_site *site
) {
    /* Search for existing site (linear search is fine - typically < 100 sites) */
    for (uint32_t i = 0; i < *count; i++) {
        drainprof_summary_site_entry *entry = &(*sites)[i];
        /* Pointer comparison for file is OK - compiler interns string literals */
        if (entry->site.file == site->file && entry->site.line == site->line) {
            return entry;
        }
    }

    /* Need to add new site - grow array if necessary */
    if (*count >= *capacity) {
        uint32_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        drainprof_summary_site_entry *new_sites = realloc(*sites,
            new_cap * sizeof(drainprof_summary_site_entry));
        if (!new_sites) {
            return NULL;
        }
        *sites = new_sites;
        *capacity = new_cap;
    }

    /* Initialize new entry */
    drainprof_summary_site_entry *entry = &(*sites)[*count];
    entry->site = *site;
    entry->pinning_count = 0;
    entry->total_allocs = 0;
    entry->total_bytes = 0;
    (*count)++;

    return entry;
}

drainprof_diagnostic_summary *drainprof_diagnostic_summary_compute(drainprof *prof) {
    if (!prof || prof->config.mode != DRAINPROF_DIAGNOSTIC) {
        return NULL;
    }

    drainprof_report_buffer *buf = prof->report_buffer;
    if (!buf) {
        return NULL;
    }

    /* Allocate summary structure */
    drainprof_diagnostic_summary *summary = calloc(1, sizeof(*summary));
    if (!summary) {
        return NULL;
    }

    drainprof_summary_site_entry *sites = NULL;
    uint32_t site_count = 0;
    uint32_t site_capacity = 0;
    uint32_t total_pinning_allocs = 0;
    uint32_t reports_analyzed = 0;

    /* Lock buffer for reading (prevents concurrent report buffer modifications) */
    pthread_mutex_lock(&buf->lock);

    /* Tracking array to detect if we've seen a site in the current granule.
     * We use UINT64_MAX as sentinel to indicate "not yet seen in any granule". */
    drainprof_granule_id *last_granule_per_site = calloc(site_capacity > 0 ? site_capacity : 16,
                                                          sizeof(drainprof_granule_id));
    if (!last_granule_per_site) {
        free(summary);
        pthread_mutex_unlock(&buf->lock);
        return NULL;
    }
    for (uint32_t i = 0; i < (site_capacity > 0 ? site_capacity : 16); i++) {
        last_granule_per_site[i] = UINT64_MAX;
    }
    uint32_t tracking_capacity = site_capacity > 0 ? site_capacity : 16;

    /* Iterate through all buffered reports (circular buffer) */
    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (buf->tail + i) % buf->capacity;
        drainprof_pinning_report *report = buf->reports[idx];

        if (!report) {
            continue;
        }

        reports_analyzed++;
        drainprof_granule_id current_granule = report->granule_id;

        /* Process each pinning allocation in this report */
        for (uint32_t j = 0; j < report->pinning_count; j++) {
            const drainprof_pinning_alloc *pa = &report->pinning_allocs[j];

            /* Find or create site entry for this allocation's source location */
            drainprof_summary_site_entry *entry = find_or_create_site(
                &sites, &site_count, &site_capacity, &pa->alloc_site);

            if (!entry) {
                /* Allocation failure - cleanup and return NULL */
                free(last_granule_per_site);
                free(sites);
                pthread_mutex_unlock(&buf->lock);
                free(summary);
                return NULL;
            }

            /* If sites array grew, we need to resize tracking array */
            if (site_capacity > tracking_capacity) {
                drainprof_granule_id *new_tracking = realloc(last_granule_per_site,
                                                              site_capacity * sizeof(drainprof_granule_id));
                if (!new_tracking) {
                    free(last_granule_per_site);
                    free(sites);
                    pthread_mutex_unlock(&buf->lock);
                    free(summary);
                    return NULL;
                }
                /* Initialize new entries to UINT64_MAX */
                for (uint32_t k = tracking_capacity; k < site_capacity; k++) {
                    new_tracking[k] = UINT64_MAX;
                }
                last_granule_per_site = new_tracking;
                tracking_capacity = site_capacity;
            }

            /* Calculate site index (entry is pointer into sites array) */
            uint32_t site_idx = (uint32_t)(entry - sites);

            /* Accumulate allocation count and bytes (always) */
            entry->total_allocs++;
            entry->total_bytes += pa->size;
            total_pinning_allocs++;

            /* Only increment pinning_count once per unique granule */
            if (last_granule_per_site[site_idx] != current_granule) {
                entry->pinning_count++;
                last_granule_per_site[site_idx] = current_granule;
            }
        }
    }

    free(last_granule_per_site);

    pthread_mutex_unlock(&buf->lock);

    /* Populate summary with aggregated results */
    summary->sites = sites;
    summary->site_count = site_count;
    summary->total_pinning_allocs = total_pinning_allocs;
    summary->reports_analyzed = reports_analyzed;

    return summary;
}

void drainprof_diagnostic_summary_free(drainprof_diagnostic_summary *summary) {
    if (!summary) {
        return;
    }

    free(summary->sites);
    free(summary);
}
