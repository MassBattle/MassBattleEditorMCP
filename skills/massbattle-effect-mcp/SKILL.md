---
name: massbattle-effect-mcp
description: Use when Codex needs to inspect, text-export, compare, graph-edit, or manifest-translate Niagara assets and related Mass Battle batch effects; especially for source-faithful VFX-to-batched-Niagara mapping, preserving original visual graphs while adding Mass Battle inputs, validating translations, or coordinating effect work with Unit MCP.
---

# MassBattle Effect MCP

## Boundary

Treat this skill as usage guidance only. The MCP provides callable Unreal tools; this skill only decides how to combine them.

Do not describe this skill as a runtime feature, Unreal plugin, MCP server, EffectSpec, or asset. Do not place skill files in the UE project. Do not implement visual behavior inside the skill.

Treat MCP tools like compiler primitives: query, read source IR, compare, duplicate, apply an explicit edit plan, validate, and save. A manifest tool may compose those primitives for many assets, but it must not guess visual edits or hide per-item results.

Do not use Effect MCP as a level-layout tool. It may generate and configure a reusable `AMassBattleFxRenderer` Blueprint asset. The user owns whether and where that actor is placed in a test map; newly placed actors should inherit the Blueprint defaults as long as the level instance is not manually overridden.

## Safety

Prefer read-only tools first. `MCP_NiagaraMergeWrite(..., bSaveAssets=false)` can still mutate loaded editor assets in memory, so use it only when the user wants an edit attempt.

`MCP_NiagaraApplyGraphEdit(..., bSaveAssets=false)` also mutates the loaded asset in memory. A failed operation reports `partial_mutation=true` when earlier operations already ran; it does not roll back the whole batch. Duplicate the source Niagara first and edit the duplicate. Saving is gated by compilation and preservation validation.

Avoid editing `MassBattleFrame` runtime code, existing Unit MCP code, or `.uasset` unit assets unless the user explicitly asks for that mutation. If another agent is editing Mass Battle or unit management, keep this workflow scoped to Niagara/effect MCP calls and report any required handoff through MCP results.

Use merge-write only for union updates. Never use merge-write to remove emitters, renderers, user parameters, modules, or array entries. Use a delete MCP for deletion.

Never translate in place. Preserve the source asset, duplicate that exact Niagara system, and edit only the duplicate. Do not substitute a visually similar template, collapse delays, replace curves with constants, remove event handlers, change CPU/GPU simulation targets, disable source modules, or shorten lifetimes unless the user explicitly approves that exact semantic difference.

Treat an optimized recreation as a separate optional artifact. Put it in a clearly named `Optimized` path and never count it as the source-faithful translation.

## Tool Map

Use these Niagara MCP tools when available:

- `MCP_NiagaraGetApiStatus()`: list Niagara MCP capabilities.
- `MCP_NiagaraQuery(QueryJson)`: find Niagara systems by path/name text.
- `MCP_NiagaraReadSummary(SystemPath, OptionsJson)`: read system, emitter, renderer, user parameter, and module summaries.
- `MCP_NiagaraReadModule(SystemPath, SelectorJson)`: read one FunctionCall module node and pins.
- `MCP_NiagaraReadGraph(SystemPath, SelectorJson)`: read system/emitter script traversals with stable node GUIDs, contextual node `reference` objects, pin IDs, directions, defaults, explicit links, and exact `stack_inputs`. Select with `scope`, `emitter`, `script_usage`, `usage_id`, or `output_node_guid`.
- `MCP_NiagaraCompareSystems(SourceSystemPath, TargetSystemPath, OptionsJson)`: compare source-neutral semantic fingerprints. Use `mode=exact` immediately after duplication and `mode=translation` after graph edits. Removed source edges are rejected unless listed in `allowed_removed_edges`.
- `MCP_NiagaraReadAll(SystemPath, OptionsJson)`: read reflected properties and all module nodes.
- `MCP_NiagaraExportText(SystemPath, OptionsJson)`: create a deterministic text dump for close reading.
- `MCP_NiagaraMergeWrite(SystemPath, PatchJson, bSaveAssets)`: union-merge property writes on `system`, `emitter_data`, or `renderer` targets.
- `MCP_NiagaraSetModulePin(SystemPath, SelectorJson, PinName, ValueText, bSaveAssets)`: set one FunctionCall module input pin default value. By default, linked pins should be treated as unsafe to overwrite unless `allow_linked=true` is explicitly passed in the selector.
- `MCP_NiagaraApplyGraphEdit(SystemPath, EditJson, bSaveAssets)`: apply an ordered graph-edit batch. Operations include `add_user_parameter`, `add_user_data_interface`, `insert_module`, `set_module_enabled`, `set_stack_input`, bulk `set_stack_inputs`, `connect_pins`, `disconnect_pins`, `disconnect_pin`, and `rewire_pin`. It compiles once, waits for CPU/GPU completion, validates preservation, and saves only on success.
- `MCP_NiagaraSetEmitterEnabled(SystemPath, EmitterName, bEnabled, bSaveAssets)`: explicitly enable or disable one emitter handle.
- `MCP_NiagaraDelete(SystemPath, DeleteJson, bSaveAssets)`: explicit deletion actions such as renderer removal, user parameter removal, or destructive emitter disabling.
- `niagara_batch_translate(manifest, apply=false, save_assets=true)`: preflight or execute an explicit `massbattle.niagara.translation_manifest.v1`. It performs source read, duplicate, exact-clone comparison, graph edit, translation comparison, and gated save per item. `apply=false` is read-only and is the default.

Use these generic effect-asset and MassBattle batch-FX MCP tools when available:

- `MCP_EffectAssetQuery(QueryJson)`: find unknown Marketplace visual assets by path/name/class. Use this before assuming Niagara.
- `MCP_EffectAssetReadSummary(AssetPath, OptionsJson)`: read typed summaries for Niagara, Cascade `UParticleSystem`, material, Blueprint, or generic assets. Summaries include hard/soft dependencies, referencers when requested, and missing project dependency warnings.
- `MCP_EffectAssetExportText(AssetPath, OptionsJson)`: export a deterministic text dump for close reading.
- `MCP_EffectAssetSoftDelete(AssetPath, OptionsJson)`: plan moving unreferenced assets to trash. Default to dry-run; live asset moves are blocked unless `allow_unsafe_asset_move=true` is explicitly supplied after referencers and editor state are reviewed.
- `MCP_EffectDuplicateAsset(SourceAssetPath, NewAssetName, PackagePath, bSaveAssets)`: duplicate reference/template assets. This is additive and does not delete or rewrite the source.
- `MCP_EffectDiscardUnsavedDuplicate(AssetPath)`: rollback only a duplicate created unsaved by this MCP session. It rejects any asset package that exists on disk and is intended for failed manifest items, not general deletion.
- `MCP_BatchFxReadRendererDefaults(TargetClassPath)`: read `AMassBattleFxRenderer` Blueprint CDO defaults inherited by newly placed actors.
- `MCP_BatchFxSetRendererDefaults(TargetClassPath, NiagaraSystemPath, NdcBurstFxPath, SubType, RenderBatchSize, PoolingCooldown, bSaveAssets)`: configure an `AMassBattleFxRenderer` Blueprint CDO for a batched FX subtype. This does not place the actor in a level.

Large Niagara graph edits can legitimately take minutes. The bundled Python transport uses a 5-second connection timeout and a separate 600-second command-response timeout; the Unreal bridge also waits 600 seconds by default and accepts `GameThreadTimeoutSeconds` (30–3600) per command. If a command reaches that limit, its queued game-thread task may still finish. Read the target back before retrying so the same graph edit is not applied twice.

Use Unit MCP only to apply an already-designed effect to a unit config. Use Niagara MCP to understand or author Niagara reference assets.

## Workflow

1. Inventory the source path with `MCP_NiagaraQuery`; record the exact source count and exclude previously generated targets.
2. Read every source with `MCP_NiagaraReadSummary`. Export text and read graphs for systems with SpawnRate curves, delayed bursts, SpawnPerUnit, ribbons, events, simulation stages, or mixed CPU/GPU emitters.
3. Treat the readback as source IR. Record lifecycle, emitter order, module order and enabled state, stack inputs, Rapid Iteration values, renderer/material bindings, event handlers, user defaults, graph edges, and dependencies.
4. Choose only the runtime protocol mapping: `Burst`, `Attached`, or `unsupported_pending_adapter`. Do not redesign the visual effect during this decision.
5. Duplicate the exact source with `MCP_EffectDuplicateAsset`. Immediately run `MCP_NiagaraCompareSystems(..., {"mode":"exact"})`; stop if it is not an exact source-neutral clone. Exact mode defaults to a structural/compile-error gate without `ready_to_run`, because a new unsaved duplicate may not have entered Niagara's compile queue yet.
6. Use `MCP_NiagaraReadGraph` before inserting or reconnecting nodes. Copy the full contextual node `reference`, stable pin identifier, and exact `stack_inputs[].name`; never infer GUIDs or pin names.
7. Apply the smallest explicit adapter edit with `MCP_NiagaraApplyGraphEdit`. Add Mass Battle inputs and adapter modules while leaving the visual subgraph unchanged. List every intentional source-edge removal in `validation.allowed_removed_edges`.
8. Run `MCP_NiagaraCompareSystems(..., {"mode":"translation"})`. Reject undeclared edge removal, source module disablement, changed source pin defaults, timing/curve changes, renderer/material changes, lost events, or simulation-target changes.
9. Save only after the graph-edit compile barrier, readiness, preservation, and translation comparison pass. Translation mode requires `ready_to_run` by default. Reload and compare again.
10. Validate behavior in a paired test scene: original and translation receive the same transforms, parameters, seed policy, and trigger times. Capture synchronized frames or SimCache evidence. Report any difference; never label an untested result “perfect”.

For many systems, build one explicit `massbattle.niagara.translation_manifest.v1`. Run `niagara_batch_translate(..., apply=false)` first, review every source/target/edit/comparison entry, then run with `apply=true`. A failed item is not counted as converted; the batch tool attempts to discard its MCP-created unsaved duplicate and reports the cleanup result.

## Source-Faithful Translation Contract

A faithful target may add adapter parameters, adapter modules, and declared adapter edges. It must preserve all source visual behavior outside that allowlist. This is analogous to compiler lowering: change the execution ABI, not the program's intended output.

Classify unsupported semantics explicitly. Delayed SpawnRate curves, per-unit motion spawning, ribbon continuity, cross-emitter events, and attached ownership can require dedicated adapter state. Do not compress them into an immediate burst. Add the missing adapter/MCP primitive or mark the item blocked.

Keep three artifacts when useful:

- `Source`: untouched Marketplace asset.
- `Faithful`: source duplicate plus minimal Mass Battle protocol adapter.
- `Optimized`: optional hand-authored recreation, stored separately and labeled non-equivalent until visual regression tests pass.

## Optional Template Workflow

Use templates only for a new effect or an explicitly requested optimized recreation. Do not use this section for a source-faithful translation.

1. Read the default style profile with Unit Editor MCP: `MCP_EditorGetProfile("style", "default")`.
2. Inspect `batch_fx_templates` and choose the closest reference:
   - `mesh_burst_hit`: visible MeshRenderer hit/death burst for proving the batch path.
   - `sprite_explosion_burst`: existing batched explosion/fire reference.
   - `projectile_muzzle_burst`: attack/muzzle reference.
3. Duplicate the template Niagara and renderer Blueprint with `MCP_EffectDuplicateAsset`.
4. Keep or disable emitters according to the template's `keep_emitters` / `disable_emitters`.
5. Use `MCP_NiagaraMergeWrite` for bounds, renderer properties, sim target, and other UObject properties.
6. Use `MCP_NiagaraSetModulePin` for module-level visual parameters such as lifetime, scale, spawn count, random ranges, or color constants.
7. Configure the renderer Blueprint CDO with `MCP_BatchFxSetRendererDefaults`; make sure its NDC asset matches the Niagara graph's NDC reader modules.

## Marketplace FX Source Mapping

When the source effect type is unknown:

1. Use `MCP_EffectAssetQuery` over the purchased pack path with broad classes first, e.g. Niagara, Cascade, Blueprint, material, static mesh, and texture.
2. Use `MCP_EffectAssetReadSummary` or `MCP_EffectAssetExportText` on likely entry assets. For Cascade fire/explosion effects, read emitter/module structure and material dependencies.
3. Decide whether the protocol mapping is `Burst`, `Attached`, or blocked pending a dedicated adapter.
4. Explosion, hit sparks, muzzle flash, and death fire burst should usually be `Burst` through `NDC_BurstFx`.
5. Aura, burning status loop, weapon trail, and selection aura should usually be `Attached` through `LocationArray_Attached` and persistent ID arrays.
6. Duplicate the exact Marketplace source with `MCP_EffectDuplicateAsset`; do not start from a Mass Battle visual template. Verify the duplicate with `MCP_NiagaraCompareSystems(..., {"mode":"exact"})`.
7. Read the duplicate with `MCP_NiagaraReadGraph`, then insert/link only the Mass Battle protocol adapter with `MCP_NiagaraApplyGraphEdit`. Preserve emitter/renderer/material/curve/timing/sim-target/event behavior and original module enabled states. It must consume MassBattle batch inputs:
   - Burst: `NDC_BurstFx` fields `BurstLocation`, `BurstOrientation`, `BurstScale`, `SubType`, `Style`.
   - Attached: `LocationArray_Attached`, `OrientationArray_Attached`, `ScaleArray_Attached`, `IsHiddenArray_Attached`, `NiagaraIDIndex_Attached`, `NiagaraIDAcquireTag_Attached`, optional `StyleArray_Attached`.
8. Duplicate `BP_FxRendererTemplate` with `MCP_DuplicateClassAsset` or a generic asset duplicate, then call `MCP_BatchFxSetRendererDefaults`.
9. Tell the user to place one instance of the generated FX renderer Blueprint in the test level manually. The actor must exist at BeginPlay because `AMassBattleFxRenderer::BeginPlay` registers the subtype with `MassBattleSubsystem->FxRenderers`.
10. Before placement, use `MCP_BatchFxReadRendererDefaults` to verify the Blueprint asset defaults. The expected batch path has a non-null Niagara system, a non-null `NDC_BurstFx`, and the same `SubType` that unit `FFxConfig` uses.
11. Use Unit MCP to merge a `FFxConfig` into `Hit.SpawnFx`, `Death.SpawnFx`, `Appear.SpawnFx`, `Attack.SpawnFx`, or `Select.SpawnOnSelected.SpawnFx`.
12. For `FFxConfig`, leave unbatched assets empty and set `SubType`, `StyleType`, `bAttached`, `Quantity`, `Delay`, `LifeSpan`, and `Transform`. In the current project JSON merge path, `SubType` and `StyleType` should be strings such as `SubType35` and `Style0`.
13. Run translation comparison and paired visual validation before counting the item as converted. If the existing Mass Battle payload cannot express source timing or ownership semantics, report the missing field/adapter instead of changing the effect.

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

## Graph Edit Shape

Use one ordered batch so newly inserted nodes can be referenced by operation ID:

```json
{
  "operations": [
    {
      "id": "batch_reader",
      "op": "insert_module",
      "graph": {
        "scope": "emitter",
        "emitter": "Explosion",
        "script_usage": "ParticleSpawnScript"
      },
      "module_asset": "/Game/MassBattle/FX/Modules/NM_ReadBurstFx.NM_ReadBurstFx",
      "stack_index": 0
    },
    {
      "op": "set_stack_input",
      "node": { "operation": "batch_reader" },
      "input": "Data Channel",
      "value": { "mode": "linked_parameter", "parameter": "User.NDC_BurstFx" }
    },
    {
      "op": "connect_pins",
      "from": { "operation": "batch_reader", "pin": "Batch Position", "direction": "output" },
      "to": {
        "node_guid": "GUID-FROM-READ-GRAPH",
        "graph": {
          "scope": "emitter",
          "emitter": "Explosion",
          "script_usage": "ParticleSpawnScript",
          "usage_id": "USAGE-ID-FROM-READ-GRAPH"
        },
        "pin_id": "PIN-ID-FROM-READ-GRAPH"
      }
    }
  ],
  "validation": {
    "require_ready_to_run": true,
    "require_no_warnings": false
  }
}
```

Add a user data interface with `add_user_data_interface` and `name`, `data_interface_class`, and optional `properties`. `set_stack_input` supports `local`, `linked_parameter`, `hlsl`, `dynamic_input`, and `data_interface`; address inputs with names returned in `stack_inputs`. For `local`, prefer round-tripping the exact `value_text` from `MCP_NiagaraReadGraph`; `enum_name` and scalar `literal`/`value` are also accepted. Use `rewire_pin` with `target`, `through_input`, and `through_output` when an existing linked visual input must flow through a new per-event transform; it requires exactly one original source and restores the original link if the replacement fails. `connect_pins` requires `replace=true` before replacing an occupied input. Always include the returned `usage_id` when selecting `ParticleEventScript` or `ParticleSimulationStageScript`; those stacks can have several instances of the same script usage.

The preservation gate compares system/emitter timing and determinism settings, emitters, renderer/material properties, CPU/GPU sim targets, event handlers, existing user-parameter defaults, Rapid Iteration values, pre-existing module identity/enabled state, graph-node identity, and original graph-pin defaults. It reports edge diffs and rejects removed source edges unless they exactly match `validation.allowed_removed_edges` (or the caller explicitly uses the unsafe `allow_removed_edges=true`). Saving is blocked on compile errors, failed preservation, undeclared edge removal, or a not-ready system.

Translation comparison canonicalizes the source and target root object/package paths before fingerprinting, so an exact duplicate can match despite living in a new package. `mode=exact` permits no semantic additions. `mode=translation` permits additive adapter parameters/modules/edges while requiring all source semantics to remain present.

## Translation Manifest Shape

Use one reviewed edit plan per source; never apply one guessed plan to heterogeneous systems:

```json
{
  "schema": "massbattle.niagara.translation_manifest.v1",
  "stop_on_error": true,
  "items": [
    {
      "id": "tank_main_muzzle",
      "source_system_path": "/Game/ArmyVFX/Niagara/MuzzleFlash/NS_MuzzleFlash_Tank_Maingun_1.NS_MuzzleFlash_Tank_Maingun_1",
      "new_asset_name": "NS_MB_MuzzleFlash_Tank_Maingun_1_Faithful",
      "package_path": "/Game/ArmyVFX/MassBattleMapped/Muzzle",
      "edit": {
        "operations": [],
        "validation": {
          "require_ready_to_run": true,
          "allowed_removed_edges": []
        }
      },
      "comparison": {
        "mode": "translation",
        "allowed_removed_edges": []
      }
    }
  ]
}
```

An empty edit is useful for validating the exact-duplicate pipeline but is not a completed batch conversion. Count an item only after its adapter operations, renderer configuration, runtime trigger test, and paired visual test pass.

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
