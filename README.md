# fbs-crowd

Deterministic fixed-tick simulation of relay-chain workers and haulers on a route graph, in C99.

## What it does

You describe a level as a graph of nodes (rooms, ports) and undirected links with a traveller capacity, place stations that produce, convert or consume integer resources, and add workers. Workers either staff a relay chain (items pass hand to hand from a source station to a destination station) or haul one unit at a time between two stations. The library advances everything in whole ticks and keeps the resource books balanced.

- `fbs_crowd_create` / `fbs_crowd_destroy`: a context sized by `fbs_crowd_config`.
- `fbs_crowd_set_graph`: nodes and links; builds a shortest-route next-hop table. `fbs_crowd_route_length` queries it.
- `fbs_crowd_add_station`, `fbs_crowd_add_chain`, `fbs_crowd_add_worker` return 64-bit handles. Removing an entity makes its handle return `FBS_CROWD_STALE`, and a reused index gets a new generation.
- `fbs_crowd_command_move`, `_slot`, `_haul`, `_idle`, plus batch forms `fbs_crowd_command_slots` and `fbs_crowd_command_moves` for mass reassignment.
- `fbs_crowd_step(ctx, ticks)`: chains respect backpressure (an item leaves the last slot only when the destination has room), a vacant slot stops the flow, and full links queue travellers in FIFO order.
- `fbs_crowd_resource_totals`: per resource, `introduced == station + slots + cargo + consumed + exported`. Removing a worker, chain or station reports what it exported.
- `fbs_crowd_state_hash`: 64-bit FNV-1a over the authoritative state, for replay comparison.
- `fbs_crowd_extract`: read-only render instances (position in metres, yaw, room, animation clip and phase, flags), with a `fraction` to interpolate between ticks.

Internally the state lives in a Flecs 4.1.6 world owned by the context. No Flecs type appears in `include/fbs/crowd.h`, and the host does not need to use Flecs.

## When to use it

- Colony or base-building games with many workers whose jobs are "stand in a line and pass things" or "carry from A to B".
- Lockstep or replay setups where the same commands at the same ticks must give the same state, checked with `fbs_crowd_state_hash`.
- Hosts with their own ECS that want to keep one handle per worker and read state back, rather than run per-worker AI (see `docs/HOST-ECS.md`).

## When not to use it

- No local steering, avoidance, collision or flow fields. Travellers move along straight links between nodes.
- A worker carries one unit at a time. Chains are straight lines from `start` to `end`.
- `fbs_crowd_set_graph` returns `FBS_CROWD_STATE` while any worker has an intent.
- No save/load API. Handles, tags and the hash are what a host would store.
- One thread at a time per context, and the tick is not parallelised.
- Fixed limits checked by `fbs_crowd_create`: 1 to 4,194,303 workers, up to 1,048,575 chains and stations each, 4,096 nodes, 65,534 links, 64 resources. The next-hop table is `max_nodes * max_nodes * 2` bytes (32 MiB at 4,096 nodes). Chain `handoff_ticks` is 1 to 65,535.
- `docs/API.md` lists two unchecked arithmetic cases: keep linked node coordinate deltas within `int32_t`, and `output_stock + output_per_cycle` within `uint32_t`.

## Example

```c
#include <fbs/crowd.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fbs_crowd_config cfg = {8, 1, 2, 4, 2, 1, 1}; /* workers, chains, stations, slots, nodes, links, resources */
    fbs_crowd_node_desc nodes[2];
    fbs_crowd_link_desc link = {0, 1, 4, 0};      /* nodes 0-1, 4 travellers at once, derived length */
    fbs_crowd_worker_desc wd = {1, 100, 0, 0};    /* spawn at node 1, 100 mm per tick */
    fbs_crowd_station_desc src, dst;
    fbs_crowd_chain_desc cd;
    fbs_crowd_handle hs = 0, hd = 0, hc = 0, hw = 0;
    fbs_crowd_chain_info info;
    fbs_crowd *crowd = NULL;
    uint32_t i;
    int ok;
    memset(nodes, 0, sizeof nodes); memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst); memset(&cd, 0, sizeof cd);
    nodes[1].position.x = 10000; /* 10 m */
    src.input_resource = FBS_CROWD_NONE; /* source at node 0: 1 unit of resource 0 per tick */
    src.output_per_cycle = 1; src.output_capacity = 20; src.cycle_ticks = 1;
    dst.output_resource = FBS_CROWD_NONE; /* sink at node 0 */
    dst.input_per_cycle = 1; dst.input_capacity = 20; dst.cycle_ticks = 1;
    cd.slot_count = 4; cd.handoff_ticks = 3; cd.end.x = 4400; /* access node 0 */
    ok = fbs_crowd_create(&cfg, &crowd) == FBS_CROWD_OK &&
         fbs_crowd_set_graph(crowd, nodes, 2, &link, 1) == FBS_CROWD_OK &&
         fbs_crowd_add_station(crowd, &src, &hs) == FBS_CROWD_OK &&
         fbs_crowd_add_station(crowd, &dst, &hd) == FBS_CROWD_OK;
    cd.source = hs; cd.destination = hd;
    ok = ok && fbs_crowd_add_chain(crowd, &cd, &hc) == FBS_CROWD_OK;
    for (i = 0; ok && i < 4; ++i) /* workers walk to node 0, then staff slot i */
        ok = fbs_crowd_add_worker(crowd, &wd, &hw) == FBS_CROWD_OK &&
             fbs_crowd_command_slot(crowd, hw, hc, i) == FBS_CROWD_OK;
    ok = ok && fbs_crowd_step(crowd, 300) == FBS_CROWD_OK &&
         fbs_crowd_chain_get(crowd, hc, &info) == FBS_CROWD_OK;
    if (ok)
        printf("tick %llu: %u of %u slots staffed, %llu delivered, hash %016llx\n",
               (unsigned long long)fbs_crowd_tick(crowd), info.staffed, info.slot_count,
               (unsigned long long)info.delivered, (unsigned long long)fbs_crowd_state_hash(crowd));
    fbs_crowd_destroy(crowd); /* accepts NULL */
    return ok ? 0 : 1;
}
```

Build it with `target_link_libraries(your_target PRIVATE fbs::crowd)` after adding this repository with `add_subdirectory` or FetchContent.

## Build and test

Requires CMake 3.16 or newer, a C99 compiler and your generator's build tool
(for example Make or Ninja). Run the commands from this repository's root. The pinned Flecs 4.1.6 core amalgamation is vendored in `third_party/flecs/` and compiled into the library; CMake refuses any other Flecs version. The target links `libm` (non-MSVC), `Threads::Threads` (native non-Windows, for Flecs' default OS layer) and `dbghelp` (Windows).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 1
(cd build && ctest --output-on-failure)
```

The default native CTest configuration runs three entries:

- `crowd_core` (`tests/test_core.c`): config validation, routing and tie breaks, chain throughput, backpressure, vacant slots, handle staleness and reuse, batch reassignment with conflicting entries, removal exports, haulers queueing on a capacity-1 link, capacity errors, reset, presentation, and conservation checked every tick in the flow tests. Its determinism test runs a seeded 900-worker scenario for 600 ticks and asserts the same state hash whether ticks are stepped 1, 7 or 600 at a time and whether or not instances are extracted. It prints `HASH` lines but does not compare them against other builds.
- `crowd_flecs_alloc` (`tests/test_flecs_alloc.c`): builds a 20,000-worker scenario with a counting Flecs allocator and asserts zero Flecs allocations during 300 ticks of stepping and extraction and during two mass reassignments with 1,200 ticks, and a constant Flecs table count through remove/add and reset cycles. Allocation counts for structural phases are printed, not asserted.
- `example`: runs `fbs_example` (`examples/basic.c`), which creates and destroys a context.

`fbs_crowd_bench` (`tests/bench.c`) is also built. It prints CPU time per tick and a hash for chosen populations; it is a smoke tool, not a performance claim. No shipped test compares state hashes between native and WebAssembly builds. `CMakeLists.txt` has an Emscripten path that runs the same tests under Node; the included CI builds natively on Linux only.

The Emscripten configuration also registers `fbs_crowd_bench` as a Node-run
CTest entry; running that suite includes a timing workload. It does not compare
its hashes with a native build.

`BUILD_SHARED_LIBS` selects static (default) or shared libraries. Use static
libraries on Windows: the public header has no DLL export annotations and the
target does not enable automatic symbol exports.

Options: `FBS_CROWD_BUILD_TESTS` (default ON, also needs `BUILD_TESTING`), `FBS_CROWD_ENABLE_SANITIZERS` (GCC/Clang ASan and UBSan), `FBS_CROWD_FLECS_SOURCE` (another copy of the pinned amalgamation).

FetchContent (set `FBS_CROWD_BUILD_TESTS` OFF if your project enables `BUILD_TESTING`):

```cmake
include(FetchContent)
set(FBS_CROWD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fbs_crowd
  GIT_REPOSITORY https://github.com/finalbuildgames-com/fbs-crowd.git
  GIT_TAG <full commit sha>) # pin a reviewed commit
FetchContent_MakeAvailable(fbs_crowd)
target_link_libraries(your_target PRIVATE fbs::crowd)
```

Installed use: `cmake --install build --prefix <prefix>`, then `find_package(FinalBuildCrowd 0.1 CONFIG REQUIRED)` with `<prefix>` on `CMAKE_PREFIX_PATH`, and link `fbs::crowd`. The installed package contains no Flecs headers.

This repository ships the C library only. Engine adapters and language bindings are not included.

## Design notes

- **Determinism.** Time is whole ticks, positions are integer millimetres and quantities are integer units; the library never reads a clock. Interactions are resolved in two sorted passes (station TAKE/PUT arbitration, then link admission) with unique keys, so Flecs storage order never decides an outcome. Floating point is used only to estimate link lengths in `fbs_crowd_set_graph` (corrected with integer arithmetic) and in `fbs_crowd_extract`, which is `const` and never feeds back. Commands and add/remove calls apply immediately between ticks, in call order. See `docs/API.md`, "Tick order".
- **Memory.** `fbs_crowd_create` allocates every fixed buffer from the config with `calloc` (handle tables, slot pool, graph, next-hop table, request buffers) and returns `FBS_CROWD_MEMORY` if any fails. Flecs allocates while entities are added and can grow past a previous peak. There are no allocator hooks in the public API. Per `docs/API.md`, allocation failure inside Flecs aborts the process.
- **Threading.** A context is used by one thread at a time. Separate contexts use separate Flecs worlds, but Flecs' OS API table is process-global.
- **Versioning.** `FBS_CROWD_VERSION` is `1` and is mixed into the state hash. The CMake project is version 0.1.0 with SOVERSION 0. There is no runtime version function and the config structs have no size field.
- **Errors.** Most calls return `fbs_crowd_result` (`OK`, `INVALID`, `CAPACITY`, `STALE`, `MEMORY`, `STATE`, `UNREACHABLE`, `BACKEND`). Single commands validate before changing anything. Batch commands are not atomic: every valid entry is applied, `results[i]` gets each outcome, and the call returns the first failure.

More detail: [API](docs/API.md) (semantics and limits),
[decisions](docs/DECISIONS.md) (why Flecs), and [host ECS integration](docs/HOST-ECS.md)
(mapping host entities to handles). The public entry point is
[include/fbs/crowd.h](include/fbs/crowd.h); [examples/basic.c](examples/basic.c)
is the minimal lifecycle program.

## License

MIT for Final Build Games' original code, see [LICENSE](LICENSE). The vendored Flecs 4.1.6 core amalgamation (unmodified, commit `fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8`) is MIT, Copyright (c) 2025 Sander Mertens, portions Copyright (c) Meta Platforms, Inc. and affiliates. Binary distributions must include its license. See [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and `third_party/flecs/`.
