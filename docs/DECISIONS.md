# Crowd design decisions

## ECS: Flecs, private to the module

**Decision.** Worker, chain and station entities and their components live in a private
[Flecs](https://github.com/SanderMertens/flecs) world owned by each `fbs_crowd` context.
Flecs **v4.1.6** (commit `fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8`) is vendored unmodified
in `third_party/flecs/` as the upstream core-only amalgamation (`distr/flecs_no_addons.*`),
MIT, with SHA-256 provenance. The owner selected Flecs after an ECS comparison
(Studio report `ecs-selection-2026-09-25.md`); EnTT and Gaia-ECS remain fallbacks.

The existing core modules considered here (`logistics`, `scheduler`, `navigation`)
use C APIs and their own bounded, handle-based state. A
purpose-built SoA pool was the alternative; it would have been smaller and allocation-free
by construction, but the owner chose a real ECS from the start so that later systems can
share entity/component infrastructure and Flecs' inspection tooling.

**How Flecs is used**

- Entity lifecycle: add/remove/reset create and delete Flecs entities. Each entity is
  created with all its components in one table move (`ecs_entity_desc_t.add`).
- Components are registered per world with explicit ids stored in the context (no global
  `ECS_COMPONENT_DECLARE` ids), so several contexts in one process are independent.
- Per-tick passes iterate **cached queries** (`EcsQueryCacheAll`) over stations, chains and
  workers.
- High-cardinality, frequently changing state (intent, route cursor, slot assignment,
  cargo) is plain component data. There are no per-worker tags, no relationship pairs and no
  per-destination tables, so reassignment never moves an entity between tables. In the recorded 2026-09-25 verification run,
  the table count stayed at 56 (Flecs built-ins plus our three archetypes) through build,
  stepping, mass reassignment, remove/add and reset at 20,000 workers.
- Specialised side structures with explicit ownership: handle tables (public identity →
  entity), the chain slot pool (items belong to slots; each chain owns a range), the route
  graph and next-hop table, link queues, and per-tick request buffers. None of these
  duplicates worker state.

**Configuration.** Only the Flecs core is compiled: no systems/pipeline, REST, HTTP,
stats, meta, script, JSON or other addons, so there is no listener or debug server in any
build. Flecs' default OS API is used (POSIX threads for mutexes natively; single-threaded
stubs under Emscripten).

**Implications**

| Area | Consequence |
|---|---|
| Public ABI | Unchanged C99 API. No Flecs type, header or id is visible; the installed package contains no Flecs headers. Hosts do not need to use Flecs. |
| Dependency | One vendored C file (≈1.6 MB source), statically linked into `libfbs_crowd`. The MIT notice is installed as `share/licenses/FinalBuildCrowd/FLECS-LICENSE`. `FBS_CROWD_FLECS_SOURCE` points at another copy; the build refuses any version other than 4.1.6. |
| Global state | Flecs' OS API table (`ecs_os_api`) is process-global. The module does not override it; a host that customises it affects every world. |
| Allocation | Our buffers are allocated at create. Flecs allocates while building (617 calls for the 20k scenario) and grows tables on demand. The 2026-09-25 run recorded with a counting Flecs OS API: 0 Flecs allocations during steady stepping, extraction, hashing, mass reassignment, 10 % remove/add cycles and reset/rebuild at the same peak population. Structural-phase counts are observations, not test assertions; growth beyond a previous peak can allocate. Flecs allocation failure aborts. |
| WASM | Builds with Emscripten 3.1.61 single-threaded. Flecs' query descriptors need more than Emscripten's 64 KiB default stack: link with `-sSTACK_SIZE=1048576` (the module's WASM tests and the demo do). |
| Determinism | Flecs iteration order is never used to decide outcomes (see API.md, "Tick order"). Hashes match across native/WASM, build types and sanitizers. |

**Cost of the choice.** Worker lookups by handle go through `ecs_get_mut_id` (a hash/index
lookup) on events such as queue admission and slot occupancy; bulk passes use cached
query iteration over contiguous columns. The [2026-09-25 verification record](../../crowd-raylib/docs/VERIFICATION.md)
reports about 0.11 ms per tick for its 20,000-worker headless scenario. That historical
smoke timing is not a current or target-hardware performance guarantee.

## Relationship to existing FBS systems

- **`fbs::logistics`** is a reservation ledger for jobs with material requirements; the host
  drives each worker's pickup/delivery with individual calls. Relay chains need a batch
  slot inventory with hand-to-hand timing and backpressure, which logistics does not model,
  and hauling 20,000 workers through per-worker host calls is the cost this module exists to
  avoid. The crowd ledger is therefore separate but follows the same conservation
  discipline (introduced = stock + in transit + consumed + exported) and export-on-removal
  semantics. A host that also uses logistics exchanges goods at station boundaries
  (for example, exported worker cargo or stock returned by station removal can be
  deposited with `fbs_logistics_deposit`). A sink consumes its input; there is no
  nondestructive station-stock withdrawal API. There is no
  silent double accounting: each unit is in exactly one ledger at a time.
- **`fbs::scheduler`** is not linked by the crowd core. Nothing in the
  first slice needs staggered decisions: stationary workers have no per-tick decision,
  waiting haulers are served first-come-first-served by arbitration.
- **`fbs::routegrid` / `fbs::navigation`**: not used by the core. One DetourCrowd agent per
  worker is explicitly avoided (capacity 128 by default, 16,383 maximum, and a documented
  head-on deadlock).

## Navigation

The first slice uses **macro routing**: a host graph of rooms/ports and passages, integer
link lengths, a precomputed all-pairs next-hop table (Dijkstra per target, ties to the lower
link index), and per-link capacity with FIFO queues. A mass command is a table lookup per
worker, not a search. Congestion is deterministic integer queueing, the same whether a
room is visible or not.

**Flow-field extension (not implemented).** Inside large rooms a later slice can add
per-room walkable/clearance grids baked from the cave field (routegrid's clearance
transform fits) and one flow field per active destination, shared by every worker heading
there. The authoritative model should stay macro (links and queues); flow fields and local
avoidance should only shape visible motion, so that visible and hidden rooms keep
identical outcomes.

## Presentation from state

Slot workers are animated from the slot's item timer (PASS) or a hashed idle phase; walkers
from their progress along the link (stride-synchronised WALK/CARRY). The renderer receives
compact instance records and never writes back. Cosmetic spreading around nodes and along
links is hashed from the worker index and is not simulated.
