/* fbs::crowd behaviour tests: conservation, backpressure, handles, reset,
 * reassignment, queues, removal/export and determinism. Determinism is checked
 * within this build only. Scenario hashes are printed ("HASH name value") so
 * different builds (for example native and WASM) can be compared by hand;
 * that cross-build comparison is not automated here. */
#include "fbs/crowd.h"

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
#define CHECK_EQ(a, b) CHECK((a) == (b))

static fbs_crowd_config small_config(void) {
    fbs_crowd_config c;
    memset(&c, 0, sizeof c);
    c.max_workers = 64;
    c.max_chains = 8;
    c.max_stations = 8;
    c.max_chain_slots = 64;
    c.max_nodes = 8;
    c.max_links = 8;
    c.resources = 2;
    return c;
}

/* Line graph 0-1-2-3 plus a detached node 4, 10 m apart. */
static void line_graph(fbs_crowd *ctx, uint32_t capacity) {
    fbs_crowd_node_desc n[5];
    fbs_crowd_link_desc l[3];
    uint32_t i;
    memset(n, 0, sizeof n);
    memset(l, 0, sizeof l);
    for (i = 0; i < 5; ++i) {
        n[i].position.x = (int32_t)(i * 10000u);
        n[i].room = i;
    }
    for (i = 0; i < 3; ++i) {
        l[i].a = i;
        l[i].b = i + 1u;
        l[i].capacity = capacity;
    }
    CHECK_EQ(fbs_crowd_set_graph(ctx, n, 5, l, 3), FBS_CROWD_OK);
}

static fbs_crowd_handle add_source(fbs_crowd *ctx, uint32_t node, uint32_t res, uint32_t cycle,
                                   uint32_t cap, uint32_t initial) {
    fbs_crowd_station_desc d;
    fbs_crowd_handle h = 0;
    memset(&d, 0, sizeof d);
    d.node = node;
    d.input_resource = FBS_CROWD_NONE;
    d.output_resource = res;
    d.output_per_cycle = 1;
    d.output_capacity = cap;
    d.cycle_ticks = cycle;
    d.initial_output = initial;
    CHECK_EQ(fbs_crowd_add_station(ctx, &d, &h), FBS_CROWD_OK);
    return h;
}

static fbs_crowd_handle add_sink(fbs_crowd *ctx, uint32_t node, uint32_t res, uint32_t per,
                                 uint32_t cycle, uint32_t cap) {
    fbs_crowd_station_desc d;
    fbs_crowd_handle h = 0;
    memset(&d, 0, sizeof d);
    d.node = node;
    d.input_resource = res;
    d.input_per_cycle = per;
    d.input_capacity = cap;
    d.output_resource = FBS_CROWD_NONE;
    d.cycle_ticks = cycle;
    CHECK_EQ(fbs_crowd_add_station(ctx, &d, &h), FBS_CROWD_OK);
    return h;
}

static fbs_crowd_handle add_chain(fbs_crowd *ctx, fbs_crowd_handle src, fbs_crowd_handle dst,
                                  uint32_t slots, uint32_t handoff, uint32_t access) {
    fbs_crowd_chain_desc c;
    fbs_crowd_handle h = 0;
    memset(&c, 0, sizeof c);
    c.source = src;
    c.destination = dst;
    c.resource = 0;
    c.slot_count = slots;
    c.handoff_ticks = handoff;
    c.access_node = access;
    c.end.x = (int32_t)(slots * 1100u);
    CHECK_EQ(fbs_crowd_add_chain(ctx, &c, &h), FBS_CROWD_OK);
    return h;
}

static fbs_crowd_handle add_worker(fbs_crowd *ctx, uint32_t node, uint32_t speed) {
    fbs_crowd_worker_desc w;
    fbs_crowd_handle h = 0;
    memset(&w, 0, sizeof w);
    w.node = node;
    w.speed_mm_per_tick = speed;
    CHECK_EQ(fbs_crowd_add_worker(ctx, &w, &h), FBS_CROWD_OK);
    return h;
}

static int conserved(const fbs_crowd *ctx, uint32_t resources) {
    uint32_t r;
    for (r = 0; r < resources; ++r) {
        fbs_crowd_totals t;
        if (fbs_crowd_resource_totals(ctx, r, &t) != FBS_CROWD_OK) return 0;
        if (t.introduced != t.station + t.slots + t.cargo + t.consumed + t.exported) {
            fprintf(stderr, "resource %u: introduced %llu station %llu slots %llu cargo %llu consumed %llu exported %llu\n",
                    r, (unsigned long long)t.introduced, (unsigned long long)t.station,
                    (unsigned long long)t.slots, (unsigned long long)t.cargo,
                    (unsigned long long)t.consumed, (unsigned long long)t.exported);
            return 0;
        }
    }
    return 1;
}

static void test_create_validation(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    CHECK_EQ(fbs_crowd_create(NULL, &ctx), FBS_CROWD_INVALID);
    c.max_workers = 0;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_INVALID);
    c = small_config();
    c.resources = FBS_CROWD_MAX_RESOURCES + 1u;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_INVALID);
    c = small_config();
    c.max_nodes = 5000;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_INVALID);
    c = small_config();
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    CHECK(ctx != NULL);
    fbs_crowd_destroy(ctx);
    fbs_crowd_destroy(NULL);
}

static void test_graph(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_node_desc n[4];
    fbs_crowd_link_desc l[4];
    uint64_t len = 0;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 2);
    CHECK_EQ(fbs_crowd_route_length(ctx, 0, 3, &len), FBS_CROWD_OK);
    CHECK_EQ(len, 30000u);
    CHECK_EQ(fbs_crowd_route_length(ctx, 3, 0, &len), FBS_CROWD_OK);
    CHECK_EQ(len, 30000u);
    CHECK_EQ(fbs_crowd_route_length(ctx, 0, 4, &len), FBS_CROWD_UNREACHABLE);
    CHECK_EQ(fbs_crowd_route_length(ctx, 0, 9, &len), FBS_CROWD_INVALID);
    /* Square with equal-length alternatives: tie goes to the lower link. */
    memset(n, 0, sizeof n);
    memset(l, 0, sizeof l);
    n[1].position.x = 1000;
    n[2].position.z = 1000;
    n[3].position.x = 1000;
    n[3].position.z = 1000;
    l[0].a = 0; l[0].b = 1; l[0].capacity = 1;
    l[1].a = 0; l[1].b = 2; l[1].capacity = 1;
    l[2].a = 1; l[2].b = 3; l[2].capacity = 1;
    l[3].a = 2; l[3].b = 3; l[3].capacity = 1;
    CHECK_EQ(fbs_crowd_set_graph(ctx, n, 4, l, 4), FBS_CROWD_OK);
    {
        fbs_crowd_handle w = add_worker(ctx, 0, 100);
        fbs_crowd_worker_info info;
        CHECK_EQ(fbs_crowd_command_move(ctx, w, 3), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_step(ctx, 1), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_worker_get(ctx, w, &info), FBS_CROWD_OK);
        CHECK_EQ(info.link, 0u);
        /* Graph changes are refused while someone travels. */
        CHECK_EQ(fbs_crowd_set_graph(ctx, n, 4, l, 4), FBS_CROWD_STATE);
        CHECK_EQ(fbs_crowd_step(ctx, 40), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_worker_get(ctx, w, &info), FBS_CROWD_OK);
        CHECK_EQ(info.node, 3u);
        CHECK_EQ(info.state, (uint32_t)FBS_CROWD_IDLE);
    }
    /* Invalid links are rejected without change. */
    l[0].b = 0;
    CHECK_EQ(fbs_crowd_set_graph(ctx, n, 4, l, 4), FBS_CROWD_INVALID);
    fbs_crowd_destroy(ctx);
}

/* A fully staffed chain moves items at one per handoff and conserves them. */
static void test_chain_flow(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, chain, w[5];
    fbs_crowd_chain_info info;
    uint32_t i, t;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 4);
    src = add_source(ctx, 0, 0, 1, 50, 10);
    dst = add_sink(ctx, 0, 0, 1, 1, 100);
    chain = add_chain(ctx, src, dst, 5, 4, 0);
    for (i = 0; i < 5; ++i) {
        w[i] = add_worker(ctx, 0, 50);
        CHECK_EQ(fbs_crowd_command_slot(ctx, w[i], chain, i), FBS_CROWD_OK);
    }
    /* Double booking is refused. */
    {
        fbs_crowd_handle extra = add_worker(ctx, 0, 50);
        CHECK_EQ(fbs_crowd_command_slot(ctx, extra, chain, 2), FBS_CROWD_STATE);
        CHECK_EQ(fbs_crowd_remove_worker(ctx, extra, NULL, NULL), FBS_CROWD_OK);
    }
    for (t = 0; t < 400; ++t) {
        CHECK_EQ(fbs_crowd_step(ctx, 1), FBS_CROWD_OK);
        CHECK(conserved(ctx, 1));
    }
    CHECK_EQ(fbs_crowd_chain_get(ctx, chain, &info), FBS_CROWD_OK);
    CHECK_EQ(info.staffed, 5u);
    /* Steady throughput is one item per handoff + 1 ticks: the last slot is
     * emptied by the station grant, after the chain pass of that tick. */
    CHECK(info.delivered >= 400u / 5u - 6u);
    CHECK(info.delivered <= 400u / 5u + 1u);
    CHECK(info.taken >= info.delivered);
    CHECK(info.taken - info.delivered <= 5u);
    fbs_crowd_destroy(ctx);
}

/* A slow sink fills up; the chain backs up without losing anything. */
static void test_backpressure(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, chain;
    fbs_crowd_chain_info info;
    fbs_crowd_station_info sink;
    uint32_t i, t;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 4);
    src = add_source(ctx, 0, 0, 1, 10, 10);
    dst = add_sink(ctx, 0, 0, 5, 100, 5); /* needs 5 before a 100-tick cycle */
    chain = add_chain(ctx, src, dst, 6, 2, 0);
    for (i = 0; i < 6; ++i) {
        fbs_crowd_handle w = add_worker(ctx, 0, 50);
        CHECK_EQ(fbs_crowd_command_slot(ctx, w, chain, i), FBS_CROWD_OK);
    }
    for (t = 0; t < 300; ++t) {
        CHECK_EQ(fbs_crowd_step(ctx, 1), FBS_CROWD_OK);
        CHECK(conserved(ctx, 1));
        CHECK_EQ(fbs_crowd_station_get(ctx, dst, &sink), FBS_CROWD_OK);
        CHECK(sink.input_stock <= 5u);
    }
    CHECK_EQ(fbs_crowd_chain_get(ctx, chain, &info), FBS_CROWD_OK);
    CHECK_EQ(info.items, 6u); /* every slot holds a waiting item */
    CHECK(info.delivered <= 5u + 5u * 3u);
    fbs_crowd_destroy(ctx);
}

/* Vacated slots keep their items; removing and reusing handles never aliases. */
static void test_vacancy_and_handles(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, chain, w[4], again;
    fbs_crowd_slot_info slot;
    fbs_crowd_chain_info before, after;
    fbs_crowd_worker_info info;
    uint32_t i, res = 0, amt = 7;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 4);
    src = add_source(ctx, 0, 0, 1, 50, 10);
    dst = add_sink(ctx, 0, 0, 1, 1, 100);
    chain = add_chain(ctx, src, dst, 4, 3, 0);
    for (i = 0; i < 4; ++i) {
        w[i] = add_worker(ctx, 0, 50);
        CHECK_EQ(fbs_crowd_command_slot(ctx, w[i], chain, i), FBS_CROWD_OK);
    }
    CHECK_EQ(fbs_crowd_step(ctx, 50), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_remove_worker(ctx, w[2], &res, &amt), FBS_CROWD_OK);
    CHECK_EQ(res, FBS_CROWD_NONE);
    CHECK_EQ(amt, 0u);
    CHECK_EQ(fbs_crowd_worker_get(ctx, w[2], &info), FBS_CROWD_STALE);
    CHECK_EQ(fbs_crowd_remove_worker(ctx, w[2], NULL, NULL), FBS_CROWD_STALE);
    CHECK_EQ(fbs_crowd_step(ctx, 20), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_chain_get(ctx, chain, &before), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_step(ctx, 40), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_chain_get(ctx, chain, &after), FBS_CROWD_OK);
    CHECK_EQ(before.delivered, after.delivered); /* flow stopped at the gap */
    CHECK(conserved(ctx, 1));
    CHECK_EQ(fbs_crowd_slot_get(ctx, chain, 2, &slot), FBS_CROWD_OK);
    CHECK_EQ(slot.worker, (fbs_crowd_handle)0);
    /* The freed index is reused with a new generation. */
    again = add_worker(ctx, 0, 50);
    CHECK(again != w[2]);
    CHECK((again & 0xFFFFFFFFu) == (w[2] & 0xFFFFFFFFu));
    CHECK_EQ(fbs_crowd_worker_get(ctx, w[2], &info), FBS_CROWD_STALE);
    CHECK_EQ(fbs_crowd_command_slot(ctx, again, chain, 2), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_step(ctx, 60), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_chain_get(ctx, chain, &after), FBS_CROWD_OK);
    CHECK(after.delivered > before.delivered);
    /* Kinds are distinct. */
    CHECK_EQ(fbs_crowd_worker_get(ctx, chain, &info), FBS_CROWD_STALE);
    CHECK_EQ(fbs_crowd_worker_get(ctx, 0, &info), FBS_CROWD_STALE);
    /* A referenced station cannot be removed. */
    CHECK_EQ(fbs_crowd_remove_station(ctx, src, NULL), FBS_CROWD_STATE);
    CHECK(conserved(ctx, 1));
    fbs_crowd_destroy(ctx);
}

/* Reassigning workers between chains keeps items in vacated slots and never
 * duplicates or loses them; removing a chain exports its items. */
static void test_reassignment_and_chain_removal(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, a, b, w[6];
    fbs_crowd_slot_command cmds[6];
    fbs_crowd_result results[6];
    fbs_crowd_chain_info ia, ib;
    fbs_crowd_worker_info info;
    uint32_t exported[2], i;
    fbs_crowd_totals tot;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 2);
    src = add_source(ctx, 0, 0, 1, 50, 10);
    dst = add_sink(ctx, 3, 0, 1, 1, 100);
    a = add_chain(ctx, src, dst, 6, 3, 0);
    b = add_chain(ctx, src, dst, 6, 3, 3);
    for (i = 0; i < 6; ++i) {
        w[i] = add_worker(ctx, 0, 200);
        CHECK_EQ(fbs_crowd_command_slot(ctx, w[i], a, i), FBS_CROWD_OK);
    }
    CHECK_EQ(fbs_crowd_step(ctx, 60), FBS_CROWD_OK);
    for (i = 0; i < 6; ++i) {
        cmds[i].worker = w[i];
        cmds[i].chain = b;
        cmds[i].slot = i;
    }
    CHECK_EQ(fbs_crowd_command_slots(ctx, cmds, 6, results), FBS_CROWD_OK);
    for (i = 0; i < 6; ++i) CHECK_EQ(results[i], FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_chain_get(ctx, a, &ia), FBS_CROWD_OK);
    CHECK_EQ(ia.staffed, 0u);
    CHECK(ia.items > 0u); /* items stay behind */
    CHECK(conserved(ctx, 1));
    /* A conflicting batch entry fails alone. */
    {
        fbs_crowd_handle extra = add_worker(ctx, 0, 200);
        fbs_crowd_slot_command bad[2];
        bad[0].worker = extra; bad[0].chain = b; bad[0].slot = 0; /* reserved */
        bad[1].worker = 0; bad[1].chain = b; bad[1].slot = 1;     /* stale */
        CHECK_EQ(fbs_crowd_command_slots(ctx, bad, 2, results), FBS_CROWD_STATE);
        CHECK_EQ(results[0], FBS_CROWD_STATE);
        CHECK_EQ(results[1], FBS_CROWD_STALE);
        CHECK_EQ(fbs_crowd_remove_worker(ctx, extra, NULL, NULL), FBS_CROWD_OK);
    }
    CHECK_EQ(fbs_crowd_step(ctx, 600), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_chain_get(ctx, b, &ib), FBS_CROWD_OK);
    CHECK_EQ(ib.staffed, 6u);
    CHECK(ib.delivered > 10u);
    CHECK(conserved(ctx, 1));
    /* Removing chain a exports exactly its stranded items. */
    CHECK_EQ(fbs_crowd_chain_get(ctx, a, &ia), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_resource_totals(ctx, 0, &tot), FBS_CROWD_OK);
    {
        uint64_t exported_before = tot.exported;
        CHECK_EQ(fbs_crowd_remove_chain(ctx, a, exported), FBS_CROWD_OK);
        CHECK_EQ(exported[0], ia.items);
        CHECK_EQ(fbs_crowd_resource_totals(ctx, 0, &tot), FBS_CROWD_OK);
        CHECK_EQ(tot.exported, exported_before + ia.items);
    }
    CHECK_EQ(fbs_crowd_chain_get(ctx, a, &ia), FBS_CROWD_STALE);
    /* Removing the staffed chain b frees its workers at its access node. */
    CHECK_EQ(fbs_crowd_remove_chain(ctx, b, exported), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_worker_get(ctx, w[0], &info), FBS_CROWD_OK);
    CHECK_EQ(info.state, (uint32_t)FBS_CROWD_IDLE);
    CHECK_EQ(info.node, 3u);
    CHECK_EQ(info.intent, (uint32_t)FBS_CROWD_INTENT_NONE);
    CHECK(conserved(ctx, 1));
    /* Stations are free to remove now; stock is exported. */
    CHECK_EQ(fbs_crowd_remove_station(ctx, src, exported), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_remove_station(ctx, dst, exported), FBS_CROWD_OK);
    CHECK(conserved(ctx, 1));
    fbs_crowd_destroy(ctx);
}

/* Haulers queue on a capacity-1 link; carried cargo is exported on removal. */
static void test_haulers_and_queues(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, w[12];
    fbs_crowd_worker_info info;
    uint32_t i, t, max_on_link = 0, saw_queue = 0, res, amt;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 1);
    src = add_source(ctx, 0, 1, 2, 30, 30);
    dst = add_sink(ctx, 3, 1, 1, 1, 30);
    for (i = 0; i < 12; ++i) {
        w[i] = add_worker(ctx, 0, 250 + i * 10u);
        CHECK_EQ(fbs_crowd_command_haul(ctx, w[i], src, dst, 1), FBS_CROWD_OK);
    }
    CHECK_EQ(fbs_crowd_command_haul(ctx, w[0], src, dst, 0), FBS_CROWD_INVALID);
    CHECK_EQ(fbs_crowd_command_haul(ctx, w[0], dst, src, 1), FBS_CROWD_INVALID);
    CHECK_EQ(fbs_crowd_remove_station(ctx, src, NULL), FBS_CROWD_STATE);
    for (t = 0; t < 1500; ++t) {
        uint32_t on[3] = {0, 0, 0};
        CHECK_EQ(fbs_crowd_step(ctx, 1), FBS_CROWD_OK);
        CHECK(conserved(ctx, 2));
        for (i = 0; i < 12; ++i) {
            CHECK_EQ(fbs_crowd_worker_get(ctx, w[i], &info), FBS_CROWD_OK);
            if (info.link != FBS_CROWD_NONE && !info.queued) on[info.link]++;
            if (info.queued) saw_queue = 1;
        }
        for (i = 0; i < 3; ++i) if (on[i] > max_on_link) max_on_link = on[i];
    }
    CHECK_EQ(max_on_link, 1u);
    CHECK(saw_queue);
    {
        fbs_crowd_station_info s;
        CHECK_EQ(fbs_crowd_station_get(ctx, dst, &s), FBS_CROWD_OK);
        CHECK(s.cycles > 20u);
    }
    /* Find a hauler with cargo and remove it: cargo is exported. */
    for (i = 0; i < 12; ++i) {
        CHECK_EQ(fbs_crowd_worker_get(ctx, w[i], &info), FBS_CROWD_OK);
        if (info.cargo_amount) break;
    }
    if (i == 12) { /* step until someone carries */
        for (t = 0; t < 200 && i == 12; ++t) {
            uint32_t k;
            fbs_crowd_step(ctx, 1);
            for (k = 0; k < 12; ++k) {
                fbs_crowd_worker_get(ctx, w[k], &info);
                if (info.cargo_amount) { i = k; break; }
            }
        }
    }
    CHECK(i < 12);
    if (i < 12) {
        fbs_crowd_totals before, after;
        CHECK_EQ(fbs_crowd_command_move(ctx, w[i], 2), FBS_CROWD_STATE); /* has cargo */
        CHECK_EQ(fbs_crowd_resource_totals(ctx, 1, &before), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_remove_worker(ctx, w[i], &res, &amt), FBS_CROWD_OK);
        CHECK_EQ(res, 1u);
        CHECK_EQ(amt, 1u);
        CHECK_EQ(fbs_crowd_resource_totals(ctx, 1, &after), FBS_CROWD_OK);
        CHECK_EQ(after.exported, before.exported + 1u);
        CHECK_EQ(after.cargo + 1u, before.cargo);
    }
    CHECK(conserved(ctx, 2));
    /* Idle stops hauling; a worker on a link finishes it first. */
    for (i = 0; i < 12; ++i) {
        if (fbs_crowd_worker_get(ctx, w[i], &info) != FBS_CROWD_OK) continue;
        CHECK_EQ(fbs_crowd_command_idle(ctx, w[i]), FBS_CROWD_OK);
    }
    CHECK_EQ(fbs_crowd_step(ctx, 400), FBS_CROWD_OK);
    for (i = 0; i < 12; ++i) {
        if (fbs_crowd_worker_get(ctx, w[i], &info) != FBS_CROWD_OK) continue;
        CHECK_EQ(info.state, (uint32_t)FBS_CROWD_IDLE);
        CHECK_EQ(info.link, FBS_CROWD_NONE);
    }
    CHECK_EQ(fbs_crowd_remove_station(ctx, src, NULL), FBS_CROWD_OK);
    CHECK(conserved(ctx, 2));
    fbs_crowd_destroy(ctx);
}

static void test_capacity_and_reset(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle hs[64], extra = 0, src, dst, ch;
    fbs_crowd_totals tot;
    fbs_crowd_stats st;
    fbs_crowd_worker_info info;
    uint32_t i;
    c.max_workers = 8;
    c.max_chain_slots = 10;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 2);
    for (i = 0; i < 8; ++i) hs[i] = add_worker(ctx, 1, 40);
    {
        fbs_crowd_worker_desc w;
        memset(&w, 0, sizeof w);
        w.node = 0;
        w.speed_mm_per_tick = 10;
        CHECK_EQ(fbs_crowd_add_worker(ctx, &w, &extra), FBS_CROWD_CAPACITY);
        w.node = 99;
        CHECK_EQ(fbs_crowd_add_worker(ctx, &w, &extra), FBS_CROWD_INVALID);
    }
    src = add_source(ctx, 0, 0, 1, 10, 3);
    dst = add_sink(ctx, 0, 0, 1, 1, 10);
    ch = add_chain(ctx, src, dst, 6, 2, 0);
    {
        fbs_crowd_chain_desc d;
        fbs_crowd_handle h2;
        memset(&d, 0, sizeof d);
        d.source = src; d.destination = dst; d.slot_count = 5; d.handoff_ticks = 1;
        CHECK_EQ(fbs_crowd_add_chain(ctx, &d, &h2), FBS_CROWD_CAPACITY); /* 6 + 5 > 10 */
        d.slot_count = 4;
        CHECK_EQ(fbs_crowd_add_chain(ctx, &d, &h2), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_remove_chain(ctx, h2, NULL), FBS_CROWD_OK);
    }
    for (i = 0; i < 6; ++i) CHECK_EQ(fbs_crowd_command_slot(ctx, hs[i], ch, i), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_step(ctx, 100), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_reset(ctx), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_tick(ctx), 0u);
    CHECK_EQ(fbs_crowd_worker_get(ctx, hs[0], &info), FBS_CROWD_STALE);
    CHECK_EQ(fbs_crowd_resource_totals(ctx, 0, &tot), FBS_CROWD_OK);
    CHECK_EQ(tot.introduced, 0u);
    CHECK_EQ(fbs_crowd_get_stats(ctx, &st), FBS_CROWD_OK);
    CHECK_EQ(st.workers, 0u);
    CHECK_EQ(st.chains, 0u);
    CHECK_EQ(st.slots_used, 0u);
    /* Everything is usable again, with fresh handles. */
    for (i = 0; i < 8; ++i) {
        fbs_crowd_handle h = add_worker(ctx, 1, 40);
        CHECK(h != hs[i]);
    }
    src = add_source(ctx, 0, 0, 1, 10, 3);
    dst = add_sink(ctx, 0, 0, 1, 1, 10);
    (void)add_chain(ctx, src, dst, 10, 2, 0);
    CHECK(conserved(ctx, 1));
    fbs_crowd_destroy(ctx);
}

/* Same operations give the same hash whatever the step batching and however
 * often (and how) presentation is extracted. */
static uint64_t run_scenario(uint32_t workers, uint32_t ticks, uint32_t batch, int extract,
                             int reassign, uint32_t *tables_out) {
    crowd_scenario s;
    crowd_scenario_params p;
    fbs_crowd_instance *inst = NULL;
    uint32_t done = 0, n = 0, total = 0, frame = 0;
    uint64_t hash;
    crowd_scenario_defaults(&p, workers, 7);
    CHECK_EQ(crowd_scenario_build(&s, &p), 0);
    if (extract) inst = (fbs_crowd_instance *)malloc(sizeof(fbs_crowd_instance) * workers);
    while (done < ticks) {
        uint32_t k = batch < ticks - done ? batch : ticks - done;
        /* Commands are issued at fixed ticks; batches end there. */
        if (reassign && done < ticks / 3u && done + k > ticks / 3u) k = ticks / 3u - done;
        if (reassign && done < 2u * ticks / 3u && done + k > 2u * ticks / 3u) k = 2u * ticks / 3u - done;
        if (reassign && (done == ticks / 3u || done == 2u * ticks / 3u)) {
            uint32_t failures = 0;
            (void)crowd_scenario_mass_reassign(&s, &failures);
            CHECK_EQ(failures, 0u);
        }
        CHECK_EQ(fbs_crowd_step(s.crowd, k), FBS_CROWD_OK);
        done += k;
        if (extract) {
            /* Varying capacity and fraction, as a culling renderer would. */
            uint32_t cap = (frame % 3u == 0) ? workers : workers / 2u;
            CHECK_EQ(fbs_crowd_extract(s.crowd, (float)(frame % 7u) / 7.0f, inst, cap, &n, &total),
                     FBS_CROWD_OK);
            CHECK_EQ(total, workers);
            CHECK(n <= cap);
            frame++;
        }
        /* Splitting a batch must not matter. */
        if (reassign && done % 90u == 0) CHECK(conserved(s.crowd, 3));
    }
    CHECK(conserved(s.crowd, 3));
    if (tables_out) {
        fbs_crowd_stats st;
        fbs_crowd_get_stats(s.crowd, &st);
        *tables_out = st.ecs_tables;
    }
    hash = fbs_crowd_state_hash(s.crowd);
    free(inst);
    crowd_scenario_free(&s);
    return hash;
}

static void test_extreme_graph_lengths(void) {
    static const struct {
        fbs_crowd_vec3i a, b;
        uint64_t length;
    } cases[] = {
        {{0, 0, 0}, {0, 0, 0}, 1},
        {{0, 0, 0}, {3, 4, 0}, 5},
        {{0, 0, 0}, {1, 1, 1}, 2},
        {{INT32_MIN, 0, 0}, {0, 0, 0}, UINT64_C(2147483648)},
        {{-1500000000, 0, 0}, {1500000000, 1000000000, 0}, UINT64_C(3162277661)},
        {{INT32_MIN, 0, 0}, {INT32_MAX, 0, 0}, UINT32_MAX},
        {{INT32_MIN, INT32_MIN, 0}, {INT32_MAX, INT32_MAX, 0}, UINT32_MAX},
        {{INT32_MIN, INT32_MIN, INT32_MIN}, {INT32_MAX, INT32_MAX, INT32_MAX}, UINT32_MAX}
    };
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_node_desc nodes[2];
    fbs_crowd_link_desc link = {0, 1, 1, 0};
    size_t i;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    if (!ctx) return;
    memset(nodes, 0, sizeof nodes);
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint64_t length = 0;
        nodes[0].position = cases[i].a;
        nodes[1].position = cases[i].b;
        CHECK_EQ(fbs_crowd_set_graph(ctx, nodes, 2, &link, 1), FBS_CROWD_OK);
        CHECK_EQ(fbs_crowd_route_length(ctx, 0, 1, &length), FBS_CROWD_OK);
        CHECK_EQ(length, cases[i].length);
    }
    fbs_crowd_destroy(ctx);
}

static void test_determinism(void) {
    uint32_t tables_a = 0, tables_b = 0;
    uint64_t a = run_scenario(900, 600, 1, 1, 1, &tables_a);
    uint64_t b = run_scenario(900, 600, 7, 0, 1, &tables_b);
    uint64_t c = run_scenario(900, 600, 600, 0, 1, NULL);
    uint64_t d = run_scenario(900, 600, 1, 0, 0, NULL);
    CHECK_EQ(a, b);
    CHECK_EQ(a, c);
    CHECK(a != d); /* reassignment changes the outcome */
    CHECK_EQ(tables_a, tables_b);
    printf("HASH scenario900_600_reassign %016llx\n", (unsigned long long)a);
    printf("HASH scenario900_600_plain %016llx\n", (unsigned long long)d);
    printf("TABLES scenario900 %u\n", tables_a);
}

/* A worker's presentation matches its authoritative state. */
static void test_presentation(void) {
    fbs_crowd_config c = small_config();
    fbs_crowd *ctx = NULL;
    fbs_crowd_handle src, dst, chain, w, h;
    fbs_crowd_instance inst[4];
    uint32_t n = 0, total = 0, i;
    uint64_t hash;
    CHECK_EQ(fbs_crowd_create(&c, &ctx), FBS_CROWD_OK);
    line_graph(ctx, 2);
    src = add_source(ctx, 0, 0, 1, 10, 5);
    dst = add_sink(ctx, 0, 0, 1, 1, 10);
    chain = add_chain(ctx, src, dst, 2, 10, 0);
    w = add_worker(ctx, 0, 50);
    h = add_worker(ctx, 0, 100);
    CHECK_EQ(fbs_crowd_command_slot(ctx, w, chain, 0), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_command_move(ctx, h, 2), FBS_CROWD_OK);
    CHECK_EQ(fbs_crowd_step(ctx, 150), FBS_CROWD_OK);
    hash = fbs_crowd_state_hash(ctx);
    CHECK_EQ(fbs_crowd_extract(ctx, 0.5f, inst, 4, &n, &total), FBS_CROWD_OK);
    CHECK_EQ(n, 2u);
    CHECK_EQ(fbs_crowd_extract(ctx, 1.5f, inst, 4, &n, &total), FBS_CROWD_INVALID);
    CHECK_EQ(fbs_crowd_state_hash(ctx), hash);
    for (i = 0; i < n; ++i) {
        if (inst[i].worker == w) {
            CHECK(inst[i].x > 0.5f && inst[i].x < 0.6f); /* first of 2 slots over 2.2 m */
            CHECK(inst[i].clip == FBS_CROWD_CLIP_PASS || inst[i].clip == FBS_CROWD_CLIP_IDLE);
        } else {
            CHECK_EQ(inst[i].worker, h);
            CHECK_EQ(inst[i].clip, (uint8_t)FBS_CROWD_CLIP_WALK);
            CHECK(inst[i].x > 10.0f && inst[i].x < 20.0f);
            CHECK_EQ(inst[i].room, 1u);
        }
    }
    fbs_crowd_destroy(ctx);
}

int main(void) {
    test_create_validation();
    test_graph();
    test_extreme_graph_lengths();
    test_chain_flow();
    test_backpressure();
    test_vacancy_and_handles();
    test_reassignment_and_chain_removal();
    test_haulers_and_queues();
    test_capacity_and_reset();
    test_presentation();
    test_determinism();
    printf("crowd core: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
