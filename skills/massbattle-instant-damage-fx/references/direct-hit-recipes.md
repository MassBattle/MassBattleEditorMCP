# Direct Hit Recipes

## Melee strike

```text
Attack.TimeOfHitAction = ApplyDMG
Attack.TimeOfHit / AnimHitTime = contact frame
Attack.SpawnFx muzzle/origin = optional weapon trail start
Attack.SpawnFx target impact = Burst, AtTarget, Delay=hit time
Damage mode = unit's configured point/direct damage path
```

A trail that persists through the swing is not a one-shot hit event; handle it as a separate Attached or animation-driven presentation layer.

## Hitscan rifle

```text
TimeOfHitAction = ApplyDMG
launch/muzzle Burst = AtSelf, Delay=0
impact Burst = AtTarget, Delay=TimeOfHit
Quantity = 1 for each logical event
```

A tracer can be visual-only, but if it needs actual travel/collision authority, route to projectile authoring.

## Instant cannon blast or spell

```text
TimeOfHitAction = ApplyDMG
muzzle/cast Burst = AtSelf
impact/explosion Burst = AtTarget at hit time
radial or other damage = gameplay configuration, not Niagara scale
```

Keep the visual explosion radius and gameplay damage radius deliberately synchronized through authoring/validation; they are not automatically the same field.

## Instant area pulse centered on self

```text
TimeOfHitAction = ApplyDMG
Burst = AtSelf, Delay=TimeOfHit
Damage = radial gameplay configuration
```

## Failure patterns

- setting `Delay=0` for a target impact and expecting it to wait until `TimeOfHit`;
- using `Quantity` as particle count;
- leaving `SoftNiagaraAsset` populated in a supposedly pure batch entry;
- using Niagara collision to determine gameplay hit;
- creating a projectile solely to play a flash that has no travel semantics;
- using direct `ApplyDMG` and projectile damage for the same intended single hit.
