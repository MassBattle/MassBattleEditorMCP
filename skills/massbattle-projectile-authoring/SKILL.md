---
name: massbattle-projectile-authoring
description: Configure, wire, validate, and troubleshoot MassBattle projectile attacks. Use for bullets represented by Mass entities, shells, arrows, grenades, missiles, rockets, torpedoes, homing shots, ballistic arcs, persistent beams, interceptable projectiles, and any attack whose travel, collision, arrival, lifetime, or projectile-owned damage matters. This skill links unit Attack.SpawnProjectile, UMassBattleProjectileConfigDataAsset movement/damage/trigger fields, Attached flight FX, and Burst launch/hit/removal FX. Use massbattle-instant-damage-fx for the one-shot Burst layers and for attacks that do not need a travelling gameplay projectile.
---

# MassBattle Projectile Authoring

## Scope

This skill owns the complete projectile gameplay chain:

```text
attacker Attack.SpawnProjectile
    -> projectile Host
    -> pooled Mass projectile Entity
    -> movement
    -> collision / arrival / lifetime conditions
    -> projectile damage and debuff
    -> OnBirth / OnHit / OnRemoval content
    -> recycle to the per-DataAsset projectile pool
```

It also coordinates presentation:

```text
launch flash                   -> Burst Batch FX
projectile body/tail/trail     -> Attached Batch FX
impact flash/explosion         -> Burst Batch FX
sound                          -> MassBattle Sound Host
```

Use `$massbattle-instant-damage-fx` to author and validate the one-shot launch, impact, and explosion Burst layers. Do not simulate authoritative projectile movement or collision in Niagara.

All linked visuals still use the single `$massbattle-effect-mcp` definition: preserve the source effect and replace only its invocation/input ABI with MassBattleFrame Burst NDC or Attached-array batching. Projectile routing selects gameplay ownership; it does not permit a different or approximate Niagara conversion.

## First Decision: Direct Hit Or Projectile

Use this skill when at least one is true:

- the shot has visible travel time that matters;
- it follows a straight interpolation, ballistic arc, or homing path;
- obstacles or units can intercept it;
- arrival at a destination triggers damage;
- projectile lifetime or collision/penetration budget controls the outcome;
- the projectile itself owns point, radial, or beam damage;
- the flight object needs persistent Attached presentation.

Otherwise use `$massbattle-instant-damage-fx` and direct `Attack.TimeOfHitAction`.

## Non-Negotiable Damage Rule

For a normal projectile-owned attack:

```text
Attack.TimeOfHitAction = None
Projectile DataAsset Damage/Debuff = authoritative damage
```

`Attack.SpawnProjectile` is created at `PreCast_FirstExec`, using its own `Delay`. The Agent's `TimeOfHitAction` still executes later unless disabled. Leaving `ApplyDMG` enabled while the projectile also damages normally causes two independent damage paths.

Only keep both when the design explicitly includes a direct component and a projectile component.

## Required Configuration Layers

A projectile attack is not complete until all three layers agree.

### 1. Unit attack layer

```text
AgentConfig.Attack
├── attack animation and timing
├── TimeOfHitAction
├── SpawnProjectile[]
├── optional launch SpawnFx[]
└── optional launch PlaySound[]
```

### 2. Projectile DataAsset layer

```text
UMassBattleProjectileConfigDataAsset
├── ProjectileParams
├── MovementMode and movement struct
├── DamageMode and matching Damage/Debuff structs
├── ProjectileEvent
└── ProjectileSpawn
    ├── OnBirth
    ├── OnHit
    └── OnRemoval
```

### 3. Batch presentation layer

```text
launch / hit / explosion  -> Burst NDC
flight body / tail / trail -> Attached arrays
renderer Blueprint         -> Niagara + SubType (+ NDC for Burst)
```

Read [projectile schema](references/projectile-data-asset-schema.md) before authoring a new DataAsset.

## Authoring Workflow

### Step 1 — Capability check

Call Unit, Effect, Niagara, and `projectile_get_api_status` first. The intended Projectile MCP surface is:

```text
projectile_get_api_status
projectile_list / projectile_query
projectile_get
projectile_get_schema
projectile_create
projectile_write
projectile_validate
projectile_delete
```

Use `projectile_get_schema` before unfamiliar writes. `projectile_write` is a source-aligned union write: omitted fields remain unchanged; array append or replacement must be explicit. Run `projectile_validate` after every create/write and read the asset back.

If the running editor has not yet loaded this Projectile MCP version:

- you may wire an already-existing projectile DataAsset through `Attack.SpawnProjectile`;
- you may inspect source code and asset dependencies;
- you may provide exact editor field settings;
- you must not claim that a new projectile DataAsset was created or modified through MCP;
- report `blocked_by_mcp_capability` for unavailable asset mutation, or request the narrow projectile MCP contract in [MCP capability gap](references/mcp-capability-gap.md).

### Step 2 — Inspect existing unit and closest projectile template

Read the unit and its current attack arrays. Find the nearest projectile DataAsset by intended movement and damage semantics, not by asset name alone.

Prefer duplication of a proven projectile DataAsset over building an unknown configuration from scratch.

### Step 3 — Choose movement mode

- `Static`: stationary damage volume or persistent beam origin.
- `Interped`: deterministic travel from start to target with optional X/Y/Z offset curves.
- `Ballistic`: gravity arc solved from pitch or speed.
- `Tracking`: homing movement with forward and lateral acceleration plus optional target leading.

Read [movement modes](references/movement-modes.md).

### Step 4 — Choose damage mode

- `Point`: first valid hit entity.
- `Radial`: area damage centered on projectile location when a trigger condition fires.
- `Beam`: swept/capsule-like damage along a configured beam direction and length.

Configure only the matching damage/debuff structures as authoritative.

### Step 5 — Configure collision, damage triggers, and removal triggers

Set:

- collision `Radius` and target `Query`;
- `DamageRepetitionMode` and `DmgCoolDown`;
- `ApplyDmgConditions`;
- `RemovalConditions`;
- environment collision fields;
- lifetime and health.

Do not rely on implicit defaults for a one-hit missile. The constructor leaves `RemovalConditions.bOnHitEntity=false`, but default `Health=1` plus `bOnNoHealth=true` can still remove it after the first entity collision. Configure the intended hit/removal semantics explicitly. Read [collision and trigger semantics](references/collision-damage-triggers.md).

### Step 6 — Wire the unit's `Attack.SpawnProjectile`

Typical entry:

```text
bEnable                    = true
ProjectileConfigDataAsset  = selected projectile asset
SpawnOrigin                = AtSelf
Transform                  = muzzle local transform
Quantity                   = 1
Delay                      = launch moment from attack start
BindToAnimIndex            = -1 or matching attack animation
Multipliers                = per-unit variation only
```

Use DataAsset defaults for shared projectile behavior. Use per-unit multipliers for intentional variants such as damage, speed, scale, gravity, pitch, acceleration, or lateral acceleration. Do not duplicate entire projectile assets solely for a small multiplicative variant unless asset organization requires it.

`SpawnProjectile.Delay` is the launch delay from `PreCast_FirstExec`; it is not the projectile travel duration.

### Step 7 — Configure projectile lifecycle presentation

Recommended chain:

```text
Attack.SpawnFx
    launch/muzzle Burst at attacker

ProjectileSpawn.OnBirth.SpawnFx
    Attached flight body/tail/trail on projectile

ProjectileSpawn.OnHit.SpawnFx
    optional damage-triggered contact Burst

ProjectileSpawn.OnRemoval.SpawnFx
    terminal explosion/removal Burst
```

Read [FX lifecycle linkage](references/fx-lifecycle-link.md).

### Step 8 — Read back and runtime-test

Validate:

- unit launch timing and `TimeOfHitAction`;
- DataAsset movement/damage modes and trigger fields;
- query/obstacle collision;
- exactly one intended damage application;
- OnHit versus OnRemoval duplication;
- Attached flight FX cleanup on projectile deactivation;
- renderer actor registration;
- projectile pooling/reuse, including OnBirth firing again after reuse;
- performance under representative projectile counts.

## Standard Homing Missile Configuration

Use this as a starting point, then tune from measured gameplay.

### Unit attack

```text
Attack.TimeOfHitAction = None
Attack.SpawnProjectile[0]
    bEnable = true
    ProjectileConfigDataAsset = DA_Projectile_Missile
    SpawnOrigin = AtSelf
    Transform = muzzle local offset/rotation
    Quantity = 1
    Delay = launch frame from attack start
    BindToAnimIndex = matching animation or -1

Attack.SpawnFx[launch]
    Burst, AtSelf, Quantity=1
```

### Projectile DataAsset

```text
MovementMode = Tracking
ProjectileMove_Tracking
    Speed = launch speed
    MaxSpeed = terminal speed
    Acceleration = forward acceleration
    LateralAcceleration = finite turn acceleration
    bPredictTargetMovement = true when desired
    PredictionMultiplier = tuned lead amount

DamageMode = Radial
Damage_Radial = authoritative explosive damage/radius
Debuff_Radial = optional

ProjectileParams
    Radius = physical hit sweep radius
    LifeSpan = maximum flight time
    Health = 1 for one collision; use higher values only for intentional penetration/multiple collisions
    DamageRepetitionMode = OnceForever
    Query = valid enemy targets only
    bRotationFollowVelocity = true
    RotationInterpSpeed = suitable visual turn rate
    bTraceOnlyOnArrival = false
    bCheckObstacle = true when environment collision is required
    CheckRadius = obstacle sweep radius; 0 means line trace
    ApplyDmgConditions.bOnHitEntity = true
    ApplyDmgConditions.bOnHitObstacle = design-specific
    ApplyDmgConditions.bOnArrival = design-specific
    RemovalConditions.bOnHitEntity = true
    RemovalConditions.bOnHitObstacle = true
    RemovalConditions.bOnArrival = true
    RemovalConditions.bOnNoLifeSpan = true
```

### Lifecycle visuals

```text
OnBirth
    Attached missile body/tail/trail
    bAttached = true
    SpawnOrigin = AtSelf
    LifeSpan = -1
    bDespawnWhenNoParent = true
    Quantity = 1

OnHit
    optional contact Burst only

OnRemoval
    explosion Burst, Quantity=1
    explosion sound
```

If OnHit and OnRemoval both contain the same explosion, an entity hit that also removes the projectile can produce the effect twice in the same simulation tick.

The full recipe is in [configuration recipes](references/configuration-recipes.md).

## Performance Rules

1. One gameplay missile normally equals one pooled Mass projectile Entity, not one Niagara Component.
2. Flight visuals use one Attached logical instance; launch/impact/explosion use one Burst event each.
3. Keep FX `Quantity=1`; emit particles inside Niagara.
4. Never run authoritative movement/collision in Niagara.
5. Use `bTraceOnlyOnArrival=true` only when mid-flight collision is intentionally irrelevant; it is inappropriate for interceptable/homing missiles that must hit obstacles or units en route.
6. Keep collision radius and query as narrow as the design permits.
7. Use `DamageRepetitionMode=OnceForever` for a conventional one-hit projectile.
8. Avoid ordinary per-projectile `SoftNiagaraAsset`/`SoftCascadeAsset` components in a pure batch presentation path.
9. Reuse DataAssets/archetypes and per-DataAsset pools; do not create unique projectile types per shot.

## Reporting

Separate these states:

```text
projectile_design_only
projectile_asset_wired_existing
projectile_asset_configured_not_runtime_verified
projectile_asset_configured_and_runtime_verified
partial_hybrid
blocked_by_mcp_capability
```

Also report the linked visual status from `$massbattle-instant-damage-fx` for launch/hit/explosion layers.
