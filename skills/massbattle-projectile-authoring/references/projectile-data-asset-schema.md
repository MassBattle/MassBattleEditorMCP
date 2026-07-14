# Projectile DataAsset Schema

The runtime asset type is `UMassBattleProjectileConfigDataAsset`.

## Common fields

```text
Projectile               FProjectileTag
SubType                  FSubType (currently not used by projectile runtime logic)
ProjectileParams         FProjectileParams
ProjectileEvent          FProjectileEvent
ProjectileSpawn          FProjectileSpawn
```

Do not confuse projectile `SubType` with Batch FX renderer `SubType`; the DataAsset tooltip states that the projectile framework currently does not use this value.

## Movement selection

```text
MovementMode
├── Static     -> ProjectileMove_Static
├── Interped   -> ProjectileMove_Interped
├── Ballistic  -> ProjectileMove_Ballistic
└── Tracking   -> ProjectileMove_Tracking
```

Only the struct matching `MovementMode` is authoritative.

## Damage selection

```text
DamageMode
├── Point   -> Damage_Point + Debuff_Point
├── Radial  -> Damage_Radial + Debuff_Radial
└── Beam    -> Damage_Beam + Debuff_Beam
```

Only the matching damage/debuff pair should be treated as active configuration.

## ProjectileParams

| Field | Meaning |
| --- | --- |
| `Radius` | Agent collision sweep radius along projectile movement. |
| `LifeSpan` | Remaining gameplay lifetime. |
| `Health` | Collision/penetration budget; entity collisions reduce it. This is not automatically a generic targetable hit-point system. |
| `DmgCoolDown` | Minimum interval between damage applications. |
| `DamageRepetitionMode` | `Periodic`, `OncePerOverlap`, `OnceForever`, or `None`. |
| `Query` | Which Mass entities can be detected/damaged. |
| `bRotationFollowVelocity` | Rotate projectile toward its flight direction. |
| `RotationInterpSpeed` | Rotation interpolation rate. |
| `InterpParams` | Smoothing parameters used by projectile rendering/Attached FX. |
| `bTraceOnlyOnArrival` | Skip continuous agent collision and check on arrival only. |
| `bCheckObstacle` | Enable environment collision. |
| `CheckRadius` | Environment sphere-sweep radius; zero uses line trace. |
| `TargetLocationSpreadXY/Z` | Random target-point spread. |
| `ApplyDmgConditions` | Conditions that cause damage/debuff. |
| `RemovalConditions` | Conditions that deactivate/recycle the projectile. |

Constructor defaults include:

```text
Removal: arrival, no lifespan, no health, obstacle = true
Removal: hit entity = false
Apply damage: hit entity = true
```

With default `Health=1` and `RemovalConditions.bOnNoHealth=true`, the first entity collision normally also removes the projectile indirectly after health reaches zero, even though `bOnHitEntity=false`. Set `bOnHitEntity` explicitly when entity impact itself is the intended terminal rule; use higher Health deliberately for penetration/multiple collisions.

## ProjectileSpawn

```text
ProjectileSpawn.OnBirth
ProjectileSpawn.OnHit
ProjectileSpawn.OnRemoval
```

Each `FProjectileSpawnContent` contains:

```text
bEnable
SpawnProjectile[]
SpawnActor[]
SpawnFx[]
PlaySound[]
```

This permits submunitions and chained content. Keep recursive spawning bounded and performance-reviewed.

## Unit-side launch entry

`FAttack.SpawnProjectile[]` stores `FProjectileConfig`:

```text
bEnable
ProjectileConfigDataAsset
Multipliers
Transform
bAttached
SpawnOrigin
Quantity
Delay
BindToAnimIndex
InheritFromInstigator
bDespawnWhenNoParent
SpawnProbability
AgentBehaviorState
```

The shared DataAsset defines the projectile class of behavior; the launch entry defines who launches it, from where, when, and with what per-instigator multipliers.
