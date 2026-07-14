import asyncio
import json
import logging
import sys
from contextlib import asynccontextmanager
from typing import Any, AsyncIterator, Dict, Optional

from mcp.server.fastmcp import FastMCP

from mcp_config import COMMAND_TIMEOUT, CONNECT_TIMEOUT, UNREAL_HOST, UNREAL_PORT

try:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    handlers=[logging.FileHandler("massbattle_mcp_server.log")],
)
logger = logging.getLogger("MassBattleMcpServer")


def _json_arg(value: Any, default: str = "{}") -> str:
    if value is None:
        return default
    if isinstance(value, str):
        return value if value.strip() else default
    return json.dumps(value, ensure_ascii=False)


def _json_arg_with_defaults(value: Any, defaults: Dict[str, Any]) -> str:
    if value is None:
        return json.dumps(defaults, ensure_ascii=False)
    if isinstance(value, dict):
        merged = dict(defaults)
        merged.update(value)
        return json.dumps(merged, ensure_ascii=False)
    if isinstance(value, str):
        if not value.strip():
            return json.dumps(defaults, ensure_ascii=False)
        try:
            parsed = json.loads(value)
        except Exception:
            return value
        if isinstance(parsed, dict):
            merged = dict(defaults)
            merged.update(parsed)
            return json.dumps(merged, ensure_ascii=False)
        return value
    return _json_arg(value)


class UnrealConnection:
    async def send_command(self, command: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(UNREAL_HOST, UNREAL_PORT),
                timeout=CONNECT_TIMEOUT,
            )
        except Exception as exc:
            return {
                "success": False,
                "error": f"Could not connect to MassBattleEditorMCP bridge at {UNREAL_HOST}:{UNREAL_PORT}: {exc}",
            }

        try:
            message = json.dumps({"command": command, "params": params or {}}, ensure_ascii=False)
            writer.write(message.encode("utf-8") + b"\0")
            await writer.drain()
            if writer.can_write_eof():
                writer.write_eof()

            chunks = []
            while True:
                chunk = await asyncio.wait_for(reader.read(4096), timeout=COMMAND_TIMEOUT)
                if not chunk:
                    break
                if b"\0" in chunk:
                    chunks.append(chunk[: chunk.find(b"\0")])
                    break
                chunks.append(chunk)

            payload = b"".join(chunks).decode("utf-8")
            if not payload:
                return {"success": False, "error": "Empty response from Unreal MassBattleEditorMCP bridge."}
            return json.loads(payload)
        except Exception as exc:
            logger.exception("MassBattle MCP command failed: %s", command)
            return {"success": False, "error": str(exc), "command": command}
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


_unreal_connection: Optional[UnrealConnection] = None


def get_connection() -> UnrealConnection:
    global _unreal_connection
    if _unreal_connection is None:
        _unreal_connection = UnrealConnection()
    return _unreal_connection


@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    get_connection()
    yield {}


mcp = FastMCP("MassBattleEditorMCP", lifespan=server_lifespan)


@mcp.tool()
async def massbattle_ping() -> Dict[str, Any]:
    """Check whether the Unreal MassBattleEditorMCP bridge is reachable."""
    return await get_connection().send_command("ping")


@mcp.tool()
async def unit_get_api_status() -> Dict[str, Any]:
    """List Unit MCP API capabilities from Unreal."""
    return await get_connection().send_command("MCP_UnitGetApiStatus")


@mcp.tool()
async def unit_list(options: Any = None) -> Dict[str, Any]:
    """List MassBattle unit config assets."""
    return await get_connection().send_command("MCP_UnitList", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def unit_get(unit_path: str, options: Any = None) -> Dict[str, Any]:
    """Read a MassBattle unit config asset."""
    return await get_connection().send_command(
        "MCP_UnitGet",
        {"UnitPath": unit_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def unit_get_schema(options: Any = None) -> Dict[str, Any]:
    """Read editable MassBattle unit schema information."""
    return await get_connection().send_command("MCP_UnitGetSchema", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def unit_export(options: Any = None) -> Dict[str, Any]:
    """Export a compact MassBattle unit balance table to JSON or CSV."""
    return await get_connection().send_command("MCP_UnitExport", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def unit_write(unit_path: str, unit_data: Any, save_assets: bool = True) -> Dict[str, Any]:
    """Union-write partial MassBattle unit data to an existing unit."""
    return await get_connection().send_command(
        "MCP_UnitMergeUpdate",
        {"UnitPath": unit_path, "UnitDataJson": _json_arg(unit_data), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def unit_create(create_spec: Any, save_assets: bool = True) -> Dict[str, Any]:
    """Create a MassBattle unit from the default or specified template."""
    return await get_connection().send_command(
        "MCP_UnitCreate",
        {"CreateSpecJson": _json_arg(create_spec), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def unit_delete(unit_path: str, options: Any = None) -> Dict[str, Any]:
    """Delete or soft-delete a MassBattle unit; options default to dry_run=true."""
    return await get_connection().send_command(
        "MCP_UnitDelete",
        {"UnitPath": unit_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def unit_find_assets(query: Any) -> Dict[str, Any]:
    """Find project assets useful for MassBattle unit authoring."""
    return await get_connection().send_command("MCP_UnitFindAssets", {"QueryJson": _json_arg(query)})


@mcp.tool()
async def projectile_get_api_status() -> Dict[str, Any]:
    """List projectile DataAsset CRUD, schema, and validation capabilities."""
    return await get_connection().send_command("MCP_ProjectileGetApiStatus")


@mcp.tool()
async def projectile_list(options: Any = None) -> Dict[str, Any]:
    """List MassBattle projectile configuration DataAssets."""
    return await get_connection().send_command(
        "MCP_ProjectileList",
        {"OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def projectile_query(query: Any = None) -> Dict[str, Any]:
    """Query MassBattle projectile configuration DataAssets by path or name."""
    return await get_connection().send_command(
        "MCP_ProjectileQuery",
        {"QueryJson": _json_arg(query)},
    )


@mcp.tool()
async def projectile_get(projectile_path: str, options: Any = None) -> Dict[str, Any]:
    """Read one projectile DataAsset in active-only or full source-aligned form."""
    return await get_connection().send_command(
        "MCP_ProjectileGet",
        {"ProjectilePath": projectile_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def projectile_get_schema(options: Any = None) -> Dict[str, Any]:
    """Read projectile field types, enums, conditions, tooltips, and write rules."""
    return await get_connection().send_command(
        "MCP_ProjectileGetSchema",
        {"OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def projectile_create(create_spec: Any, save_assets: bool = True) -> Dict[str, Any]:
    """Create a projectile DataAsset from a template or class defaults."""
    return await get_connection().send_command(
        "MCP_ProjectileCreate",
        {"CreateSpecJson": _json_arg(create_spec), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def projectile_write(
    projectile_path: str,
    patch: Any,
    save_assets: bool = True,
) -> Dict[str, Any]:
    """Union-write source-aligned projectile data after transient preflight and validation."""
    return await get_connection().send_command(
        "MCP_ProjectileWrite",
        {
            "ProjectilePath": projectile_path,
            "PatchJson": _json_arg(patch),
            "bSaveAssets": save_assets,
        },
    )


@mcp.tool()
async def projectile_validate(projectile_path: str, options: Any = None) -> Dict[str, Any]:
    """Validate movement, damage ownership, triggers, lifecycle FX, and duplicate explosions."""
    return await get_connection().send_command(
        "MCP_ProjectileValidate",
        {"ProjectilePath": projectile_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def projectile_delete(projectile_path: str, options: Any = None) -> Dict[str, Any]:
    """Plan or delete a projectile DataAsset; dry_run=true by default."""
    return await get_connection().send_command(
        "MCP_ProjectileDelete",
        {"ProjectilePath": projectile_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def style_summarize_units(options: Any = None) -> Dict[str, Any]:
    """Summarize MassBattle unit organization by style, family, and path category."""
    return await get_connection().send_command("MCP_StyleSummarizeUnits", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def style_plan_organize_units(options: Any = None) -> Dict[str, Any]:
    """Plan style-based MassBattle unit folder organization without moving assets."""
    return await get_connection().send_command("MCP_StylePlanOrganizeUnits", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def editor_get_status() -> Dict[str, Any]:
    """List MassBattle unit editor workflow capabilities."""
    return await get_connection().send_command("MCP_EditorGetStatus")


@mcp.tool()
async def editor_list_profiles(options: Any = None) -> Dict[str, Any]:
    """List MassBattle unit style profiles and authoring recipes."""
    return await get_connection().send_command("MCP_EditorListProfiles", {"OptionsJson": _json_arg(options)})


@mcp.tool()
async def editor_get_profile(profile_type: str, profile_id: str) -> Dict[str, Any]:
    """Read one MassBattle unit style profile or recipe."""
    return await get_connection().send_command(
        "MCP_EditorGetProfile",
        {"ProfileType": profile_type, "ProfileId": profile_id},
    )


@mcp.tool()
async def editor_plan_create_vat_unit(spec: Any) -> Dict[str, Any]:
    """Diagnostic: preview the MassBattleTools DoAll-equivalent VAT unit plan; strict apply still requires complete canonical inputs."""
    return await get_connection().send_command("MCP_EditorPlanCreateVatUnit", {"SpecJson": _json_arg(spec)})


@mcp.tool()
async def editor_validate_create_vat_unit(spec: Any) -> Dict[str, Any]:
    """Diagnostic: validate DoAll-equivalent VAT unit inputs without writing assets."""
    return await get_connection().send_command("MCP_EditorValidateCreateVatUnit", {"SpecJson": _json_arg(spec)})


@mcp.tool()
async def editor_apply_create_vat_unit(
    spec: Any,
    save_assets: bool = True,
    compact_response: bool = True,
) -> Dict[str, Any]:
    """Primary non-selection DoAll-equivalent VAT unit authoring entry; requires canonical complete inputs and validates before writing assets."""
    return await get_connection().send_command(
        "MCP_EditorApplyCreateVatUnit",
        {"SpecJson": _json_arg_with_defaults(spec, {"compact_response": compact_response}), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def unit_plan_write(unit_path: str, unit_data: Any) -> Dict[str, Any]:
    """Plan a union-write and return its diff without mutating the unit."""
    return await get_connection().send_command(
        "MCP_UnitPlanMergeUpdate",
        {"UnitPath": unit_path, "UnitDataJson": _json_arg(unit_data)},
    )


@mcp.tool()
async def unit_preview_plan(plan_id: str) -> Dict[str, Any]:
    """Read a previously created unit mutation plan and its complete diff."""
    return await get_connection().send_command(
        "MCP_UnitPreviewDiff",
        {"PlanId": plan_id},
    )


@mcp.tool()
async def unit_apply_plan(plan_id: str, save_assets: bool = True) -> Dict[str, Any]:
    """Apply a reviewed unit mutation plan and optionally save the asset."""
    return await get_connection().send_command(
        "MCP_UnitApplyPlan",
        {"PlanId": plan_id, "bSaveAssets": save_assets},
    )


@mcp.tool()
async def editor_plan_create_vat_unit_from_selection(options: Any = None) -> Dict[str, Any]:
    """Diagnostic: infer the DoAll spec from current editor selection or selected_assets and return it for review."""
    return await get_connection().send_command(
        "MCP_EditorPlanCreateVatUnitFromSelection",
        {"OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def editor_apply_create_vat_unit_from_selection(
    options: Any = None,
    save_assets: bool = True,
    compact_response: bool = True,
) -> Dict[str, Any]:
    """Primary one-click current selection -> generate entry matching the MassBattleTools DoAll workflow."""
    return await get_connection().send_command(
        "MCP_EditorApplyCreateVatUnitFromSelection",
        {"OptionsJson": _json_arg_with_defaults(options, {"compact_response": compact_response}), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def editor_inspect_actor_assembly(actor_path: str, options: Any = None) -> Dict[str, Any]:
    """Inspect an Actor's modular skeletal/static meshes, effective materials, and resolved weapon bind bones without writing assets."""
    return await get_connection().send_command(
        "MCP_EditorInspectActorAssembly",
        {"ActorPath": actor_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def editor_plan_create_vat_unit_from_actor(spec: Any) -> Dict[str, Any]:
    """Plan Actor component assembly followed by strict VAT unit authoring without writing assets."""
    return await get_connection().send_command(
        "MCP_EditorPlanCreateVatUnitFromActor",
        {"SpecJson": _json_arg(spec)},
    )


@mcp.tool()
async def editor_apply_create_vat_unit_from_actor(
    spec: Any,
    save_assets: bool = True,
    compact_response: bool = True,
) -> Dict[str, Any]:
    """Assemble an Actor and configured weapon into an animation-compatible SkeletalMesh, then author its strict VAT unit."""
    return await get_connection().send_command(
        "MCP_EditorApplyCreateVatUnitFromActor",
        {"SpecJson": _json_arg_with_defaults(spec, {"compact_response": compact_response}), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def editor_plan_organize_unit_assets(unit_path: str, options: Any = None) -> Dict[str, Any]:
    """Plan moving a MassBattle unit and linked generated/source assets into the selected style layout."""
    return await get_connection().send_command(
        "MCP_EditorPlanOrganizeUnitAssets",
        {"UnitPath": unit_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def editor_apply_organize_unit_assets(
    unit_path: str,
    options: Any = None,
    save_assets: bool = True,
) -> Dict[str, Any]:
    """Apply a reviewed MassBattle unit organization plan; options default to dry_run=true."""
    return await get_connection().send_command(
        "MCP_EditorApplyOrganizeUnitAssets",
        {"UnitPath": unit_path, "OptionsJson": _json_arg(options), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def effect_asset_get_api_status() -> Dict[str, Any]:
    """List generic MassBattle effect asset API capabilities."""
    return await get_connection().send_command("MCP_EffectAssetGetApiStatus")


@mcp.tool()
async def effect_asset_query(query: Any) -> Dict[str, Any]:
    """Query Niagara, Cascade, material, Blueprint, and related visual effect assets."""
    return await get_connection().send_command("MCP_EffectAssetQuery", {"QueryJson": _json_arg(query)})


@mcp.tool()
async def effect_asset_read_summary(asset_path: str, options: Any = None) -> Dict[str, Any]:
    """Read a typed summary for an effect-related asset."""
    return await get_connection().send_command(
        "MCP_EffectAssetReadSummary",
        {"AssetPath": asset_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def effect_asset_export_text(asset_path: str, options: Any = None) -> Dict[str, Any]:
    """Export deterministic text for an effect-related asset."""
    return await get_connection().send_command(
        "MCP_EffectAssetExportText",
        {"AssetPath": asset_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def effect_asset_soft_delete(asset_path: str, options: Any = None) -> Dict[str, Any]:
    """Move an unreferenced generic asset to trash; dry_run=true by default."""
    return await get_connection().send_command(
        "MCP_EffectAssetSoftDelete",
        {"AssetPath": asset_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def effect_duplicate_asset(source_asset_path: str, new_asset_name: str, package_path: str, save_assets: bool = True) -> Dict[str, Any]:
    """Duplicate an effect-related asset additively."""
    return await get_connection().send_command(
        "MCP_EffectDuplicateAsset",
        {
            "SourceAssetPath": source_asset_path,
            "NewAssetName": new_asset_name,
            "PackagePath": package_path,
            "bSaveAssets": save_assets,
        },
    )


@mcp.tool()
async def batch_fx_read_renderer_defaults(target_class_path: str) -> Dict[str, Any]:
    """Read AMassBattleFxRenderer Blueprint class defaults used by newly placed actors."""
    return await get_connection().send_command(
        "MCP_BatchFxReadRendererDefaults",
        {"TargetClassPath": target_class_path},
    )


@mcp.tool()
async def batch_fx_set_renderer_defaults(
    target_class_path: str,
    niagara_system_path: str,
    ndc_burst_fx_path: str,
    subtype: int,
    render_batch_size: int = 2048,
    pooling_cooldown: float = 3.0,
    save_assets: bool = True,
) -> Dict[str, Any]:
    """Set AMassBattleFxRenderer Blueprint CDO defaults for a batched FX subtype."""
    return await get_connection().send_command(
        "MCP_BatchFxSetRendererDefaults",
        {
            "TargetClassPath": target_class_path,
            "NiagaraSystemPath": niagara_system_path,
            "NdcBurstFxPath": ndc_burst_fx_path,
            "SubType": subtype,
            "RenderBatchSize": render_batch_size,
            "PoolingCooldown": pooling_cooldown,
            "bSaveAssets": save_assets,
        },
    )


@mcp.tool()
async def niagara_get_api_status() -> Dict[str, Any]:
    """List Niagara MCP API capabilities."""
    return await get_connection().send_command("MCP_NiagaraGetApiStatus")


@mcp.tool()
async def niagara_query(query: Any) -> Dict[str, Any]:
    """Query Niagara systems by path or name text."""
    return await get_connection().send_command("MCP_NiagaraQuery", {"QueryJson": _json_arg(query)})


@mcp.tool()
async def niagara_read_summary(system_path: str, options: Any = None) -> Dict[str, Any]:
    """Read a Niagara system summary."""
    return await get_connection().send_command(
        "MCP_NiagaraReadSummary",
        {"SystemPath": system_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def niagara_read_module(system_path: str, selector: Any) -> Dict[str, Any]:
    """Read one Niagara module node and pins."""
    return await get_connection().send_command(
        "MCP_NiagaraReadModule",
        {"SystemPath": system_path, "SelectorJson": _json_arg(selector)},
    )


@mcp.tool()
async def effect_discard_unsaved_duplicate(asset_path: str) -> Dict[str, Any]:
    """Discard only an unsaved in-memory duplicate created by this MCP editor session."""
    return await get_connection().send_command(
        "MCP_EffectDiscardUnsavedDuplicate",
        {"AssetPath": asset_path},
    )


@mcp.tool()
async def niagara_read_graph(system_path: str, selector: Any = None) -> Dict[str, Any]:
    """Read Niagara script graphs with stable node/pin ids and explicit links."""
    return await get_connection().send_command(
        "MCP_NiagaraReadGraph",
        {"SystemPath": system_path, "SelectorJson": _json_arg(selector)},
    )


@mcp.tool()
async def niagara_compare_systems(
    source_system_path: str,
    target_system_path: str,
    options: Any = None,
) -> Dict[str, Any]:
    """Compare a source Niagara system with a duplicate/translation using source-neutral fingerprints."""
    return await get_connection().send_command(
        "MCP_NiagaraCompareSystems",
        {
            "SourceSystemPath": source_system_path,
            "TargetSystemPath": target_system_path,
            "OptionsJson": _json_arg(options),
        },
    )


@mcp.tool()
async def niagara_read_all(system_path: str, options: Any = None) -> Dict[str, Any]:
    """Read reflected Niagara properties plus every function-call module and pin."""
    return await get_connection().send_command(
        "MCP_NiagaraReadAll",
        {"SystemPath": system_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def niagara_export_text(system_path: str, options: Any = None) -> Dict[str, Any]:
    """Export deterministic Niagara text for close reading."""
    return await get_connection().send_command(
        "MCP_NiagaraExportText",
        {"SystemPath": system_path, "OptionsJson": _json_arg(options)},
    )


@mcp.tool()
async def niagara_merge_write(system_path: str, patch: Any, save_assets: bool = False) -> Dict[str, Any]:
    """Union-merge Niagara property writes. This does not delete."""
    return await get_connection().send_command(
        "MCP_NiagaraMergeWrite",
        {"SystemPath": system_path, "PatchJson": _json_arg(patch), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def niagara_set_module_pin(
    system_path: str,
    selector: Any,
    pin_name: str,
    value_text: str,
    save_assets: bool = True,
) -> Dict[str, Any]:
    """Set one Niagara function-call module input pin default value."""
    return await get_connection().send_command(
        "MCP_NiagaraSetModulePin",
        {
            "SystemPath": system_path,
            "SelectorJson": _json_arg(selector),
            "PinName": pin_name,
            "ValueText": value_text,
            "bSaveAssets": save_assets,
        },
    )


@mcp.tool()
async def niagara_apply_graph_edit(
    system_path: str,
    edit: Any,
    save_assets: bool = False,
) -> Dict[str, Any]:
    """Apply ordered Niagara graph edits, compile once, validate preservation, and optionally save."""
    return await get_connection().send_command(
        "MCP_NiagaraApplyGraphEdit",
        {"SystemPath": system_path, "EditJson": _json_arg(edit), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def niagara_batch_translate(
    manifest: Any,
    apply: bool = False,
    save_assets: bool = True,
) -> Dict[str, Any]:
    """Preflight or execute an explicit source-first Niagara translation manifest; never guesses visual edits."""
    if isinstance(manifest, str):
        try:
            manifest = json.loads(manifest)
        except Exception as exc:
            return {"success": False, "error": f"Manifest is not valid JSON: {exc}"}
    if not isinstance(manifest, dict):
        return {"success": False, "error": "Manifest must be a JSON object."}

    schema = manifest.get("schema", "massbattle.niagara.translation_manifest.v1")
    if schema != "massbattle.niagara.translation_manifest.v1":
        return {"success": False, "error": f"Unsupported manifest schema: {schema}"}
    items = manifest.get("items")
    if not isinstance(items, list) or not items:
        return {"success": False, "error": "Manifest requires a non-empty items array."}
    if len(items) > 256:
        return {"success": False, "error": "Manifest items is limited to 256 entries."}

    stop_on_error = bool(manifest.get("stop_on_error", True))
    preflight: list[Dict[str, Any]] = []
    preflight_errors: list[Dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_targets: set[str] = set()

    for index, raw_item in enumerate(items):
        item_errors: list[str] = []
        if not isinstance(raw_item, dict):
            preflight_errors.append({"index": index, "errors": ["item must be an object"]})
            continue

        item_id = str(raw_item.get("id") or f"item_{index:03d}")
        source = raw_item.get("source_system_path")
        new_name = raw_item.get("new_asset_name")
        package_path = raw_item.get("package_path")
        edit = raw_item.get("edit", {"operations": []})
        comparison = raw_item.get("comparison", {})
        if isinstance(edit, str):
            try:
                edit = json.loads(edit)
            except Exception as exc:
                item_errors.append(f"edit is not valid JSON: {exc}")
        if isinstance(comparison, str):
            try:
                comparison = json.loads(comparison)
            except Exception as exc:
                item_errors.append(f"comparison is not valid JSON: {exc}")

        if item_id in seen_ids:
            item_errors.append(f"duplicate id: {item_id}")
        seen_ids.add(item_id)
        if not isinstance(source, str) or not source.strip():
            item_errors.append("source_system_path is required")
        if not isinstance(new_name, str) or not new_name.strip():
            item_errors.append("new_asset_name is required")
        if not isinstance(package_path, str) or not package_path.startswith("/Game/"):
            item_errors.append("package_path must start with /Game/")
        if not isinstance(edit, dict) or not isinstance(edit.get("operations"), list):
            item_errors.append("edit must be an object with an operations array")
        if not isinstance(comparison, dict):
            item_errors.append("comparison must be an object")

        target = ""
        if isinstance(new_name, str) and isinstance(package_path, str):
            package_path = package_path.rstrip("/")
            target = f"{package_path}/{new_name}.{new_name}"
            if target in seen_targets:
                item_errors.append(f"duplicate target: {target}")
            seen_targets.add(target)
            if source == target:
                item_errors.append("source and target must be different assets")

        source_read: Dict[str, Any] = {}
        target_read: Dict[str, Any] = {}
        if not item_errors:
            source_read = await get_connection().send_command(
                "MCP_NiagaraReadSummary",
                {"SystemPath": source, "OptionsJson": '{"include_modules":false}'},
            )
            if not source_read.get("success"):
                item_errors.append(f"source read failed: {source_read.get('error', source_read)}")
            else:
                target_read = await get_connection().send_command(
                    "MCP_NiagaraReadSummary",
                    {"SystemPath": target, "OptionsJson": '{"include_modules":false}'},
                )
                if target_read.get("success"):
                    item_errors.append(f"target already exists: {target}")

        record = {
            "index": index,
            "id": item_id,
            "source": source,
            "target": target,
            "edit": edit,
            "comparison": comparison,
            "source_ready_to_run": source_read.get("ready_to_run"),
            "errors": item_errors,
        }
        preflight.append(record)
        if item_errors:
            preflight_errors.append({"index": index, "id": item_id, "errors": item_errors})

    if preflight_errors or not apply:
        return {
            "success": not preflight_errors,
            "schema": schema,
            "apply_requested": apply,
            "mutated": False,
            "item_count": len(items),
            "preflight": preflight,
            "errors": preflight_errors,
        }

    results: list[Dict[str, Any]] = []

    async def discard_failed_duplicate(result: Dict[str, Any], target: str) -> None:
        cleanup = await get_connection().send_command(
            "MCP_EffectDiscardUnsavedDuplicate",
            {"AssetPath": target},
        )
        result["cleanup"] = cleanup

    for item in preflight:
        result: Dict[str, Any] = {
            "index": item["index"],
            "id": item["id"],
            "source": item["source"],
            "target": item["target"],
            "success": False,
            "saved": False,
        }
        duplicate = await get_connection().send_command(
            "MCP_EffectDuplicateAsset",
            {
                "SourceAssetPath": item["source"],
                "NewAssetName": item["target"].rsplit("/", 1)[-1].split(".", 1)[0],
                "PackagePath": item["target"].rsplit("/", 1)[0],
                "bSaveAssets": False,
            },
        )
        result["duplicate"] = duplicate
        if not duplicate.get("success"):
            result["error"] = f"duplicate failed: {duplicate.get('error', duplicate)}"
            results.append(result)
            if stop_on_error:
                break
            continue

        target = duplicate.get("asset_path", item["target"])
        clone_compare = await get_connection().send_command(
            "MCP_NiagaraCompareSystems",
            {
                "SourceSystemPath": item["source"],
                "TargetSystemPath": target,
                # Asset duplication is intentionally unsaved and may not have entered Niagara's
                # compile queue yet.  This gate proves source-neutral structural identity; the
                # following graph-edit barrier compiles and enforces ready-to-run before save.
                "OptionsJson": '{"mode":"exact","require_ready_to_run":false}',
            },
        )
        result["clone_comparison"] = clone_compare
        if not clone_compare.get("success"):
            result["error"] = "duplicate is not an exact source-neutral clone"
            await discard_failed_duplicate(result, target)
            results.append(result)
            if stop_on_error:
                break
            continue

        edit_result = await get_connection().send_command(
            "MCP_NiagaraApplyGraphEdit",
            {
                "SystemPath": target,
                "EditJson": _json_arg(item["edit"]),
                "bSaveAssets": False,
            },
        )
        result["edit"] = edit_result
        if not edit_result.get("success"):
            result["error"] = f"graph edit failed: {edit_result.get('error', edit_result)}"
            await discard_failed_duplicate(result, target)
            results.append(result)
            if stop_on_error:
                break
            continue

        comparison = dict(item["comparison"])
        comparison.setdefault("mode", "translation")
        comparison.setdefault("require_ready_to_run", True)
        translation_compare = await get_connection().send_command(
            "MCP_NiagaraCompareSystems",
            {
                "SourceSystemPath": item["source"],
                "TargetSystemPath": target,
                "OptionsJson": _json_arg(comparison),
            },
        )
        result["translation_comparison"] = translation_compare
        if not translation_compare.get("success"):
            result["error"] = "translation did not satisfy the declared source-preservation policy"
            await discard_failed_duplicate(result, target)
            results.append(result)
            if stop_on_error:
                break
            continue

        if save_assets:
            save_result = await get_connection().send_command(
                "MCP_NiagaraApplyGraphEdit",
                {
                    "SystemPath": target,
                    "EditJson": '{"operations":[],"validation":{"require_ready_to_run":true}}',
                    "bSaveAssets": True,
                },
            )
            result["save"] = save_result
            if not save_result.get("success") or not save_result.get("saved"):
                result["error"] = f"save validation failed: {save_result.get('error', save_result)}"
                await discard_failed_duplicate(result, target)
                results.append(result)
                if stop_on_error:
                    break
                continue
            result["saved"] = True

        result["success"] = True
        results.append(result)

    failed = [item for item in results if not item.get("success")]
    return {
        "success": not failed and len(results) == len(items),
        "schema": schema,
        "apply_requested": True,
        "mutated": bool(results),
        "save_requested": save_assets,
        "item_count": len(items),
        "processed_count": len(results),
        "failed_count": len(failed),
        "results": results,
    }


@mcp.tool()
async def niagara_set_emitter_enabled(system_path: str, emitter_name: str, enabled: bool, save_assets: bool = True) -> Dict[str, Any]:
    """Explicitly enable or disable one Niagara emitter handle."""
    return await get_connection().send_command(
        "MCP_NiagaraSetEmitterEnabled",
        {
            "SystemPath": system_path,
            "EmitterName": emitter_name,
            "bEnabled": enabled,
            "bSaveAssets": save_assets,
        },
    )


@mcp.tool()
async def niagara_delete(system_path: str, delete_spec: Any, save_assets: bool = False) -> Dict[str, Any]:
    """Run explicit Niagara delete operations such as renderer or user-parameter removal."""
    return await get_connection().send_command(
        "MCP_NiagaraDelete",
        {"SystemPath": system_path, "DeleteJson": _json_arg(delete_spec), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def niagara_add_sprite_renderer(
    system_path: str,
    emitter_name: str,
    renderer: Any,
    save_assets: bool = False,
) -> Dict[str, Any]:
    """Add one configured sprite renderer to an existing Niagara emitter."""
    return await get_connection().send_command(
        "MCP_NiagaraAddSpriteRenderer",
        {
            "SystemPath": system_path,
            "EmitterName": emitter_name,
            "RendererJson": _json_arg(renderer),
            "bSaveAssets": save_assets,
        },
    )


if __name__ == "__main__":
    try:
        logger.info("Starting MassBattleEditorMCP server on stdio; Unreal bridge target %s:%s", UNREAL_HOST, UNREAL_PORT)
        mcp.run(transport="stdio")
    except Exception as exc:
        logger.exception("MassBattleEditorMCP server failed to start: %s", exc)
        print("MASSBATTLE_MCP_SERVER_START_FAILED")
