---
name: massbattle-unit-authoring
description: Create, inspect, edit, delete, and validate MassBattle unit assets through the MassBattleEditorMCP/Unit MCP workflow in UE5. Use when Codex needs to manage MassBattle AgentConfig units, generate VAT/static-mesh unit assets from existing skeletal meshes, apply style defaults, bind renderer/Niagara/projectile/sound attack data, or batch-edit unit JSON without direct .uasset mutation.
---

# MassBattle Unit Authoring

Use UE editor APIs, commandlets, or the MassBattleEditorMCP tools. Do not edit `.uasset` files directly. Keep names and paths aligned with existing MassBattle source conventions; prefer cloning a nearby official/demo unit template and applying a small union patch.

## Inspect Or Edit

1. List or locate units with the Unit MCP discovery tools, scoped to style roots such as `/Game/Unit`, `/Game/Toon_Soldiers_WW2`, `/Game/StylizedArmyPackA`, and `/Game/World_Flags`.
2. Read normal unit data with simple JSON first. Request detailed mode or a specific object only for complex fields such as `Attack`, `Visualize`, `LODShared`, or `AnimShared`.
3. Apply edits through `MCP_UnitPlanMergeUpdate` then `MCP_UnitApplyPlan`; omitted fields must remain unchanged. Use `MCP_UnitDelete` for planned deletes.
4. For array fields such as `Attack.SpawnProjectile`, `Attack.SpawnFx`, and `Attack.PlaySound`, union merge can append missing elements by default. Pass `append_arrays=false` only when append should be rejected.

## Create Unit

1. Discover source assets first: source skeletal mesh, compatible renderer Blueprint class, Niagara system, material/skin, animation search roots, and a template unit.
2. Use `MCP_EditorPlanCreateVatUnit` to build the plan, inspect missing fields and warnings, then run `MCP_EditorValidateCreateVatUnit`.
3. Apply with `MCP_EditorApplyCreateVatUnit` only after validation is valid. Use `overwrite_existing=true` for repeatable editor runs; existing planned units are patched, missing units are cloned from the template.
4. Read back the generated unit with `MCP_UnitGet` and verify asset existence, renderer class, material slot, attack enabled state, projectile/effect arrays, range, damage, and subtype.

## Defaults

Use style defaults for boilerplate: package roots, family naming, renderer/LOD defaults, and common projectile/effect/sound bindings. Keep per-unit specs small:

- `material_overrides` for skins or flag/country variants.
- `source_renderer_class` and `niagara_system` for renderer binding.
- `unit_patch` for gameplay changes such as `SubType`, `Trace`, `Attack`, `Damage`, and `Visualize`.

Known validated examples in Winyunq:

- City flag: `/Game/Unit/Actor/Building/City/MCPGenerated/Gen_MCP_CityFlag_CN/AgentConfig_MCP_CityFlag_CN.AgentConfig_MCP_CityFlag_CN`
- China infantry: `/Game/Unit/Actor/Army/Soldier/China/MCPGenerated/Gen_MCP_ChinaInfantry_A/AgentConfig_MCP_ChinaInfantry_A.AgentConfig_MCP_ChinaInfantry_A`
- China officer: `/Game/Unit/Actor/Army/Officer/China/MCPGenerated/Gen_MCP_ChinaOfficer_A/AgentConfig_MCP_ChinaOfficer_A.AgentConfig_MCP_ChinaOfficer_A`

## Verification

Run the UE editor commandlet for deterministic validation when possible. A successful create pass should show `plan_success`, `validate_valid`, `apply_success`, `read_after_success`, and asset existence as true. Infantry/officer attack validation should confirm at least one projectile and one attack FX entry.
