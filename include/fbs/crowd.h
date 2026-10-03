#ifndef FBS_CROWD_H
#define FBS_CROWD_H
/* FinalBuildSystems crowd: deterministic worker simulation for large
 * populations of relay-chain workers and haulers.
 *
 * Authoritative worker, chain and station state lives in a private Flecs world
 * owned by the context. No Flecs type appears in this header. Handles are
 * opaque 64-bit values (kind, slot index, generation); zero is invalid and a
 * removed handle never aliases a later entity.
 *
 * Simulation advances in fixed integer ticks. Quantities are integer units,
 * positions are integer millimetres. Iteration order of the private storage
 * never decides an outcome: competing requests are ordered explicitly (see
 * docs/API.md, "Tick order"). Presentation queries are read-only and cannot
 * change any authoritative state.
 *
 * A context is used by one thread at a time. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FBS_CROWD_VERSION 1u
#define FBS_CROWD_NONE 0xFFFFFFFFu /* no node, link, slot or resource */
#define FBS_CROWD_MAX_RESOURCES 64u

typedef uint64_t fbs_crowd_handle;
typedef struct fbs_crowd fbs_crowd;

typedef enum fbs_crowd_result {
    FBS_CROWD_OK = 0,
    FBS_CROWD_INVALID = 1,     /* malformed argument; nothing changed */
    FBS_CROWD_CAPACITY = 2,    /* a configured capacity would be exceeded */
    FBS_CROWD_STALE = 3,       /* handle removed, reset or of another kind */
    FBS_CROWD_MEMORY = 4,      /* allocation failed */
    FBS_CROWD_STATE = 5,       /* operation not allowed in the current state */
    FBS_CROWD_UNREACHABLE = 6, /* no route between the nodes */
    FBS_CROWD_BACKEND = 7      /* the ECS backend refused an operation */
} fbs_crowd_result;

typedef struct fbs_crowd_config {
    uint32_t max_workers;     /* 1 .. 4,194,303 */
    uint32_t max_chains;      /* 0 .. 1,048,575 */
    uint32_t max_stations;    /* 0 .. 1,048,575 */
    uint32_t max_chain_slots; /* total slots over all chains */
    uint32_t max_nodes;       /* route graph nodes, 0 .. 4096 */
    uint32_t max_links;       /* route graph links, 0 .. 65,534 */
    uint32_t resources;       /* 1 .. FBS_CROWD_MAX_RESOURCES */
} fbs_crowd_config;

typedef struct fbs_crowd_vec3i {
    int32_t x, y, z; /* millimetres, +Y up */
} fbs_crowd_vec3i;

/* ---- Route graph ----------------------------------------------------------
 * Host-supplied macro graph: rooms, ports and passages. Links are undirected.
 * capacity is the number of travellers allowed on the link at once; arrivals
 * that find it full wait in a FIFO queue at the link. length_mm 0 derives the
 * length from the node positions (rounded up, at least 1, clamped to
 * UINT32_MAX millimetres). */
typedef struct fbs_crowd_node_desc {
    fbs_crowd_vec3i position;
    uint32_t room; /* host room/visibility id, passed through to presentation */
} fbs_crowd_node_desc;
typedef struct fbs_crowd_link_desc {
    uint32_t a, b;
    uint32_t capacity;  /* >= 1 */
    uint32_t length_mm; /* 0 = derived */
} fbs_crowd_link_desc;

/* Replaces the graph and rebuilds the next-hop table (shortest integer length,
 * ties to the lower link index). Refused with STATE while any worker has an
 * intent (moving, hauling, holding or travelling to a slot), or while any
 * worker, chain or station refers to a node that would no longer exist. */
fbs_crowd_result fbs_crowd_set_graph(fbs_crowd *ctx, const fbs_crowd_node_desc *nodes,
                                     uint32_t node_count, const fbs_crowd_link_desc *links,
                                     uint32_t link_count);
/* Shortest route length in millimetres, or UNREACHABLE. */
fbs_crowd_result fbs_crowd_route_length(const fbs_crowd *ctx, uint32_t from, uint32_t to,
                                        uint64_t *length_mm);

/* ---- Lifetime ---------------------------------------------------------- */
fbs_crowd_result fbs_crowd_create(const fbs_crowd_config *config, fbs_crowd **out);
void fbs_crowd_destroy(fbs_crowd *ctx);
/* Removes every worker, chain and station and zeroes the ledgers and tick.
 * The graph and capacities are kept. Every earlier handle becomes STALE. */
fbs_crowd_result fbs_crowd_reset(fbs_crowd *ctx);

/* ---- Stations --------------------------------------------------------------
 * A station sits at a node. Each cycle of cycle_ticks it consumes
 * input_per_cycle of input_resource (if any) and produces output_per_cycle of
 * output_resource (if any). A cycle only starts when the input is present and
 * the output fits. A station with no input is a source (production counts as
 * introduced); one with no output is a sink (input counts as consumed). */
typedef struct fbs_crowd_station_desc {
    uint32_t node;
    uint32_t input_resource;  /* FBS_CROWD_NONE for a source */
    uint32_t input_per_cycle;
    uint32_t input_capacity;
    uint32_t output_resource; /* FBS_CROWD_NONE for a sink */
    uint32_t output_per_cycle;
    uint32_t output_capacity;
    uint32_t cycle_ticks;     /* >= 1 */
    uint32_t initial_output;  /* counted as introduced; <= output_capacity */
    uint64_t tag;             /* host id, uninterpreted */
} fbs_crowd_station_desc;
typedef struct fbs_crowd_station_info {
    uint64_t tag;
    uint32_t node;
    uint32_t input_stock, output_stock;
    uint32_t cycle_progress; /* ticks into the running cycle, 0 when idle */
    uint64_t cycles;         /* completed cycles */
} fbs_crowd_station_info;
fbs_crowd_result fbs_crowd_add_station(fbs_crowd *ctx, const fbs_crowd_station_desc *desc,
                                       fbs_crowd_handle *out);
/* STATE while a chain or hauler refers to it. Remaining stock is exported:
 * exported[r] receives the amount of resource r (array of config.resources). */
fbs_crowd_result fbs_crowd_remove_station(fbs_crowd *ctx, fbs_crowd_handle station,
                                          uint32_t *exported);
fbs_crowd_result fbs_crowd_station_get(const fbs_crowd *ctx, fbs_crowd_handle station,
                                       fbs_crowd_station_info *out);

/* ---- Relay chains ----------------------------------------------------------
 * A chain moves one resource from a source station's output to a destination
 * station's input through slot_count worker slots, hand to hand. Items belong
 * to slots, not to workers: a slot holds at most one item. An item waits
 * handoff_ticks in a staffed slot before it can pass on; it passes only into
 * the next staffed, empty slot, and leaves the last slot only when the
 * destination has input room (backpressure). A vacant slot stops the flow at
 * that point; items already in it stay. Workers join a slot on arrival at the
 * chain's access node. Slot positions for presentation are spread evenly from
 * start to end. A fully staffed chain with an unblocked destination delivers
 * one item per handoff_ticks + 1 ticks: the last slot is emptied by the
 * destination's grant, after that tick's hand-to-hand pass. */
typedef struct fbs_crowd_chain_desc {
    fbs_crowd_handle source, destination; /* stations */
    uint32_t resource;
    uint32_t slot_count;    /* >= 1 */
    uint32_t handoff_ticks; /* >= 1 */
    uint32_t access_node;
    fbs_crowd_vec3i start, end;
    uint32_t room;          /* presentation room of the slot workers */
    uint64_t tag;
} fbs_crowd_chain_desc;
typedef struct fbs_crowd_chain_info {
    uint64_t tag;
    uint32_t slot_count, staffed, items;
    uint64_t taken, delivered; /* items taken from source / delivered */
} fbs_crowd_chain_info;
typedef struct fbs_crowd_slot_info {
    fbs_crowd_handle worker;   /* 0 if vacant */
    fbs_crowd_handle reserved; /* worker travelling to it, or 0 */
    uint32_t item;             /* resource, or FBS_CROWD_NONE */
    uint32_t timer;            /* ticks until the item may pass */
} fbs_crowd_slot_info;
fbs_crowd_result fbs_crowd_add_chain(fbs_crowd *ctx, const fbs_crowd_chain_desc *desc,
                                     fbs_crowd_handle *out);
/* Items in the slots are exported (exported[r] as for stations). Staffed
 * workers become idle at the access node; workers travelling to a slot of
 * the chain continue to its access node and then idle. */
fbs_crowd_result fbs_crowd_remove_chain(fbs_crowd *ctx, fbs_crowd_handle chain,
                                        uint32_t *exported);
fbs_crowd_result fbs_crowd_chain_get(const fbs_crowd *ctx, fbs_crowd_handle chain,
                                     fbs_crowd_chain_info *out);
fbs_crowd_result fbs_crowd_slot_get(const fbs_crowd *ctx, fbs_crowd_handle chain,
                                    uint32_t slot, fbs_crowd_slot_info *out);

/* ---- Workers ------------------------------------------------------------- */
typedef enum fbs_crowd_worker_state {
    FBS_CROWD_IDLE = 0,    /* standing at a node with no intent */
    FBS_CROWD_TRAVEL = 1,  /* on a link, or queued for one */
    FBS_CROWD_SLOT = 2,    /* holding a chain slot */
    FBS_CROWD_HAUL = 3,    /* hauling loop: travelling, loading or unloading */
    FBS_CROWD_BLOCKED = 4  /* intent unreachable; idle until commanded */
} fbs_crowd_worker_state;
typedef enum fbs_crowd_intent {
    FBS_CROWD_INTENT_NONE = 0,
    FBS_CROWD_INTENT_MOVE = 1,
    FBS_CROWD_INTENT_SLOT = 2,
    FBS_CROWD_INTENT_HAUL = 3
} fbs_crowd_intent;
typedef struct fbs_crowd_worker_desc {
    uint32_t node;             /* spawn node */
    uint32_t speed_mm_per_tick; /* >= 1 */
    uint32_t variant;          /* presentation variant, 0..255 */
    uint64_t tag;
} fbs_crowd_worker_desc;
typedef struct fbs_crowd_worker_info {
    uint64_t tag;
    uint32_t state, intent;
    uint32_t node;             /* current or last node */
    uint32_t link;             /* link travelled or queued for, or NONE */
    uint32_t progress_mm;
    uint32_t queued;           /* waiting in the link's queue */
    uint32_t goal_node;
    fbs_crowd_handle chain;    /* SLOT intent */
    uint32_t slot;
    fbs_crowd_handle pickup, dropoff; /* HAUL intent */
    uint32_t cargo_resource;   /* NONE when empty */
    uint32_t cargo_amount;
    uint32_t variant;
} fbs_crowd_worker_info;
fbs_crowd_result fbs_crowd_add_worker(fbs_crowd *ctx, const fbs_crowd_worker_desc *desc,
                                      fbs_crowd_handle *out);
/* Carried cargo leaves the simulation as exported and is reported. A held
 * slot becomes vacant; its item stays in the slot. */
fbs_crowd_result fbs_crowd_remove_worker(fbs_crowd *ctx, fbs_crowd_handle worker,
                                         uint32_t *resource, uint32_t *amount);
fbs_crowd_result fbs_crowd_worker_get(const fbs_crowd *ctx, fbs_crowd_handle worker,
                                      fbs_crowd_worker_info *out);

/* Commands. Each replaces the worker's intent. A worker leaving a slot
 * vacates it (items stay). A worker on a link finishes the link first.
 * A hauler keeps its cargo across a command; a MOVE or SLOT command given to
 * a worker with cargo is refused with STATE (unload by hauling or removal). */
fbs_crowd_result fbs_crowd_command_move(fbs_crowd *ctx, fbs_crowd_handle worker, uint32_t node);
fbs_crowd_result fbs_crowd_command_slot(fbs_crowd *ctx, fbs_crowd_handle worker,
                                        fbs_crowd_handle chain, uint32_t slot);
/* Loop: travel to pickup, take one unit of resource from its output, travel
 * to dropoff, put it into its input, repeat. */
fbs_crowd_result fbs_crowd_command_haul(fbs_crowd *ctx, fbs_crowd_handle worker,
                                        fbs_crowd_handle pickup, fbs_crowd_handle dropoff,
                                        uint32_t resource);
fbs_crowd_result fbs_crowd_command_idle(fbs_crowd *ctx, fbs_crowd_handle worker);

/* Batch commands for mass reassignment. Each entry is validated and applied
 * in array order exactly as the single call would; results[i] receives its
 * outcome (optional). The call returns OK when every entry was applied, or
 * the first failure's result otherwise (other entries are still applied). */
typedef struct fbs_crowd_slot_command {
    fbs_crowd_handle worker, chain;
    uint32_t slot;
} fbs_crowd_slot_command;
fbs_crowd_result fbs_crowd_command_slots(fbs_crowd *ctx, const fbs_crowd_slot_command *commands,
                                         uint32_t count, fbs_crowd_result *results);
fbs_crowd_result fbs_crowd_command_moves(fbs_crowd *ctx, const fbs_crowd_handle *workers,
                                         uint32_t count, uint32_t node,
                                         fbs_crowd_result *results);

/* ---- Stepping ------------------------------------------------------------- */
/* Advances ticks whole ticks. Splitting the same number of ticks into
 * different calls gives identical state. */
fbs_crowd_result fbs_crowd_step(fbs_crowd *ctx, uint32_t ticks);
uint64_t fbs_crowd_tick(const fbs_crowd *ctx);

/* ---- Ledgers, hashing and statistics -------------------------------------
 * For every resource: introduced == station + slots + cargo + consumed +
 * exported, at every tick boundary. */
typedef struct fbs_crowd_totals {
    uint64_t introduced, station, slots, cargo, consumed, exported;
} fbs_crowd_totals;
fbs_crowd_result fbs_crowd_resource_totals(const fbs_crowd *ctx, uint32_t resource,
                                           fbs_crowd_totals *out);
/* 64-bit FNV-1a over a defined little-endian serialisation of the
 * authoritative state, in handle order. Independent of storage order,
 * padding, presentation queries and step batching. */
uint64_t fbs_crowd_state_hash(const fbs_crowd *ctx);

typedef struct fbs_crowd_stats {
    uint32_t workers, chains, stations, slots_used;
    uint32_t travelling, queued, slotted, hauling, idle, blocked;
    uint32_t ecs_tables;         /* Flecs table count (archetypes incl. builtin) */
    uint64_t ecs_tables_created; /* cumulative */
    size_t context_bytes;        /* fixed buffers allocated at create */
    uint64_t requests_last_tick; /* arbitrated station/link requests */
} fbs_crowd_stats;
fbs_crowd_result fbs_crowd_get_stats(const fbs_crowd *ctx, fbs_crowd_stats *out);

/* ---- Presentation (read-only) --------------------------------------------
 * Derived from authoritative state; never changes it. fraction in [0,1]
 * projects travellers from the current tick toward the next, clamped at the
 * end of the current link. Offsets that
 * keep figures from overlapping are cosmetic and not simulated. */
typedef enum fbs_crowd_clip {
    FBS_CROWD_CLIP_IDLE = 0,
    FBS_CROWD_CLIP_WALK = 1,
    FBS_CROWD_CLIP_PASS = 2,  /* receive and hand on an item */
    FBS_CROWD_CLIP_CARRY = 3, /* walk while carrying */
    FBS_CROWD_CLIP_COUNT = 4
} fbs_crowd_clip;
enum {
    FBS_CROWD_FLAG_ITEM = 1u,    /* holding an item */
    FBS_CROWD_FLAG_QUEUED = 2u,  /* waiting for a link */
    FBS_CROWD_FLAG_BLOCKED = 4u
};
typedef struct fbs_crowd_instance {
    fbs_crowd_handle worker;
    float x, y, z;  /* metres */
    float yaw;      /* radians about +Y; 0 faces +Z */
    uint32_t room;
    uint32_t key;   /* dense id < config.max_workers, stable while the worker lives */
    uint16_t phase; /* 0..65535 over the clip */
    uint8_t clip, variant, flags, item_resource;
    uint16_t pad;
} fbs_crowd_instance;
/* Writes up to capacity instances, one per live worker, and the total live
 * count. Order is unspecified; use worker for identity. */
fbs_crowd_result fbs_crowd_extract(const fbs_crowd *ctx, float fraction,
                                   fbs_crowd_instance *out, uint32_t capacity,
                                   uint32_t *written, uint32_t *total);
/* Node position in metres (for hosts drawing the graph). */
fbs_crowd_result fbs_crowd_node_position(const fbs_crowd *ctx, uint32_t node, float xyz[3]);

#ifdef __cplusplus
}
#endif
#endif
