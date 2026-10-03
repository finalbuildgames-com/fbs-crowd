/* Seeded relay-chain + hauler scenario for tests, benchmarks and demos.
 *
 * Host-side example code, not part of the fbs::crowd API. It builds a grid of
 * cave "rooms" on two stacked levels, one route node per room, links between
 * neighbouring rooms and ramps between levels. Each room holds parallel relay
 * chains fed by a shared source station and emptied into a shared sink
 * station (competing consumers). The remaining workers haul between depot
 * stations in different rooms, so routes cross rooms and queue at links.
 * About 12% extra chains start unstaffed; crowd_scenario_mass_reassign()
 * moves the staff of whole chains onto them (and back on the next call).
 *
 * Header-only; define CROWD_SCENARIO_IMPLEMENTATION in exactly one file. */
#ifndef FBS_CROWD_SCENARIO_H
#define FBS_CROWD_SCENARIO_H
#include "fbs/crowd.h"

#include <stdint.h>

typedef struct crowd_scenario_params {
    uint32_t workers;       /* total population */
    uint32_t seed;
    uint32_t slot_share_pct; /* share of workers in chain slots, default 70 */
    uint32_t slots_per_chain; /* default 30 */
    uint32_t handoff_ticks;   /* default 18 (0.6 s at 30 Hz) */
    uint32_t tick_hz;         /* default 30 */
} crowd_scenario_params;

typedef struct crowd_scenario_room {
    float min[3], max[3]; /* metres, floor box */
    uint32_t level, node;
} crowd_scenario_room;

typedef struct crowd_scenario {
    crowd_scenario_params params;
    fbs_crowd *crowd;
    uint32_t room_count, levels, grid;
    crowd_scenario_room *rooms;
    uint32_t chain_count, staffed_chains;
    fbs_crowd_handle *chains;       /* chain_count */
    uint32_t *chain_room;
    fbs_crowd_handle *workers;      /* params.workers */
    uint32_t slot_workers, haulers;
    fbs_crowd_handle *stations;     /* 4 per room: chain source, chain sink, depot source, depot sink */
    uint32_t reassign_parity;
    float level_height;
} crowd_scenario;

#define CROWD_SCENARIO_MM(v) ((int32_t)((float)(v) * 1000.0f))

void crowd_scenario_defaults(crowd_scenario_params *p, uint32_t workers, uint32_t seed);
/* Creates the context and everything in it. 0 on success. */
int crowd_scenario_build(crowd_scenario *s, const crowd_scenario_params *p);
void crowd_scenario_free(crowd_scenario *s);
/* Moves the staff of every chain in one half of the rooms to spare chains
 * elsewhere (a mass reassignment of about 12% of the slot workers). Returns
 * the number of commands issued, *failures receives rejected commands. */
uint32_t crowd_scenario_mass_reassign(crowd_scenario *s, uint32_t *failures);

#ifdef CROWD_SCENARIO_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>

static uint32_t crowd_scenario_rng(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9E3779B9u;
    return *state;
}

void crowd_scenario_defaults(crowd_scenario_params *p, uint32_t workers, uint32_t seed) {
    memset(p, 0, sizeof *p);
    p->workers = workers;
    p->seed = seed ? seed : 1u;
    p->slot_share_pct = 70;
    p->slots_per_chain = 30;
    p->handoff_ticks = 18;
    p->tick_hz = 30;
}

#define CS_ROOM_SIZE 44.0f
#define CS_CHAINS_PER_ROOM 12u
#define CS_SLOT_SPACING 1.1f
#define CS_LINE_SPACING 3.0f

int crowd_scenario_build(crowd_scenario *s, const crowd_scenario_params *p) {
    fbs_crowd_config cfg;
    fbs_crowd_node_desc *nodes;
    fbs_crowd_link_desc *links;
    uint32_t i, rooms_needed, per_level, grid, link_count = 0, rng = p->seed;
    uint32_t slot_workers, chains_staffed, chains_total, haulers, next_worker = 0;
    memset(s, 0, sizeof *s);
    s->params = *p;
    s->level_height = 9.0f;
    slot_workers = (uint32_t)((uint64_t)p->workers * p->slot_share_pct / 100u);
    chains_staffed = slot_workers / p->slots_per_chain;
    slot_workers = chains_staffed * p->slots_per_chain;
    chains_total = chains_staffed + (chains_staffed * 12u + 99u) / 100u;
    if (chains_total == 0) chains_total = 1;
    haulers = p->workers - slot_workers;
    rooms_needed = (chains_total + CS_CHAINS_PER_ROOM - 1u) / CS_CHAINS_PER_ROOM;
    if (rooms_needed < 4) rooms_needed = 4;
    per_level = (rooms_needed + 1u) / 2u;
    grid = 1;
    while (grid * grid < per_level) grid++;
    s->levels = 2;
    s->grid = grid;
    s->room_count = grid * grid * 2u;
    s->chain_count = chains_total;
    s->staffed_chains = chains_staffed;
    s->slot_workers = slot_workers;
    s->haulers = haulers;

    memset(&cfg, 0, sizeof cfg);
    cfg.max_workers = p->workers ? p->workers : 1u;
    cfg.max_chains = chains_total;
    cfg.max_stations = s->room_count * 4u;
    cfg.max_chain_slots = chains_total * p->slots_per_chain;
    cfg.max_nodes = s->room_count;
    cfg.max_links = s->room_count * 3u;
    cfg.resources = 3; /* 0 chain goods, 1 depot goods, 2 unused */
    if (fbs_crowd_create(&cfg, &s->crowd) != FBS_CROWD_OK) return 1;

    s->rooms = (crowd_scenario_room *)calloc(s->room_count, sizeof *s->rooms);
    s->chains = (fbs_crowd_handle *)calloc(chains_total, sizeof *s->chains);
    s->chain_room = (uint32_t *)calloc(chains_total, sizeof *s->chain_room);
    s->workers = (fbs_crowd_handle *)calloc(p->workers ? p->workers : 1u, sizeof *s->workers);
    s->stations = (fbs_crowd_handle *)calloc(s->room_count * 4u, sizeof *s->stations);
    nodes = (fbs_crowd_node_desc *)calloc(s->room_count, sizeof *nodes);
    links = (fbs_crowd_link_desc *)calloc(cfg.max_links, sizeof *links);
    if (!s->rooms || !s->chains || !s->chain_room || !s->workers || !s->stations || !nodes || !links) {
        free(nodes);
        free(links);
        return 2;
    }
    for (i = 0; i < s->room_count; ++i) {
        uint32_t lvl = i / (grid * grid), cell = i % (grid * grid);
        uint32_t gx = cell % grid, gz = cell / grid;
        crowd_scenario_room *r = &s->rooms[i];
        float x0 = (float)gx * (CS_ROOM_SIZE + 8.0f), z0 = (float)gz * (CS_ROOM_SIZE + 8.0f);
        r->min[0] = x0;
        r->min[1] = (float)lvl * s->level_height;
        r->min[2] = z0;
        r->max[0] = x0 + CS_ROOM_SIZE;
        r->max[1] = r->min[1];
        r->max[2] = z0 + CS_ROOM_SIZE;
        r->level = lvl;
        r->node = i;
        nodes[i].position.x = CROWD_SCENARIO_MM(x0 + CS_ROOM_SIZE * 0.5f);
        nodes[i].position.y = CROWD_SCENARIO_MM(r->min[1]);
        nodes[i].position.z = CROWD_SCENARIO_MM(z0 + CS_ROOM_SIZE - 2.0f);
        nodes[i].room = i;
        if (gx + 1u < grid) {
            links[link_count].a = i;
            links[link_count].b = i + 1u;
            links[link_count].capacity = 120;
            link_count++;
        }
        if (gz + 1u < grid) {
            links[link_count].a = i;
            links[link_count].b = i + grid;
            links[link_count].capacity = 120;
            link_count++;
        }
        if (lvl == 0 && gx == 0) { /* ramp up to the room above */
            links[link_count].a = i;
            links[link_count].b = i + grid * grid;
            links[link_count].capacity = 40;
            links[link_count].length_mm = (uint32_t)CROWD_SCENARIO_MM(s->level_height * 2.5f);
            link_count++;
        }
    }
    if (fbs_crowd_set_graph(s->crowd, nodes, s->room_count, links, link_count) != FBS_CROWD_OK) {
        free(nodes);
        free(links);
        return 3;
    }
    free(nodes);
    free(links);

    for (i = 0; i < s->room_count; ++i) {
        fbs_crowd_station_desc d;
        memset(&d, 0, sizeof d);
        d.node = i;
        /* chain source: 1 unit per tick */
        d.input_resource = FBS_CROWD_NONE;
        d.output_resource = 0;
        d.output_per_cycle = 1;
        d.output_capacity = 40;
        d.cycle_ticks = 1;
        d.initial_output = 20;
        d.tag = i * 4u;
        if (fbs_crowd_add_station(s->crowd, &d, &s->stations[i * 4u]) != FBS_CROWD_OK) return 4;
        /* chain sink: consumes 3 per 4 ticks, so busy rooms back up */
        memset(&d, 0, sizeof d);
        d.node = i;
        d.input_resource = 0;
        d.input_per_cycle = 3;
        d.input_capacity = 24;
        d.output_resource = FBS_CROWD_NONE;
        d.cycle_ticks = 4;
        d.tag = i * 4u + 1u;
        if (fbs_crowd_add_station(s->crowd, &d, &s->stations[i * 4u + 1u]) != FBS_CROWD_OK) return 4;
        /* depot source and sink for haulers */
        memset(&d, 0, sizeof d);
        d.node = i;
        d.input_resource = FBS_CROWD_NONE;
        d.output_resource = 1;
        d.output_per_cycle = 1;
        d.output_capacity = 60;
        d.cycle_ticks = 3;
        d.initial_output = 30;
        d.tag = i * 4u + 2u;
        if (fbs_crowd_add_station(s->crowd, &d, &s->stations[i * 4u + 2u]) != FBS_CROWD_OK) return 4;
        memset(&d, 0, sizeof d);
        d.node = i;
        d.input_resource = 1;
        d.input_per_cycle = 1;
        d.input_capacity = 60;
        d.output_resource = FBS_CROWD_NONE;
        d.cycle_ticks = 2;
        d.tag = i * 4u + 3u;
        if (fbs_crowd_add_station(s->crowd, &d, &s->stations[i * 4u + 3u]) != FBS_CROWD_OK) return 4;
    }

    /* Chains: staffed ones fill rooms from the first; spares go to the last rooms. */
    for (i = 0; i < chains_total; ++i) {
        fbs_crowd_chain_desc c;
        uint32_t room = i < chains_staffed ? i / CS_CHAINS_PER_ROOM
                                           : s->room_count - 1u - (i - chains_staffed) / CS_CHAINS_PER_ROOM;
        uint32_t line = i < chains_staffed ? i % CS_CHAINS_PER_ROOM
                                           : CS_CHAINS_PER_ROOM - 1u - (i - chains_staffed) % CS_CHAINS_PER_ROOM;
        const crowd_scenario_room *r = &s->rooms[room];
        float len = CS_SLOT_SPACING * (float)p->slots_per_chain;
        float x = r->min[0] + (CS_ROOM_SIZE - len) * 0.5f;
        float z = r->min[2] + 3.0f + (float)line * CS_LINE_SPACING;
        memset(&c, 0, sizeof c);
        c.source = s->stations[room * 4u];
        c.destination = s->stations[room * 4u + 1u];
        c.resource = 0;
        c.slot_count = p->slots_per_chain;
        c.handoff_ticks = p->handoff_ticks;
        c.access_node = room;
        c.start.x = CROWD_SCENARIO_MM(x);
        c.start.y = CROWD_SCENARIO_MM(r->min[1]);
        c.start.z = CROWD_SCENARIO_MM(z);
        c.end.x = CROWD_SCENARIO_MM(x + len);
        c.end.y = c.start.y;
        c.end.z = c.start.z;
        c.room = room;
        c.tag = i;
        s->chain_room[i] = room;
        if (fbs_crowd_add_chain(s->crowd, &c, &s->chains[i]) != FBS_CROWD_OK) return 5;
    }

    /* Slot workers spawn at their chain's access node and take their slot. */
    for (i = 0; i < chains_staffed; ++i) {
        uint32_t k;
        for (k = 0; k < p->slots_per_chain; ++k) {
            fbs_crowd_worker_desc w;
            memset(&w, 0, sizeof w);
            w.node = s->chain_room[i];
            w.speed_mm_per_tick = 38u + crowd_scenario_rng(&rng) % 12u;
            w.variant = crowd_scenario_rng(&rng) % 8u;
            w.tag = next_worker;
            if (fbs_crowd_add_worker(s->crowd, &w, &s->workers[next_worker]) != FBS_CROWD_OK) return 6;
            if (fbs_crowd_command_slot(s->crowd, s->workers[next_worker], s->chains[i], k) != FBS_CROWD_OK) return 7;
            next_worker++;
        }
    }
    /* Haulers: carry depot goods to a depot in another room. */
    for (i = 0; i < haulers; ++i) {
        fbs_crowd_worker_desc w;
        uint32_t from = crowd_scenario_rng(&rng) % s->room_count;
        uint32_t to = (from + 1u + crowd_scenario_rng(&rng) % 3u) % s->room_count;
        memset(&w, 0, sizeof w);
        w.node = from;
        w.speed_mm_per_tick = 38u + crowd_scenario_rng(&rng) % 12u;
        w.variant = crowd_scenario_rng(&rng) % 8u;
        w.tag = next_worker;
        if (fbs_crowd_add_worker(s->crowd, &w, &s->workers[next_worker]) != FBS_CROWD_OK) return 8;
        if (fbs_crowd_command_haul(s->crowd, s->workers[next_worker], s->stations[from * 4u + 2u],
                                   s->stations[to * 4u + 3u], 1) != FBS_CROWD_OK) {
            return 9;
        }
        next_worker++;
    }
    return 0;
}

void crowd_scenario_free(crowd_scenario *s) {
    fbs_crowd_destroy(s->crowd);
    free(s->rooms);
    free(s->chains);
    free(s->chain_room);
    free(s->workers);
    free(s->stations);
    memset(s, 0, sizeof *s);
}

uint32_t crowd_scenario_mass_reassign(crowd_scenario *s, uint32_t *failures) {
    /* Parity 0: staff of chains [0, spare) move to the spare chains.
     * Parity 1: they move back. */
    const uint32_t spare = s->chain_count - s->staffed_chains;
    uint32_t c, k, issued = 0, failed = 0;
    for (c = 0; c < spare; ++c) {
        fbs_crowd_handle from = s->reassign_parity ? s->chains[s->staffed_chains + c] : s->chains[c];
        fbs_crowd_handle to = s->reassign_parity ? s->chains[c] : s->chains[s->staffed_chains + c];
        for (k = 0; k < s->params.slots_per_chain; ++k) {
            fbs_crowd_slot_info slot;
            if (fbs_crowd_slot_get(s->crowd, from, k, &slot) != FBS_CROWD_OK) continue;
            {
                fbs_crowd_handle w = slot.worker ? slot.worker : slot.reserved;
                if (!w) continue;
                if (fbs_crowd_command_slot(s->crowd, w, to, k) != FBS_CROWD_OK) failed++;
                issued++;
            }
        }
    }
    s->reassign_parity ^= 1u;
    if (failures) *failures = failed;
    return issued;
}
#endif
#endif
