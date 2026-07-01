---
name: massbattle-effect-mcp
description: Use when Codex needs to inspect, text-export, edit, or delete Niagara assets and related Mass Battle batch-effect assets through low-level MCP tools; especially when using Niagara systems as references, reading Niagara modules, converting Niagara assets to text, doing union-merge writes, or coordinating effect work with Unit MCP without treating a skill as an MCP.
---

# MassBattle Effect MCP

## Boundary

Treat this skill as usage guidance only. The MCP provides callable Unreal tools; this skill only decides how to combine them.

Do not describe this skill as a runtime feature, Unreal plugin, MCP server, EffectSpec, or asset. Do not place skill files in the UE project. Do not implement visual behavior inside the skill.

Treat MCP tools like command-line primitives: query, read, export text, merge-write, and delete. Do not ask for a new high-level MCP button until primitive tools cannot reach the required UE data or mutation.

Do not use Effect MCP as a level-layout tool. It may generate and configure a reusable `AMassBattleFxRenderer` Blueprint, but the user owns whether and where that actor is placed in a test map.

## Safety

Prefer read-only tools first. `MCP_NiagaraMergeWrite(..., bSaveAssets=false)` can still mutate loaded editor assets in memory, so use it only when the user wants an edit attempt.

Avoid editing `MassBattleFrame` runtime code, existing Unit MCP code, or `.uasset` unit assets unless the user explicitly asks for that mutation. If another agent is editing Mass Battle or unit management, keep this workflow scoped to Niagara/effect MCP calls and report any required handoff through MCP results.

Use merge-write only for union updates. Never use merge-write to remove emitters, renderers, user parameters, modules, or array entries. Use a delete MCP for deletion.

## Tool Map

Use these Niagara MCP tools when available:

- `MCP_NiagaraGetApiStatus()`: list Niagara MCP capabilities.
- `MCP_NiagaraQuery(QueryJson)`: find Niagara systems by path/name text.
- `MCP_NiagaraReadSummary(SystemPath, OptionsJson)`: read system, emitter, renderer, user parameter, and module summaries.
- `MCP_NiagaraReadModule(SystemPath, SelectorJson)`: read one FunctionCall module node and pins.
- `MCP_NiagaraReadAll(SystemPath, OptionsJson)`: read reflected properties and all module nodes.
- `MCP_NiagaraExportText(SystemPath, OptionsJson)`: create a deterministic text dump for close reading.
- `MCP_NiagaraMergeWrite(SystemPath, PatchJson, bSaveAssets)`: union-merge property writes on `system`, `emitter_data`, or `renderer` targets.
- `MCP_NiagaraDelete(SystemPath, DeleteJson, bSaveAssets)`: explicit deletion actions such as renderer removal, user parameter removal, or emitter disabling.

Use these generic effect-asset and MassBattle batch-FX MCP tools when available:

- `MCP_EffectAssetQuery(QueryJson)`: find unknown Marketplace visual assets by path/name/class. Use this before assuming Niagara.
- `MCP_EffectAssetReadSummary(AssetPath, OptionsJson)`: read typed summaries for Niagara, Cascade `UParticleSystem`, material, Blueprint, or generic assets. Cascade summaries expose emitters, LODs, and modules.
- `MCP_EffectAssetExportText(AssetPath, OptionsJson)`: export a deterministic text dump for close reading.
- `MCP_EffectDuplicateAsset(SourceAssetPath, NewAssetName, PackagePath, bSaveAssets)`: duplicate reference/template assets. This is additive and does not delete or rewrite the source.
- `MCP_BatchFxSetRendererDefaults(TargetClassPath, NiagaraSystemPath, NdcBurstFxPath, SubType, RenderBatchSize, PoolingCooldown, bSaveAssets)`: configure an `AMassBattleFxRenderer` Blueprint CDO for a batched FX subtype. This does not place the actor in a level.

Use Unit MCP only to apply an already-designed effect to a unit config. Use Niagara MCP to understand or author Niagara reference assets.

## Workflow

1. Query references with `MCP_NiagaraQuery`, using path/name hints from the user.
2. Read broad structure with `MCP_NiagaraReadSummary`.
3. Convert promising references to text with `MCP_NiagaraExportText` when detailed reasoning is needed.
4. Read a specific module with `MCP_NiagaraReadModule` when the summary reveals the relevant FunctionCall node.
5. Use `MCP_NiagaraReadAll` only when summary/module reads are insufficient or when preparing a write.
6. Write with `MCP_NiagaraMergeWrite` only for additive/overwriting property changes. Use `value_text` for complex UE import syntax.
7. Delete with `MCP_NiagaraDelete` only after the target is explicit.
8. If the task requires graph surgery that MCP cannot express, request or add a narrower auxiliary MCP rather than folding the workflow into one high-level button.

## Marketplace FX To Batch FX

When the source effect type is unknown:

1. Use `MCP_EffectAssetQuery` over the purchased pack path with broad classes first, e.g. Niagara, Cascade, Blueprint, material, static mesh, and texture.
2. Use `MCP_EffectAssetReadSummary` or `MCP_EffectAssetExportText` on likely entry assets. For Cascade fire/explosion effects, read emitter/module structure and material dependencies.
3. Decide whether the MassBattle remake should be `Burst` or `Attached`.
4. Explosion, hit sparks, muzzle flash, and death fire burst should usually be `Burst` through `NDC_BurstFx`.
5. Aura, burning status loop, weapon trail, and selection aura should usually be `Attached` through `LocationArray_Attached` and persistent ID arrays.
6. Duplicate `NS_FxRendererTemplate` or an existing batched FX renderer Niagara with `MCP_EffectDuplicateAsset`.
7. Author or edit the duplicated Niagara with low-level Niagara tools. It must consume MassBattle batch inputs:
   - Burst: `NDC_BurstFx` fields `BurstLocation`, `BurstOrientation`, `BurstScale`, `SubType`, `Style`.
   - Attached: `LocationArray_Attached`, `OrientationArray_Attached`, `ScaleArray_Attached`, `IsHiddenArray_Attached`, `NiagaraIDIndex_Attached`, `NiagaraIDAcquireTag_Attached`, optional `StyleArray_Attached`.
8. Duplicate `BP_FxRendererTemplate` with `MCP_DuplicateClassAsset` or a generic asset duplicate, then call `MCP_BatchFxSetRendererDefaults`.
9. Tell the user to place one instance of the generated FX renderer Blueprint in the test level manually. The actor must exist at BeginPlay because `AMassBattleFxRenderer::BeginPlay` registers the subtype with `MassBattleSubsystem->FxRenderers`.
10. Use Unit MCP to merge a `FFxConfig` into `Hit.SpawnFx`, `Death.SpawnFx`, `Appear.SpawnFx`, `Attack.SpawnFx`, or `Select.SpawnOnSelected.SpawnFx`.
11. For `FFxConfig`, leave unbatched assets empty and set `SubType_Batched`, `StyleType_Batched`, `bAttached`, `Quantity`, `Delay`, `LifeSpan`, and `Transform`.

## Merge Write Shape

Prefer explicit patches:

```json
{
  "patches": [
    {
      "target": "system",
      "property": "WarmupTime",
      "value": 0.15
    },
    {
      "target": "emitter_data",
      "emitter": "Sparks",
      "property": "SimTarget",
      "value_text": "GPUComputeSim"
    },
    {
      "target": "renderer",
      "emitter": "Sparks",
      "renderer_index": 0,
      "property": "bIsEnabled",
      "value": true
    }
  ]
}
```

Use `value_text` for enum, struct, object, or array import text when scalar JSON is ambiguous.

## Delete Shape

Examples:

```json
{"type": "renderer", "emitter": "Sparks", "renderer_index": 0}
```

```json
{"type": "user_parameter", "name": "User.ImpactColor"}
```

```json
{"type": "disable_emitter", "emitter": "Smoke"}
```

## Mass Battle Use

For Mass Battle batch effects, first use Niagara MCP to inspect or author reference visual behavior. Then use existing Mass Battle/unit MCP only for attaching the resulting asset/config to units.

If a requested batch effect does not need Niagara, identify the missing primitive first: material parameter read/write, VAT metadata read/write, Blueprint graph read, C++ callgraph read, or unit config write. Add that MCP only when direct filesystem/source access cannot provide it.
