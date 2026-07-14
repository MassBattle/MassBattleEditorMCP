# Burst Batch FX Contract

## Definition

A MassBattle Burst FX is one logical one-shot event delivered to a shared Niagara renderer. It is not one Niagara Component per event.

```text
many FFxConfig Burst events
    -> MassBattle FX render processor
    -> one batched NDC write per renderer/subtype
    -> shared Niagara system instances
```

## Current fields

The runtime writer can supply:

```text
BurstPosition and/or BurstLocation
BurstOrientation
BurstScale
SubType
Style
```

A new visual normally needs a new Niagara implementation or Style branch, not a new NDC schema.

## Renderer requirements

- `NiagaraSystemAsset` is required.
- `NDC_BurstFx` is required for Burst consumption.
- renderer `SubType` must equal the `FFxConfig.SubType` used by the event.
- the renderer actor must exist in the world at BeginPlay so it can register the subtype.

## Pure batch FFxConfig

```text
SubType != None
bAttached = false
SoftNiagaraAsset = empty
SoftCascadeAsset = empty
Quantity = 1 in the ordinary case
```

Setting `SubType` and an ordinary Niagara/Cascade asset together can execute both batch and unbatched paths. Label that result hybrid; do not call it fully batched.

## SubType versus Style

Use `Style` for per-event variants that share one graph and renderer:

- surface type;
- color family;
- mesh/material variant;
- launch versus impact versus explosion branches when the graph remains coherent.

Use a different `SubType` only when a different Niagara system/renderer contract is required. Excessive subtypes fragment batches.

## Quantity

`FFxConfig.Quantity` multiplies logical Host/event instances. Particle count belongs inside Niagara.

```text
Correct for 40 sparks:
    Quantity = 1
    Niagara Spawn Count = 40

Usually wrong:
    Quantity = 40
    Niagara Spawn Count = 1
```

## When NDC must be extended

Extend the schema only when every event needs data that cannot be represented by transform, scale, subtype, and style—for example independent dynamic color, impact energy, surface identifier, multiple independent radii, or removal reason.

A schema extension requires coordinated changes to the NDC asset, C++ event storage/writer, Niagara reader, MCP contract, and validation. Adding a field to only one layer is incomplete.
