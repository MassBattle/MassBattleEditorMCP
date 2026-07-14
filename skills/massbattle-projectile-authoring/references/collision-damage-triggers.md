# Collision, Damage, And Trigger Semantics

## Agent collision

The projectile processor sweeps a sphere from the previous to the current projectile location using `ProjectileParams.Radius`, filtered by `ProjectileParams.Query`.

- Point damage uses the first valid hit entity.
- Radial damage is evaluated around the projectile location.
- Beam damage uses the projectile transform plus configured beam offset, orientation, length, and radius.

## Environment collision

When `bCheckObstacle=true` and obstacle object types are configured:

- `CheckRadius=0` performs a line trace;
- `CheckRadius>0` performs a sphere sweep.

Match `CheckRadius` to the gameplay collision body, not necessarily the visible flame or smoke width.

## Continuous versus arrival-only trace

`bTraceOnlyOnArrival=true` skips continuous entity collision and performs the relevant check at arrival. This can reduce work for large volleys when mid-flight collisions are intentionally irrelevant.

Do not enable it for:

- interceptable missiles;
- shots that must hit units crossing the path;
- projectiles that must collide with environment before arrival;
- large slow projectiles whose physical presence matters.

## Damage repetition

- `Periodic`: repeat while conditions remain true, limited by cooldown.
- `OncePerOverlap`: once per overlap, reset after exit.
- `OnceForever`: one damage event during the projectile activation.
- `None`: no projectile damage; `OnHit` spawn content tied to damage processing also will not run.

Conventional bullets, shells, arrows, and missiles normally use `OnceForever`.

## Apply conditions versus removal conditions

These are independent masks.

Example conventional impact missile:

```text
ApplyDmgConditions.bOnHitEntity = true
RemovalConditions.bOnHitEntity = true
```

Example proximity/arrival explosion:

```text
ApplyDmgConditions.bOnArrival = true
RemovalConditions.bOnArrival = true
```

Example pass-through beam projectile:

```text
DamageRepetitionMode = Periodic
RemovalConditions.bOnHitEntity = false
```

## OnHit behavior

`ProjectileSpawn.OnHit` is executed inside the damage-processing branch when `bShouldApplyDmg` is true and repetition mode is not `None`. It is closer to “damage-trigger content” than an unconditional low-level collision callback.

Consequences:

- `DamageRepetitionMode=None` prevents this current OnHit path;
- a radial arrival trigger can execute OnHit even without a direct entity contact, depending configured conditions;
- periodic damage can execute OnHit repeatedly;
- use it for contact/damage feedback only after reviewing the trigger mode.

## OnRemoval behavior

`ProjectileSpawn.OnRemoval` executes whenever the projectile is removed due to any enabled removal condition:

- arrival;
- lifespan exhaustion;
- health exhaustion;
- obstacle hit;
- entity hit.

The current spawn content does not receive a distinct removal reason. If different terminal visuals are required, use separate projectile assets/configurations or extend runtime event/style selection.

## Duplicate event trap

A projectile may apply damage and meet a removal condition in the same tick. In that case both `OnHit` and `OnRemoval` can run.

Use one of these patterns:

```text
A. OnHit = small contact flash; OnRemoval = explosion
B. OnHit disabled; OnRemoval = complete impact/explosion
C. both enabled intentionally with visually distinct layers
```

Do not place the same full explosion in both by default.
