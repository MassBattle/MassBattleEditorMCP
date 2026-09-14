# Projectile Configuration Recipes

These are starting profiles, not universal balance values. Keep field semantics exact and tune numbers in runtime tests.

## 1. Straight bullet or bolt

```text
MovementMode = Interped
ProjectileMove_Interped.Speed = desired travel speed
DamageMode = Point
ProjectileParams.Radius = small physical sweep radius
DamageRepetitionMode = OnceForever
ApplyDmgConditions.bOnHitEntity = true
RemovalConditions.bOnHitEntity = true
RemovalConditions.bOnHitObstacle = true
RemovalConditions.bOnNoLifeSpan = true
bCheckObstacle = true when terrain/walls block the shot
bTraceOnlyOnArrival = false when crossing targets can be hit
```

Visuals:

```text
launch Burst
optional Attached tracer/body
impact Burst or OnRemoval Burst
```

For true hitscan with no gameplay travel, use the instant-damage skill instead.

## 2. Ballistic shell or grenade

```text
MovementMode = Ballistic
SolveMode = FromPitch or FromSpeed
Gravity = world/design gravity
MaxSpeed = safety limit
DamageMode = Radial
DamageRepetitionMode = OnceForever
ApplyDmgConditions = arrival and/or obstacle/entity hit
RemovalConditions = same terminal conditions
TargetLocationSpreadXY/Z = accuracy dispersion when desired
```

Choose:

- `FromPitch` when art/design requires a consistent launch angle;
- `FromSpeed` when weapon muzzle velocity is authoritative.

Use OnBirth Attached smoke/tracer and OnRemoval explosion. Test unreachable targets and high/low arc behavior.

## 3. Homing missile

```text
MovementMode = Tracking
Speed = initial speed
MaxSpeed = terminal speed
Acceleration = forward acceleration
LateralAcceleration = finite steering acceleration
bPredictTargetMovement = true/false by design
PredictionMultiplier = lead tuning

DamageMode = Radial
DamageRepetitionMode = OnceForever
ProjectileParams.Radius = missile contact sweep
LifeSpan = maximum flight time
Health = 1 for one collision; higher only for intentional penetration/multiple collisions
bRotationFollowVelocity = true
bTraceOnlyOnArrival = false
bCheckObstacle = true
ApplyDmgConditions.bOnHitEntity = true
RemovalConditions.bOnHitEntity = true
RemovalConditions.bOnHitObstacle = true
RemovalConditions.bOnArrival = true
RemovalConditions.bOnNoLifeSpan = true
```

Unit:

```text
Attack.TimeOfHitAction = None
SpawnProjectile.Quantity = 1
SpawnProjectile.Delay = launch frame
```

Visuals:

```text
Attack launch Burst
OnBirth Attached missile mesh/tail/trail
OnHit optional small contact Burst
OnRemoval full explosion Burst
```

## 4. Persistent beam or sweeping damage object

```text
MovementMode = Static or Tracking, depending whether origin moves
DamageMode = Beam
DamageRepetitionMode = Periodic
DmgCoolDown = damage tick interval
RemovalConditions = lifetime/health/arrival as appropriate
```

Use a persistent Attached visual. If the beam is truly instantaneous and damage occurs once, direct instant-damage authoring may be cheaper and semantically clearer.

## Worked missile checklist

### Unit layer

- [ ] attack range allows launch;
- [ ] `TimeOfHitAction=None`;
- [ ] DataAsset path is valid;
- [ ] local muzzle transform is correct;
- [ ] launch delay matches animation;
- [ ] quantity is 1;
- [ ] launch Burst uses same timing.

### DataAsset layer

- [ ] Tracking movement selected;
- [ ] speed/acceleration/turn radius are plausible;
- [ ] target prediction tested;
- [ ] radial damage configured;
- [ ] query excludes allies/owner as intended;
- [ ] collision radius is physical, not visual smoke width;
- [ ] entity hit applies damage and removes projectile;
- [ ] obstacle/arrival/timeout behavior is deliberate;
- [ ] repetition is once forever;
- [ ] lifecycle spawn content does not double-explode.

### Presentation layer

- [ ] OnBirth Attached FX follows and disappears with parent;
- [ ] launch/impact/explosion are pure Batch FX;
- [ ] renderer subtypes match configs;
- [ ] renderer actors exist in the test map;
- [ ] no per-projectile ordinary Niagara component exists;
- [ ] explosion visual radius is reviewed against gameplay radius.
