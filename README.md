# MassBattleEditorMCP

[中文文档](zh.md)

I think RTS games are one of the best ways to reason about an AI society. Humans should not be trapped in every low-level action. Humans should define strategy, constraints, tradeoffs, and goals; AI and tools should turn those goals into executable tactical work. In the future, the scarce people will not be the ones who merely repeat implementation details. They will be the ones who can set direction, organize systems, judge outcomes, and take responsibility.

That is also why RTS matters to me. An ambitious RTS should not start with a vision of massive war, then cut the unit count down to a handful because the technology cannot carry the design. Scale is not decoration. Scale changes the gameplay, the tactical space, the presentation layer, and the tools required to build the game.

[Mass Battle Frame](https://github.com/MassBattle/MassBattleFrame) fits my taste because it aims at extreme performance. Only when the foundation is fast enough can we afford the things that look wasteful but create the experience: denser units, richer visuals, larger battlefields, and more tactical feedback. If your RTS target is thousands or tens of thousands of units, [Mass Battle Frame on Fab](https://www.fab.com/listings/191850b4-44d3-4455-aa76-874bc0196a10?lang=zh-cn) is not a luxury. It is necessary infrastructure.

When a plugin is good enough, hobbyists naturally start building around it. MassBattleEditorMCP is one proof of that. It is not a runtime feature of Mass Battle Frame. It is an editor tool hand that AI can use. It exposes unit, effect, material, renderer, Niagara, and DataAsset authoring as callable, readable, batchable editor operations, so AI can handle tactical asset translation while humans keep ownership of design judgment.

If your game already has thousands of units, this ecosystem is valuable. The story above may be dry, but it is real: strong core technology attracts surrounding community tools. Alongside this MCP plugin, there are other community plugins around Mass Battle and RTS workflows:

- [fogofwar](https://github.com/winyunq/FogOfWar): fog of war plugin, including the minimap plugin and scene fog plugin.
- [RTS InputSystem](https://github.com/winyunq/RTSInputSystem): RTS control system, including an RTS camera, a minimap-visible camera view that can work with the fogofwar minimap, and a unit selection panel.
- [landmark](https://github.com/winyunq/LandmarkSystem): scene editing system, including editor text at specific coordinates, building generation in specific areas, and spawning specified counts of Mass Battle Frame units at specific locations.

## Plugin Positioning: Editor Tools For Large-Scale Mass Battle Workflows

MassBattleEditorMCP turns tactical asset and configuration translation in Mass Battle workflows into callable, readable, batchable editor operations. It supports unit creation, configuration translation, batch-effect conversion, and presentation-chain translation across animation, materials, meshes, and FX.

Developers should not spend most of their time manually adapting one unit or effect at a time. This plugin lets them verify whether rules match strategic design goals faster.

## Core Capabilities

The default rhythm is read first, create or union-write next, explicitly delete only when needed, and read back after changes. The goal is repeatability and traceability.
You can use it to differentiate unit presentation and balance data, or to adapt non-batched assets into scalable batch-processing systems.

## Entry Points

Online documentation: `https://github.com/MassBattle/MassBattleEditorMCP`
Local documentation entry: `Document/index.html` (`Document` worktree / `Document` branch)

## AI Skills

This repository includes Codex skills under `skills/`:

- `skills/massbattle-effect-mcp`: converts arbitrary source VFX into source-faithful Niagara systems that consume the MassBattleFrame Batch FX protocol.
- `skills/massbattle-instant-damage-fx`: authors direct, agent-resolved attacks and their one-shot launch/impact Burst FX.
- `skills/massbattle-projectile-authoring`: authors projectile-owned travel, collision, damage, lifecycle, Attached flight FX, and Burst lifecycle FX.
- `skills/massbattle-unit-authoring`: creates and edits MassBattle units, then links validated direct-attack or projectile configurations into unit arrays.

There is only one meaning of Niagara batch conversion in these skills:

`BatchE([C0, C1, ..., Cn]) == [E(C0), E(C1), ..., E(Cn)]`

The source effect's visual graph remains the effect. The conversion replaces per-unit spawn/transform scheduling with MassBattleFrame Burst NDC or Attached array input. GPU simulation, pooling, template duplication, renderer CDO edits, or setting `SubType` alone are not batch conversion. Instant versus projectile is a gameplay-ownership decision; both routes use this same Niagara conversion contract.

Install to local Codex:

```powershell
Copy-Item -Recurse -Force .\skills\massbattle-unit-authoring $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-effect-mcp $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-instant-damage-fx $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-projectile-authoring $env:USERPROFILE\.codex\skills\
```

If `CODEX_HOME` is set, copy them to `$env:CODEX_HOME\skills\`.
MCP is the editor tool interface. For unit authoring, the default AI-facing path mirrors the official `/MassBattle/Core/MassBattleTools` DoAll button: one apply call creates or refreshes the mesh, materials, VAT textures, renderer, and unit config while returning warnings for inferred fields. Plan/validate tools remain available as diagnostics, but they are not the normal creation workflow.

### Codex MCP Server Installation

MassBattleEditorMCP has two Codex-facing layers:

1. A local TCP bridge inside the UE editor plugin, listening on `127.0.0.1:55258` by default.
2. `Resources/Python/MassBattleMcpServer.py`, a STDIO MCP server that forwards Codex tool calls to the UE bridge.

Install to Codex:

```powershell
.\Scripts\Install-CodexMassBattleMCP.ps1
```

Quickly check the installation and UE bridge:

```powershell
.\Scripts\QuickStart-CodexMassBattleMCP.ps1
```

After installation, restart Codex or start a new session. The UE editor must also load this plugin before the bridge starts listening.
After successful installation, you should see `massbattle-editor-mcp` and be able to call primitive tools such as `unit_get`, `unit_create`, `projectile_get`, `projectile_write`, `projectile_validate`, `editor_apply_create_vat_unit_from_selection`, `effect_asset_read_summary`, `niagara_set_module_pin`, `batch_fx_read_renderer_defaults`, and `batch_fx_set_renderer_defaults`.

Note: `FFxConfig.AgentBehaviorState` uses `EAgentBehaviorState`. Writable values include `None`, `Appearing`, `Sleeping`, `Patrolling`, `Attacking`, `Hit`, and `Dying`. Hit FX should use `Hit`; do not write the runtime flag name `BeingHit` into this field.

## MCP Capability List

| Category | MCP Tool | Status | Purpose |
| --- | --- | :---: | --- |
| Connection / diagnostics | `massbattle_ping` | Available | Confirm that the Codex MCP server can connect to the UE editor bridge. |
| Connection / diagnostics | `unit_get_api_status` | Available | Read Unit MCP capabilities. |
| Connection / diagnostics | `effect_asset_get_api_status` | Available | Read Effect Asset / Batch FX MCP capabilities. |
| Connection / diagnostics | `niagara_get_api_status` | Available | Read Niagara MCP capabilities. |
| Connection / diagnostics | `projectile_get_api_status` | Available | Read Projectile DataAsset CRUD, schema, and validation capabilities. |
| Unit MCP | `unit_list` | Available | List `MassBattleAgentConfigDataAsset` unit config assets. |
| Unit MCP | `unit_get` | Available | Read one unit config with simple/full views and default filtering. |
| Unit MCP | `unit_get_schema` | Available | Read editable unit fields, types, roles, and tooltips. |
| Unit MCP | `unit_export` | Available | Export compact unit balance tables for analysis or batch review. |
| Unit MCP | `unit_find_assets` | Available | Find candidate SkeletalMesh, Renderer, Niagara, and related assets for unit authoring. |
| Unit MCP | `unit_create` | Available | Create a new unit; use the default template when no template is provided; optional initial unit data is supported. |
| Unit MCP | `unit_plan_write` | Available | Plan a union-write and return a reviewable diff without mutating the unit. |
| Unit MCP | `unit_preview_plan` | Available | Read a saved unit mutation plan and its complete diff. |
| Unit MCP | `unit_apply_plan` | Available | Apply a reviewed unit mutation plan and optionally save the unit. |
| Unit MCP | `unit_write` | Available | Union-write partial source-aligned JSON to an existing unit; omitted fields stay unchanged. |
| Unit MCP | `unit_delete` | Available | Explicitly delete or soft-delete a unit; dry-run by default. |
| Projectile MCP | `projectile_list` | Available | List `MassBattleProjectileConfigDataAsset` assets under selected roots. |
| Projectile MCP | `projectile_query` | Available | Query projectile configs by path or name. |
| Projectile MCP | `projectile_get` | Available | Read one projectile in active-only or complete source-aligned form. |
| Projectile MCP | `projectile_get_schema` | Available | Read projectile fields, enum values, edit conditions, and write rules. |
| Projectile MCP | `projectile_create` | Available | Create a projectile from class defaults or an existing template after transient validation. |
| Projectile MCP | `projectile_write` | Available | Union-write a partial projectile patch; array append/replace must be explicit and validation runs before mutation. |
| Projectile MCP | `projectile_validate` | Available | Validate movement, damage ownership, triggers, lifecycle FX, linked units, and duplicate explosions. |
| Projectile MCP | `projectile_delete` | Available | Plan or execute guarded soft/hard deletion; dry-run by default. |
| Style MCP | `style_summarize_units` | Available | Summarize unit assets by style, family, and path category. |
| Style MCP | `style_plan_organize_units` | Available | Plan style-based unit folder organization without moving assets. |
| Unit Editor MCP | `editor_get_status` | Available | Read unit editor workflow capabilities. |
| Unit Editor MCP | `editor_list_profiles` | Available | List style profiles and authoring recipes. |
| Unit Editor MCP | `editor_get_profile` | Available | Read one profile or recipe. |
| Unit Editor MCP | `editor_plan_create_vat_unit` | Diagnostic | Preview the DoAll-equivalent VAT unit plan; strict apply still requires complete canonical inputs. |
| Unit Editor MCP | `editor_apply_create_vat_unit` | Available | Primary non-selection DoAll-equivalent generation entry; validates complete canonical inputs before mesh conversion, VAT material creation, VAT bake, renderer duplication, and unit config writes. |
| Unit Editor MCP | `editor_plan_create_vat_unit_from_selection` | Diagnostic | Infer a DoAll spec from current editor selection or `selected_assets`, then return a reviewable plan. |
| Unit Editor MCP | `editor_apply_create_vat_unit_from_selection` | Available | Primary one-click "current selection -> generate" entry point for AI-driven unit creation. |
| Unit Editor MCP | `editor_inspect_actor_assembly` | Available | Inspect an Actor's modular skeletal/static components, effective materials, socket transforms, and resolved weapon bind bones without writing assets. |
| Unit Editor MCP | `editor_plan_create_vat_unit_from_actor` | Diagnostic | Validate an Actor component/weapon override recipe and preview its persistent assembled SkeletalMesh plus strict VAT unit plan. |
| Unit Editor MCP | `editor_apply_create_vat_unit_from_actor` | Available | Assemble an Actor and rigidly bound static weapon into an animation-compatible persistent SkeletalMesh, then run the strict DoAll-equivalent VAT workflow. |
| Unit Editor MCP | `editor_plan_organize_unit_assets` | Available | Plan moving one unit and linked generated assets into the style layout. |
| Unit Editor MCP | `editor_apply_organize_unit_assets` | Available | Apply a reviewed unit asset organization plan; dry-run by default. |
| Effect Asset MCP | `effect_asset_query` | Available | Query visual assets such as Niagara, Cascade, Blueprint, Material, Texture, and Sound by `query/root/classes/limit`. |
| Effect Asset MCP | `effect_asset_read_summary` | Available | Read summaries for unknown effect asset types; returns dependency, referencer, and missing project dependency details. |
| Effect Asset MCP | `effect_asset_export_text` | Available | Export deterministic text for close AI reading and review. |
| Effect Asset MCP | `effect_asset_soft_delete` | Available | Plan a move of unreferenced assets to `_Trash`; live moves are blocked unless explicitly forced. |
| Effect Asset MCP | `effect_duplicate_asset` | Available | Additively duplicate assets without deleting or overwriting the source. |
| Effect Asset MCP | `effect_discard_unsaved_duplicate` | Available | Roll back only a duplicate created unsaved by this MCP session; any package already persisted to disk is rejected. |
| Niagara MCP | `niagara_query` | Available | Query Niagara Systems by path or name. |
| Niagara MCP | `niagara_read_summary` | Available | Read Niagara system, emitter, renderer, user parameter, and module summaries. |
| Niagara MCP | `niagara_read_module` | Available | Read one Niagara module node and its pins. |
| Niagara MCP | `niagara_read_graph` | Available | Read selected system/emitter script traversals with stable node GUIDs, pin IDs, directions, defaults, and explicit links. |
| Niagara MCP | `niagara_compare_systems` | Available | Compare an exact duplicate or translated target with its source using source-neutral semantic fingerprints. |
| Niagara MCP | `niagara_read_all` | Available | Read reflected Niagara properties and every function-call module/pin. |
| Niagara MCP | `niagara_export_text` | Available | Export deterministic Niagara text. |
| Niagara MCP | `niagara_merge_write` | Available | Union-write Niagara properties; does not handle deletion. |
| Niagara MCP | `niagara_set_module_pin` | Available | Write one Niagara FunctionCall module input pin default; linked pins are rejected by default. |
| Niagara MCP | `niagara_apply_graph_edit` | Available | Apply ordered user-DI, module insertion, stack-input, connect/disconnect, and lossless rewire operations; compile once and save only after preservation validation passes. |
| Niagara MCP | `niagara_batch_translate` | Available | Preflight or execute an explicit source-first translation manifest; defaults to read-only preflight and never guesses visual edits. |
| Niagara MCP | `niagara_set_emitter_enabled` | Available | Explicitly enable or disable one Niagara emitter handle. |
| Niagara MCP | `niagara_delete` | Available | Explicitly delete renderers, user parameters, disable emitters, etc. |
| Niagara MCP | `niagara_add_sprite_renderer` | Available | Add one configured sprite renderer to an existing emitter. |
| Batch FX MCP | `batch_fx_read_renderer_defaults` | Available | Read `AMassBattleFxRenderer` Blueprint defaults inherited by newly placed actors. |
| Batch FX MCP | `batch_fx_set_renderer_defaults` | Available | Set `AMassBattleFxRenderer` Blueprint defaults, including `NiagaraSystemAsset`, `NDC_BurstFx`, `SubType`, batch size, and pooling cooldown. |

For source-faithful batch translation, duplicate the exact source Niagara, prove the duplicate with `niagara_compare_systems(mode=exact)`, add only the MassBattleFrame protocol adapter, and validate again with `mode=translation`. Exact mode defaults to structural identity plus compile-error checks because an unsaved duplicate may not have entered Niagara's compile queue yet; the graph-edit barrier and translation mode enforce runtime readiness before save. A template-based recreation is a separate optional optimized artifact and must not be reported as a faithful mapping. After translation, verify renderer Blueprint defaults, use available level-editing tools to place the renderer in a test level, and route the effect through Unit or Projectile MCP. Do not hand level setup back to the user when an editor tool can perform it.

Direct attacks and projectiles must not both own the same damage. A normal projectile-owned attack disables the agent's hit-time damage and lets the projectile DataAsset own travel, collision, damage, and lifecycle. Validation also rejects identical enabled `OnHit.SpawnFx` and `OnRemoval.SpawnFx` arrays, because both callbacks can run in one simulation tick and otherwise emit the same explosion twice.

The Python bridge uses a short 5-second connection timeout and a separate 600-second command-response timeout. The Unreal bridge also waits up to 600 seconds for game-thread work by default; launch with `-MassBattleMCPGameThreadTimeoutSeconds=N` or pass `GameThreadTimeoutSeconds` in command params to choose a value from 30 to 3600 seconds. If that limit is reached, the editor task may still finish, so read the target back before retrying.

VAT unit creation first uses discovered original textures. If filename-based discovery misses a source material texture, the editor MCP can inherit common texture parameters and used textures from the source skeletal material before creating the VAT material instance. Treat `defaulted_original_textures_from_source_material` warnings as review items and verify the generated material dependencies.

Actor-driven VAT authoring accepts `actor_class` plus per-component overrides. Modular SkeletalMesh components are merged through a persistent editor `MeshDescription`; visible StaticMesh components such as weapons are transformed into root space and rigidly weighted to the socket's resolved bone. Source material slots remain canonical, while their direct texture expressions and package dependencies populate generated VAT material inputs. The source Actor and `MassBattleFrame` assets are never modified.

Strict non-selection VAT create requires canonical complete inputs: `skeletal_mesh`, `unit_name`, `target_package_path`, `parent_material`, `source_renderer_class`, `niagara_system`, `vat_sample_rate`, and `animations`. Existing-unit refresh uses `target_unit`; new-unit creation also requires `template_unit`, `target_unit_package_path`, and `subtype`. Existing generated StaticMeshes block the run unless `overwrite_existing=true` is supplied, so refreshes cannot silently reuse stale mesh/material assets.

MassBattleTools DoAll correspondence for VAT units:

1. `FillTex`, `FillLod`, and `FillAnim` resolve textures, LOD settings, and animation arrays.
2. `CreateStaticMeshAndMat` converts the skeletal mesh and creates VAT material instances.
3. `CreateVATTextures` creates or updates the AnimToTexture DataAsset, bakes VAT textures, writes AnimDataTex, and updates material instances.
4. `CreateDataAsset` writes `Visualize`, `LODShared`, `AnimShared`, and runtime sample-rate defaults to the unit config.
5. `CreateRenderer` duplicates or updates the renderer Blueprint defaults.

The apply call runs validation before writing assets. The plan and validate calls expose these steps for review, but a normal AI command should still provide the complete spec up front.

## Lossless Niagara Graph Editing

Use `niagara_read_graph` before any graph mutation. A selector can narrow by `scope`, `emitter`, `script_usage`, `usage_id`, or `output_node_guid`; returned nodes expose stable `node_guid`, `pin_id`, `persistent_guid`, direction, type, defaults, and links. Copy the returned node `reference` object for later edits because cloned or inherited emitters can contain the same node GUID. Stack modules also return `stack_inputs` with their exact authored names, types, visibility, and editability. Event-handler and simulation-stage stacks are resolved with their non-empty `usage_id`.

`niagara_apply_graph_edit` executes an ordered `operations` array, compiles CPU and GPU scripts once, and returns per-script diagnostics plus before/after preservation fingerprints. Saving is blocked unless the system has no compile errors, is ready to run, and retains source emitters, renderers/materials, timing and determinism, CPU/GPU targets, events, user defaults, Rapid Iteration values, module identity/enabled state, graph-node identity, and original graph-pin defaults. Removed source edges are rejected unless each edge is explicitly listed in `validation.allowed_removed_edges`. A failed later operation can leave an unsaved in-memory partial mutation, reported as `partial_mutation=true`, so perform this workflow only on a duplicate.

`niagara_batch_translate` accepts `massbattle.niagara.translation_manifest.v1`. Each item declares its source, duplicate target, exact graph-edit plan, and comparison policy. The default `apply=false` performs source/target preflight only. With `apply=true`, each item runs duplicate -> exact comparison -> unsaved edit/compile -> translation comparison -> gated save. A failed item is reported, never counted as converted, and its MCP-created unsaved duplicate is discarded on a best-effort basis; a persisted package can never be removed by that rollback path.

Example shape (replace paths, graph selector, GUIDs, and pin names with values returned by the read tools):

```json
{
  "operations": [
    {
      "id": "add_batch_di",
      "op": "add_user_data_interface",
      "name": "User.NDC_BurstFx",
      "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceDataChannelRead",
      "properties": {
        "Channel": "/MassBattle/Core/FxRenderer/NS_Modules/NDC_BurstFx.NDC_BurstFx"
      }
    },
    {
      "id": "insert_batch_reader",
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
      "node": { "operation": "insert_batch_reader" },
      "input": "Data Channel",
      "value": { "mode": "linked_parameter", "parameter": "User.NDC_BurstFx" }
    },
    {
      "op": "rewire_pin",
      "target": {
        "node_guid": "TARGET-NODE-GUID",
        "graph": {
          "scope": "emitter",
          "emitter": "Explosion",
          "script_usage": "ParticleSpawnScript",
          "usage_id": "USAGE-ID-FROM-READ-GRAPH"
        },
        "pin": "Position",
        "direction": "input"
      },
      "through_input": {
        "operation": "insert_batch_reader",
        "pin": "Original Position",
        "direction": "input"
      },
      "through_output": {
        "operation": "insert_batch_reader",
        "pin": "Batch Position",
        "direction": "output"
      }
    }
  ],
  "validation": {
    "require_ready_to_run": true,
    "require_no_warnings": false,
    "allowed_removed_edges": []
  }
}
```

Pin references accept an existing contextual node `reference` or an earlier insert operation ID plus `pin`/`pin_name`, `pin_id`, or `persistent_guid`. A bare `node_guid` is accepted only when it is unique in the system. `connect_pins` refuses to replace an occupied input unless `replace=true`; `rewire_pin` requires exactly one original source and restores the original link if either replacement connection fails. `set_stack_input` supports `local`, `linked_parameter`, `hlsl`, `dynamic_input`, and `data_interface` modes; `set_stack_inputs` applies an ordered `inputs` array to one module while reusing one Niagara editor context. Use names returned in `stack_inputs` (common display-name spacing is normalized). A local value accepts the exact `value_text` returned by `MCP_NiagaraReadGraph`, an `enum_name`, or a scalar `literal`/`value`. For `ParticleEventScript` and `ParticleSimulationStageScript`, include the returned `usage_id` in every graph selector.

## Default Style And Optional Template Workflow

The default style profile is `Resources/UnitManagementStyles/default.massbattle_unit_style.json`.
It is not a runtime feature. It is authoring context for AI and MCP tools: scan roots, unit organization rules, unit authoring defaults, and batch FX templates.
When `unit_create` does not receive `template_unit`, it reads `authoring_defaults.default_unit_template` as the default unit template. Configure that path before using default-template creation.

Use this template workflow only for a new effect or an explicitly requested optimized recreation. A source-faithful Marketplace translation must duplicate the exact source instead.

Recommended template flow:

1. Read `style/default` with `editor_get_profile`.
2. Choose the closest template from `batch_fx_templates`, such as `mesh_burst_hit`, `sprite_explosion_burst`, or `projectile_muzzle_burst`.
3. Duplicate the template Niagara and renderer Blueprint with `effect_duplicate_asset`.
4. Disable unnecessary emitters with `niagara_set_emitter_enabled` / `niagara_delete`.
5. Write renderer, emitter data, bounds, and other UObject properties with `niagara_merge_write`.
6. Fine-tune module input pins such as Lifetime, Scale, Spawn Count, or random ranges with `niagara_set_module_pin`.
7. Write renderer CDO Niagara, NDC, SubType, and batch size with `batch_fx_set_renderer_defaults`.
8. Use `unit_write` to point unit `FFxConfig` entries to the corresponding `SubType`.

This pattern is similar to a code diff: templates carry most of the structure, and MCP performs small reviewable edits.
