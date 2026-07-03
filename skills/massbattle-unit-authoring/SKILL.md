---
name: massbattle-unit-authoring
description: Create, inspect, edit, delete, and validate MassBattle unit assets through the MassBattleEditorMCP/Unit MCP workflow in UE5. Use when Codex needs to manage MassBattle AgentConfig units, generate VAT/static-mesh unit assets from existing skeletal meshes, apply style defaults, bind renderer/Niagara/projectile/sound attack data, or batch-edit unit JSON without direct .uasset mutation.
---

# MassBattle Unit Authoring

Use UE editor APIs, commandlets, or the MassBattleEditorMCP tools. Do not edit `.uasset` files directly. Keep names and paths aligned with existing MassBattle source conventions; prefer cloning a nearby official/demo unit template and applying a small union patch.

## Inspect Or Edit

1. List or locate units with the Unit MCP discovery tools, scoped to style roots such as `/Game/Unit`, `/Game/Toon_Soldiers_WW2`, `/Game/StylizedArmyPackA`, and `/Game/World_Flags`.
2. Read normal unit data with simple JSON first. Request detailed mode or a specific object only for complex fields such as `Attack`, `Visualize`, `LODShared`, or `AnimShared`.
3. Apply edits through `unit_write`; omitted fields must remain unchanged. Use `unit_delete` for explicit deletes.
4. For array fields such as `Attack.SpawnProjectile`, `Attack.SpawnFx`, and `Attack.PlaySound`, union merge can append missing elements by default. Pass `append_arrays=false` only when append should be rejected.

## Create Unit

1. Discover source assets first: source skeletal mesh, compatible renderer Blueprint class, Niagara system, material/skin, animation search roots, and a template unit.
2. Use `unit_create` when a new unit DataAsset is needed; omit `template_unit` only when the project default template is configured.
3. Use editor VAT tools only for the mesh/material/renderer authoring steps that cannot be expressed as unit reads, creates, writes, or deletes.
4. Read back the generated unit with `MCP_UnitGet` and verify asset existence, renderer class, material slot, attack enabled state, projectile/effect arrays, range, damage, and subtype.

## Defaults

Use style defaults for boilerplate: package roots, family naming, renderer/LOD defaults, and common projectile/effect/sound bindings. Keep per-unit specs small:

- `material_overrides` for skins or flag/country variants.
- `source_renderer_class` and `niagara_system` for renderer binding.
- `unit_patch` for gameplay changes such as `SubType`, `Trace`, `Attack`, `Damage`, and `Visualize`.

## Balance Baseline (Confirmed)

Project scale definition for this game:

- `1 格 = 16 uu = 1 km`
- Use old-game "格/秒" as balance baseline and convert to runtime uu/second.

### Movement formula

1. Let `H_kmh` be historical speed in km/h.
2. `v = H_kmh / 3.6` (m/s).
3. `Move格/秒 = 16 × ln(1 + v) / ln(1 + 4 / 3.6)`
4. `Move uu/秒 = 16 × Move格/秒`

This keeps:
- 步兵 4 km/h -> 16 格/秒 -> 256 uu/秒 (old 1.00)
- 坦克 40 km/h -> 53.4 格/秒 -> 854 uu/秒 (old ~3.34)

Old unit scale check:
- `Move_旧格每秒 = Move格每秒 / 16`

### Range formula

1. Let `R_m` be effective combat range in meters.
2. `Range_格 = 64 × ln(1 + R_m / 10) / ln(1 + 500 / 10)`
3. `Range uu = 16 × Range_格`

This keeps:
- 步兵 500m -> 64 格 -> 1024 uu (old 4.00)
- 5.8km炮兵有效射程 -> ~113 格 -> ~1800 uu (old ~7.1)

Old unit scale check:
- `Range_旧格 = Range_格 / 16`

### Runtime mobility-firing hard rule

To prevent indefinite tank kiting, apply movement-fire penalties when a unit is moving and firing:
- `Range_移动开火 = 原射程 × 0.6`
- `Damage_移动开火 = 原伤害 × 0.5`

This is a gameplay rule for balance, not a replacement formula.

When adjusting infantry (Japanese, China, UK, Soviet, Germany, etc.):

1. Find the unit by `unit_list` (scope your style roots).
2. Read unit in simple mode (`unit_get`) and capture required fields.
3. Build `unit_patch` with new `Move.XY.MoveSpeed` and `Attack.Range` in uu.
4. Apply via `unit_write`.
5. Verify with `unit_get`.

Example:
- 步兵（H=4, R=500）=> `MoveSpeed=256`，`Attack.Range=1024`

Known validated examples in Winyunq:

- City flag: `/Game/Unit/Actor/Building/City/MCPGenerated/Gen_MCP_CityFlag_CN/AgentConfig_MCP_CityFlag_CN.AgentConfig_MCP_CityFlag_CN`
- China infantry: `/Game/Unit/Actor/Army/Soldier/China/MCPGenerated/Gen_MCP_ChinaInfantry_A/AgentConfig_MCP_ChinaInfantry_A.AgentConfig_MCP_ChinaInfantry_A`
- China officer: `/Game/Unit/Actor/Army/Officer/China/MCPGenerated/Gen_MCP_ChinaOfficer_A/AgentConfig_MCP_ChinaOfficer_A.AgentConfig_MCP_ChinaOfficer_A`

## Verification

Run the UE editor commandlet for deterministic validation when possible. A successful create pass should show `plan_success`, `validate_valid`, `apply_success`, `read_after_success`, and asset existence as true. Infantry/officer attack validation should confirm at least one projectile and one attack FX entry.
