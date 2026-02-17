# libdrainprof: Integration & Ecosystem Roadmap

**Author:** Dayna Blackwell
**Status:** Active
**Started:** 2026-02-16
**Depends on:** M1 ✅, M2 ✅ complete

---

## Phase 1: Instrument tslab (Own Allocator)

**Goal:** First real-world integration. Validate the profiler against the allocator that produced the paper's results. Reproduce the p-sweep DSR values through instrumentation rather than synthetic tests.

**Duration:** 1-2 days

### 1.1 Add instrumentation calls to tslab

Four insertion points in the existing allocator code:

1. `epoch_open()` → add `drainprof_granule_open(prof, epoch_id)`
2. `tslab_alloc()` → add `DRAINPROF_ALLOC_REGISTER(prof, epoch_id, (uintptr_t)ptr, size)`
3. `tslab_free()` → add `drainprof_alloc_deregister(prof, epoch_id, (uintptr_t)ptr)`
4. `epoch_close()` → add `drainprof_granule_close(prof, epoch_id)`

Guard all calls behind a `if (g_profiler)` null check so the profiler is opt-in.

### 1.2 Reproduce the p-sweep through the profiler

Run the paper's mixed-routing workload at p=0, 0.01, 0.05, 0.10, 0.25, 0.50, 1.0. For each run, record the DSR from `drainprof_snapshot`. Verify:

- p=0.0 → DSR = 1.0
- p=0.10 → DSR ≈ 0.90
- p=1.0 → DSR = 0.0
- DSR ≈ 1.0 - p across all values

This is the validation that the profiler correctly measures what the paper proves.

### 1.3 Run diagnostic mode on the p=0.25 workload

Enable diagnostic mode. Run the mixed-routing workload. Collect the diagnostic summary. Verify that:

- The summary identifies the correct allocation site (the line where session objects are routed into request epochs)
- The pinning count matches the expected violation fraction
- The granule-pinned-per-site count is proportional to p

### 1.4 Record before/after

Run the mixed workload with violations (p=0.25). Record DSR, RSS over time, and the diagnostic summary output. Then switch to isolated routing (p=0). Record the same metrics. Save both as screenshots/text for the blog post.

### 1.5 Deliverables

- Instrumented tslab with profiler integration (committed to tslab repo or examples/ in profiler repo)
- `tslab_psweep_validation.c` test that reproduces the p-sweep through the profiler
- Before/after output samples saved for blog post

---

## Phase 2: Blog Post — "Catching a Structural Leak with libdrainprof"

**Goal:** Show the profiler finding a real bug in a real allocator. This is the post that gets people to clone the repo.

**Duration:** 1 day

### 2.1 Structure

1. **Setup.** "I built an epoch-based allocator. It leaks memory. Valgrind says it's clean."
2. **Production mode.** Add the four profiler calls. Run the workload. DSR drops to 0.75. "Now I know I have a structural leak."
3. **Diagnostic mode.** Switch to diagnostic. Run again. Summary output shows `session.c:84` pinning 75% of epochs. "Now I know where it is."
4. **The fix.** Route session objects to a separate arena. DSR returns to 1.0. RSS stabilizes.
5. **The theory.** One paragraph linking to the paper. "The math proves this is not a heuristic. DSR < 1.0 guarantees unbounded growth."

### 2.2 Distribution

- Publish on blog
- Share on LinkedIn as follow-up to paper post ("Built the profiler. Here's it catching a real leak.")
- Submit to Hacker News, lobste.rs
- Post in r/systems, r/rust, r/programming
- Share in any Crossbeam/RCU discussion forums

---

## Phase 3: Find a Real Bug in Open Source

**Goal:** Diagnose a structural leak in a project people actually use. This is the result with the most reach.

**Duration:** 1-2 weeks (research + instrumentation + write-up)

### 3.1 Target selection criteria

The target must:

- Use explicit coarse-grained allocation (arenas, pools, epochs, regions)
- Have granule boundaries visible in source code (not hidden inside a general-purpose allocator)
- Be written in C or C++ (or Rust, after M3)
- Ideally have open issues mentioning unexplained memory growth

### 3.2 Candidate targets (prioritized)

**Tier 1: Explicit arena/pool allocators with known memory issues**

| Project | Granule Type | Instrumentation Point | Why |
|---|---|---|---|
| **Nginx** | `ngx_pool_t` per-request memory pools | `ngx_create_pool` / `ngx_destroy_pool` / `ngx_palloc` | Nginx pools are textbook arenas. Long-lived connections can pin request pools. Well-documented memory growth issues in high-connection scenarios. |
| **Apache httpd** | `apr_pool_t` (APR pools) | `apr_pool_create` / `apr_pool_destroy` / `apr_palloc` | APR pools are hierarchical arenas. Child pools can pin parent pools. Large codebase with decades of pool lifetime bugs. |
| **PostgreSQL** | `MemoryContext` (memory contexts) | `AllocSetContextCreate` / `MemoryContextDelete` / `palloc` | PostgreSQL memory contexts are arenas with explicit lifetimes. Known issues with long-running queries pinning contexts. |

**Tier 2: Epoch/generation-based systems**

| Project | Granule Type | Instrumentation Point | Why |
|---|---|---|---|
| **Redis** | `zmalloc` with jemalloc arenas | Requires jemalloc arena tracking | Redis has well-known memory fragmentation issues that may include structural leaks. Harder to instrument because granule boundaries are inside jemalloc. |
| **Crossbeam (Rust)** | Epoch-based reclamation | `pin()` / `defer_destroy()` / epoch advance | Directly relevant to paper's Section 8. Requires M3 (Rust bindings). The Rust community knows epoch pinning is a problem. |

**Tier 3: Game engines and embedded systems**

| Project | Granule Type | Notes |
|---|---|---|
| **Godot** | Custom arena allocators | Smaller community but explicit arena usage |
| **SQLite** | Lookaside allocators | Very well-studied codebase, less likely to have undiscovered bugs |

### 3.3 Recommended first target: Nginx

Reasons:

- `ngx_pool_t` is a clean, explicit arena with create/destroy lifecycle
- The pool API is small and well-documented
- Memory growth under high connection counts is a known operational issue
- Instrumentation requires ~20 lines of code in the pool allocator
- Nginx is universally known — a finding here travels far
- C codebase, no binding work needed

### 3.4 Approach for Nginx

1. Fork nginx, add `#include <drainprof.h>` to pool allocator
2. Instrument `ngx_create_pool` → `drainprof_granule_open`
3. Instrument `ngx_palloc` / `ngx_pnalloc` → `drainprof_alloc_register`
4. Instrument `ngx_pfree` → `drainprof_alloc_deregister`
5. Instrument `ngx_destroy_pool` / `ngx_reset_pool` → `drainprof_granule_close`
6. Run under realistic load (wrk or vegeta with mixed request sizes and keep-alive connections)
7. Monitor DSR over time
8. If DSR < 1.0: enable diagnostic mode, identify pinning sites, write up findings
9. If DSR = 1.0: nginx's pool management is drainable for that workload. Report that result too — confirming drainability is also valuable.

### 3.5 Deliverable

Blog post: "Is Nginx Leaking Memory? Measuring Drainability in ngx_pool_t"

If a structural leak is found: file an issue on the nginx tracker with the diagnostic output and a suggested fix. Link the blog post. This is how you enter a project's community with credibility.

If no leak is found: the post becomes "Nginx's memory pools are well-designed. Here's the metric that proves it." Still valuable — it validates the tool and the concept.

---

## Phase 4: Rust Bindings (M3)

**Goal:** Publish `drainability-profiler` crate to crates.io. Enable instrumentation of bumpalo and Crossbeam.

**Duration:** Half day for bindings, half day for bumpalo example

**Trigger:** Do this either when Phase 3 is complete, or when the blog posts generate demand from the Rust community. Whichever comes first.

### 4.1 Tasks

1. Create `drainability-profiler-sys` crate with `build.rs` (cc + bindgen)
2. Create `drainability-profiler` safe wrapper crate
3. Write `ProfiledBump` example (from architecture doc Section 7.2)
4. Publish to crates.io
5. Announce on r/rust with a short post linking the blog and the crate

### 4.2 Crossbeam integration (stretch goal)

Instrument crossbeam-epoch to measure DSR during epoch-based reclamation. This directly validates the paper's Section 8 claim that epoch pinning is a drainability violation. If this shows that crossbeam's default epoch advancement policy produces low DSR under certain workloads, that's a publishable finding on its own.

---

## Phase 5: Prometheus & Grafana Dashboard

**Goal:** Make DSR a first-class production metric alongside CPU, memory, and latency.

**Duration:** 1 day

### 5.1 Tasks

1. Implement `drainprof_prometheus()` (M4 from architecture doc)
2. Create a sample Grafana dashboard JSON showing DSR over time, pin rate, and close rate
3. Write a short blog post: "Adding Drainability Monitoring to Your Service"
4. Include docker-compose example: service + prometheus + grafana with DSR panel

---

## Phase 6: Conference Submission (Optional)

**Goal:** Submit to a systems venue if the work has gained traction and feedback supports it.

**Trigger:** Only if Phases 1-3 produce strong results and feedback from outreach (McKenney, Guerraoui group, community response) suggests the work is at that level.

### 6.1 Candidate venues

| Venue | Type | Fit |
|---|---|---|
| USENIX ATC | Systems conference | Best fit. Practical systems contribution with theory backing. |
| EuroSys | Systems conference | Strong European systems community. Guerraoui's group publishes here. |
| ASPLOS | Architecture + PL + OS | Good if the allocator-agnostic angle is emphasized. |
| PLDI (workshop) | PL tools | If static analysis direction (Phase 7) is developed. |

### 6.2 What would need to change

- Peer review requires related work section comparing to existing memory profiling tools (heaptrack, massif, jemalloc profiling)
- Need at least one real-application case study (Phase 3)
- Reviewer expectations for experimental methodology are higher than a technical report — may need multiple workloads, multiple allocators

---

## Decision Points

After Phase 1: Does the tslab integration work cleanly? If not, fix API ergonomics before external targets.

After Phase 2: Does the blog post get traction? If yes, prioritize Phase 3 (real bug) and Phase 4 (Rust). If no, still do Phase 3 — the real-world result is intrinsically valuable regardless of audience.

After Phase 3: Did you find a structural leak in an open-source project? If yes, the blog post and the bug report together are a stronger artifact than anything else on this list. If no, report the negative result and move to the next target.

After outreach responses: If McKenney or the Guerraoui group engage, ask whether they think the work is suitable for a systems venue. Let their feedback determine Phase 6.

---

## Current Priority Order

1. **Phase 1** — tslab integration (this week)
2. **Phase 2** — blog post with real demo (immediately after Phase 1)
3. **McKenney email** (tomorrow morning)
4. **Phase 3** — nginx instrumentation (next week)
5. **Phase 4** — Rust bindings (when demand appears or Phase 3 is done)
6. **Phase 5** — Prometheus integration (when someone asks for it)
7. **Phase 6** — conference submission (if feedback supports it)