# One-Shot Source VFX Conversion

## Source classification

The source may be:

- Niagara System;
- Cascade `UParticleSystem`;
- Blueprint FX actor;
- mesh/material/decal/flipbook setup;
- composite effect combining particles, light, sound, camera, collision, and gameplay events.

“VFX” is a presentation category, not an Unreal asset class. Locate the actual entry asset and dependency graph before editing.

## Semantic inventory

Record:

```text
visual layers
spawn timing and duration
particle counts and rates
position/orientation/scale behavior
materials, textures, meshes, ribbons, decals, lights
curves, random ranges, collision modules
sound and camera behavior
gameplay damage/collision/events
```

## Decomposition

Route each layer to its owning system without changing the visual program:

| Source behavior | Target system |
| --- | --- |
| one-shot particles/mesh burst | Burst batch Niagara |
| damage/debuff | Agent `FDamage` / `FDebuff` |
| sound | MassBattle sound configuration |
| camera shake | camera system |
| gameplay collision/event | gameplay logic |
| long persistent residue | source-faithful persistent/Attached batch path |
| decal | preserved decal layer plus the required MassBattle trigger adapter |
| dynamic light | preserved Niagara light renderer or source-equivalent batched companion |

A composite source may route gameplay, sound, camera, and visuals to different owning systems, but every source visual layer must be accounted for. Missing layers make the conversion partial or blocked; they do not become an accepted approximation implicitly.

## Accepted translation

A proven MassBattle module may supply transport plumbing, but the accepted target starts from the exact source visual:

- duplicate the source and prove exact source-neutral identity;
- add only the Burst NDC adapter and declared adapter edges;
- preserve source materials, meshes, textures, renderers, curves, events, timing, and random policy;
- compare the translation structurally and in a paired runtime scene;
- report `blocked_by_mcp_capability` or `blocked_by_runtime_abi` when the required adapter cannot be expressed.

An explicitly requested template recreation belongs under `Optimized`/`Approx` and never counts as the source-faithful conversion.

## Static and runtime evidence

Static evidence:

- dependency/read summaries;
- Niagara module and renderer readback;
- renderer CDO readback;
- unit config readback.

Runtime evidence:

- renderer actor registered;
- one event produces one intended burst;
- visual timing aligns with damage;
- no ordinary per-hit Niagara/Cascade component is spawned;
- component/system counts scale by batch, not by hit count.
