/* Records Flecs allocation and table behaviour of fbs::crowd through a
 * counting Flecs OS API (installed before the first world is created).
 *
 * Asserted: steady stepping, presentation extraction, hashing and stats make
 * no Flecs allocation; mass reassignment creates no tables and no Flecs
 * allocation; the table count stays constant through create/remove/reset
 * cycles. Allocation counts of structural phases are printed (ALLOC lines),
 * not asserted, because Flecs' internal growth policy is upstream's. */
#include "fbs/crowd.h"

#include "flecs_no_addons.h"

#define CROWD_SCENARIO_IMPLEMENTATION
#include "../examples/crowd_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!(cond)) {                                                                \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

static long long g_mallocs, g_callocs, g_reallocs, g_frees;
static ecs_os_api_malloc_t g_malloc;
static ecs_os_api_calloc_t g_calloc;
static ecs_os_api_realloc_t g_realloc;
static ecs_os_api_free_t g_free;

static void *count_malloc(ecs_size_t size) { g_mallocs++; return g_malloc(size); }
static void *count_calloc(ecs_size_t size) { g_callocs++; return g_calloc(size); }
static void *count_realloc(void *p, ecs_size_t size) { g_reallocs++; return g_realloc(p, size); }
static void count_free(void *p) { if (p) g_frees++; g_free(p); }
static long long allocations(void) { return g_mallocs + g_callocs + g_reallocs; }

static void report(const char *phase, long long before, const fbs_crowd *ctx) {
    fbs_crowd_stats st;
    fbs_crowd_get_stats(ctx, &st);
    printf("ALLOC %-28s flecs_allocs=%lld tables=%u tables_created=%llu workers=%u\n", phase,
           allocations() - before, st.ecs_tables, (unsigned long long)st.ecs_tables_created,
           st.workers);
}

int main(int argc, char **argv) {
    crowd_scenario s;
    crowd_scenario_params p;
    fbs_crowd_stats st0, st1;
    fbs_crowd_instance *inst;
    uint32_t workers = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 20000u;
    uint32_t n = 0, total = 0, failures = 0, i, cycle;
    long long mark;
    uint64_t tables_created;

    ecs_os_set_api_defaults();
    {
        ecs_os_api_t api = ecs_os_api;
        g_malloc = api.malloc_;
        g_calloc = api.calloc_;
        g_realloc = api.realloc_;
        g_free = api.free_;
        api.malloc_ = count_malloc;
        api.calloc_ = count_calloc;
        api.realloc_ = count_realloc;
        api.free_ = count_free;
        ecs_os_set_api(&api);
    }

    mark = allocations();
    crowd_scenario_defaults(&p, workers, 11);
    CHECK(crowd_scenario_build(&s, &p) == 0);
    report("build", mark, s.crowd);

    mark = allocations();
    CHECK(fbs_crowd_step(s.crowd, 60) == FBS_CROWD_OK); /* warm-up */
    report("warmup_60_ticks", mark, s.crowd);

    inst = (fbs_crowd_instance *)malloc(sizeof *inst * workers);
    fbs_crowd_get_stats(s.crowd, &st0);
    mark = allocations();
    for (i = 0; i < 300; ++i) {
        CHECK(fbs_crowd_step(s.crowd, 1) == FBS_CROWD_OK);
        CHECK(fbs_crowd_extract(s.crowd, 0.5f, inst, workers, &n, &total) == FBS_CROWD_OK);
        if (i % 50 == 0) (void)fbs_crowd_state_hash(s.crowd);
    }
    report("steady_300_ticks_extract", mark, s.crowd);
    CHECK(allocations() - mark == 0);

    mark = allocations();
    (void)crowd_scenario_mass_reassign(&s, &failures);
    CHECK(failures == 0);
    CHECK(fbs_crowd_step(s.crowd, 600) == FBS_CROWD_OK);
    (void)crowd_scenario_mass_reassign(&s, &failures);
    CHECK(failures == 0);
    CHECK(fbs_crowd_step(s.crowd, 600) == FBS_CROWD_OK);
    report("mass_reassign_x2_1200_ticks", mark, s.crowd);
    CHECK(allocations() - mark == 0);
    fbs_crowd_get_stats(s.crowd, &st1);
    CHECK(st1.ecs_tables == st0.ecs_tables);
    CHECK(st1.ecs_tables_created == st0.ecs_tables_created);
    tables_created = st1.ecs_tables_created;

    /* Remove and re-add a tenth of the workers, three times. */
    for (cycle = 0; cycle < 3; ++cycle) {
        uint32_t k, tenth = workers / 10u;
        mark = allocations();
        for (k = 0; k < tenth; ++k) {
            uint32_t idx = s.slot_workers + k; /* haulers */
            if (idx >= workers) break;
            CHECK(fbs_crowd_remove_worker(s.crowd, s.workers[idx], NULL, NULL) == FBS_CROWD_OK);
        }
        for (k = 0; k < tenth; ++k) {
            fbs_crowd_worker_desc w;
            uint32_t idx = s.slot_workers + k;
            if (idx >= workers) break;
            memset(&w, 0, sizeof w);
            w.node = k % s.room_count;
            w.speed_mm_per_tick = 40;
            CHECK(fbs_crowd_add_worker(s.crowd, &w, &s.workers[idx]) == FBS_CROWD_OK);
            CHECK(fbs_crowd_command_haul(s.crowd, s.workers[idx], s.stations[(k % s.room_count) * 4u + 2u],
                                         s.stations[((k + 1u) % s.room_count) * 4u + 3u], 1) ==
                  FBS_CROWD_OK);
        }
        CHECK(fbs_crowd_step(s.crowd, 30) == FBS_CROWD_OK);
        report(cycle == 0 ? "remove_add_10pct_cycle0" : cycle == 1 ? "remove_add_10pct_cycle1"
                                                                    : "remove_add_10pct_cycle2",
               mark, s.crowd);
        fbs_crowd_get_stats(s.crowd, &st1);
        CHECK(st1.ecs_tables == st0.ecs_tables);
        CHECK(st1.ecs_tables_created == tables_created);
    }

    /* Reset and rebuild the same population in the same context, twice. */
    for (cycle = 0; cycle < 2; ++cycle) {
        uint32_t k;
        mark = allocations();
        CHECK(fbs_crowd_reset(s.crowd) == FBS_CROWD_OK);
        for (k = 0; k < s.room_count; ++k) {
            fbs_crowd_station_desc d;
            memset(&d, 0, sizeof d);
            d.node = k;
            d.input_resource = FBS_CROWD_NONE;
            d.output_resource = 1;
            d.output_per_cycle = 1;
            d.output_capacity = 60;
            d.cycle_ticks = 3;
            CHECK(fbs_crowd_add_station(s.crowd, &d, &s.stations[k * 4u + 2u]) == FBS_CROWD_OK);
            memset(&d, 0, sizeof d);
            d.node = k;
            d.input_resource = 1;
            d.input_per_cycle = 1;
            d.input_capacity = 60;
            d.output_resource = FBS_CROWD_NONE;
            d.cycle_ticks = 2;
            CHECK(fbs_crowd_add_station(s.crowd, &d, &s.stations[k * 4u + 3u]) == FBS_CROWD_OK);
        }
        for (k = 0; k < workers; ++k) {
            fbs_crowd_worker_desc w;
            memset(&w, 0, sizeof w);
            w.node = k % s.room_count;
            w.speed_mm_per_tick = 40;
            CHECK(fbs_crowd_add_worker(s.crowd, &w, &s.workers[k]) == FBS_CROWD_OK);
            CHECK(fbs_crowd_command_haul(s.crowd, s.workers[k], s.stations[(k % s.room_count) * 4u + 2u],
                                         s.stations[((k + 1u) % s.room_count) * 4u + 3u], 1) ==
                  FBS_CROWD_OK);
        }
        CHECK(fbs_crowd_step(s.crowd, 30) == FBS_CROWD_OK);
        report(cycle ? "reset_rebuild_cycle1" : "reset_rebuild_cycle0", mark, s.crowd);
        fbs_crowd_get_stats(s.crowd, &st1);
        CHECK(st1.ecs_tables == st0.ecs_tables);
    }

    mark = allocations();
    crowd_scenario_free(&s);
    printf("ALLOC destroy flecs_allocs=%lld\n", allocations() - mark);
    printf("FLECS mallocs=%lld callocs=%lld reallocs=%lld frees=%lld\n", g_mallocs, g_callocs,
           g_reallocs, g_frees);
    free(inst);
    printf("crowd flecs alloc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
