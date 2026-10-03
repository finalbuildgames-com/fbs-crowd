# Using fbs::crowd from a host with its own ECS

The crowd context is the **only authoritative owner** of worker, chain and station
simulation state. A host ECS (Flecs, EnTT, a hand-written registry, a host's C# model) may
refer to crowd entities, but must not keep a second authoritative copy of their state.

## Rules

1. **Store the handle, not the state.** Give host entities a component such as
   `CrowdWorkerRef { fbs_crowd_handle handle; }`. Read state through
   `fbs_crowd_worker_get` / `fbs_crowd_extract`; change it only through crowd commands.
2. **One direction of ownership.** Create crowd workers from host logic (or a host data import),
   store the returned handle, and remove the crowd worker when the host entity is
   destroyed. If the crowd reports `STALE`, the host entity's reference is dead: drop it.
3. **Host tags for identity.** Put the host's stable id in `desc.tag`. Save files store the
   host id and the crowd handle/tag; the crowd state hash covers both.
4. **No shadow worlds.** Do not mirror crowd components into host components each frame
   for gameplay decisions. Derived caches (render instances, selection highlight, UI rows)
   are fine when they are rebuilt from crowd queries and never written back.
5. **Structural changes between ticks.** Issue commands and add/remove calls between
   `fbs_crowd_step` calls. The crowd applies them immediately; ordering between them is
   the call order.
6. **Determinism belongs to the host too.** Issue commands at fixed ticks in a stable order
   (e.g. sorted by host id). Never derive commands from frame timing or visibility.
7. **Do not share the Flecs world.** The crowd's Flecs world is private. A host that also
   uses Flecs keeps its own world; there is no API to reach the crowd's world, and using a
   crowd component id in another world is undefined.
8. **Threads.** One thread at a time per crowd context. A host job system may run the crowd
   step as one job; it must not call the crowd from other jobs concurrently.

## Mapping example (host Flecs, C)

This is a host integration sketch: the host supplies its Flecs component registration,
spawn/destruction hooks, buffers and error handling.

```c
typedef struct CrowdWorkerRef { fbs_crowd_handle handle; } CrowdWorkerRef;

/* spawn */
fbs_crowd_worker_desc d = {node, speed_mm_per_tick, variant, (uint64_t)host_entity};
fbs_crowd_handle h;
if (fbs_crowd_add_worker(crowd, &d, &h) == FBS_CROWD_OK)
    ecs_set(host_world, host_entity, CrowdWorkerRef, {h});

/* host entity destroyed (e.g. in an OnRemove hook) */
uint32_t res, amount;
fbs_crowd_remove_worker(crowd, ref->handle, &res, &amount); /* cargo comes back as exported */

/* per frame */
fbs_crowd_step(crowd, ticks);            /* fixed ticks */
fbs_crowd_extract(crowd, frac, inst, cap, &n, &total);
/* Render inst[]. fbs_crowd_worker_get(crowd, inst[i].worker, &info)
   returns info.tag for mapping back to the host. */
```

## Host data and other modules

Neither other FinalBuildSystems modules nor the host need to convert to Flecs. A host's
world data can describe rooms, passages, stations and chains; the host translates that data
into `fbs_crowd_set_graph`, `add_station` and `add_chain` calls.
