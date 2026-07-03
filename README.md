# MassBattleEditorMCP

[中文文档](zh.md)

I think RTS games are one of the best ways to reason about an AI society. Humans should not be trapped in every low-level action. Humans should define strategy, constraints, tradeoffs, and goals; AI and tools should turn those goals into executable tactical work. In the future, the scarce people will not be the ones who merely repeat implementation details. They will be the ones who can set direction, organize systems, judge outcomes, and take responsibility.

That is also why RTS matters to me. An ambitious RTS should not start with a vision of massive war, then cut the unit count down to a handful because the technology cannot carry the design. Scale is not decoration. Scale changes the gameplay, the tactical space, the presentation layer, and the tools required to build the game.

[Mass Battle Frame](https://github.com/MassBattle/MassBattleFrame) fits my taste because it aims at extreme performance. Only when the foundation is fast enough can we afford the things that look wasteful but create the experience: denser units, richer visuals, larger battlefields, and more tactical feedback. If your RTS target is thousands or tens of thousands of units, [Mass Battle Frame on Fab](https://www.fab.com/listings/191850b4-44d3-4455-aa76-874bc0196a10?lang=zh-cn) is not a luxury. It is necessary infrastructure.

When a plugin is good enough, hobbyists naturally start building around it. MassBattleEditorMCP is one proof of that. It is not a runtime feature of Mass Battle Frame. It is an editor tool hand that AI can use. It exposes unit, effect, material, renderer, Niagara, and DataAsset authoring as callable, readable, batchable editor operations, so AI can handle tactical asset translation while humans keep ownership of design judgment.

If your game already has thousands of units, this ecosystem is valuable. The story above may be dry, but it is real: strong core technology attracts surrounding community tools. Alongside this MCP plugin, there are other community plugins around Mass Battle and RTS workflows:

- [fogofwar](https://github.com/winyunq/FogOfWar): fog of war; includes the minimap plugin.
- [landmark](https://github.com/winyunq/LandmarkSystem): landmark system, also usable as an editor tool.
- [openrtscamera](https://github.com/winyunq/OpenRTSCamera): open-source RTS camera and RTS command panel.

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

`skills/massbattle-unit-authoring`: guides AI through Unit MCP creation, reading, union-writing, deletion, and validation for MassBattle unit configs.
`skills/massbattle-effect-mcp`: guides AI through Niagara / Effect MCP querying, reading, exporting, duplication, and batch-effect configuration, with Unit MCP coordination.

Install to local Codex:

```powershell
Copy-Item -Recurse -Force .\skills\massbattle-unit-authoring $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-effect-mcp $env:USERPROFILE\.codex\skills\
```

If `CODEX_HOME` is set, copy them to `$env:CODEX_HOME\skills\`.
MCP is the editor tool interface. A skill only describes how to compose those tools; it should not turn a workflow into one large button.

### Codex MCP Server Installation

MassBattleEditorMCP has two Codex-facing layers:

1. A local TCP bridge inside the UE editor plugin, listening on `127.0.0.1:55558` by default.
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
After successful installation, you should see `massbattle-editor-mcp` and be able to call primitive tools such as `unit_get`, `unit_create`, `unit_write`, `unit_delete`, `effect_asset_read_summary`, `niagara_set_module_pin`, `batch_fx_read_renderer_defaults`, and `batch_fx_set_renderer_defaults`.

Note: `FFxConfig.AgentBehaviorState` uses `EAgentBehaviorState`. Writable values include `None`, `Appearing`, `Sleeping`, `Patrolling`, `Attacking`, `Hit`, and `Dying`. Hit FX should use `Hit`; do not write the runtime flag name `BeingHit` into this field.

## MCP Capability List

| Category | MCP Tool | Status | Purpose |
| --- | --- | :---: | --- |
| Connection / diagnostics | `massbattle_ping` | Available | Confirm that the Codex MCP server can connect to the UE editor bridge. |
| Connection / diagnostics | `unit_get_api_status` | Available | Read Unit MCP capabilities. |
| Connection / diagnostics | `effect_asset_get_api_status` | Available | Read Effect Asset / Batch FX MCP capabilities. |
| Connection / diagnostics | `niagara_get_api_status` | Available | Read Niagara MCP capabilities. |
| Unit MCP | `unit_list` | Available | List `MassBattleAgentConfigDataAsset` unit config assets. |
| Unit MCP | `unit_get` | Available | Read one unit config with simple/full views and default filtering. |
| Unit MCP | `unit_get_schema` | Available | Read editable unit fields, types, roles, and tooltips. |
| Unit MCP | `unit_export` | Available | Export compact unit balance tables for analysis or batch review. |
| Unit MCP | `unit_find_assets` | Available | Find candidate SkeletalMesh, Renderer, Niagara, and related assets for unit authoring. |
| Unit MCP | `unit_create` | Available | Create a new unit; use the default template when no template is provided; optional initial unit data is supported. |
| Unit MCP | `unit_write` | Available | Union-write partial source-aligned JSON to an existing unit; omitted fields stay unchanged. |
| Unit MCP | `unit_delete` | Available | Explicitly delete or soft-delete a unit; dry-run by default. |
| Style MCP | `style_summarize_units` | Available | Summarize unit assets by style, family, and path category. |
| Style MCP | `style_plan_organize_units` | Available | Plan style-based unit folder organization without moving assets. |
| Unit Editor MCP | `editor_get_status` | Available | Read unit editor workflow capabilities. |
| Unit Editor MCP | `editor_list_profiles` | Available | List style profiles and authoring recipes. |
| Unit Editor MCP | `editor_get_profile` | Available | Read one profile or recipe. |
| Unit Editor MCP | `editor_plan_organize_unit_assets` | Available | Plan moving one unit and linked generated assets into the style layout. |
| Unit Editor MCP | `editor_apply_organize_unit_assets` | Available | Apply a reviewed unit asset organization plan; dry-run by default. |
| Effect Asset MCP | `effect_asset_query` | Available | Query visual assets such as Niagara, Cascade, Blueprint, Material, Texture, and Sound by `query/root/classes/limit`. |
| Effect Asset MCP | `effect_asset_read_summary` | Available | Read summaries for unknown effect asset types; Cascade returns emitter, LOD, module, and dependency details. |
| Effect Asset MCP | `effect_asset_export_text` | Available | Export deterministic text for close AI reading and review. |
| Effect Asset MCP | `effect_asset_soft_delete` | Available | Read references, then move unreferenced assets to `_Trash`; dry-run by default. |
| Effect Asset MCP | `effect_duplicate_asset` | Available | Additively duplicate assets without deleting or overwriting the source. |
| Niagara MCP | `niagara_query` | Available | Query Niagara Systems by path or name. |
| Niagara MCP | `niagara_read_summary` | Available | Read Niagara system, emitter, renderer, user parameter, and module summaries. |
| Niagara MCP | `niagara_read_module` | Available | Read one Niagara module node and its pins. |
| Niagara MCP | `niagara_export_text` | Available | Export deterministic Niagara text. |
| Niagara MCP | `niagara_merge_write` | Available | Union-write Niagara properties; does not handle deletion. |
| Niagara MCP | `niagara_set_module_pin` | Available | Write one Niagara FunctionCall module input pin default; linked pins are rejected by default. |
| Niagara MCP | `niagara_set_emitter_enabled` | Available | Explicitly enable or disable one Niagara emitter handle. |
| Niagara MCP | `niagara_delete` | Available | Explicitly delete renderers, user parameters, disable emitters, etc. |
| Batch FX MCP | `batch_fx_read_renderer_defaults` | Available | Read `AMassBattleFxRenderer` Blueprint defaults inherited by newly placed actors. |
| Batch FX MCP | `batch_fx_set_renderer_defaults` | Available | Set `AMassBattleFxRenderer` Blueprint defaults, including `NiagaraSystemAsset`, `NDC_BurstFx`, `SubType`, batch size, and pooling cooldown. |

The batch FX loop is: read or duplicate reference effect assets, prepare batched Niagara/NDC/Renderer Blueprints, let MCP write and verify renderer Blueprint defaults, have the user place the renderer actor in a test level, then use Unit MCP to write `FFxConfig` into arrays such as `Hit.SpawnFx`, `Death.SpawnFx`, and `Attack.SpawnFx`. MCP does not automatically modify the current level layout. As long as the user does not override instance parameters in the level, newly placed actors should inherit asset defaults.

## Default Style And Template-Based Workflow

The default style profile is `Resources/UnitManagementStyles/default.massbattle_unit_style.json`.
It is not a runtime feature. It is authoring context for AI and MCP tools: scan roots, unit organization rules, unit authoring defaults, and batch FX templates.
When `unit_create` does not receive `template_unit`, it reads `authoring_defaults.default_unit_template` as the default unit template. Configure that path before using default-template creation.

Batch FX should not start from blank assets. Recommended flow:

1. Read `style/default` with `editor_get_profile`.
2. Choose the closest template from `batch_fx_templates`, such as `mesh_burst_hit`, `sprite_explosion_burst`, or `projectile_muzzle_burst`.
3. Duplicate the template Niagara and renderer Blueprint with `effect_duplicate_asset`.
4. Disable unnecessary emitters with `niagara_set_emitter_enabled` / `niagara_delete`.
5. Write renderer, emitter data, bounds, and other UObject properties with `niagara_merge_write`.
6. Fine-tune module input pins such as Lifetime, Scale, Spawn Count, or random ranges with `niagara_set_module_pin`.
7. Write renderer CDO Niagara, NDC, SubType, and batch size with `batch_fx_set_renderer_defaults`.
8. Use `unit_write` to point unit `FFxConfig` entries to the corresponding `SubType`.

This pattern is similar to a code diff: templates carry most of the structure, and MCP performs small reviewable edits.
