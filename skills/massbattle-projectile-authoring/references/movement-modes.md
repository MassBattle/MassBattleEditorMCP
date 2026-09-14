# Projectile Movement Modes

## Static

Use for:

- stationary hazard/damage volume;
- persistent beam origin;
- area object whose transform is not advanced by projectile movement.

A Static projectile can still apply damage based on configured trigger logic. Ensure lifecycle and damage repetition are deliberate.

## Interped

Configuration:

```text
Speed
XOffset / YOffset / ZOffset runtime curves
XOffsetRangeMap / YOffsetRangeMap / ZOffsetRangeMap
X/Y/Z random multiplier ranges
```

Use for deterministic travel to a target/destination with authored visual curvature. The runtime stores start point, end point, target handle, target offset, birth time, and randomized curve multipliers.

Suitable for:

- arrows or bolts with controlled travel;
- stylized magic arcs;
- bullets when an actual travelling Entity is required but physical homing is not;
- lob-like visuals that should not use gravity solving.

The offset curves are presentation/gameplay path offsets, not Niagara-only trails.

## Ballistic

Configuration:

```text
MaxSpeed
Gravity
Iterations
SolveMode = FromPitch or FromSpeed
Pitch      when FromPitch
Speed and bFavorHighArc when FromSpeed
```

Use for shells, grenades, mortars, and gravity arcs.

### FromPitch

The launch angle is specified and the solver determines the required velocity within constraints.

### FromSpeed

Initial speed is specified and the solver chooses an arc; `bFavorHighArc` selects high versus flatter solution when available.

Validate unreachable target cases, maximum speed, target elevation, and obstacle interaction in runtime tests.

## Tracking

Configuration:

```text
MaxSpeed
Speed
Acceleration
LateralAcceleration
bPredictTargetMovement
PredictionMultiplier
```

Use for missiles, rockets, torpedoes, homing magic, and guided shots.

The important relationship is approximately:

```text
turn radius = speed² / lateral acceleration
```

Higher lateral acceleration tightens turns. In the current tooltip, zero means instant turn. Use a finite positive value when the missile should display plausible steering.

`bPredictTargetMovement` leads a moving target. Tune `PredictionMultiplier`; do not assume `1.0` is correct for all acceleration profiles.

## Choosing a mode

| Desired behavior | Mode |
| --- | --- |
| no travel | direct attack or Static projectile |
| deterministic straight/curved travel | Interped |
| gravity arc | Ballistic |
| active homing | Tracking |
| persistent directional damage volume | Static/Tracking with Beam damage, depending movement |

Do not choose Tracking merely to make a visual face the target. Use it only when the gameplay flight path must home.
