# Projectile FX Lifecycle Link

## Principle

Projectile gameplay and projectile presentation are separate:

```text
Mass projectile Entity
    authoritative position, target, collision, damage, removal

Batch FX
    visual body, trail, launch, impact, explosion
```

## Launch event

Author through the attacker's `Attack.SpawnFx`:

```text
Burst
SpawnOrigin = AtSelf
Delay = same launch time as SpawnProjectile.Delay
Quantity = 1
```

Use `$massbattle-instant-damage-fx` to convert the source muzzle/cast/launch VFX and verify the Burst renderer contract.

## Flight presentation

Author through `ProjectileSpawn.OnBirth.SpawnFx`:

```text
bEnable = true
bAttached = true
SpawnOrigin = AtSelf
Quantity = 1
LifeSpan = -1 for parent-controlled duration
bDespawnWhenNoParent = true
SubType = Attached flight renderer
SoftNiagaraAsset / SoftCascadeAsset = empty for pure batch
```

One Attached logical instance can drive:

- projectile mesh;
- engine flame;
- glow;
- trail source;
- smoke source.

Do not create separate logical FX Hosts for every visual emitter unless their independent lifecycles require it.

Attached presentation uses persistent arrays, not the Burst NDC. The projectile renderer maintains interpolated transform data so the visual can follow fixed-step simulation smoothly.

## Impact event

Use `OnHit.SpawnFx` only after understanding that OnHit is tied to damage processing. Suitable for:

- contact spark;
- shield flash;
- small armor hit.

It may repeat under periodic damage and may execute in the same tick as OnRemoval.

## Terminal event

Use `OnRemoval.SpawnFx` for the reliable final event:

- full explosion;
- disappearance flash;
- airburst;
- timeout fizzle, if one common terminal visual is acceptable.

Because current spawn content lacks removal reason, all configured removal causes share this content.

## Sounds

Place launch sound at the attacker and terminal sound in the relevant projectile spawn content. Do not embed gameplay sound ownership into Niagara merely because the visual shares timing.

## NDC customization

Standard launch/impact/explosion uses existing Burst fields and does not require a new NDC.

Extend Burst data only when each event needs additional independent data such as impact energy, surface type, dynamic color, or terminal reason. Continuous values such as exact projectile velocity belong in Attached arrays or can be derived from persistent position history; do not add them to a one-shot Burst channel by default.
