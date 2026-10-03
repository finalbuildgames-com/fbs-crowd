/* FinalBuildSystems crowd: Flecs-backed deterministic worker simulation.
 *
 * Ownership:
 *   - Flecs (private world) owns worker, chain and station entities and their
 *     components. This is the only authoritative copy of that state.
 *   - The context owns fixed side structures whose ownership is explicit:
 *     handle tables (public identity -> entity), the chain slot pool (items
 *     belong to slots, a chain owns a contiguous range), the route graph and
 *     next-hop table, link occupancy/queues and per-tick request buffers.
 *
 * Determinism: storage (table) order never decides an outcome. Per-entity
 * updates in a pass are independent; every interaction between entities is
 * collected as a request and resolved in an explicit sorted order. */
#include "fbs/crowd.h"

#include "flecs_no_addons.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KIND_WORKER 1u
#define KIND_CHAIN 2u
#define KIND_STATION 3u
#define GEN_MAX 0x3FFFFFFFu
#define NO_HOP 0xFFFFu
#define NONE FBS_CROWD_NONE
#define NEVER UINT64_MAX
#define ITEM_NONE 0xFFu

enum { HAUL_TO_PICKUP = 0, HAUL_WAIT_PICKUP = 1, HAUL_TO_DROP = 2, HAUL_WAIT_DROP = 3 };
enum { OP_TAKE = 0, OP_PUT = 1 };
enum { REQ_CHAIN = 0, REQ_HAULER = 1 };

/* ---- Components (private) ------------------------------------------------ */
typedef struct CWorker {
    uint32_t index;   /* handle slot */
    uint32_t speed;   /* mm per tick */
    uint32_t variant;
    uint32_t pad;
    uint64_t tag;
} CWorker;

typedef struct CTask {
    uint32_t state, intent;
    uint32_t node;     /* current node, or the node a link was entered from */
    uint32_t goal;     /* NONE when not moving */
    uint32_t link;     /* link travelled or queued for, NONE at a node */
    uint32_t progress; /* mm along link */
    uint32_t queued;
    uint32_t qprev, qnext; /* worker index + 1 in the link queue */
    uint32_t chain, slot, slot_global, handoff, room;
    int32_t sx, sy, sz, sdx, sdz; /* slot pose for presentation */
    uint32_t pickup, dropoff, resource, haul_phase;
    uint32_t cargo_res, cargo_amt;
    uint64_t wait_since;
} CTask;

typedef struct CChain {
    uint32_t index, source, dest, resource;
    uint32_t offset, count, handoff, access, room, pad;
    int32_t start[3], end[3];
    uint64_t tag, taken, delivered, take_since, put_since;
} CChain;

typedef struct CStation {
    uint32_t index, node;
    uint32_t in_res, in_per, in_cap;
    uint32_t out_res, out_per, out_cap;
    uint32_t cycle, progress, running;
    uint32_t in_stock, out_stock, refs;
    uint64_t tag, cycles;
} CStation;

/* ---- Side structures ------------------------------------------------------ */
typedef struct handle_table {
    uint32_t cap, high, free_count, live;
    ecs_entity_t *entity; /* 0 when free */
    uint32_t *gen;
    uint32_t *free_list;
} handle_table;

typedef struct range { uint32_t start, count; } range;

typedef struct request {
    uint32_t station, op;
    uint64_t since;
    uint32_t kind, index; /* chain or worker handle index */
} request;

typedef struct link_request { uint32_t link, worker; } link_request;

struct fbs_crowd {
    fbs_crowd_config cfg;
    ecs_world_t *world;
    ecs_entity_t c_worker, c_task, c_chain, c_station;
    ecs_query_t *q_workers, *q_chains, *q_stations;
    handle_table workers, chains, stations;
    uint64_t tick;

    /* slot pool: items belong to slots; a chain owns [offset, offset+count) */
    uint8_t *slot_item;
    uint32_t *slot_timer, *slot_worker, *slot_reserved; /* worker index + 1 */
    range *free_ranges;
    uint32_t free_range_count, slots_used;

    /* graph */
    uint32_t node_count, link_count;
    fbs_crowd_vec3i *node_pos;
    uint32_t *node_room;
    fbs_crowd_link_desc *links; /* with derived lengths */
    uint32_t *link_occupancy, *link_head, *link_tail; /* worker index + 1 */
    uint16_t *next_hop;          /* [from * max_nodes + to] -> link */
    uint32_t *adj_offset, *adj_link;
    uint64_t *dist;
    uint32_t *heap, *heap_pos;
    uint32_t *queued_links;      /* links with a nonempty queue, scratch */
    uint8_t *link_marked;

    /* requests */
    request *requests, *requests_tmp;
    uint32_t request_count, request_cap;
    link_request *link_requests, *link_requests_tmp;
    uint32_t link_request_count;
    uint64_t requests_last_tick;

    /* ledgers */
    uint64_t introduced[FBS_CROWD_MAX_RESOURCES];
    uint64_t consumed[FBS_CROWD_MAX_RESOURCES];
    uint64_t exported[FBS_CROWD_MAX_RESOURCES];

    size_t context_bytes;
};

/* ---- Allocation helpers ------------------------------------------------- */
static void *crowd_alloc(fbs_crowd *ctx, size_t count, size_t size, int *failed) {
    void *p;
    if (count == 0) count = 1;
    if (count > SIZE_MAX / size) {
        *failed = 1;
        return NULL;
    }
    p = calloc(count, size);
    if (!p) *failed = 1;
    else ctx->context_bytes += count * size;
    return p;
}

static int table_init(fbs_crowd *ctx, handle_table *t, uint32_t cap, int *failed) {
    t->cap = cap;
    t->entity = (ecs_entity_t *)crowd_alloc(ctx, cap, sizeof(ecs_entity_t), failed);
    t->gen = (uint32_t *)crowd_alloc(ctx, cap, sizeof(uint32_t), failed);
    t->free_list = (uint32_t *)crowd_alloc(ctx, cap, sizeof(uint32_t), failed);
    return !*failed;
}

static void table_free(handle_table *t) {
    free(t->entity);
    free(t->gen);
    free(t->free_list);
}

static fbs_crowd_handle make_handle(uint32_t kind, uint32_t index, uint32_t gen) {
    return ((uint64_t)kind << 62) | ((uint64_t)gen << 32) | (uint64_t)(index + 1u);
}

/* Returns 1 and the slot index when a slot is available. */
static int table_acquire(handle_table *t, uint32_t *index) {
    if (t->free_count > 0) {
        *index = t->free_list[--t->free_count];
    } else if (t->high < t->cap) {
        *index = t->high++;
        t->gen[*index] = 0;
    } else {
        return 0;
    }
    t->gen[*index] += 1u; /* live generations start at 1 */
    t->live++;
    return 1;
}

static void table_release(handle_table *t, uint32_t index) {
    t->entity[index] = 0;
    t->live--;
    if (t->gen[index] < GEN_MAX) {
        t->free_list[t->free_count++] = index; /* retired at GEN_MAX: never aliases */
    }
}

static int resolve(const fbs_crowd *ctx, fbs_crowd_handle h, uint32_t kind, uint32_t *index,
                   ecs_entity_t *entity) {
    const handle_table *t;
    uint32_t idx, gen;
    if (!ctx || h == 0 || (uint32_t)(h >> 62) != kind) return 0;
    t = kind == KIND_WORKER ? &ctx->workers : kind == KIND_CHAIN ? &ctx->chains : &ctx->stations;
    idx = (uint32_t)(h & 0xFFFFFFFFu);
    gen = (uint32_t)((h >> 32) & GEN_MAX);
    if (idx == 0 || idx > t->high) return 0;
    idx -= 1u;
    if (t->entity[idx] == 0 || t->gen[idx] != gen) return 0;
    *index = idx;
    if (entity) *entity = t->entity[idx];
    return 1;
}

static fbs_crowd_handle handle_of(const handle_table *t, uint32_t kind, uint32_t index) {
    if (index >= t->high || t->entity[index] == 0) return 0;
    return make_handle(kind, index, t->gen[index]);
}

#define TASK(ctx, e) ((CTask *)ecs_get_mut_id((ctx)->world, (e), (ctx)->c_task))
#define WORKER(ctx, e) ((CWorker *)ecs_get_mut_id((ctx)->world, (e), (ctx)->c_worker))
#define CHAIN(ctx, e) ((CChain *)ecs_get_mut_id((ctx)->world, (e), (ctx)->c_chain))
#define STATION(ctx, e) ((CStation *)ecs_get_mut_id((ctx)->world, (e), (ctx)->c_station))

static CTask *task_at(const fbs_crowd *ctx, uint32_t windex) {
    return (CTask *)ecs_get_mut_id(ctx->world, ctx->workers.entity[windex], ctx->c_task);
}
static CChain *chain_at(const fbs_crowd *ctx, uint32_t cindex) {
    return (CChain *)ecs_get_mut_id(ctx->world, ctx->chains.entity[cindex], ctx->c_chain);
}
static CStation *station_at(const fbs_crowd *ctx, uint32_t sindex) {
    return (CStation *)ecs_get_mut_id(ctx->world, ctx->stations.entity[sindex], ctx->c_station);
}

/* ---- Slot ranges (first fit, coalescing) --------------------------------- */
static int ranges_acquire(fbs_crowd *ctx, uint32_t count, uint32_t *offset) {
    uint32_t i;
    for (i = 0; i < ctx->free_range_count; ++i) {
        range *r = &ctx->free_ranges[i];
        if (r->count >= count) {
            *offset = r->start;
            r->start += count;
            r->count -= count;
            if (r->count == 0) {
                memmove(r, r + 1, (size_t)(ctx->free_range_count - i - 1u) * sizeof(range));
                ctx->free_range_count--;
            }
            ctx->slots_used += count;
            return 1;
        }
    }
    return 0;
}

static void ranges_release(fbs_crowd *ctx, uint32_t offset, uint32_t count) {
    uint32_t i = 0;
    range *r;
    while (i < ctx->free_range_count && ctx->free_ranges[i].start < offset) ++i;
    /* The array holds at most max_chains + 1 disjoint ranges. */
    memmove(ctx->free_ranges + i + 1, ctx->free_ranges + i,
            (size_t)(ctx->free_range_count - i) * sizeof(range));
    ctx->free_ranges[i].start = offset;
    ctx->free_ranges[i].count = count;
    ctx->free_range_count++;
    ctx->slots_used -= count;
    r = ctx->free_ranges;
    if (i + 1u < ctx->free_range_count && r[i].start + r[i].count == r[i + 1u].start) {
        r[i].count += r[i + 1u].count;
        memmove(r + i + 1, r + i + 2, (size_t)(ctx->free_range_count - i - 2u) * sizeof(range));
        ctx->free_range_count--;
    }
    if (i > 0 && r[i - 1u].start + r[i - 1u].count == r[i].start) {
        r[i - 1u].count += r[i].count;
        memmove(r + i, r + i + 1, (size_t)(ctx->free_range_count - i - 1u) * sizeof(range));
        ctx->free_range_count--;
    }
}

static void ranges_reset(fbs_crowd *ctx) {
    ctx->free_range_count = ctx->cfg.max_chain_slots ? 1u : 0u;
    if (ctx->free_range_count) {
        ctx->free_ranges[0].start = 0;
        ctx->free_ranges[0].count = ctx->cfg.max_chain_slots;
    }
    ctx->slots_used = 0;
}

/* ---- Flecs setup ----------------------------------------------------------- */
static ecs_entity_t define_component(ecs_world_t *world, const char *name, size_t size,
                                     size_t alignment) {
    ecs_entity_desc_t ed;
    ecs_component_desc_t cd;
    memset(&ed, 0, sizeof ed);
    ed.name = name;
    ed.symbol = name;
    memset(&cd, 0, sizeof cd);
    cd.entity = ecs_entity_init(world, &ed);
    cd.type.size = (ecs_size_t)size;
    cd.type.alignment = (ecs_size_t)alignment;
    return ecs_component_init(world, &cd);
}

static ecs_query_t *cached_query(ecs_world_t *world, ecs_entity_t a, ecs_entity_t b) {
    ecs_query_desc_t qd;
    memset(&qd, 0, sizeof qd);
    qd.terms[0].id = a;
    if (b) qd.terms[1].id = b;
    qd.cache_kind = EcsQueryCacheAll;
    return ecs_query_init(world, &qd);
}

static ecs_entity_t new_entity(fbs_crowd *ctx, ecs_entity_t c0, ecs_entity_t c1) {
    ecs_entity_desc_t ed;
    ecs_id_t add[3];
    add[0] = c0;
    add[1] = c1;
    add[2] = 0;
    memset(&ed, 0, sizeof ed);
    ed.add = add; /* one table move for both components */
    return ecs_entity_init(ctx->world, &ed);
}

/* ---- Create / destroy ------------------------------------------------------ */
fbs_crowd_result fbs_crowd_create(const fbs_crowd_config *config, fbs_crowd **out) {
    fbs_crowd *ctx;
    int failed = 0;
    size_t hops;
    uint32_t i;
    if (!config || !out) return FBS_CROWD_INVALID;
    *out = NULL;
    if (config->max_workers == 0 || config->max_workers > 4194303u ||
        config->max_chains > 1048575u || config->max_stations > 1048575u ||
        config->max_nodes > 4096u || config->max_links > 65534u || config->resources == 0 ||
        config->resources > FBS_CROWD_MAX_RESOURCES || config->max_chain_slots > 0x7FFFFFFFu ||
        (config->max_chains > 0 && config->max_chain_slots == 0)) {
        return FBS_CROWD_INVALID;
    }
    ctx = (fbs_crowd *)calloc(1, sizeof *ctx);
    if (!ctx) return FBS_CROWD_MEMORY;
    ctx->cfg = *config;
    ctx->context_bytes = sizeof *ctx;
    table_init(ctx, &ctx->workers, config->max_workers, &failed);
    table_init(ctx, &ctx->chains, config->max_chains, &failed);
    table_init(ctx, &ctx->stations, config->max_stations, &failed);
    ctx->slot_item = (uint8_t *)crowd_alloc(ctx, config->max_chain_slots, 1, &failed);
    ctx->slot_timer = (uint32_t *)crowd_alloc(ctx, config->max_chain_slots, 4, &failed);
    ctx->slot_worker = (uint32_t *)crowd_alloc(ctx, config->max_chain_slots, 4, &failed);
    ctx->slot_reserved = (uint32_t *)crowd_alloc(ctx, config->max_chain_slots, 4, &failed);
    ctx->free_ranges = (range *)crowd_alloc(ctx, (size_t)config->max_chains + 2u, sizeof(range), &failed);
    ctx->node_pos = (fbs_crowd_vec3i *)crowd_alloc(ctx, config->max_nodes, sizeof(fbs_crowd_vec3i), &failed);
    ctx->node_room = (uint32_t *)crowd_alloc(ctx, config->max_nodes, 4, &failed);
    ctx->links = (fbs_crowd_link_desc *)crowd_alloc(ctx, config->max_links, sizeof(fbs_crowd_link_desc), &failed);
    ctx->link_occupancy = (uint32_t *)crowd_alloc(ctx, config->max_links, 4, &failed);
    ctx->link_head = (uint32_t *)crowd_alloc(ctx, config->max_links, 4, &failed);
    ctx->link_tail = (uint32_t *)crowd_alloc(ctx, config->max_links, 4, &failed);
    ctx->queued_links = (uint32_t *)crowd_alloc(ctx, config->max_links, 4, &failed);
    ctx->link_marked = (uint8_t *)crowd_alloc(ctx, config->max_links, 1, &failed);
    hops = (size_t)config->max_nodes * (size_t)config->max_nodes;
    ctx->next_hop = (uint16_t *)crowd_alloc(ctx, hops, sizeof(uint16_t), &failed);
    ctx->adj_offset = (uint32_t *)crowd_alloc(ctx, (size_t)config->max_nodes + 1u, 4, &failed);
    ctx->adj_link = (uint32_t *)crowd_alloc(ctx, (size_t)config->max_links * 2u, 4, &failed);
    ctx->dist = (uint64_t *)crowd_alloc(ctx, config->max_nodes, 8, &failed);
    ctx->heap = (uint32_t *)crowd_alloc(ctx, config->max_nodes, 4, &failed);
    ctx->heap_pos = (uint32_t *)crowd_alloc(ctx, config->max_nodes, 4, &failed);
    ctx->request_cap = config->max_workers + 2u * config->max_chains;
    ctx->requests = (request *)crowd_alloc(ctx, ctx->request_cap, sizeof(request), &failed);
    ctx->requests_tmp = (request *)crowd_alloc(ctx, ctx->request_cap, sizeof(request), &failed);
    ctx->link_requests = (link_request *)crowd_alloc(ctx, config->max_workers, sizeof(link_request), &failed);
    ctx->link_requests_tmp = (link_request *)crowd_alloc(ctx, config->max_workers, sizeof(link_request), &failed);
    if (failed) {
        fbs_crowd_destroy(ctx);
        return FBS_CROWD_MEMORY;
    }
    for (i = 0; i < config->max_chain_slots; ++i) ctx->slot_item[i] = ITEM_NONE;
    ranges_reset(ctx);

    ctx->world = ecs_mini();
    if (!ctx->world) {
        fbs_crowd_destroy(ctx);
        return FBS_CROWD_BACKEND;
    }
    ctx->c_worker = define_component(ctx->world, "FbsCrowdWorker", sizeof(CWorker), ECS_ALIGNOF(CWorker));
    ctx->c_task = define_component(ctx->world, "FbsCrowdTask", sizeof(CTask), ECS_ALIGNOF(CTask));
    ctx->c_chain = define_component(ctx->world, "FbsCrowdChain", sizeof(CChain), ECS_ALIGNOF(CChain));
    ctx->c_station = define_component(ctx->world, "FbsCrowdStation", sizeof(CStation), ECS_ALIGNOF(CStation));
    ctx->q_workers = cached_query(ctx->world, ctx->c_task, ctx->c_worker);
    ctx->q_chains = cached_query(ctx->world, ctx->c_chain, 0);
    ctx->q_stations = cached_query(ctx->world, ctx->c_station, 0);
    if (!ctx->c_worker || !ctx->c_task || !ctx->c_chain || !ctx->c_station || !ctx->q_workers ||
        !ctx->q_chains || !ctx->q_stations) {
        fbs_crowd_destroy(ctx);
        return FBS_CROWD_BACKEND;
    }
    /* Reserve the entity index for the configured population. */
    {
        uint64_t total = (uint64_t)config->max_workers + config->max_chains + config->max_stations;
        ecs_dim(ctx->world, (int32_t)(total > (uint64_t)INT32_MAX / 2 ? INT32_MAX / 2 : total));
    }
    *out = ctx;
    return FBS_CROWD_OK;
}

void fbs_crowd_destroy(fbs_crowd *ctx) {
    if (!ctx) return;
    if (ctx->world) ecs_fini(ctx->world); /* also finalises queries */
    table_free(&ctx->workers);
    table_free(&ctx->chains);
    table_free(&ctx->stations);
    free(ctx->slot_item);
    free(ctx->slot_timer);
    free(ctx->slot_worker);
    free(ctx->slot_reserved);
    free(ctx->free_ranges);
    free(ctx->node_pos);
    free(ctx->node_room);
    free(ctx->links);
    free(ctx->link_occupancy);
    free(ctx->link_head);
    free(ctx->link_tail);
    free(ctx->queued_links);
    free(ctx->link_marked);
    free(ctx->next_hop);
    free(ctx->adj_offset);
    free(ctx->adj_link);
    free(ctx->dist);
    free(ctx->heap);
    free(ctx->heap_pos);
    free(ctx->requests);
    free(ctx->requests_tmp);
    free(ctx->link_requests);
    free(ctx->link_requests_tmp);
    free(ctx);
}

/* ---- Graph ----------------------------------------------------------------- */
static uint32_t link_other(const fbs_crowd *ctx, uint32_t link, uint32_t node) {
    const fbs_crowd_link_desc *l = &ctx->links[link];
    return l->a == node ? l->b : l->a;
}

static int heap_less(const fbs_crowd *ctx, uint32_t a, uint32_t b) {
    if (ctx->dist[a] != ctx->dist[b]) return ctx->dist[a] < ctx->dist[b];
    return a < b;
}

static void heap_swap(fbs_crowd *ctx, uint32_t i, uint32_t j) {
    uint32_t t = ctx->heap[i];
    ctx->heap[i] = ctx->heap[j];
    ctx->heap[j] = t;
    ctx->heap_pos[ctx->heap[i]] = i;
    ctx->heap_pos[ctx->heap[j]] = j;
}

static void heap_up(fbs_crowd *ctx, uint32_t i) {
    while (i > 0) {
        uint32_t p = (i - 1u) / 2u;
        if (!heap_less(ctx, ctx->heap[i], ctx->heap[p])) break;
        heap_swap(ctx, i, p);
        i = p;
    }
}

static void heap_down(fbs_crowd *ctx, uint32_t n, uint32_t i) {
    for (;;) {
        uint32_t l = 2u * i + 1u, r = l + 1u, m = i;
        if (l < n && heap_less(ctx, ctx->heap[l], ctx->heap[m])) m = l;
        if (r < n && heap_less(ctx, ctx->heap[r], ctx->heap[m])) m = r;
        if (m == i) break;
        heap_swap(ctx, i, m);
        i = m;
    }
}

/* Dijkstra from every target; next_hop[u][t] is the link leaving u on a
 * shortest path to t, ties to the lower link index. */
static void build_next_hops(fbs_crowd *ctx) {
    const uint32_t n = ctx->node_count, stride = ctx->cfg.max_nodes;
    uint32_t t, i;
    for (t = 0; t < n; ++t) {
        uint32_t heap_n = 0;
        for (i = 0; i < n; ++i) {
            ctx->dist[i] = NEVER;
            ctx->heap_pos[i] = NONE;
            ctx->next_hop[(size_t)i * stride + t] = NO_HOP;
        }
        ctx->dist[t] = 0;
        ctx->heap[heap_n] = t;
        ctx->heap_pos[t] = heap_n++;
        while (heap_n > 0) {
            uint32_t u = ctx->heap[0], k;
            heap_swap(ctx, 0, heap_n - 1u);
            heap_n--;
            ctx->heap_pos[u] = NONE - 1u; /* settled */
            heap_down(ctx, heap_n, 0);
            for (k = ctx->adj_offset[u]; k < ctx->adj_offset[u + 1u]; ++k) {
                uint32_t link = ctx->adj_link[k];
                uint32_t v = link_other(ctx, link, u);
                uint64_t nd = ctx->dist[u] + ctx->links[link].length_mm;
                uint16_t *hop = &ctx->next_hop[(size_t)v * stride + t];
                if (ctx->heap_pos[v] == NONE - 1u) continue;
                if (nd < ctx->dist[v] || (nd == ctx->dist[v] && link < *hop)) {
                    ctx->dist[v] = nd;
                    *hop = (uint16_t)link;
                    if (ctx->heap_pos[v] == NONE) {
                        ctx->heap[heap_n] = v;
                        ctx->heap_pos[v] = heap_n++;
                    }
                    heap_up(ctx, ctx->heap_pos[v]);
                }
            }
        }
    }
}

static uint32_t hop(const fbs_crowd *ctx, uint32_t from, uint32_t to) {
    uint16_t h = ctx->next_hop[(size_t)from * ctx->cfg.max_nodes + to];
    return h == NO_HOP ? NONE : h;
}

static int reachable(const fbs_crowd *ctx, uint32_t from, uint32_t to) {
    return from == to || hop(ctx, from, to) != NONE;
}

static uint64_t isqrt64(uint64_t v) {
    uint64_t r = (uint64_t)sqrt((double)v);
    while (r && r > v / r) --r;
    while (r < UINT32_MAX && r + 1u <= v / (r + 1u)) ++r;
    return r;
}

static uint32_t derived_length(const fbs_crowd_vec3i *a, const fbs_crowd_vec3i *b) {
    const uint64_t limit = (uint64_t)UINT32_MAX * UINT32_MAX;
    const int64_t delta[3] = {(int64_t)b->x - a->x, (int64_t)b->y - a->y, (int64_t)b->z - a->z};
    uint64_t squared = 0, length;
    uint32_t i;
    for (i = 0; i < 3; ++i) {
        uint64_t distance = (uint64_t)(delta[i] < 0 ? -delta[i] : delta[i]);
        uint64_t square = distance * distance;
        /* Each unsigned square fits, but their sum need not. Clamp at the
         * link-length limit before adding, preserving every representable length. */
        if (square >= limit - squared) return UINT32_MAX;
        squared += square;
    }
    length = isqrt64(squared);
    if (length * length < squared) ++length;
    return length == 0 ? 1u : (uint32_t)length;
}

static int graph_busy(const fbs_crowd *ctx) {
    uint32_t i;
    for (i = 0; i < ctx->workers.high; ++i) {
        const CTask *t;
        if (!ctx->workers.entity[i]) continue;
        t = task_at(ctx, i);
        if (t->goal != NONE || t->link != NONE || t->intent != FBS_CROWD_INTENT_NONE) return 1;
    }
    return 0;
}

fbs_crowd_result fbs_crowd_set_graph(fbs_crowd *ctx, const fbs_crowd_node_desc *nodes,
                                     uint32_t node_count, const fbs_crowd_link_desc *links,
                                     uint32_t link_count) {
    uint32_t i;
    if (!ctx || node_count > ctx->cfg.max_nodes || link_count > ctx->cfg.max_links ||
        (node_count && !nodes) || (link_count && !links)) {
        return FBS_CROWD_INVALID;
    }
    for (i = 0; i < link_count; ++i) {
        if (links[i].a >= node_count || links[i].b >= node_count || links[i].a == links[i].b ||
            links[i].capacity == 0) {
            return FBS_CROWD_INVALID;
        }
    }
    if (graph_busy(ctx)) return FBS_CROWD_STATE;
    for (i = 0; i < ctx->workers.high; ++i) {
        if (ctx->workers.entity[i] && task_at(ctx, i)->node >= node_count) return FBS_CROWD_STATE;
    }
    for (i = 0; i < ctx->chains.high; ++i) {
        if (ctx->chains.entity[i] && chain_at(ctx, i)->access >= node_count) return FBS_CROWD_STATE;
    }
    for (i = 0; i < ctx->stations.high; ++i) {
        if (ctx->stations.entity[i] && station_at(ctx, i)->node >= node_count) return FBS_CROWD_STATE;
    }
    ctx->node_count = node_count;
    ctx->link_count = link_count;
    for (i = 0; i < node_count; ++i) {
        ctx->node_pos[i] = nodes[i].position;
        ctx->node_room[i] = nodes[i].room;
    }
    memset(ctx->adj_offset, 0, ((size_t)ctx->cfg.max_nodes + 1u) * sizeof(uint32_t));
    for (i = 0; i < link_count; ++i) {
        fbs_crowd_link_desc l = links[i];
        if (l.length_mm == 0) {
            l.length_mm = derived_length(&nodes[l.a].position, &nodes[l.b].position);
        }
        ctx->links[i] = l;
        ctx->link_occupancy[i] = 0;
        ctx->link_head[i] = 0;
        ctx->link_tail[i] = 0;
        ctx->link_marked[i] = 0;
        ctx->adj_offset[l.a + 1u]++;
        ctx->adj_offset[l.b + 1u]++;
    }
    for (i = 0; i < node_count; ++i) ctx->adj_offset[i + 1u] += ctx->adj_offset[i];
    {
        /* Fill adjacency in link order; heap_pos doubles as a write cursor. */
        for (i = 0; i < node_count; ++i) ctx->heap_pos[i] = ctx->adj_offset[i];
        for (i = 0; i < link_count; ++i) {
            ctx->adj_link[ctx->heap_pos[ctx->links[i].a]++] = i;
            ctx->adj_link[ctx->heap_pos[ctx->links[i].b]++] = i;
        }
    }
    build_next_hops(ctx);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_route_length(const fbs_crowd *ctx, uint32_t from, uint32_t to,
                                        uint64_t *length_mm) {
    uint64_t total = 0;
    uint32_t node = from, guard = 0;
    if (!ctx || !length_mm || from >= ctx->node_count || to >= ctx->node_count) {
        return FBS_CROWD_INVALID;
    }
    if (!reachable(ctx, from, to)) return FBS_CROWD_UNREACHABLE;
    while (node != to && guard++ <= ctx->node_count) {
        uint32_t l = hop(ctx, node, to);
        total += ctx->links[l].length_mm;
        node = link_other(ctx, l, node);
    }
    *length_mm = total;
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_node_position(const fbs_crowd *ctx, uint32_t node, float xyz[3]) {
    if (!ctx || !xyz || node >= ctx->node_count) return FBS_CROWD_INVALID;
    xyz[0] = (float)ctx->node_pos[node].x * 0.001f;
    xyz[1] = (float)ctx->node_pos[node].y * 0.001f;
    xyz[2] = (float)ctx->node_pos[node].z * 0.001f;
    return FBS_CROWD_OK;
}

/* ---- Link queues ------------------------------------------------------------ */
static void queue_push(fbs_crowd *ctx, uint32_t link, uint32_t windex, CTask *t) {
    uint32_t tail = ctx->link_tail[link];
    t->qprev = tail;
    t->qnext = 0;
    if (tail) task_at(ctx, tail - 1u)->qnext = windex + 1u;
    else ctx->link_head[link] = windex + 1u;
    ctx->link_tail[link] = windex + 1u;
}

static void queue_unlink(fbs_crowd *ctx, uint32_t link, CTask *t) {
    if (t->qprev) task_at(ctx, t->qprev - 1u)->qnext = t->qnext;
    else ctx->link_head[link] = t->qnext;
    if (t->qnext) task_at(ctx, t->qnext - 1u)->qprev = t->qprev;
    else ctx->link_tail[link] = t->qprev;
    t->qprev = t->qnext = 0;
}

/* Stop waiting for a link. Outside fbs_crowd_step every queued worker is in
 * its link's queue. A worker already on a link keeps it (finishes it). */
static void cancel_queue(fbs_crowd *ctx, CTask *t) {
    if (t->link != NONE && t->queued) {
        queue_unlink(ctx, t->link, t);
        t->link = NONE;
        t->queued = 0;
    }
}

/* ---- Worker job transitions -------------------------------------------------- */
static void leave_job(fbs_crowd *ctx, uint32_t windex, CTask *t) {
    if (t->intent == FBS_CROWD_INTENT_SLOT && t->chain != NONE) {
        if (ctx->slot_worker[t->slot_global] == windex + 1u) ctx->slot_worker[t->slot_global] = 0;
        if (ctx->slot_reserved[t->slot_global] == windex + 1u) ctx->slot_reserved[t->slot_global] = 0;
    }
    if (t->intent == FBS_CROWD_INTENT_HAUL) {
        station_at(ctx, t->pickup)->refs--;
        station_at(ctx, t->dropoff)->refs--;
    }
    t->intent = FBS_CROWD_INTENT_NONE;
    t->chain = NONE;
    t->slot = NONE;
    t->slot_global = NONE;
    t->pickup = t->dropoff = NONE;
    t->haul_phase = 0;
    t->wait_since = NEVER;
}

/* Node the worker will stand at when it can next choose a link. */
static uint32_t decision_node(const fbs_crowd *ctx, const CTask *t) {
    if (t->link != NONE && !t->queued) return link_other(ctx, t->link, t->node);
    return t->node;
}

static void set_goal(fbs_crowd *ctx, CTask *t, uint32_t goal) {
    cancel_queue(ctx, t);
    t->goal = goal;
    t->state = FBS_CROWD_TRAVEL;
}

/* ---- Entities ------------------------------------------------------------------ */
fbs_crowd_result fbs_crowd_add_station(fbs_crowd *ctx, const fbs_crowd_station_desc *d,
                                       fbs_crowd_handle *out) {
    uint32_t index;
    ecs_entity_t e;
    CStation *s;
    if (!ctx || !d || !out) return FBS_CROWD_INVALID;
    if (d->node >= ctx->node_count || d->cycle_ticks == 0 ||
        (d->input_resource == NONE && d->output_resource == NONE)) {
        return FBS_CROWD_INVALID;
    }
    if (d->input_resource != NONE &&
        (d->input_resource >= ctx->cfg.resources || d->input_per_cycle == 0 ||
         d->input_capacity < d->input_per_cycle)) {
        return FBS_CROWD_INVALID;
    }
    if (d->output_resource != NONE &&
        (d->output_resource >= ctx->cfg.resources || d->output_per_cycle == 0 ||
         d->output_capacity < d->output_per_cycle || d->initial_output > d->output_capacity)) {
        return FBS_CROWD_INVALID;
    }
    if (d->output_resource == NONE && d->initial_output) return FBS_CROWD_INVALID;
    if (!table_acquire(&ctx->stations, &index)) return FBS_CROWD_CAPACITY;
    e = new_entity(ctx, ctx->c_station, 0);
    if (!e) {
        ctx->stations.gen[index] -= 1u;
        ctx->stations.live--;
        ctx->stations.free_list[ctx->stations.free_count++] = index;
        return FBS_CROWD_BACKEND;
    }
    ctx->stations.entity[index] = e;
    s = STATION(ctx, e);
    memset(s, 0, sizeof *s);
    s->index = index;
    s->node = d->node;
    s->in_res = d->input_resource;
    s->in_per = d->input_per_cycle;
    s->in_cap = d->input_capacity;
    s->out_res = d->output_resource;
    s->out_per = d->output_per_cycle;
    s->out_cap = d->output_capacity;
    s->cycle = d->cycle_ticks;
    s->out_stock = d->initial_output;
    s->tag = d->tag;
    if (d->initial_output) ctx->introduced[d->output_resource] += d->initial_output;
    *out = make_handle(KIND_STATION, index, ctx->stations.gen[index]);
    return FBS_CROWD_OK;
}

static void export_stock(fbs_crowd *ctx, uint32_t res, uint32_t amount, uint32_t *exported) {
    if (res == NONE || amount == 0) return;
    ctx->exported[res] += amount;
    if (exported) exported[res] += amount;
}

fbs_crowd_result fbs_crowd_remove_station(fbs_crowd *ctx, fbs_crowd_handle h, uint32_t *exported) {
    uint32_t index;
    ecs_entity_t e;
    CStation *s;
    if (!resolve(ctx, h, KIND_STATION, &index, &e)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    s = STATION(ctx, e);
    if (s->refs) return FBS_CROWD_STATE;
    if (exported) memset(exported, 0, ctx->cfg.resources * sizeof(uint32_t));
    export_stock(ctx, s->in_res, s->in_stock, exported);
    export_stock(ctx, s->out_res, s->out_stock, exported);
    ecs_delete(ctx->world, e);
    table_release(&ctx->stations, index);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_station_get(const fbs_crowd *ctx, fbs_crowd_handle h,
                                       fbs_crowd_station_info *out) {
    uint32_t index;
    const CStation *s;
    if (!out) return FBS_CROWD_INVALID;
    if (!resolve(ctx, h, KIND_STATION, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    s = station_at(ctx, index);
    out->tag = s->tag;
    out->node = s->node;
    out->input_stock = s->in_stock;
    out->output_stock = s->out_stock;
    out->cycle_progress = s->running ? s->progress : 0;
    out->cycles = s->cycles;
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_add_chain(fbs_crowd *ctx, const fbs_crowd_chain_desc *d,
                                     fbs_crowd_handle *out) {
    uint32_t index, src, dst, offset, i;
    ecs_entity_t e;
    CChain *c;
    CStation *s, *t;
    if (!ctx || !d || !out) return FBS_CROWD_INVALID;
    if (d->slot_count == 0 || d->handoff_ticks == 0 || d->handoff_ticks > 0xFFFFu ||
        d->access_node >= ctx->node_count || d->resource >= ctx->cfg.resources) {
        return FBS_CROWD_INVALID;
    }
    if (!resolve(ctx, d->source, KIND_STATION, &src, NULL) ||
        !resolve(ctx, d->destination, KIND_STATION, &dst, NULL)) {
        return FBS_CROWD_STALE;
    }
    s = station_at(ctx, src);
    t = station_at(ctx, dst);
    if (src == dst || s->out_res != d->resource || t->in_res != d->resource) return FBS_CROWD_INVALID;
    if (ctx->chains.live >= ctx->chains.cap || d->slot_count > ctx->cfg.max_chain_slots - ctx->slots_used) {
        return FBS_CROWD_CAPACITY;
    }
    if (!ranges_acquire(ctx, d->slot_count, &offset)) return FBS_CROWD_CAPACITY; /* fragmented */
    if (!table_acquire(&ctx->chains, &index)) {
        ranges_release(ctx, offset, d->slot_count);
        return FBS_CROWD_CAPACITY;
    }
    e = new_entity(ctx, ctx->c_chain, 0);
    if (!e) {
        ranges_release(ctx, offset, d->slot_count);
        ctx->chains.gen[index] -= 1u;
        ctx->chains.live--;
        ctx->chains.free_list[ctx->chains.free_count++] = index;
        return FBS_CROWD_BACKEND;
    }
    ctx->chains.entity[index] = e;
    c = CHAIN(ctx, e);
    memset(c, 0, sizeof *c);
    c->index = index;
    c->source = src;
    c->dest = dst;
    c->resource = d->resource;
    c->offset = offset;
    c->count = d->slot_count;
    c->handoff = d->handoff_ticks;
    c->access = d->access_node;
    c->room = d->room;
    c->start[0] = d->start.x;
    c->start[1] = d->start.y;
    c->start[2] = d->start.z;
    c->end[0] = d->end.x;
    c->end[1] = d->end.y;
    c->end[2] = d->end.z;
    c->tag = d->tag;
    c->take_since = c->put_since = NEVER;
    for (i = 0; i < d->slot_count; ++i) {
        ctx->slot_item[offset + i] = ITEM_NONE;
        ctx->slot_timer[offset + i] = 0;
        ctx->slot_worker[offset + i] = 0;
        ctx->slot_reserved[offset + i] = 0;
    }
    station_at(ctx, src)->refs++;
    station_at(ctx, dst)->refs++;
    *out = make_handle(KIND_CHAIN, index, ctx->chains.gen[index]);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_remove_chain(fbs_crowd *ctx, fbs_crowd_handle h, uint32_t *exported) {
    uint32_t index, i;
    ecs_entity_t e;
    CChain c;
    if (!resolve(ctx, h, KIND_CHAIN, &index, &e)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    c = *CHAIN(ctx, e);
    if (exported) memset(exported, 0, ctx->cfg.resources * sizeof(uint32_t));
    for (i = 0; i < c.count; ++i) {
        uint32_t g = c.offset + i;
        if (ctx->slot_item[g] != ITEM_NONE) export_stock(ctx, ctx->slot_item[g], 1, exported);
        ctx->slot_item[g] = ITEM_NONE;
        if (ctx->slot_worker[g]) {
            uint32_t w = ctx->slot_worker[g] - 1u;
            CTask *t = task_at(ctx, w);
            leave_job(ctx, w, t);
            t->state = FBS_CROWD_IDLE;
            t->node = c.access;
        }
        if (ctx->slot_reserved[g]) {
            uint32_t w = ctx->slot_reserved[g] - 1u;
            CTask *t = task_at(ctx, w);
            leave_job(ctx, w, t);
            t->intent = FBS_CROWD_INTENT_MOVE; /* continue to the access node, then idle */
        }
        ctx->slot_worker[g] = 0;
        ctx->slot_reserved[g] = 0;
    }
    station_at(ctx, c.source)->refs--;
    station_at(ctx, c.dest)->refs--;
    ranges_release(ctx, c.offset, c.count);
    ecs_delete(ctx->world, e);
    table_release(&ctx->chains, index);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_chain_get(const fbs_crowd *ctx, fbs_crowd_handle h,
                                     fbs_crowd_chain_info *out) {
    uint32_t index, i;
    const CChain *c;
    if (!out) return FBS_CROWD_INVALID;
    if (!resolve(ctx, h, KIND_CHAIN, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    c = chain_at(ctx, index);
    memset(out, 0, sizeof *out);
    out->tag = c->tag;
    out->slot_count = c->count;
    out->taken = c->taken;
    out->delivered = c->delivered;
    for (i = 0; i < c->count; ++i) {
        if (ctx->slot_worker[c->offset + i]) out->staffed++;
        if (ctx->slot_item[c->offset + i] != ITEM_NONE) out->items++;
    }
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_slot_get(const fbs_crowd *ctx, fbs_crowd_handle h, uint32_t slot,
                                    fbs_crowd_slot_info *out) {
    uint32_t index, g;
    const CChain *c;
    if (!out) return FBS_CROWD_INVALID;
    if (!resolve(ctx, h, KIND_CHAIN, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    c = chain_at(ctx, index);
    if (slot >= c->count) return FBS_CROWD_INVALID;
    g = c->offset + slot;
    out->worker = ctx->slot_worker[g] ? handle_of(&ctx->workers, KIND_WORKER, ctx->slot_worker[g] - 1u) : 0;
    out->reserved = ctx->slot_reserved[g] ? handle_of(&ctx->workers, KIND_WORKER, ctx->slot_reserved[g] - 1u) : 0;
    out->item = ctx->slot_item[g] == ITEM_NONE ? NONE : ctx->slot_item[g];
    out->timer = ctx->slot_timer[g];
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_add_worker(fbs_crowd *ctx, const fbs_crowd_worker_desc *d,
                                      fbs_crowd_handle *out) {
    uint32_t index;
    ecs_entity_t e;
    CWorker *w;
    CTask *t;
    if (!ctx || !d || !out) return FBS_CROWD_INVALID;
    if (d->node >= ctx->node_count || d->speed_mm_per_tick == 0 || d->variant > 255u) {
        return FBS_CROWD_INVALID;
    }
    if (!table_acquire(&ctx->workers, &index)) return FBS_CROWD_CAPACITY;
    e = new_entity(ctx, ctx->c_task, ctx->c_worker);
    if (!e) {
        ctx->workers.gen[index] -= 1u;
        ctx->workers.live--;
        ctx->workers.free_list[ctx->workers.free_count++] = index;
        return FBS_CROWD_BACKEND;
    }
    ctx->workers.entity[index] = e;
    w = WORKER(ctx, e);
    memset(w, 0, sizeof *w);
    w->index = index;
    w->speed = d->speed_mm_per_tick;
    w->variant = d->variant;
    w->tag = d->tag;
    t = TASK(ctx, e);
    memset(t, 0, sizeof *t);
    t->state = FBS_CROWD_IDLE;
    t->intent = FBS_CROWD_INTENT_NONE;
    t->node = d->node;
    t->goal = t->link = NONE;
    t->chain = t->slot = t->slot_global = NONE;
    t->pickup = t->dropoff = t->resource = NONE;
    t->cargo_res = NONE;
    t->wait_since = NEVER;
    *out = make_handle(KIND_WORKER, index, ctx->workers.gen[index]);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_remove_worker(fbs_crowd *ctx, fbs_crowd_handle h, uint32_t *resource,
                                         uint32_t *amount) {
    uint32_t index;
    ecs_entity_t e;
    CTask *t;
    if (!resolve(ctx, h, KIND_WORKER, &index, &e)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    t = TASK(ctx, e);
    leave_job(ctx, index, t);
    if (t->link != NONE) {
        if (t->queued) cancel_queue(ctx, t);
        else ctx->link_occupancy[t->link]--;
    }
    if (resource) *resource = t->cargo_amt ? t->cargo_res : NONE;
    if (amount) *amount = t->cargo_amt;
    if (t->cargo_amt) ctx->exported[t->cargo_res] += t->cargo_amt;
    ecs_delete(ctx->world, e);
    table_release(&ctx->workers, index);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_worker_get(const fbs_crowd *ctx, fbs_crowd_handle h,
                                      fbs_crowd_worker_info *out) {
    uint32_t index;
    const CTask *t;
    const CWorker *w;
    if (!out) return FBS_CROWD_INVALID;
    if (!resolve(ctx, h, KIND_WORKER, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    t = task_at(ctx, index);
    w = (const CWorker *)ecs_get_id(ctx->world, ctx->workers.entity[index], ctx->c_worker);
    memset(out, 0, sizeof *out);
    out->tag = w->tag;
    out->variant = w->variant;
    out->state = t->state;
    out->intent = t->intent;
    out->node = t->node;
    out->link = t->link;
    out->progress_mm = t->progress;
    out->queued = t->link != NONE && t->queued;
    out->goal_node = t->goal;
    out->chain = t->chain != NONE ? handle_of(&ctx->chains, KIND_CHAIN, t->chain) : 0;
    out->slot = t->slot;
    out->pickup = t->pickup != NONE ? handle_of(&ctx->stations, KIND_STATION, t->pickup) : 0;
    out->dropoff = t->dropoff != NONE ? handle_of(&ctx->stations, KIND_STATION, t->dropoff) : 0;
    out->cargo_resource = t->cargo_amt ? t->cargo_res : NONE;
    out->cargo_amount = t->cargo_amt;
    return FBS_CROWD_OK;
}

/* ---- Commands ------------------------------------------------------------------ */
fbs_crowd_result fbs_crowd_command_move(fbs_crowd *ctx, fbs_crowd_handle h, uint32_t node) {
    uint32_t index;
    CTask *t;
    if (!resolve(ctx, h, KIND_WORKER, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    if (node >= ctx->node_count) return FBS_CROWD_INVALID;
    t = task_at(ctx, index);
    if (t->cargo_amt) return FBS_CROWD_STATE;
    if (!reachable(ctx, decision_node(ctx, t), node)) return FBS_CROWD_UNREACHABLE;
    leave_job(ctx, index, t);
    t->intent = FBS_CROWD_INTENT_MOVE;
    set_goal(ctx, t, node);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_command_slot(fbs_crowd *ctx, fbs_crowd_handle h, fbs_crowd_handle chain,
                                        uint32_t slot) {
    uint32_t index, cindex, g;
    const CChain *c;
    CTask *t;
    if (!resolve(ctx, h, KIND_WORKER, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    if (!resolve(ctx, chain, KIND_CHAIN, &cindex, NULL)) return FBS_CROWD_STALE;
    c = chain_at(ctx, cindex);
    if (slot >= c->count) return FBS_CROWD_INVALID;
    g = c->offset + slot;
    t = task_at(ctx, index);
    if (ctx->slot_worker[g] == index + 1u || ctx->slot_reserved[g] == index + 1u) return FBS_CROWD_OK;
    if (ctx->slot_worker[g] || ctx->slot_reserved[g] || t->cargo_amt) return FBS_CROWD_STATE;
    if (!reachable(ctx, decision_node(ctx, t), c->access)) return FBS_CROWD_UNREACHABLE;
    leave_job(ctx, index, t);
    t->intent = FBS_CROWD_INTENT_SLOT;
    t->chain = cindex;
    t->slot = slot;
    t->slot_global = g;
    t->handoff = c->handoff;
    t->room = c->room;
    {
        int64_t n2 = 2 * (int64_t)c->count, k = 2 * (int64_t)slot + 1;
        t->sx = (int32_t)(c->start[0] + ((int64_t)c->end[0] - c->start[0]) * k / n2);
        t->sy = (int32_t)(c->start[1] + ((int64_t)c->end[1] - c->start[1]) * k / n2);
        t->sz = (int32_t)(c->start[2] + ((int64_t)c->end[2] - c->start[2]) * k / n2);
        t->sdx = (int32_t)(((int64_t)c->end[0] - c->start[0]) / 2);
        t->sdz = (int32_t)(((int64_t)c->end[2] - c->start[2]) / 2);
    }
    ctx->slot_reserved[g] = index + 1u;
    set_goal(ctx, t, c->access);
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_command_haul(fbs_crowd *ctx, fbs_crowd_handle h, fbs_crowd_handle pickup,
                                        fbs_crowd_handle dropoff, uint32_t resource) {
    uint32_t index, p, d, from;
    CStation *ps, *ds;
    CTask *t;
    if (!resolve(ctx, h, KIND_WORKER, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    if (!resolve(ctx, pickup, KIND_STATION, &p, NULL) || !resolve(ctx, dropoff, KIND_STATION, &d, NULL)) {
        return FBS_CROWD_STALE;
    }
    if (resource >= ctx->cfg.resources || p == d) return FBS_CROWD_INVALID;
    ps = station_at(ctx, p);
    ds = station_at(ctx, d);
    if (ps->out_res != resource || ds->in_res != resource) return FBS_CROWD_INVALID;
    t = task_at(ctx, index);
    if (t->cargo_amt && t->cargo_res != resource) return FBS_CROWD_STATE;
    from = decision_node(ctx, t);
    if (!reachable(ctx, from, ps->node) || !reachable(ctx, from, ds->node) ||
        !reachable(ctx, ps->node, ds->node)) {
        return FBS_CROWD_UNREACHABLE;
    }
    leave_job(ctx, index, t);
    ps->refs++;
    ds->refs++;
    t->intent = FBS_CROWD_INTENT_HAUL;
    t->pickup = p;
    t->dropoff = d;
    t->resource = resource;
    t->haul_phase = t->cargo_amt ? HAUL_TO_DROP : HAUL_TO_PICKUP;
    set_goal(ctx, t, t->cargo_amt ? ds->node : ps->node);
    t->state = FBS_CROWD_HAUL;
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_command_idle(fbs_crowd *ctx, fbs_crowd_handle h) {
    uint32_t index;
    CTask *t;
    if (!resolve(ctx, h, KIND_WORKER, &index, NULL)) return ctx ? FBS_CROWD_STALE : FBS_CROWD_INVALID;
    t = task_at(ctx, index);
    leave_job(ctx, index, t);
    cancel_queue(ctx, t);
    if (t->link != NONE) {
        t->intent = FBS_CROWD_INTENT_MOVE; /* finish the link, then idle */
        t->goal = link_other(ctx, t->link, t->node);
        t->state = FBS_CROWD_TRAVEL;
    } else {
        t->goal = NONE;
        t->state = FBS_CROWD_IDLE;
    }
    return FBS_CROWD_OK;
}

fbs_crowd_result fbs_crowd_command_slots(fbs_crowd *ctx, const fbs_crowd_slot_command *cmds,
                                         uint32_t count, fbs_crowd_result *results) {
    fbs_crowd_result first = FBS_CROWD_OK;
    uint32_t i;
    if (!ctx || (count && !cmds)) return FBS_CROWD_INVALID;
    for (i = 0; i < count; ++i) {
        fbs_crowd_result r = fbs_crowd_command_slot(ctx, cmds[i].worker, cmds[i].chain, cmds[i].slot);
        if (results) results[i] = r;
        if (r != FBS_CROWD_OK && first == FBS_CROWD_OK) first = r;
    }
    return first;
}

fbs_crowd_result fbs_crowd_command_moves(fbs_crowd *ctx, const fbs_crowd_handle *workers,
                                         uint32_t count, uint32_t node, fbs_crowd_result *results) {
    fbs_crowd_result first = FBS_CROWD_OK;
    uint32_t i;
    if (!ctx || (count && !workers)) return FBS_CROWD_INVALID;
    for (i = 0; i < count; ++i) {
        fbs_crowd_result r = fbs_crowd_command_move(ctx, workers[i], node);
        if (results) results[i] = r;
        if (r != FBS_CROWD_OK && first == FBS_CROWD_OK) first = r;
    }
    return first;
}

/* ---- Tick ------------------------------------------------------------------------ */
static void push_request(fbs_crowd *ctx, uint32_t station, uint32_t op, uint64_t since,
                         uint32_t kind, uint32_t index) {
    request *r = &ctx->requests[ctx->request_count++];
    r->station = station;
    r->op = op;
    r->since = since;
    r->kind = kind;
    r->index = index;
}

/* Keys are unique, so any correct sort gives the same order on every
 * platform. Bottom-up merge sort into preallocated scratch: no allocation. */
static int request_less(const request *x, const request *y) {
    if (x->station != y->station) return x->station < y->station;
    if (x->op != y->op) return x->op < y->op;
    if (x->since != y->since) return x->since < y->since;
    if (x->kind != y->kind) return x->kind < y->kind;
    return x->index < y->index;
}

static int link_request_less(const link_request *x, const link_request *y) {
    if (x->link != y->link) return x->link < y->link;
    return x->worker < y->worker;
}

#define DEFINE_MERGE_SORT(NAME, TYPE, LESS)                                              \
    static void NAME(TYPE *a, TYPE *tmp, uint32_t n) {                                   \
        uint32_t width;                                                                  \
        TYPE *src = a, *dst = tmp;                                                       \
        for (width = 1; width < n; width *= 2u) {                                        \
            uint32_t lo;                                                                 \
            for (lo = 0; lo < n; lo += 2u * width) {                                     \
                uint32_t mid = lo + width < n ? lo + width : n;                          \
                uint32_t hi = lo + 2u * width < n ? lo + 2u * width : n;                 \
                uint32_t i = lo, j = mid, k = lo;                                        \
                while (i < mid && j < hi) dst[k++] = LESS(&src[j], &src[i]) ? src[j++] : src[i++]; \
                while (i < mid) dst[k++] = src[i++];                                     \
                while (j < hi) dst[k++] = src[j++];                                      \
            }                                                                            \
            { TYPE *t = src; src = dst; dst = t; }                                       \
            if (width > n / 2u) break;                                                   \
        }                                                                                \
        if (src != a) memcpy(a, src, (size_t)n * sizeof(TYPE));                          \
    }
DEFINE_MERGE_SORT(sort_requests, request, request_less)
DEFINE_MERGE_SORT(sort_link_requests, link_request, link_request_less)

/* Phase 1: stations run their cycles. Independent per station. */
static void phase_stations(fbs_crowd *ctx) {
    ecs_iter_t it = ecs_query_iter(ctx->world, ctx->q_stations);
    while (ecs_query_next(&it)) {
        CStation *s = (CStation *)ecs_field_w_size(&it, sizeof(CStation), 0);
        int32_t i;
        for (i = 0; i < it.count; ++i) {
            CStation *st = &s[i];
            if (!st->running) {
                int has_in = st->in_res == NONE || st->in_stock >= st->in_per;
                int has_room = st->out_res == NONE || st->out_stock + st->out_per <= st->out_cap;
                if (has_in && has_room) {
                    if (st->in_res != NONE) {
                        st->in_stock -= st->in_per;
                        ctx->consumed[st->in_res] += st->in_per;
                    }
                    st->running = 1;
                    st->progress = 0;
                }
            }
            if (st->running && ++st->progress >= st->cycle) {
                if (st->out_res != NONE) {
                    st->out_stock += st->out_per;
                    ctx->introduced[st->out_res] += st->out_per;
                }
                st->cycles++;
                st->running = 0;
                st->progress = 0;
            }
        }
    }
}

/* Phase 2: items move hand to hand inside each chain (independent per chain);
 * station transfers become requests. */
static void phase_chains(fbs_crowd *ctx) {
    ecs_iter_t it = ecs_query_iter(ctx->world, ctx->q_chains);
    while (ecs_query_next(&it)) {
        CChain *cs = (CChain *)ecs_field_w_size(&it, sizeof(CChain), 0);
        int32_t k;
        for (k = 0; k < it.count; ++k) {
            CChain *c = &cs[k];
            uint8_t *item = ctx->slot_item + c->offset;
            uint32_t *timer = ctx->slot_timer + c->offset;
            const uint32_t *staff = ctx->slot_worker + c->offset;
            uint32_t n = c->count, i = n;
            int want_put = 0;
            while (i-- > 0) {
                if (item[i] == ITEM_NONE || !staff[i]) continue;
                if (timer[i] > 0) timer[i]--;
                if (timer[i] > 0) continue;
                if (i + 1u == n) {
                    want_put = 1;
                } else if (staff[i + 1u] && item[i + 1u] == ITEM_NONE) {
                    item[i + 1u] = item[i];
                    timer[i + 1u] = c->handoff;
                    item[i] = ITEM_NONE;
                }
            }
            if (want_put) {
                if (c->put_since == NEVER) c->put_since = ctx->tick;
                push_request(ctx, c->dest, OP_PUT, c->put_since, REQ_CHAIN, c->index);
            } else {
                c->put_since = NEVER;
            }
            if (item[0] == ITEM_NONE && staff[0]) {
                if (c->take_since == NEVER) c->take_since = ctx->tick;
                push_request(ctx, c->source, OP_TAKE, c->take_since, REQ_CHAIN, c->index);
            } else {
                c->take_since = NEVER;
            }
        }
    }
}

static void request_link(fbs_crowd *ctx, uint32_t windex, CTask *t) {
    uint32_t l = hop(ctx, t->node, t->goal);
    link_request *r;
    if (l == NONE) { /* graph unchanged while moving, so this cannot happen */
        t->goal = NONE;
        t->state = FBS_CROWD_BLOCKED;
        return;
    }
    t->link = l;
    t->queued = 1;
    t->progress = 0;
    r = &ctx->link_requests[ctx->link_request_count++];
    r->link = l;
    r->worker = windex;
}

/* Independent per worker: arrival handling that touches only this worker or a
 * slot reserved exclusively for it. */
static void arrive(fbs_crowd *ctx, uint32_t windex, CTask *t) {
    t->goal = NONE;
    switch (t->intent) {
    case FBS_CROWD_INTENT_SLOT:
        if (t->chain != NONE && ctx->slot_reserved[t->slot_global] == windex + 1u) {
            ctx->slot_reserved[t->slot_global] = 0;
            ctx->slot_worker[t->slot_global] = windex + 1u;
            t->state = FBS_CROWD_SLOT;
        } else {
            t->intent = FBS_CROWD_INTENT_NONE;
            t->state = FBS_CROWD_IDLE;
        }
        break;
    case FBS_CROWD_INTENT_HAUL:
        t->state = FBS_CROWD_HAUL;
        if (t->haul_phase == HAUL_TO_PICKUP) t->haul_phase = HAUL_WAIT_PICKUP;
        else if (t->haul_phase == HAUL_TO_DROP) t->haul_phase = HAUL_WAIT_DROP;
        t->wait_since = ctx->tick;
        break;
    default:
        t->intent = FBS_CROWD_INTENT_NONE;
        t->state = FBS_CROWD_IDLE;
        break;
    }
}

/* Phase 3: workers move; interactions become requests. */
static void phase_workers(fbs_crowd *ctx) {
    ecs_iter_t it = ecs_query_iter(ctx->world, ctx->q_workers);
    while (ecs_query_next(&it)) {
        CTask *ts = (CTask *)ecs_field_w_size(&it, sizeof(CTask), 0);
        const CWorker *ws = (const CWorker *)ecs_field_w_size(&it, sizeof(CWorker), 1);
        int32_t i;
        for (i = 0; i < it.count; ++i) {
            CTask *t = &ts[i];
            uint32_t windex = ws[i].index;
            if (t->goal != NONE) {
                if (t->link != NONE) {
                    if (!t->queued) {
                        uint32_t len = ctx->links[t->link].length_mm;
                        uint64_t p = (uint64_t)t->progress + ws[i].speed;
                        if (p >= len) {
                            ctx->link_occupancy[t->link]--;
                            t->node = link_other(ctx, t->link, t->node);
                            t->link = NONE;
                            t->progress = 0;
                            if (t->node == t->goal) arrive(ctx, windex, t);
                            else request_link(ctx, windex, t);
                        } else {
                            t->progress = (uint32_t)p;
                        }
                    }
                } else if (t->node == t->goal) {
                    arrive(ctx, windex, t);
                } else {
                    request_link(ctx, windex, t);
                }
            }
            if (t->intent == FBS_CROWD_INTENT_HAUL && t->goal == NONE) {
                if (t->haul_phase == HAUL_WAIT_PICKUP) {
                    push_request(ctx, t->pickup, OP_TAKE, t->wait_since, REQ_HAULER, windex);
                } else if (t->haul_phase == HAUL_WAIT_DROP) {
                    push_request(ctx, t->dropoff, OP_PUT, t->wait_since, REQ_HAULER, windex);
                }
            }
        }
    }
}

/* Phase 4: station requests in (station, op, since, kind, index) order. */
static void phase_arbitrate(fbs_crowd *ctx) {
    uint32_t i;
    sort_requests(ctx->requests, ctx->requests_tmp, ctx->request_count);
    for (i = 0; i < ctx->request_count; ++i) {
        const request *r = &ctx->requests[i];
        CStation *s = station_at(ctx, r->station);
        if (r->kind == REQ_CHAIN) {
            CChain *c = chain_at(ctx, r->index);
            if (r->op == OP_TAKE) {
                if (s->out_stock == 0) continue;
                s->out_stock--;
                ctx->slot_item[c->offset] = (uint8_t)c->resource;
                ctx->slot_timer[c->offset] = c->handoff;
                c->taken++;
                c->take_since = NEVER;
            } else {
                uint32_t last = c->offset + c->count - 1u;
                if (s->in_stock >= s->in_cap) continue;
                s->in_stock++;
                ctx->slot_item[last] = ITEM_NONE;
                ctx->slot_timer[last] = 0;
                c->delivered++;
                c->put_since = NEVER;
            }
        } else {
            CTask *t = task_at(ctx, r->index);
            if (r->op == OP_TAKE) {
                if (s->out_stock == 0) continue;
                s->out_stock--;
                t->cargo_res = t->resource;
                t->cargo_amt = 1;
                t->haul_phase = HAUL_TO_DROP;
                t->goal = station_at(ctx, t->dropoff)->node;
            } else {
                if (s->in_stock >= s->in_cap) continue;
                s->in_stock++;
                t->cargo_res = NONE;
                t->cargo_amt = 0;
                t->haul_phase = HAUL_TO_PICKUP;
                t->goal = station_at(ctx, t->pickup)->node;
            }
            t->wait_since = NEVER;
        }
    }
    ctx->requests_last_tick = ctx->request_count;
    ctx->request_count = 0;
}

/* Phase 5: link entries in (link, worker) order join FIFO queues; queues
 * admit in link order while capacity allows. */
static void phase_links(fbs_crowd *ctx) {
    uint32_t i;
    sort_link_requests(ctx->link_requests, ctx->link_requests_tmp, ctx->link_request_count);
    for (i = 0; i < ctx->link_request_count; ++i) {
        const link_request *r = &ctx->link_requests[i];
        queue_push(ctx, r->link, r->worker, task_at(ctx, r->worker));
    }
    ctx->requests_last_tick += ctx->link_request_count;
    ctx->link_request_count = 0;
    for (i = 0; i < ctx->link_count; ++i) {
        while (ctx->link_head[i] && ctx->link_occupancy[i] < ctx->links[i].capacity) {
            uint32_t w = ctx->link_head[i] - 1u;
            CTask *t = task_at(ctx, w);
            queue_unlink(ctx, i, t);
            t->queued = 0;
            t->progress = 0;
            ctx->link_occupancy[i]++;
        }
    }
}

fbs_crowd_result fbs_crowd_step(fbs_crowd *ctx, uint32_t ticks) {
    uint32_t k;
    if (!ctx) return FBS_CROWD_INVALID;
    for (k = 0; k < ticks; ++k) {
        ctx->requests_last_tick = 0;
        phase_stations(ctx);
        phase_chains(ctx);
        phase_workers(ctx);
        phase_arbitrate(ctx);
        phase_links(ctx);
        ctx->tick++;
    }
    return FBS_CROWD_OK;
}

uint64_t fbs_crowd_tick(const fbs_crowd *ctx) { return ctx ? ctx->tick : 0; }

/* ---- Reset ------------------------------------------------------------------------ */
static void table_reset(fbs_crowd *ctx, handle_table *t) {
    uint32_t i;
    for (i = 0; i < t->high; ++i) {
        if (t->entity[i]) {
            ecs_delete(ctx->world, t->entity[i]);
            t->entity[i] = 0;
        }
    }
    t->live = 0;
    t->free_count = 0;
    /* Descending so that later allocations reuse ascending indices. */
    i = t->high;
    while (i-- > 0) {
        if (t->gen[i] < GEN_MAX) t->free_list[t->free_count++] = i;
    }
}

fbs_crowd_result fbs_crowd_reset(fbs_crowd *ctx) {
    uint32_t i;
    if (!ctx) return FBS_CROWD_INVALID;
    table_reset(ctx, &ctx->workers);
    table_reset(ctx, &ctx->chains);
    table_reset(ctx, &ctx->stations);
    for (i = 0; i < ctx->cfg.max_chain_slots; ++i) {
        ctx->slot_item[i] = ITEM_NONE;
        ctx->slot_timer[i] = ctx->slot_worker[i] = ctx->slot_reserved[i] = 0;
    }
    ranges_reset(ctx);
    for (i = 0; i < ctx->link_count; ++i) {
        ctx->link_occupancy[i] = ctx->link_head[i] = ctx->link_tail[i] = 0;
    }
    memset(ctx->introduced, 0, sizeof ctx->introduced);
    memset(ctx->consumed, 0, sizeof ctx->consumed);
    memset(ctx->exported, 0, sizeof ctx->exported);
    ctx->tick = 0;
    ctx->request_count = ctx->link_request_count = 0;
    ctx->requests_last_tick = 0;
    return FBS_CROWD_OK;
}

/* ---- Ledgers ------------------------------------------------------------------------ */
fbs_crowd_result fbs_crowd_resource_totals(const fbs_crowd *ctx, uint32_t resource,
                                           fbs_crowd_totals *out) {
    uint32_t i;
    if (!ctx || !out || resource >= ctx->cfg.resources) return FBS_CROWD_INVALID;
    memset(out, 0, sizeof *out);
    out->introduced = ctx->introduced[resource];
    out->consumed = ctx->consumed[resource];
    out->exported = ctx->exported[resource];
    for (i = 0; i < ctx->stations.high; ++i) {
        const CStation *s;
        if (!ctx->stations.entity[i]) continue;
        s = station_at(ctx, i);
        if (s->in_res == resource) out->station += s->in_stock;
        if (s->out_res == resource) out->station += s->out_stock;
    }
    for (i = 0; i < ctx->chains.high; ++i) {
        const CChain *c;
        uint32_t k;
        if (!ctx->chains.entity[i]) continue;
        c = chain_at(ctx, i);
        for (k = 0; k < c->count; ++k) {
            if (ctx->slot_item[c->offset + k] == resource) out->slots++;
        }
    }
    for (i = 0; i < ctx->workers.high; ++i) {
        const CTask *t;
        if (!ctx->workers.entity[i]) continue;
        t = task_at(ctx, i);
        if (t->cargo_amt && t->cargo_res == resource) out->cargo += t->cargo_amt;
    }
    return FBS_CROWD_OK;
}

/* ---- Hash ---------------------------------------------------------------------------- */
typedef struct hasher { uint64_t h; } hasher;
static void h_u8(hasher *s, uint8_t v) {
    s->h ^= v;
    s->h *= 1099511628211ull;
}
static void h_u32(hasher *s, uint32_t v) {
    int i;
    for (i = 0; i < 4; ++i) h_u8(s, (uint8_t)(v >> (8 * i)));
}
static void h_u64(hasher *s, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) h_u8(s, (uint8_t)(v >> (8 * i)));
}
static void h_i32(hasher *s, int32_t v) { h_u32(s, (uint32_t)v); }

uint64_t fbs_crowd_state_hash(const fbs_crowd *ctx) {
    hasher s;
    uint32_t i, r;
    if (!ctx) return 0;
    s.h = 14695981039346656037ull;
    h_u32(&s, FBS_CROWD_VERSION);
    h_u64(&s, ctx->tick);
    for (r = 0; r < ctx->cfg.resources; ++r) {
        h_u64(&s, ctx->introduced[r]);
        h_u64(&s, ctx->consumed[r]);
        h_u64(&s, ctx->exported[r]);
    }
    h_u32(&s, ctx->stations.high);
    for (i = 0; i < ctx->stations.high; ++i) {
        const CStation *st;
        h_u32(&s, ctx->stations.gen[i]);
        h_u8(&s, ctx->stations.entity[i] ? 1 : 0);
        if (!ctx->stations.entity[i]) continue;
        st = station_at(ctx, i);
        h_u32(&s, st->node);
        h_u32(&s, st->in_res);
        h_u32(&s, st->in_per);
        h_u32(&s, st->in_cap);
        h_u32(&s, st->out_res);
        h_u32(&s, st->out_per);
        h_u32(&s, st->out_cap);
        h_u32(&s, st->cycle);
        h_u32(&s, st->progress);
        h_u32(&s, st->running);
        h_u32(&s, st->in_stock);
        h_u32(&s, st->out_stock);
        h_u32(&s, st->refs);
        h_u64(&s, st->tag);
        h_u64(&s, st->cycles);
    }
    h_u32(&s, ctx->chains.high);
    for (i = 0; i < ctx->chains.high; ++i) {
        const CChain *c;
        uint32_t k;
        h_u32(&s, ctx->chains.gen[i]);
        h_u8(&s, ctx->chains.entity[i] ? 1 : 0);
        if (!ctx->chains.entity[i]) continue;
        c = chain_at(ctx, i);
        h_u32(&s, c->source);
        h_u32(&s, c->dest);
        h_u32(&s, c->resource);
        h_u32(&s, c->offset);
        h_u32(&s, c->count);
        h_u32(&s, c->handoff);
        h_u32(&s, c->access);
        h_u64(&s, c->tag);
        h_u64(&s, c->taken);
        h_u64(&s, c->delivered);
        h_u64(&s, c->take_since);
        h_u64(&s, c->put_since);
        for (k = 0; k < c->count; ++k) {
            uint32_t g = c->offset + k;
            h_u8(&s, ctx->slot_item[g]);
            h_u32(&s, ctx->slot_timer[g]);
            h_u32(&s, ctx->slot_worker[g]);
            h_u32(&s, ctx->slot_reserved[g]);
        }
    }
    h_u32(&s, ctx->workers.high);
    for (i = 0; i < ctx->workers.high; ++i) {
        const CTask *t;
        const CWorker *w;
        h_u32(&s, ctx->workers.gen[i]);
        h_u8(&s, ctx->workers.entity[i] ? 1 : 0);
        if (!ctx->workers.entity[i]) continue;
        t = task_at(ctx, i);
        w = (const CWorker *)ecs_get_id(ctx->world, ctx->workers.entity[i], ctx->c_worker);
        h_u32(&s, w->speed);
        h_u32(&s, w->variant);
        h_u64(&s, w->tag);
        h_u32(&s, t->state);
        h_u32(&s, t->intent);
        h_u32(&s, t->node);
        h_u32(&s, t->goal);
        h_u32(&s, t->link);
        h_u32(&s, t->progress);
        h_u32(&s, t->queued);
        h_u32(&s, t->qprev);
        h_u32(&s, t->qnext);
        h_u32(&s, t->chain);
        h_u32(&s, t->slot);
        h_u32(&s, t->slot_global);
        h_u32(&s, t->pickup);
        h_u32(&s, t->dropoff);
        h_u32(&s, t->resource);
        h_u32(&s, t->haul_phase);
        h_u32(&s, t->cargo_res);
        h_u32(&s, t->cargo_amt);
        h_u64(&s, t->wait_since);
        h_i32(&s, t->sx);
        h_i32(&s, t->sy);
        h_i32(&s, t->sz);
    }
    h_u32(&s, ctx->link_count);
    for (i = 0; i < ctx->link_count; ++i) {
        h_u32(&s, ctx->link_occupancy[i]);
        h_u32(&s, ctx->link_head[i]);
        h_u32(&s, ctx->link_tail[i]);
    }
    h_u32(&s, ctx->slots_used);
    h_u32(&s, ctx->free_range_count);
    for (i = 0; i < ctx->free_range_count; ++i) {
        h_u32(&s, ctx->free_ranges[i].start);
        h_u32(&s, ctx->free_ranges[i].count);
    }
    return s.h;
}

/* ---- Stats ------------------------------------------------------------------------- */
fbs_crowd_result fbs_crowd_get_stats(const fbs_crowd *ctx, fbs_crowd_stats *out) {
    const ecs_world_info_t *info;
    ecs_iter_t it;
    if (!ctx || !out) return FBS_CROWD_INVALID;
    memset(out, 0, sizeof *out);
    out->workers = ctx->workers.live;
    out->chains = ctx->chains.live;
    out->stations = ctx->stations.live;
    out->slots_used = ctx->slots_used;
    it = ecs_query_iter(ctx->world, ctx->q_workers);
    while (ecs_query_next(&it)) {
        const CTask *ts = (const CTask *)ecs_field_w_size(&it, sizeof(CTask), 0);
        int32_t i;
        for (i = 0; i < it.count; ++i) {
            const CTask *t = &ts[i];
            if (t->link != NONE && t->queued) out->queued++;
            switch (t->state) {
            case FBS_CROWD_TRAVEL: out->travelling++; break;
            case FBS_CROWD_SLOT: out->slotted++; break;
            case FBS_CROWD_HAUL: out->hauling++; break;
            case FBS_CROWD_BLOCKED: out->blocked++; break;
            default: out->idle++; break;
            }
        }
    }
    info = ecs_get_world_info(ctx->world);
    out->ecs_tables = (uint32_t)info->table_count;
    out->ecs_tables_created = (uint64_t)info->table_create_total;
    out->context_bytes = ctx->context_bytes;
    out->requests_last_tick = ctx->requests_last_tick;
    return FBS_CROWD_OK;
}

/* ---- Presentation --------------------------------------------------------------------- */
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

#define STRIDE_MM 1300u
#define IDLE_TICKS 150u

fbs_crowd_result fbs_crowd_extract(const fbs_crowd *ctx, float fraction, fbs_crowd_instance *out,
                                   uint32_t capacity, uint32_t *written, uint32_t *total) {
    ecs_iter_t it;
    uint32_t n = 0;
    if (!ctx || (capacity && !out) || !(fraction >= 0.0f && fraction <= 1.0f)) return FBS_CROWD_INVALID;
    it = ecs_query_iter(ctx->world, ctx->q_workers);
    while (ecs_query_next(&it)) {
        const CTask *ts = (const CTask *)ecs_field_w_size(&it, sizeof(CTask), 0);
        const CWorker *ws = (const CWorker *)ecs_field_w_size(&it, sizeof(CWorker), 1);
        int32_t i;
        for (i = 0; i < it.count && n < capacity; ++i) {
            const CTask *t = &ts[i];
            const CWorker *w = &ws[i];
            fbs_crowd_instance *o = &out[n++];
            uint32_t hsh = mix32(w->index * 2654435761u + 17u);
            float x, y, z;
            memset(o, 0, sizeof *o);
            o->worker = make_handle(KIND_WORKER, w->index, ctx->workers.gen[w->index]);
            o->variant = (uint8_t)w->variant;
            o->key = w->index;
            o->item_resource = 0xFF;
            if (t->cargo_amt) {
                o->flags |= FBS_CROWD_FLAG_ITEM;
                o->item_resource = (uint8_t)t->cargo_res;
            }
            if (t->state == FBS_CROWD_SLOT) {
                uint32_t g = t->slot_global;
                x = (float)t->sx * 0.001f;
                y = (float)t->sy * 0.001f;
                z = (float)t->sz * 0.001f;
                /* Face across the chain; items pass from right hand to left. */
                o->yaw = atan2f((float)t->sdx, (float)t->sdz) - 1.5707964f;
                o->room = t->room;
                if (ctx->slot_item[g] != ITEM_NONE) {
                    uint32_t h = t->handoff ? t->handoff : 1u;
                    uint32_t done = h - (ctx->slot_timer[g] > h ? h : ctx->slot_timer[g]);
                    float f = ((float)done + fraction) / (float)h;
                    if (f > 1.0f) f = 1.0f;
                    o->clip = FBS_CROWD_CLIP_PASS;
                    o->phase = (uint16_t)(f * 65535.0f);
                    o->flags |= FBS_CROWD_FLAG_ITEM;
                    o->item_resource = ctx->slot_item[g];
                } else {
                    o->clip = FBS_CROWD_CLIP_IDLE;
                    o->phase = (uint16_t)((((ctx->tick + hsh) % IDLE_TICKS) * 65536u) / IDLE_TICKS);
                }
            } else if (t->link != NONE && !t->queued) {
                const fbs_crowd_link_desc *l = &ctx->links[t->link];
                uint32_t other = link_other(ctx, t->link, t->node);
                const fbs_crowd_vec3i *a = &ctx->node_pos[t->node], *b = &ctx->node_pos[other];
                float f = ((float)t->progress + fraction * (float)w->speed) / (float)l->length_mm;
                float side = ((float)(hsh & 1023u) / 1023.0f - 0.5f) * 0.9f; /* cosmetic */
                float dx = (float)(b->x - a->x), dz = (float)(b->z - a->z);
                float len = sqrtf(dx * dx + dz * dz);
                if (f > 1.0f) f = 1.0f;
                x = ((float)a->x + (float)(b->x - a->x) * f) * 0.001f;
                y = ((float)a->y + (float)(b->y - a->y) * f) * 0.001f;
                z = ((float)a->z + (float)(b->z - a->z) * f) * 0.001f;
                if (len > 0.0f) {
                    x += -dz / len * side;
                    z += dx / len * side;
                }
                o->yaw = atan2f(dx, dz);
                o->room = ctx->node_room[f < 0.5f ? t->node : other];
                o->clip = t->cargo_amt ? FBS_CROWD_CLIP_CARRY : FBS_CROWD_CLIP_WALK;
                o->phase = (uint16_t)((((uint64_t)t->progress + (uint64_t)(fraction * (float)w->speed) +
                                        (hsh % STRIDE_MM)) % STRIDE_MM) * 65536u / STRIDE_MM);
            } else {
                const fbs_crowd_vec3i *a = &ctx->node_pos[t->node];
                float ang = (float)(hsh % 6283u) * 0.001f;
                float rad = 0.4f + (float)((hsh >> 12) % 1600u) * 0.001f; /* cosmetic spread */
                x = (float)a->x * 0.001f + cosf(ang) * rad;
                y = (float)a->y * 0.001f;
                z = (float)a->z * 0.001f + sinf(ang) * rad;
                o->yaw = ang + 3.1415927f;
                o->room = ctx->node_room[t->node];
                o->clip = FBS_CROWD_CLIP_IDLE;
                o->phase = (uint16_t)((((ctx->tick + hsh) % IDLE_TICKS) * 65536u) / IDLE_TICKS);
                if (t->link != NONE && t->queued) o->flags |= FBS_CROWD_FLAG_QUEUED;
                if (t->state == FBS_CROWD_BLOCKED) o->flags |= FBS_CROWD_FLAG_BLOCKED;
            }
            o->x = x;
            o->y = y;
            o->z = z;
        }
    }
    if (written) *written = n;
    if (total) *total = ctx->workers.live;
    return FBS_CROWD_OK;
}
