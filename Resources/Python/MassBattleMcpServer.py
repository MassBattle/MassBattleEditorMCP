import asyncio
import json
import logging
import sys
from contextlib import asynccontextmanager
from typing import Any, AsyncIterator, Dict, Optional

from mcp.server.fastmcp import FastMCP

from mcp_config import SOCKET_TIMEOUT, UNREAL_HOST, UNREAL_PORT

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
                timeout=SOCKET_TIMEOUT,
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
                chunk = await asyncio.wait_for(reader.read(4096), timeout=SOCKET_TIMEOUT)
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
    """Plan VAT unit authoring, resolving style defaults and returning warnings for inferred or incomplete inputs."""
    return await get_connection().send_command("MCP_EditorPlanCreateVatUnit", {"SpecJson": _json_arg(spec)})


@mcp.tool()
async def editor_validate_create_vat_unit(spec: Any) -> Dict[str, Any]:
    """Validate VAT unit authoring readiness, including defaulted fields and warnings that should be reviewed."""
    return await get_connection().send_command("MCP_EditorValidateCreateVatUnit", {"SpecJson": _json_arg(spec)})


@mcp.tool()
async def editor_apply_create_vat_unit(spec: Any, save_assets: bool = True) -> Dict[str, Any]:
    """Execute VAT unit authoring using resolved defaults; warnings in the result identify fields the AI should refine."""
    return await get_connection().send_command(
        "MCP_EditorApplyCreateVatUnit",
        {"SpecJson": _json_arg(spec), "bSaveAssets": save_assets},
    )


@mcp.tool()
async def editor_plan_create_vat_unit_from_selection(options: Any = None) -> Dict[str, Any]:
    """Build a VAT unit create spec from current editor selection or selected_assets, then return a reviewable plan."""
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
    """Build a VAT unit create spec from current editor selection or selected_assets, then execute the create workflow."""
    return await get_connection().send_command(
        "MCP_EditorApplyCreateVatUnitFromSelection",
        {"OptionsJson": _json_arg_with_defaults(options, {"compact_response": compact_response}), "bSaveAssets": save_assets},
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


if __name__ == "__main__":
    try:
        logger.info("Starting MassBattleEditorMCP server on stdio; Unreal bridge target %s:%s", UNREAL_HOST, UNREAL_PORT)
        mcp.run(transport="stdio")
    except Exception as exc:
        logger.exception("MassBattleEditorMCP server failed to start: %s", exc)
        print("MASSBATTLE_MCP_SERVER_START_FAILED")
