/* Headless simulation smoke timing: builds the example scenario at the given
 * populations and reports CPU time per tick (clock()), extraction time and
 * the final state hash. Functional evidence only: a shared machine, CPU clock
 * resolution and no warm-cache control make this unsuitable as a performance
 * acceptance measurement. Usage: fbs_crowd_bench [ticks] [count ...] */
#include "fbs/crowd.h"

#define CROWD_SCENARIO_IMPLEMENTATION
#include "../examples/crowd_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double ms_since(clock_t start) {
    return (double)(clock() - start) * 1000.0 / (double)CLOCKS_PER_SEC;
}

int main(int argc, char **argv) {
    static const uint32_t defaults[] = {1000u, 20000u};
    uint32_t ticks = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 300u;
    int count_n = argc > 2 ? argc - 2 : 2, c;
    for (c = 0; c < count_n; ++c) {
        uint32_t workers = argc > 2 ? (uint32_t)strtoul(argv[c + 2], NULL, 10) : defaults[c];
        crowd_scenario s;
        crowd_scenario_params p;
        fbs_crowd_instance *inst = (fbs_crowd_instance *)malloc(sizeof *inst * (workers ? workers : 1u));
        fbs_crowd_stats st;
        uint32_t n = 0, total = 0, failures = 0;
        clock_t t0;
        double build_ms, step_ms, reassign_ms, extract_ms;
        crowd_scenario_defaults(&p, workers, 3);
        t0 = clock();
        if (crowd_scenario_build(&s, &p) != 0) {
            fprintf(stderr, "build failed for %u\n", workers);
            return 1;
        }
        build_ms = ms_since(t0);
        fbs_crowd_step(s.crowd, 60);
        t0 = clock();
        fbs_crowd_step(s.crowd, ticks);
        step_ms = ms_since(t0);
        t0 = clock();
        (void)crowd_scenario_mass_reassign(&s, &failures);
        fbs_crowd_step(s.crowd, 30);
        reassign_ms = ms_since(t0);
        t0 = clock();
        fbs_crowd_extract(s.crowd, 0.5f, inst, workers, &n, &total);
        extract_ms = ms_since(t0);
        fbs_crowd_get_stats(s.crowd, &st);
        printf("BENCH workers=%u chains=%u rooms=%u build_ms=%.1f ticks=%u step_ms_per_tick=%.3f "
               "reassign_plus_30_ticks_ms=%.1f extract_ms=%.3f tables=%u travelling=%u queued=%u "
               "slotted=%u hauling=%u failures=%u hash=%016llx\n",
               workers, st.chains, s.room_count, build_ms, ticks, step_ms / (double)ticks, reassign_ms,
               extract_ms, st.ecs_tables, st.travelling, st.queued, st.slotted, st.hauling, failures,
               (unsigned long long)fbs_crowd_state_hash(s.crowd));
        crowd_scenario_free(&s);
        free(inst);
    }
    return 0;
}
