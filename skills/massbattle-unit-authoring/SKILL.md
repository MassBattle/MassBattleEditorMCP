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

## Organize Or Clean Up Assets

Treat MCP as the asset-registry hand, not as judgement. Build the relationship graph before deleting or moving anything.

1. Start from the unit with `MCP_UnitGet`, then inspect its linked renderer and generated assets with `MCP_EffectAssetReadSummary(include_dependencies=true, include_referencers=true)`.
2. Treat `missing_hard_dependencies` and `missing_soft_dependencies` as blockers. Repair or replace references before cleanup.
3. Use `MCP_EditorPlanOrganizeUnitAssets` for linked generated/source assets because it follows the Asset Registry from the unit. Review `moves`, `blocked_count`, and referencers before any apply.
4. `MCP_EffectAssetSoftDelete` is a review tool by default. It blocks live asset moves unless `allow_unsafe_asset_move=true` is explicitly supplied after referencers and editor state are reviewed.
5. Keep one canonical generated asset set per playable unit. Old generated folders, tests, and marketplace source folders are duplicates until their referencers prove otherwise.

## Create Unit

Mirror the official `/MassBattle/Core/MassBattleTools` editor widget. Normal AI authoring should use one DoAll-equivalent apply call, not manually sequence internal stages.

1. If the user selected assets in the editor, or asks for "current/selected -> generate", call `MCP_EditorApplyCreateVatUnitFromSelection` directly. It infers `skeletal_mesh`, target/template unit, selected animations, materials, VAT data asset, Niagara, and renderer template from current selection or explicit `selected_assets`.
2. Otherwise call `MCP_EditorApplyCreateVatUnit` directly with the smallest useful spec. It resolves style defaults for package layout, template unit, VAT parent material, renderer template, Niagara system, 24 Hz VAT sampling, and animation search roots.
3. Use `MCP_EditorPlanCreateVatUnit`, `MCP_EditorValidateCreateVatUnit`, or `MCP_EditorPlanCreateVatUnitFromSelection` only as diagnostics after an apply warning/failure, or when the user explicitly asks for a dry-run plan.
4. Inspect `warnings` from the apply result. Defaulted fields are allowed, but warnings are work items for exact art or behavior.
5. When animation names are nonstandard, pass an explicit `animations` category map. The editor MCP can fall back to same-skeleton AnimSequences, but it will warn because the state mapping is guessed.
6. Read back the generated unit with `MCP_UnitGet` and verify asset existence, renderer class, material slot, attack enabled state, projectile/effect arrays, range, damage, and subtype.
7. For VAT materials, expect filename discovery first and source-material texture inheritance as a fallback. Treat `defaulted_original_textures_from_source_material` warnings as review items: verify the resulting material depends on the intended BaseColor/Normal/ARM textures with `MCP_EffectAssetReadSummary`.
8. VAT create specs use `unit_name`, `target_package_path`, and `target_unit_package_path` as the canonical output naming fields. Do not use older field names for these concepts.

Official DoAll correspondence:

- `FillTex` -> `FindAndFillOriginalTextures`, with source-material inheritance when filename discovery is incomplete.
- `FillLod` -> `FindAndFillLODSettings`.
- `FillAnim` -> `FindAndFillAnimSequences`, explicit `animations`, or compatible-animation fallback with warnings.
- `CreateStaticMeshAndMat` -> `ConvertSkeletalMeshToStaticMeshWithLODs` then `CreateMaterialInstanceForStaticMeshWithLODs`.
- `CreateVATTextures` -> create/update `UAnimToTextureDataAsset`, create VAT textures, run `AnimationToTexture`, update material instances.
- `CreateDataAsset` -> merge `Visualize`, `LODShared`, `AnimShared`, and runtime animation sample-rate defaults into the target unit.
- `CreateRenderer` -> duplicate/update the renderer Blueprint defaults for mesh, Niagara system, and subtype.

## Default-Tolerant Create

The editor MCP should still create a runnable unit when an AI command is incomplete, as long as a skeletal mesh is provided and referenced assets can load.

- Missing `template_unit`: use the style family default from `authoring_defaults.default_unit_templates`; otherwise use `authoring_defaults.default_unit_template`.
- Missing `parent_material`: use `/MassBattle/Core/AgentRenderer/VAT/M_VATMaster.M_VATMaster`.
- Missing `source_renderer_class`: use `/MassBattle/Core/AgentRenderer/BP_AgentRenderer_Template.BP_AgentRenderer_Template_C`.
- Missing `niagara_system`: use `/MassBattle/Core/AgentRenderer/NS_AgentRenderer_Template.NS_AgentRenderer_Template`.
- Missing `animation_name_filter`: use an empty filter and scan same-skeleton AnimSequences under `animation_search_path`.
- Missing or incomplete original texture discovery: inherit common texture parameters and used textures from the source skeletal material, return `defaulted_original_textures_from_source_material`, and let the AI verify material dependencies.
- If automatic animation categorization is empty, accept compatible AnimSequences as a fallback and return a warning asking the AI to provide explicit `animations`.
- If no animation can be found, default apply may continue as a static fallback and return `warning_static_fallback`; set `allow_static_fallback=false` when an animated VAT bake is mandatory.
- Selection-based tools accept `selected_assets` to simulate current editor selection in scripted runs. Select a unit plus a mesh to patch that unit; pass `selected_unit_role="template"` when the selected unit should be cloned instead.

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

- City flag: `/Game/Unit/Actor/Building/City/AgentConfigCity.AgentConfigCity`
- China infantry: `/Game/Unit/Actor/Army/Soldier/China/MCPGenerated/Gen_MCP_ChinaInfantry_A/AgentConfig_MCP_ChinaInfantry_A.AgentConfig_MCP_ChinaInfantry_A`
- China officer: `/Game/Unit/Actor/Army/Officer/China/MCPGenerated/Gen_MCP_ChinaOfficer_A/AgentConfig_MCP_ChinaOfficer_A.AgentConfig_MCP_ChinaOfficer_A`

## Verification

Run the UE editor commandlet for deterministic validation when possible. A successful create pass should show `plan_success`, `validate_valid`, `apply_success`, `read_after_success`, and asset existence as true. Also inspect `warnings` and `execution_steps`; do not claim exact animation/state correctness if the result used fallback animation mapping. Infantry/officer attack validation should confirm at least one projectile and one attack FX entry.
