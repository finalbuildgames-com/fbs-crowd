# fbs::crowd API and semantics

Header: `include/fbs/crowd.h` (C99, `extern "C"`). Package `FinalBuildCrowd`, target
`fbs::crowd`. Version 0.1.0 (`FBS_CROWD_VERSION` 1).

## Model

| Concept | What it is | Storage |
|---|---|---|
| Worker | One goblin: identity, speed, variant, intent, position on the route graph, cargo | Flecs entity with two components, `FbsCrowdWorker` (identity) and `FbsCrowdTask` (state). All workers share one table. |
| Chain | A relay line of `slot_count` worker slots moving one resource from a source station's output to a destination station's input | Flecs entity (`FbsCrowdChain`). Its slots are a contiguous range of the context's slot pool. |
| Slot | One position in a chain. Holds at most one item, a staffing worker and a reservation | Context-owned slot pool (chain-owned inventory). Items belong to slots, not workers. |
| Station | Source, sink or converter at a node, with integer stock and a production cycle | Flecs entity (`FbsCrowdStation`) |
| Route graph | Host-supplied nodes (rooms/ports, with a room id) and undirected links with a traveller capacity | Context arrays plus a precomputed next-hop table |

Positions are integer millimetres, +Y up. Quantities are integer units. Time is whole ticks.
The simulation never reads a clock; the host decides the tick rate.

## Lifetime and errors

`fbs_crowd_create` validates the configuration and allocates every fixed buffer:
handle tables, slot pool, graph and next-hop table (`max_nodes²` × 2 bytes), and request
buffers. It then creates the private Flecs world, registers the components, builds three
cached queries and reserves the entity index. `fbs_crowd_destroy` releases everything.

Most operations return `fbs_crowd_result`; destruction returns `void`, and the
tick/hash queries return `uint64_t`. Single-worker commands validate before
changing the worker's intent. Batch commands are **not atomic**: they return the
first error while still applying other valid entries. Output arguments may be
cleared on failure (for example, `create` sets `*out` to NULL).

| Result | Meaning |
|---|---|
| `INVALID` | malformed argument (bad node, zero speed, NULL output, …) |
| `CAPACITY` | a configured capacity would be exceeded (workers, chains, stations, slots) |
| `STALE` | handle removed, reset, zero, or of another kind |
| `STATE` | not allowed now: slot taken, cargo on a MOVE/SLOT command, station still referenced, graph change while workers have intents |
| `UNREACHABLE` | no route from the worker's next decision node to the goal |
| `MEMORY`, `BACKEND` | allocation failure in our buffers / Flecs refused an operation |

Allocation failure *inside Flecs* aborts the process (upstream behaviour, `ecs_os_abort`).

## Handles and save identity

`fbs_crowd_handle` is 64 bits: kind (2 bits), generation (30 bits), slot index + 1 (32 bits).
Zero is invalid. A removed handle becomes `STALE`; a slot whose generation reaches 2³⁰−1
is retired rather than reused, so handles never alias. `fbs_crowd_reset` invalidates every
handle and reuses slot indices in ascending order with new generations.

Handles are deterministic: the same sequence of add/remove/reset calls produces the same
handles on every platform. They are the stable identity to store in a save file together
with the host `tag` fields. (A snapshot save/load API is not part of this slice.)

`fbs_crowd_instance.key` is the dense slot index (< `max_workers`), stable while the worker
lives; renderers use it for per-figure state such as LOD hysteresis.

## Commands

| Call | Effect |
|---|---|
| `command_move(worker, node)` | Travel to `node`, then idle. Refused with cargo. |
| `command_slot(worker, chain, slot)` | Reserve the slot, travel to the chain's access node, then staff the slot. Refused if the slot is staffed or reserved by someone else, or with cargo. Idempotent for the same worker. |
| `command_haul(worker, pickup, dropoff, resource)` | Loop: travel to pickup, take one unit from its output, travel to dropoff, put it into its input. A worker already carrying that resource starts with the delivery. |
| `command_idle(worker)` | Stop. A worker on a link finishes the link first. |
| `command_slots`, `command_moves` | Batch forms for mass reassignment: entries are applied in array order exactly like the single calls; `results[i]` gets each outcome. |

A successful command replaces the intent (repeating the same slot assignment is a no-op). Leaving a slot vacates it; its item stays in the
slot. Commands and add/remove/reset are applied immediately between ticks, which are the
structural boundaries (no structural change happens inside `fbs_crowd_step`).

## Tick order

`fbs_crowd_step(ctx, n)` runs `n` ticks. Splitting the same number of ticks over different
calls gives identical state. Each tick:

1. **Stations** (cached query; independent per station). An idle station starts a cycle
   when its input is present and its output fits, consuming the input (counted *consumed*);
   a cycle completing adds the output (counted *introduced*).
2. **Chains** (cached query; independent per chain). Slots are scanned from the last to the
   first. An item in a staffed slot counts down `handoff_ticks`; at zero it passes into the
   next slot if that slot is staffed and empty (so a full line moves like a belt in one
   scan). A ready item in the last slot requests a PUT at the destination; an empty staffed
   first slot requests a TAKE at the source. Vacant slots stop the flow at that point.
3. **Workers** (cached query; independent per worker). Travellers advance by their speed;
   reaching a link's end they leave it and either arrive (MOVE → idle, SLOT → staff the
   reserved slot, HAUL → wait at the station) or request the next link. Waiting haulers
   request TAKE/PUT.
4. **Station arbitration.** All TAKE/PUT requests are sorted by (station, operation,
   waiting-since tick, kind (chain before hauler), handle index) and granted in that order
   while stock/room lasts. Waiting-since makes service first-come, first-served.
5. **Link admission.** Link requests are sorted by (link, worker index) and appended to each
   link's FIFO queue; links are then visited in index order, admitting queue heads while the
   link has capacity.

Only steps 4 and 5 resolve interactions between entities, and both use explicit sorted
orders with unique keys. The storage order of Flecs tables therefore never affects the
outcome. Sorting is a bottom-up merge sort into preallocated scratch, so it allocates
nothing and is identical on every platform.

A fully staffed chain whose destination has room delivers one item per
`handoff_ticks + 1` ticks: the last slot is emptied by the grant in step 4, after that
tick's pass in step 2.

## Conservation

For each resource, at every tick boundary:

```
introduced == station stock + items in slots + carried cargo + consumed + exported
```

`fbs_crowd_resource_totals` computes the right-hand stock terms by scanning the live state,
not from counters, so the equality checks the real state. Removal exports what leaves:
worker cargo (`remove_worker`), slot items (`remove_chain`), station stock
(`remove_station`), and the per-resource amounts are returned to the caller.

## Determinism and hashing

`fbs_crowd_state_hash` is 64-bit FNV-1a over a defined little-endian serialisation of the
authoritative state in handle order (generations, liveness, component fields, slot pool,
link occupancy and queues, free slot ranges, ledgers). It never hashes struct bytes or
padding and never depends on table order.

What the shipped tests check (`tests/test_core.c`), within one build: the 900-worker
scenario with mass reassignment gives the same hash whether ticks are stepped 1, 7 or 600
at a time and whether or not presentation is extracted, and extraction leaves the hash
unchanged. The test prints the scenario hashes as `HASH name value` lines, and
`fbs_crowd_bench` prints a final hash per population.

Cross-build comparison (native against WebAssembly, Debug against Release, sanitizer
builds) is done by comparing those printed `HASH` lines between runs. It is not automated
in this repository, and no shipped test compares hashes across builds. An earlier run of
that manual comparison found identical hashes for native Debug, native Release, an
ASan/UBSan build and Emscripten single-threaded WebAssembly; treat that as a historical
observation, not a result for the current build.

Tick advancement and arbitration use integer state. Graph setup uses a floating-point
square-root estimate when deriving link lengths, corrected with integer arithmetic.
Presentation (`fbs_crowd_extract`) uses floating point and never feeds back.

The hash is a replay comparison aid for an identically configured scenario, not a
complete save-state fingerprint. It omits graph definitions, configured capacities,
handle free-list order and some presentation fields; equal hashes alone cannot prove
that differently configured or edited contexts will behave identically.

## Presentation

`fbs_crowd_extract(ctx, fraction, out, capacity, &written, &total)` fills one
`fbs_crowd_instance` per live worker, in unspecified order: world position (metres), yaw,
room, clip (`IDLE`, `WALK`, `PASS`, `CARRY`), phase (0..65535), variant, flags
(`ITEM`, `QUEUED`, `BLOCKED`) and carried item resource. It is `const`: it cannot change any
state. `fraction` in `[0, 1]` projects travellers from the current tick toward the next,
clamping at the end of their current link. The output may be truncated to `capacity`;
`written` and `total` are optional pointers.

- Slot workers stand at their slot position facing across the chain; the PASS phase is the
  item's progress through its hand-off, so animation is a pure function of state. No
  per-frame AI or pathfinding runs for them.
- Travellers interpolate linearly along the link between node positions, with a hashed
  sideways offset (cosmetic).
- Idle and queued workers stand on a hashed ring around their node (cosmetic).

Cosmetic offsets are never simulated: they do not affect collision, queues or throughput.

## Statistics

`fbs_crowd_get_stats`: live counts by state, slots used, Flecs table count and cumulative
tables created, fixed context bytes and station/link requests considered in the last tick (including
requests that could not be granted).

## Capacities and limits

| Limit | Value |
|---|---|
| workers | 1 .. 4,194,303 |
| chains, stations | 0 .. 1,048,575 each |
| total chain slots | 0 .. 2³¹−1 (contiguous per chain, first-fit with coalescing; fragmentation can refuse a long chain with `CAPACITY`) |
| nodes | 0 .. 4,096 (next-hop table is `max_nodes²` × 2 bytes: 32 MiB at 4,096) |
| links | 0 .. 65,534 |
| resources | 1 .. 64 |
| handoff | 1 .. 65,535 ticks |

## Known limits of this slice

- **Macro routing only.** Links are straight segments between nodes; travellers have no
  local steering, avoidance or collision. There is no flow field. See
  [DECISIONS.md](DECISIONS.md#navigation) for the flow-field extension.
- **Chain geometry is a straight line** from `start` to `end`; slot heights interpolate.
  A curved or stepped chain needs several chains or a future per-slot position array.
- **Workers join a slot at the access node** (no walk from the node to the slot).
- **Carry capacity is one unit.**
- **Graph changes require no active intents** (`STATE` otherwise).
- **No save/load API yet**; handles, tags and the hash define what a snapshot must hold.
- **Single thread.** A context is used by one thread at a time; the tick is not parallelised.

Current arithmetic limits also require care with extreme inputs: presentation
subtracts node coordinates as signed 32-bit values, and station production checks
output stock plus a cycle's output using unsigned 32-bit addition. Keep each linked
coordinate delta within `int32_t` and `output_stock + output_per_cycle` within
`uint32_t`. The library does not check these cases.
