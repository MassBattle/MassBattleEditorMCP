# Attack Timing And Wiring

## Actual execution order

The Agent attack state machine performs these actions when entering `PreCast_FirstExec`:

```text
start attack animation
spawn Attack.SpawnProjectile entries
spawn Attack.SpawnActor entries
spawn Attack.SpawnFx entries
spawn Attack.PlaySound entries
emit attack-begin event
```

The spawned Hosts may wait on their own `Delay`. Later, while the attack is in `PreCast`, the state machine compares attack time with the resolved hit time and executes `TimeOfHitAction` once.

Consequences:

- `SpawnFx.Delay=0` means attack-start/launch time, not hit time.
- An `AtTarget` impact must be delayed to the direct hit timestamp.
- `SpawnProjectile.Delay` is the projectile launch delay relative to attack start.
- `Attack.TimeOfHitAction=ApplyDMG` is independent from `Attack.SpawnFx`.

## Fixed hit time

When `bAnimAsDuration=false`:

```text
resolved hit time = Attack.TimeOfHit
```

Typical setup:

```text
muzzle FX Delay = 0
impact FX Delay = Attack.TimeOfHit
impact sound Delay = Attack.TimeOfHit
```

## Animation-specific hit time

When `bCanPlayAnim=true` and `bAnimAsDuration=true`:

```text
resolved hit time = Attack.AnimHitTime[selected animation index]
```

If animations 0, 1, and 2 hit at different times, use separate entries:

```text
SpawnFx entry A: BindToAnimIndex=0, Delay=AnimHitTime.Anim0
SpawnFx entry B: BindToAnimIndex=1, Delay=AnimHitTime.Anim1
SpawnFx entry C: BindToAnimIndex=2, Delay=AnimHitTime.Anim2
```

Do not use one generic delayed impact entry when hit frames differ materially.

## Speed multiplier

The runtime scales the attack clock with attack speed and divides each spawn delay by the same speed multiplier. Author delays in the base animation/attack timeline. Do not pre-divide the configured values.

## Direct damage versus projectile damage

Direct instant attack:

```text
Attack.TimeOfHitAction = ApplyDMG or SuicideATK
Agent FDamage/FDebuff owns the result
```

Projectile-owned attack:

```text
Attack.TimeOfHitAction = None
Projectile DataAsset owns damage/debuff
```

Leaving `ApplyDMG` enabled while the projectile also damages on hit normally creates two gameplay damage paths. Only keep both when the design intentionally has a direct component plus a projectile component.

## Spawn origin

- `AtSelf` uses the attacker transform and configured local offset. Use for muzzle/cast/launch.
- `AtTarget` uses the target location. Use for direct impact visuals.

For direct attacks, `AtTarget` is resolved when the Host is configured at attack start. Treat fast-moving-target accuracy as a runtime behavior to test; do not assume it is a late-bound hit transform without verification.
