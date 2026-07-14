---
name: massbattle-instant-damage-fx
description: Author and validate MassBattle direct instant-damage attacks and source-faithfully convert their one-shot visuals to MassBattleFrame Burst Batch FX. Use for melee hits, hitscan shots, direct spell impacts, instant radial damage, muzzle flashes, impact flashes, and explosions that do not require a travelling gameplay projectile. Route any attack with flight, homing, ballistic travel, collision, interception, or projectile-owned delayed damage to massbattle-projectile-authoring.
---

# MassBattle Instant Damage FX

## Scope

This skill owns attacks whose gameplay damage is resolved directly by the attacking Agent at `Attack.TimeOfHit` / `Attack.AnimHitTime`.

Examples:

- melee strike;
- hitscan rifle or cannon;
- direct magic hit;
- instant radial pulse;
- one-shot launch flash, target impact, explosion, debris burst, or death burst when no travelling gameplay projectile is required.

It also routes arbitrary one-shot source VFX through `$massbattle-effect-mcp`, whose only conversion target is a source-faithful MassBattleFrame Burst Batch FX. The source may be Niagara, Cascade, a Blueprint FX actor, a material/mesh/flipbook effect, or a composite Marketplace effect.

Do **not** use this skill for a missile, shell, arrow, grenade, moving beam head, homing object, interceptable shot, or anything whose travel/collision determines gameplay. Use `$massbattle-projectile-authoring` instead.

## Core Model

```text
Gameplay authority
    Agent attack state machine
    -> TimeOfHitAction
    -> FDamage / FDebuff

Visual authority
    Attack.SpawnFx
    -> FFxConfig Burst
    -> shared AMassBattleFxRenderer
    -> Burst NDC
    -> Niagara particles
```

Niagara never decides whether the attack hit or how much damage is dealt. The NDC transports one-shot visual events only.

## Mandatory Routing Test

Before authoring, answer:

```text
Does a gameplay object need to exist between launch and impact?
Does travel time, trajectory, obstacle collision, target motion, interception,
or projectile health affect the result?
```

- If **no**, continue with this skill.
- If **yes** to any item, hand off to `$massbattle-projectile-authoring`.

A merely delayed direct hit is still instant-damage authoring if no travelling gameplay object exists.

## Timing Rule — Read This First

`Attack.SpawnFx`, `Attack.SpawnProjectile`, `Attack.SpawnActor`, and `Attack.PlaySound` are instantiated when the attack enters `PreCast_FirstExec`, not when `TimeOfHit` is reached.

Therefore:

- a muzzle/launch flash normally uses `Delay = 0`;
- an impact FX at the target normally uses `Delay = Attack.TimeOfHit`;
- when `bAnimAsDuration=true`, create animation-bound entries and use the matching `AnimHitTime[index]` as each entry's authored delay;
- use `BindToAnimIndex` when attack animations have different hit frames;
- author delay in normal-speed seconds. Runtime divides the spawn delay by the current attack speed multiplier, matching the speed-scaled attack clock.

Read [attack timing and wiring](references/attack-timing-and-wiring.md) before editing an attack.

## Direct Attack Configuration

For a normal direct hit:

```text
Attack.bEnable                 = true
Attack.TimeOfHitAction         = ApplyDMG
Attack.TimeOfHit               = desired hit moment
Attack.Range / angle fields    = gameplay design
Damage / Debuff fragments      = actual gameplay result
Attack.SpawnProjectile         = empty unless another intentional projectile exists
```

Add separate `Attack.SpawnFx` entries for distinct event locations:

### Muzzle or cast origin

```text
SpawnOrigin       = AtSelf
Delay             = 0 or the launch-frame delay
bAttached         = false
Quantity          = 1
SubType            = matching Burst renderer
StyleType          = launch/muzzle style
SoftNiagaraAsset   = empty
SoftCascadeAsset   = empty
```

### Target impact

```text
SpawnOrigin       = AtTarget
Delay             = TimeOfHit or matching AnimHitTime
bAttached         = false
Quantity          = 1
SubType            = matching Burst renderer
StyleType          = impact/explosion style
SoftNiagaraAsset   = empty
SoftCascadeAsset   = empty
```

Use Niagara Spawn Count for sparks, fragments, smoke particles, and visual sub-elements. `FFxConfig.Quantity` multiplies logical Host/Burst instances and should normally remain `1`.

## Arbitrary Source VFX Conversion

“Convert” means preserve the visual function while replacing only its invocation/input ABI with MassBattleFrame batching:

```text
BatchE([C0, C1, ..., Cn]) == [E(C0), E(C1), ..., E(Cn)]
```

1. Identify the true source entry asset.
2. Enumerate visual layers: flash, core, smoke, debris, shockwave, trail, mesh, decal, light, sound, timing, curves, and event dependencies.
3. Separate visual behavior from gameplay, audio, camera, and collision logic.
4. Duplicate the exact source visual and prove source-neutral identity before editing.
5. Add only the Burst NDC adapter required by MassBattleFrame; preserve the source renderers, materials, curves, timing, random policy, events, and enabled state.
6. Run translation comparison and paired runtime validation against the source.
7. Keep sound in MassBattle sound configuration, damage in `FDamage`/`FDebuff`, and camera/gameplay logic in their own systems.
8. Read back the Niagara, renderer CDO, level renderer actor, and unit `FFxConfig`.

If the current ABI cannot express the source, report `blocked_by_runtime_abi`. Do not replace it with a visually similar template and count that as conversion.

An explicitly requested optimized recreation may be kept under a separate `Optimized`/`Approx` path, but it is not part of the accepted conversion count.

Read [one-shot conversion workflow](references/one-shot-conversion-workflow.md) for composite sources.

## MCP Workflow

1. Call API status for Unit, Effect Asset, and Niagara MCP.
2. Read the unit with `unit_get`; use `unit_get_schema` before writing unfamiliar nested fields.
3. Query and summarize the source VFX with Effect Asset MCP.
4. Inspect the exact source graph and dependencies through `$massbattle-effect-mcp` primitives.
5. Duplicate the exact source asset; do not overwrite it and do not start the accepted translation from a visual template.
6. Add the Burst NDC adapter with Niagara graph primitives. If graph insertion/rewiring or the runtime ABI is insufficient, report the precise blocker.
7. Configure and read back the renderer Blueprint defaults.
8. Build a partial source-aligned unit patch for `Attack`, `Damage`, `Debuff`, and `Attack.SpawnFx` as required.
9. Use `unit_write` / the advertised Unit MCP write flow, then read the unit back.
10. Place the corresponding renderer actor and validate source/translation timing and visuals in a test map.

Do not invent a high-level “convert” MCP call. Combine primitive tools and report every capability gap.

## Batch FX Contract

A direct one-shot visual normally uses Burst:

```text
BurstPosition and/or BurstLocation
BurstOrientation
BurstScale
SubType
Style
```

The renderer's `SubType` must match the unit `FFxConfig.SubType`. A Burst renderer requires a non-null Niagara system and matching Burst NDC. Pure batch configuration leaves ordinary Niagara/Cascade asset fields empty.

Read [Burst Batch FX contract](references/burst-batch-contract.md).

## Verification Gates

Do not report completion until the applicable gates are distinguished:

1. **Gameplay static gate**: `TimeOfHitAction`, damage/debuff, range, and hit timing are correct.
2. **FX static gate**: source semantics accounted for; Niagara consumes Burst data; renderer defaults and `SubType` agree; unbatched asset fields are empty.
3. **Timing gate**: muzzle and impact appear at intended launch/hit times for every bound attack animation.
4. **Runtime gate**: direct damage occurs once; the Burst triggers once; the renderer is registered in the world.
5. **Performance gate**: logical effects share renderer/Niagara components by batch rather than spawning one component per hit.

Use one final status:

```text
analysis_only
blocked_by_mcp_capability
blocked_by_runtime_abi
converted_not_runtime_verified
converted_and_runtime_verified
partial_hybrid
```

## Handoff To Projectile Skill

Hand off immediately when the user asks for:

- missile, shell, grenade, arrow, rocket, bullet entity, torpedo;
- visible travel that must track or arc;
- collision with obstacles or units during flight;
- projectile interception or projectile health;
- impact damage owned by the travelling object;
- `ProjectileSpawn.OnBirth`, `OnHit`, or `OnRemoval` behavior.

The projectile skill may call back into this skill for the launch, impact, and explosion Burst layers.
