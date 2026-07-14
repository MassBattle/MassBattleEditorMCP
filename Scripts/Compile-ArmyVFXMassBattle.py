#!/usr/bin/env python3
"""Compile ArmyVFX Niagara systems into source-faithful MassBattle mappings.

The compiler deliberately separates two delivery modes:

* ``native_burst`` replaces only an immediate Spawn Burst scheduling layer with
  the MassBattle NDC burst adapter. The authored visual graph, renderers,
  materials, simulation targets, and event handlers remain intact.
* ``exact_component_pool`` produces a structurally exact, compiled copy for
  systems whose scheduling semantics cannot be represented by the current NDC
  contract (per-controller curves, Spawn Per Unit, continuous attachment, or
  delayed phases). Runtime pooling is handled by the Demo/controller layer.

Every native edit is probed on an unsaved scratch duplicate first. The exact
set of source edges displaced by stack insertion is then declared on the real
edit; no native target is saved behind an unrestricted edge-removal policy.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


SCRIPT_PATH = Path(__file__).resolve()
PLUGIN_ROOT = SCRIPT_PATH.parent.parent
PROJECT_ROOT = PLUGIN_ROOT.parent.parent
DEFAULT_SOURCE_IR = (
    PROJECT_ROOT
    / "Saved"
    / "MassBattleEditorMCP"
    / "Validation"
    / "ArmyVFX_Niagara_SourceIR.json"
)
DEFAULT_OUTPUT_DIR = (
    PROJECT_ROOT / "Saved" / "MassBattleEditorMCP" / "Validation"
)

BRIDGE_HOST = os.environ.get("MASSBATTLE_UNREAL_HOST", "127.0.0.1")
BRIDGE_PORT = int(os.environ.get("MASSBATTLE_UNREAL_PORT", "55258"))
CONNECT_TIMEOUT = float(os.environ.get("MASSBATTLE_CONNECT_TIMEOUT", "10"))
COMMAND_TIMEOUT = float(os.environ.get("MASSBATTLE_COMMAND_TIMEOUT", "1200"))

INIT_MODULE = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NM_BurstFxInitFromNDC.NM_BurstFxInitFromNDC"
)
BURST_DATA_CHANNEL = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NDC_BurstFx.NDC_BurstFx"
)
SPAWN_MODULE = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NM_BurstFxSpawnFromNDC.NM_BurstFxSpawnFromNDC"
)
READ_MODULE = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NM_BurstFxReadFromNDC.NM_BurstFxReadFromNDC"
)
SET_PARAMS_MODULE = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NM_SetBurstFxParams.NM_SetBurstFxParams"
)

NATIVE_SUBTYPE_BASE = 80
NDC_MIN_LISTENER_DURATION = 0.5
SYSTEM_STATE_DURATION_PARAMETER = "Constants.SystemState.Loop Duration"
RANDOM_INT_RE = re.compile(
    r"(?:rand|random(?:\s+range)?(?:\s+int)?)\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)",
    re.IGNORECASE,
)


class CompileError(RuntimeError):
    """Raised when a source-faithfulness or compile gate fails."""


@dataclass(frozen=True)
class AssetPlan:
    source: str
    name: str
    category: str
    exact_target: str
    mode: str
    reasons: tuple[str, ...]
    native_target: str | None = None
    subtype: int | None = None
    edit: dict[str, Any] | None = None


class UnrealBridge:
    def __init__(self, host: str = BRIDGE_HOST, port: int = BRIDGE_PORT) -> None:
        self.host = host
        self.port = port

    async def command(self, command: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=CONNECT_TIMEOUT,
            )
        except Exception as exc:  # pragma: no cover - exercised against UE
            raise CompileError(
                f"Cannot connect to MassBattleEditorMCP at {self.host}:{self.port}: {exc}"
            ) from exc

        try:
            payload = json.dumps(
                {"command": command, "params": params or {}},
                ensure_ascii=False,
            ).encode("utf-8") + b"\0"
            writer.write(payload)
            await writer.drain()
            if writer.can_write_eof():
                writer.write_eof()

            chunks: list[bytes] = []
            while True:
                chunk = await asyncio.wait_for(reader.read(65536), timeout=COMMAND_TIMEOUT)
                if not chunk:
                    break
                marker = chunk.find(b"\0")
                if marker >= 0:
                    chunks.append(chunk[:marker])
                    break
                chunks.append(chunk)
            if not chunks:
                raise CompileError(f"{command}: Unreal returned an empty response")
            result = json.loads(b"".join(chunks).decode("utf-8"))
            if not isinstance(result, dict):
                raise CompileError(f"{command}: Unreal returned a non-object response")
            return result
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


def json_arg(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def object_name(object_path: str) -> str:
    return object_path.rsplit("/", 1)[-1].split(".", 1)[0]


def category_from_path(object_path: str) -> str:
    marker = "/ArmyVFX/Niagara/"
    suffix = object_path.split(marker, 1)[-1]
    pieces = suffix.split("/")
    return pieces[0] if len(pieces) > 1 else "Uncategorized"


def stack_modules(graph: dict[str, Any]) -> list[dict[str, Any]]:
    return sorted(
        (
            node
            for node in graph.get("nodes", [])
            if node.get("kind") == "function_call" and node.get("is_stack_module")
        ),
        key=lambda node: int(node.get("traversal_index", 0)),
    )


def select_graph(
    system: dict[str, Any], emitter: str, script_usage: str
) -> dict[str, Any] | None:
    for graph in system.get("graph", {}).get("graphs", []):
        if graph.get("emitter", "") == emitter and graph.get("script_usage") == script_usage:
            return graph
    return None


def stack_input(node: dict[str, Any], name: str) -> dict[str, Any] | None:
    normalized = name.replace(" ", "").lower()
    for item in node.get("stack_inputs", []):
        if str(item.get("name", "")).replace(" ", "").lower() == normalized:
            return item
    return None


def numeric_display(value: dict[str, Any] | None) -> float | None:
    if not value:
        return None
    candidates = [value.get("display_value"), value.get("value_text")]
    for candidate in candidates:
        if candidate is None:
            continue
        match = re.search(r"-?\d+(?:\.\d+)?", str(candidate))
        if match:
            return float(match.group(0))
    return None


def count_values(count: dict[str, Any] | None) -> tuple[dict[str, Any], dict[str, Any]]:
    if not count:
        raise CompileError("Spawn Burst has no readable Spawn Count input")
    mode = count.get("value_mode")
    if mode == "linked_parameter":
        parameter = count.get("linked_parameter")
        if not parameter:
            raise CompileError("Spawn Count is linked but has no parameter name")
        value = {"mode": "linked_parameter", "parameter": parameter}
        return value, dict(value)
    if mode == "local":
        value_text = count.get("value_text")
        if not value_text:
            value = numeric_display(count)
            if value is None:
                raise CompileError("Local Spawn Count has no importable value")
            value_text = f"(Value={int(value)})"
        value = {"mode": "local", "value_text": value_text}
        return value, dict(value)
    if mode == "dynamic_input":
        display = str(count.get("display_value", ""))
        match = RANDOM_INT_RE.search(display)
        if not match:
            raise CompileError(
                f"Unsupported dynamic Spawn Count; expected integer range, got {display!r}"
            )
        low, high = sorted((int(match.group(1)), int(match.group(2))))
        return (
            {"mode": "local", "value_text": f"(Value={low})"},
            {"mode": "local", "value_text": f"(Value={high})"},
        )
    raise CompileError(f"Unsupported Spawn Count value mode: {mode!r}")


def graph_selector(emitter: str, usage: str) -> dict[str, Any]:
    return {"scope": "emitter", "emitter": emitter, "script_usage": usage}


def burst_edit_for(system: dict[str, Any]) -> tuple[dict[str, Any] | None, list[str]]:
    graphs = system.get("graph", {}).get("graphs", [])
    function_nodes = [
        node
        for graph in graphs
        for node in graph.get("nodes", [])
        if node.get("kind") == "function_call" and node.get("is_stack_module")
    ]
    burst_nodes = [
        (graph, node)
        for graph in graphs
        for node in stack_modules(graph)
        if node.get("function_name") == "SpawnBurst_Instantaneous"
    ]
    blockers: list[str] = []
    for node in function_nodes:
        function_name = str(node.get("function_name", ""))
        function_path = str(node.get("function_script", ""))
        if function_name == "SpawnRate" or function_path.endswith("/SpawnRate.SpawnRate"):
            blockers.append("SpawnRate requires a per-controller age/rate scheduler")
        if function_name == "SpawnPerUnit" or function_path.endswith("/SpawnPerUnit.SpawnPerUnit"):
            blockers.append("SpawnPerUnit requires per-controller travel-distance state")
    if not burst_nodes:
        blockers.append("No immediate Spawn Burst scheduling layer")

    emitters: dict[str, list[tuple[dict[str, Any], dict[str, Any]]]] = {}
    for graph, node in burst_nodes:
        emitter = str(graph.get("emitter", ""))
        if not emitter:
            blockers.append("System-scoped Spawn Burst is not supported")
            continue
        spawn_time = numeric_display(stack_input(node, "Spawn Time"))
        if spawn_time is None or abs(spawn_time) > 1.0e-6:
            blockers.append(
                f"{emitter} has delayed Spawn Burst at {spawn_time if spawn_time is not None else 'unknown'}s"
            )
        try:
            count_values(stack_input(node, "Spawn Count"))
        except CompileError as exc:
            blockers.append(f"{emitter}: {exc}")
        emitters.setdefault(emitter, []).append((graph, node))

    blockers = sorted(set(blockers))
    if blockers:
        return None, blockers

    operations: list[dict[str, Any]] = [
        {
            "id": "add_enable_burst_fx",
            "op": "add_user_parameter",
            "name": "User.EnableBurstFx",
            "value_type": "bool",
            "default": False,
        },
        {
            "id": "add_subtype",
            "op": "add_user_parameter",
            "name": "User.SubType",
            "value_type": "int",
            "default": 0,
        },
    ]
    scheduler_guids: list[str] = []
    allowed_rapid_iteration_changes: list[str] = []

    system_update_graph = select_graph(system, "", "SystemUpdateScript")
    system_state_nodes = [
        module
        for module in stack_modules(system_update_graph or {})
        if module.get("function_name") == "SystemState"
    ]
    if len(system_state_nodes) == 1:
        system_state = system_state_nodes[0]
        loop_behavior = str(
            (stack_input(system_state, "Loop Behavior") or {}).get(
                "display_value", ""
            )
        ).lower()
        loop_duration = numeric_display(stack_input(system_state, "Loop Duration"))
        if (
            loop_behavior == "once"
            and loop_duration is not None
            and loop_duration < NDC_MIN_LISTENER_DURATION
        ):
            system_state_guid = str(system_state.get("node_guid", ""))
            system_state_reference = system_state.get("reference") or {
                "node_guid": system_state_guid,
                "graph": {"scope": "system", "script_usage": "SystemUpdateScript"},
            }
            operations.append(
                {
                    "id": "system_state_ndc_listener_window",
                    "op": "set_stack_input",
                    "node": system_state_reference,
                    "input": "Loop Duration",
                    "value": {
                        "mode": "local",
                        "value_text": f"(Value={NDC_MIN_LISTENER_DURATION:.6f})",
                    },
                    "adapter_reason": (
                        "Keep the NDC listener alive through its first consumable frame; "
                        "particle visual/lifetime modules remain unchanged"
                    ),
                }
            )
            scheduler_guids.append(system_state_guid)
            allowed_rapid_iteration_changes.append(
                SYSTEM_STATE_DURATION_PARAMETER
            )

    for emitter in sorted(emitters):
        emitter_slug = re.sub(r"[^A-Za-z0-9]+", "_", emitter).strip("_").lower()
        emitter_spawn = graph_selector(emitter, "EmitterSpawnScript")
        emitter_update = graph_selector(emitter, "EmitterUpdateScript")
        particle_spawn = graph_selector(emitter, "ParticleSpawnScript")

        particle_graph = select_graph(system, emitter, "ParticleSpawnScript")
        if not particle_graph:
            raise CompileError(f"{emitter}: missing ParticleSpawnScript graph")
        particle_modules = stack_modules(particle_graph)
        initialize_index = next(
            (
                index
                for index, module in enumerate(particle_modules)
                if module.get("function_name") == "InitializeParticle"
            ),
            -1,
        )
        read_index = initialize_index + 1

        init_operation_id = f"{emitter_slug}_burst_init"
        operations.append(
            {
                "id": init_operation_id,
                "op": "insert_module",
                "graph": emitter_spawn,
                "module_asset": INIT_MODULE,
                "stack_index": 0,
            }
        )
        # UE 5.8's stack insertion can create the DI override without copying
        # the module's object-backed Channel default. Keep the authored module
        # default restoration in the MCP, but also declare this required
        # protocol binding explicitly so the manifest is deterministic and
        # old generated assets can be repaired in place.
        operations.append(
            {
                "op": "set_stack_input",
                "node": {"operation": init_operation_id},
                "input": "Data Channel",
                "value": {
                    "mode": "data_interface",
                    "properties": {"Channel": BURST_DATA_CHANNEL},
                },
            }
        )

        update_graph = select_graph(system, emitter, "EmitterUpdateScript")
        if not update_graph:
            raise CompileError(f"{emitter}: missing EmitterUpdateScript graph")
        update_modules = stack_modules(update_graph)
        inserted_before = 0
        for ordinal, (_graph, burst_node) in enumerate(emitters[emitter]):
            source_index = next(
                (
                    index
                    for index, module in enumerate(update_modules)
                    if module.get("node_guid") == burst_node.get("node_guid")
                ),
                -1,
            )
            if source_index < 0:
                raise CompileError(f"{emitter}: Spawn Burst stack index could not be resolved")
            operation_id = f"{emitter_slug}_burst_spawn_{ordinal}"
            operations.append(
                {
                    "id": operation_id,
                    "op": "insert_module",
                    "graph": emitter_update,
                    "module_asset": SPAWN_MODULE,
                    "stack_index": source_index + inserted_before,
                }
            )
            minimum, maximum = count_values(stack_input(burst_node, "Spawn Count"))
            operations.append(
                {
                    "op": "set_stack_inputs",
                    "node": {"operation": operation_id},
                    "inputs": [
                        {
                            "input": "Spawn Enabled",
                            "value": {
                                "mode": "linked_parameter",
                                "parameter": "User.EnableBurstFx",
                            },
                        },
                        {"input": "Min Count", "value": minimum},
                        {"input": "Max Count", "value": maximum},
                        {
                            "input": "SubType",
                            "value": {
                                "mode": "linked_parameter",
                                "parameter": "User.SubType",
                            },
                        },
                        {
                            "input": "StyleType",
                            "value": {"mode": "local", "value_text": "(Value=0)"},
                        },
                    ],
                }
            )
            scheduler_guid = str(burst_node.get("node_guid", ""))
            scheduler_guids.append(scheduler_guid)
            operations.append(
                {
                    "op": "set_module_enabled",
                    "node": {
                        "node_guid": scheduler_guid,
                        "graph": emitter_update,
                    },
                    "enabled": False,
                }
            )
            inserted_before += 1

        operations.extend(
            [
                {
                    "id": f"{emitter_slug}_burst_read",
                    "op": "insert_module",
                    "graph": particle_spawn,
                    "module_asset": READ_MODULE,
                    "stack_index": read_index,
                },
                {
                    "id": f"{emitter_slug}_burst_params",
                    "op": "insert_module",
                    "graph": particle_spawn,
                    "module_asset": SET_PARAMS_MODULE,
                    "stack_index": read_index + 1,
                },
            ]
        )

    scheduler_guids = sorted(set(filter(None, scheduler_guids)))
    return (
        {
            "operations": operations,
            "validation": {
                "require_ready_to_run": True,
                "require_no_warnings": False,
                "allowed_scheduler_node_guids": scheduler_guids,
                "allowed_module_state_change_node_guids": scheduler_guids,
                "allowed_rapid_iteration_parameter_changes": sorted(
                    set(allowed_rapid_iteration_changes)
                ),
                "allowed_removed_edges": [],
            },
        },
        [],
    )


def build_plans(source_ir: dict[str, Any]) -> list[AssetPlan]:
    provisional: list[tuple[dict[str, Any], dict[str, Any] | None, list[str]]] = []
    for system in sorted(source_ir.get("systems", []), key=lambda item: item.get("path", "")):
        edit, blockers = burst_edit_for(system)
        provisional.append((system, edit, blockers))

    native_index = 0
    plans: list[AssetPlan] = []
    for system, edit, blockers in provisional:
        source = str(system["path"])
        name = object_name(source)
        category = category_from_path(source)
        exact_name = f"NS_MB_{name.removeprefix('NS_')}_Exact"
        exact_target = (
            f"/Game/ArmyVFX/MassBattleConverted/ExactMapped/{category}/"
            f"{exact_name}.{exact_name}"
        )
        if edit is None:
            plans.append(
                AssetPlan(
                    source=source,
                    name=name,
                    category=category,
                    exact_target=exact_target,
                    mode="exact_component_pool",
                    reasons=tuple(blockers),
                )
            )
            continue

        native_name = f"NS_MB_{name.removeprefix('NS_')}_NativeBurst"
        native_target = (
            f"/Game/ArmyVFX/MassBattleConverted/NativeBurst/{category}/"
            f"{native_name}.{native_name}"
        )
        native_reasons = [
            "Immediate Spawn Burst is exactly expressible by the current NDC contract"
        ]
        if any(
            operation.get("id") == "system_state_ndc_listener_window"
            for operation in edit.get("operations", [])
        ):
            native_reasons.append(
                "Native scheduler only: SystemState listener window raised to 0.5s; visual and particle lifetime graphs remain unchanged"
            )
        plans.append(
            AssetPlan(
                source=source,
                name=name,
                category=category,
                exact_target=exact_target,
                mode="native_burst",
                reasons=tuple(native_reasons),
                native_target=native_target,
                subtype=NATIVE_SUBTYPE_BASE + native_index,
                edit=edit,
            )
        )
        native_index += 1
    return plans


def manifest_record(plan: AssetPlan) -> dict[str, Any]:
    return {
        "source": plan.source,
        "source_name": plan.name,
        "category": plan.category,
        "mode": plan.mode,
        "reasons": list(plan.reasons),
        "exact_target": plan.exact_target,
        "native_target": plan.native_target,
        "subtype": plan.subtype,
        "native_edit": plan.edit,
    }


def compact_result(result: dict[str, Any]) -> dict[str, Any]:
    compile_state = result.get("compile_state") or result.get("target_compile_state") or {}
    preservation = result.get("preservation") or {}
    return {
        "success": result.get("success", False),
        "saved": result.get("saved"),
        "asset_path": result.get("asset_path"),
        "error": result.get("error"),
        "compile_status": compile_state.get("aggregate_status"),
        "ready_to_run": compile_state.get("ready_to_run"),
        "has_errors": compile_state.get("has_errors"),
        "all_preserved": preservation.get("all_preserved"),
        "existing_rapid_iteration_values_preserved": preservation.get(
            "existing_rapid_iteration_values_preserved"
        ),
        "missing_rapid_iteration_values": preservation.get(
            "missing_rapid_iteration_values", []
        ),
        "source_preserved": result.get("source_preserved"),
        "comparison_passed": result.get("comparison_passed"),
        "removed_edges": preservation.get("removed_edges", []),
        "unexpected_removed_edges": preservation.get("unexpected_removed_edges", []),
        "failed_operation_index": result.get("failed_operation_index"),
    }


def is_zero_rapid_iteration_compile_normalization(result: dict[str, Any]) -> bool:
    """Accept only UE's first-compile removal of all-zero RI cache entries.

    This is intentionally narrower than a general Rapid Iteration allowlist. All
    authored semantic categories must still pass, and every missing value must be
    a non-empty hexadecimal byte string containing only zeroes.
    """

    preservation = result.get("preservation") or {}
    required_true = (
        "system_timing_and_determinism_preserved",
        "emitters_preserved",
        "emitter_settings_preserved",
        "renderers_and_materials_preserved",
        "renderer_identities_and_materials_preserved",
        "renderer_serialized_properties_preserved",
        "sim_targets_preserved",
        "event_handlers_preserved",
        "existing_user_parameters_preserved",
        "existing_module_nodes_preserved",
        "existing_graph_nodes_preserved",
        "existing_graph_pin_defaults_preserved",
    )
    if not all(preservation.get(field) is True for field in required_true):
        return False
    if preservation.get("existing_rapid_iteration_values_preserved") is not False:
        return False
    missing = preservation.get("missing_rapid_iteration_values") or []
    if not missing:
        return False
    for entry in missing:
        value = str(entry).rsplit("|", 1)[-1]
        if not value or len(value) % 2 or any(character != "0" for character in value):
            return False
    return True


def is_rapid_iteration_only_compile_normalization(result: dict[str, Any]) -> bool:
    """Return true when first compile changed only derived RI cache state.

    The exact-clone gate is run before compilation. This post-compile check then
    requires every non-RI preservation fingerprint to remain byte-identical;
    therefore user parameters, graph topology/defaults, modules, renderers,
    events, settings, and simulation targets cannot hide behind this exception.
    """

    preservation = result.get("preservation") or {}
    before = preservation.get("before_fingerprints") or {}
    after = preservation.get("after_fingerprints") or {}
    if not before or set(before) != set(after):
        return False
    if before.get("rapid_iteration_parameters") == after.get("rapid_iteration_parameters"):
        return False
    for key in before:
        if key == "rapid_iteration_parameters":
            continue
        if before.get(key) != after.get(key):
            return False
    return True


async def asset_exists(bridge: UnrealBridge, target: str) -> bool:
    result = await bridge.command(
        "MCP_NiagaraReadSummary",
        {"SystemPath": target, "OptionsJson": '{"include_modules":false}'},
    )
    return bool(result.get("success"))


async def duplicate_unsaved(bridge: UnrealBridge, source: str, target: str) -> dict[str, Any]:
    package_path, object_part = target.rsplit("/", 1)
    name = object_part.split(".", 1)[0]
    return await bridge.command(
        "MCP_EffectDuplicateAsset",
        {
            "SourceAssetPath": source,
            "NewAssetName": name,
            "PackagePath": package_path,
            "bSaveAssets": False,
        },
    )


async def compare(
    bridge: UnrealBridge,
    source: str,
    target: str,
    options: dict[str, Any],
) -> dict[str, Any]:
    return await bridge.command(
        "MCP_NiagaraCompareSystems",
        {
            "SourceSystemPath": source,
            "TargetSystemPath": target,
            "OptionsJson": json_arg(options),
        },
    )


async def graph_edit(
    bridge: UnrealBridge,
    target: str,
    edit: dict[str, Any],
    save: bool,
) -> dict[str, Any]:
    return await bridge.command(
        "MCP_NiagaraApplyGraphEdit",
        {
            "SystemPath": target,
            "EditJson": json_arg(edit),
            "bSaveAssets": save,
        },
    )


async def read_graph(bridge: UnrealBridge, target: str) -> dict[str, Any]:
    return await bridge.command(
        "MCP_NiagaraReadGraph",
        {
            "SystemPath": target,
            "SelectorJson": json_arg(
                {"include_compile_state": True, "include_stack_inputs": True}
            ),
        },
    )


def native_init_data_channel_bindings(graph: dict[str, Any]) -> list[dict[str, Any]]:
    bindings: list[dict[str, Any]] = []
    expected_package = BURST_DATA_CHANNEL.split(".", 1)[0].lower()
    for script_graph in graph.get("graphs", []):
        selector = {
            "scope": script_graph.get("scope", "emitter"),
            "emitter": script_graph.get("emitter", ""),
            "script_usage": script_graph.get("script_usage", ""),
            "usage_id": script_graph.get("usage_id", ""),
        }
        for node in script_graph.get("nodes", []):
            if node.get("function_name") != "NM_BurstFxInitFromNDC":
                continue
            channel_value = ""
            data_channel_input = stack_input(node, "Data Channel")
            if data_channel_input:
                for prop in data_channel_input.get("data_interface_properties", []):
                    if str(prop.get("name", "")).lower() == "channel":
                        channel_value = str(prop.get("value_text", ""))
                        break
            bindings.append(
                {
                    "node_guid": node.get("node_guid"),
                    "reference": node.get("reference"),
                    "graph": selector,
                    "channel": channel_value,
                    "valid": expected_package in channel_value.lower(),
                }
            )
    return bindings


async def ensure_native_data_channel_bindings(
    bridge: UnrealBridge,
    plan: AssetPlan,
) -> dict[str, Any]:
    if not plan.native_target or not plan.edit:
        return {"success": False, "status": "not_native"}

    expected_count = sum(
        operation.get("op") == "insert_module"
        and operation.get("module_asset") == INIT_MODULE
        for operation in plan.edit.get("operations", [])
    )
    before_graph = await read_graph(bridge, plan.native_target)
    if not before_graph.get("success"):
        return {
            "success": False,
            "status": "readback_failed",
            "error": before_graph.get("error"),
        }
    before = native_init_data_channel_bindings(before_graph)
    if len(before) != expected_count:
        return {
            "success": False,
            "status": "init_module_count_mismatch",
            "expected_count": expected_count,
            "actual_count": len(before),
            "bindings": before,
        }

    invalid = [binding for binding in before if not binding["valid"]]
    repair_result: dict[str, Any] | None = None
    save_barriers: list[dict[str, Any]] = []
    if invalid:
        operations: list[dict[str, Any]] = []
        for binding in invalid:
            node_reference = binding.get("reference") or {
                "node_guid": binding["node_guid"],
                "graph": binding["graph"],
            }
            operations.append(
                {
                    "op": "set_stack_input",
                    "node": node_reference,
                    "input": "Data Channel",
                    "value": {
                        "mode": "data_interface",
                        "properties": {"Channel": BURST_DATA_CHANNEL},
                    },
                }
            )
        repair_result = await graph_edit(
            bridge,
            plan.native_target,
            {
                "operations": operations,
                "validation": {
                    "require_ready_to_run": True,
                    "require_no_warnings": False,
                    "allowed_removed_edges": [],
                },
            },
            True,
        )

        # Setting a DI value replaces Niagara's generated override node. The
        # first compile may therefore report the old generated edge and stale
        # Rapid Iteration cache as removed even though the requested binding is
        # already present in memory. Verify the semantic result first, then use
        # a no-op preservation barrier to capture and save the normalized graph.
        intermediate_graph = await read_graph(bridge, plan.native_target)
        intermediate = native_init_data_channel_bindings(intermediate_graph)
        intermediate_valid = (
            bool(intermediate_graph.get("success"))
            and len(intermediate) == expected_count
            and all(binding["valid"] for binding in intermediate)
        )
        if not intermediate_valid:
            return {
                "success": False,
                "status": "repair_readback_failed",
                "before": before,
                "intermediate": intermediate,
                "repair": compact_result(repair_result),
            }

    # Always pass a no-op compile/save barrier. Besides proving the current
    # graph is losslessly stable, this persists a valid in-memory repair left
    # by an earlier interrupted run.
    saved = False
    for _attempt in range(2):
        barrier = await graph_edit(
            bridge,
            plan.native_target,
            {
                "operations": [],
                "validation": {
                    "require_ready_to_run": True,
                    "require_no_warnings": False,
                    "allowed_removed_edges": [],
                },
            },
            True,
        )
        save_barriers.append(compact_result(barrier))
        if barrier.get("success") and barrier.get("saved"):
            saved = True
            break
    if not saved:
        return {
            "success": False,
            "status": "save_barrier_failed",
            "before": before,
            "repair": compact_result(repair_result or {}),
            "save_barriers": save_barriers,
        }

    after_graph = await read_graph(bridge, plan.native_target)
    after = native_init_data_channel_bindings(after_graph)
    success = (
        bool(after_graph.get("success"))
        and len(after) == expected_count
        and all(binding["valid"] for binding in after)
    )
    return {
        "success": success,
        "status": (
            "repaired_and_verified"
            if invalid and success
            else "verified"
            if success
            else "post_repair_readback_failed"
        ),
        "expected_channel": BURST_DATA_CHANNEL,
        "before": before,
        "after": after,
        "repair": compact_result(repair_result or {}),
        "save_barriers": save_barriers,
    }


def native_listener_window_operation(plan: AssetPlan) -> dict[str, Any] | None:
    if not plan.edit:
        return None
    return next(
        (
            operation
            for operation in plan.edit.get("operations", [])
            if operation.get("id") == "system_state_ndc_listener_window"
        ),
        None,
    )


def stack_input_by_node_guid(
    graph: dict[str, Any], node_guid: str, input_name: str
) -> dict[str, Any] | None:
    for script_graph in graph.get("graphs", []):
        for node in script_graph.get("nodes", []):
            if str(node.get("node_guid", "")).lower() == node_guid.lower():
                return stack_input(node, input_name)
    return None


async def ensure_native_listener_window(
    bridge: UnrealBridge,
    plan: AssetPlan,
) -> dict[str, Any]:
    operation = native_listener_window_operation(plan)
    if not operation:
        return {"success": True, "status": "not_required"}
    if not plan.native_target or not plan.edit:
        return {"success": False, "status": "not_native"}

    node_guid = str((operation.get("node") or {}).get("node_guid", ""))
    expected = numeric_display(operation.get("value"))
    if not node_guid or expected is None:
        return {
            "success": False,
            "status": "invalid_listener_window_operation",
        }

    before_graph = await read_graph(bridge, plan.native_target)
    before_input = stack_input_by_node_guid(
        before_graph, node_guid, str(operation.get("input", ""))
    )
    before_value = numeric_display(before_input)
    changed = before_value is None or abs(before_value - expected) > 1.0e-6
    apply_result: dict[str, Any] | None = None
    validation = {
        "require_ready_to_run": True,
        "require_no_warnings": False,
        "allowed_removed_edges": [],
        "allowed_rapid_iteration_parameter_changes": [
            SYSTEM_STATE_DURATION_PARAMETER
        ],
    }
    if changed:
        apply_result = await graph_edit(
            bridge,
            plan.native_target,
            {"operations": [operation], "validation": validation},
            True,
        )

    intermediate_graph = await read_graph(bridge, plan.native_target)
    intermediate_input = stack_input_by_node_guid(
        intermediate_graph, node_guid, str(operation.get("input", ""))
    )
    intermediate_value = numeric_display(intermediate_input)
    if intermediate_value is None or abs(intermediate_value - expected) > 1.0e-6:
        return {
            "success": False,
            "status": "listener_window_readback_failed",
            "before": before_value,
            "expected": expected,
            "actual": intermediate_value,
            "apply": compact_result(apply_result or {}),
        }

    barriers: list[dict[str, Any]] = []
    saved = bool(apply_result and apply_result.get("success") and apply_result.get("saved"))
    if not saved:
        for _attempt in range(2):
            barrier = await graph_edit(
                bridge,
                plan.native_target,
                {"operations": [], "validation": validation},
                True,
            )
            barriers.append(compact_result(barrier))
            if barrier.get("success") and barrier.get("saved"):
                saved = True
                break
    if not saved and changed:
        return {
            "success": False,
            "status": "listener_window_save_failed",
            "before": before_value,
            "expected": expected,
            "apply": compact_result(apply_result or {}),
            "save_barriers": barriers,
        }

    after_graph = await read_graph(bridge, plan.native_target)
    after_value = numeric_display(
        stack_input_by_node_guid(
            after_graph, node_guid, str(operation.get("input", ""))
        )
    )
    success = after_value is not None and abs(after_value - expected) <= 1.0e-6
    return {
        "success": success,
        "status": (
            "adjusted_and_verified" if changed and success else "verified"
        ),
        "parameter": SYSTEM_STATE_DURATION_PARAMETER,
        "before": before_value,
        "expected": expected,
        "after": after_value,
        "apply": compact_result(apply_result or {}),
        "save_barriers": barriers,
    }


async def discard_unsaved(bridge: UnrealBridge, target: str) -> dict[str, Any]:
    return await bridge.command(
        "MCP_EffectDiscardUnsavedDuplicate",
        {"AssetPath": target},
    )


async def ensure_exact_mapping(bridge: UnrealBridge, plan: AssetPlan) -> dict[str, Any]:
    target = plan.exact_target
    if await asset_exists(bridge, target):
        verification = await compare(
            bridge,
            plan.source,
            target,
            {"mode": "exact", "require_ready_to_run": False},
        )
        normalized = (
            is_zero_rapid_iteration_compile_normalization(verification)
            or is_rapid_iteration_only_compile_normalization(verification)
        )
        accepted = bool(verification.get("success")) or normalized
        return {
            "status": (
                "verified_existing"
                if verification.get("success")
                else "verified_existing_compile_normalized"
                if normalized
                else "invalid_existing"
            ),
            "target": target,
            "verification": compact_result(verification),
            "compile_normalized_zero_rapid_iteration": normalized,
            "success": accepted,
        }

    duplicate = await duplicate_unsaved(bridge, plan.source, target)
    if not duplicate.get("success"):
        return {"success": False, "status": "duplicate_failed", "result": compact_result(duplicate)}

    exact = await compare(
        bridge,
        plan.source,
        target,
        {"mode": "exact", "require_ready_to_run": False},
    )
    if not exact.get("success"):
        cleanup = await discard_unsaved(bridge, target)
        return {
            "success": False,
            "status": "exact_clone_gate_failed",
            "verification": compact_result(exact),
            "cleanup": compact_result(cleanup),
        }

    save_result = await graph_edit(
        bridge,
        target,
        {"operations": [], "validation": {"require_ready_to_run": True}},
        True,
    )
    compile_normalized = is_zero_rapid_iteration_compile_normalization(save_result)
    normalization_result: dict[str, Any] | None = None
    if compile_normalized:
        # The first compile already completed and normalized derived RI data. A
        # second no-op barrier captures that compiled state as both before/after,
        # so saving remains guarded by the full preservation gate.
        normalization_result = save_result
        save_result = await graph_edit(
            bridge,
            target,
            {"operations": [], "validation": {"require_ready_to_run": True}},
            True,
        )
    if not save_result.get("success") or not save_result.get("saved"):
        cleanup = await discard_unsaved(bridge, target)
        return {
            "success": False,
            "status": "compile_or_save_failed",
            "save": compact_result(save_result),
            "compile_normalization": compact_result(normalization_result or {}),
            "cleanup": compact_result(cleanup),
        }

    verification = await compare(
        bridge,
        plan.source,
        target,
        {"mode": "exact", "require_ready_to_run": False},
    )
    post_normalized = (
        is_zero_rapid_iteration_compile_normalization(verification)
        or is_rapid_iteration_only_compile_normalization(verification)
    )
    accepted = bool(verification.get("success")) or post_normalized
    return {
        "success": accepted,
        "status": (
            "created"
            if verification.get("success")
            else "created_compile_normalized"
            if post_normalized
            else "post_save_verification_failed"
        ),
        "target": target,
        "save": compact_result(save_result),
        "compile_normalization": compact_result(normalization_result or {}),
        "compile_normalized_zero_rapid_iteration": compile_normalized or post_normalized,
        "verification": compact_result(verification),
    }


def validation_with_edges(edit: dict[str, Any], edges: Iterable[str], probe: bool) -> dict[str, Any]:
    result = json.loads(json.dumps(edit))
    validation = result.setdefault("validation", {})
    if probe:
        validation.pop("allowed_removed_edges", None)
        validation["allow_removed_edges"] = True
    else:
        validation.pop("allow_removed_edges", None)
        validation["allowed_removed_edges"] = sorted(set(edges))
    return result


async def probe_native_edges(
    bridge: UnrealBridge,
    plan: AssetPlan,
) -> tuple[list[str], dict[str, Any]]:
    if not plan.native_target or not plan.edit:
        raise CompileError(f"{plan.name}: native plan is incomplete")
    native_name = object_name(plan.native_target)
    # A discarded UObject can remain discoverable by LoadObject until the next
    # editor GC even though its package was removed and never saved. Use a unique
    # probe name so resumable compiler runs never collide with that orphan.
    scratch_name = f"{native_name}__MCPProbe_{uuid.uuid4().hex[:8]}"
    scratch = (
        f"/Game/ArmyVFX/MassBattleConverted/_Scratch/{scratch_name}.{scratch_name}"
    )
    await discard_unsaved(bridge, scratch)
    if await asset_exists(bridge, scratch):
        raise CompileError(f"Scratch asset already exists on disk and will not be overwritten: {scratch}")

    baseline = plan.exact_target
    duplicate = await duplicate_unsaved(bridge, baseline, scratch)
    if not duplicate.get("success"):
        raise CompileError(f"{plan.name}: scratch duplicate failed: {duplicate.get('error', duplicate)}")
    try:
        exact = await compare(
            bridge,
            baseline,
            scratch,
            {"mode": "exact", "require_ready_to_run": False},
        )
        if not exact.get("success"):
            raise CompileError(f"{plan.name}: scratch duplicate is not exact")
        probe_edit = validation_with_edges(plan.edit, (), probe=True)
        applied = await graph_edit(bridge, scratch, probe_edit, False)
        if not applied.get("success"):
            raise CompileError(
                f"{plan.name}: scratch native edit failed: {applied.get('error', applied)}"
            )
        preservation = applied.get("preservation", {})
        edges = sorted(set(preservation.get("removed_edges", [])))
        options = dict(probe_edit.get("validation", {}))
        options.pop("allow_removed_edges", None)
        options["mode"] = "translation"
        options["require_ready_to_run"] = False
        options["allowed_removed_edges"] = edges
        verified = await compare(bridge, baseline, scratch, options)
        if not verified.get("source_preserved") or not verified.get("preservation", {}).get(
            "removed_edge_policy_passed"
        ):
            raise CompileError(f"{plan.name}: scratch translation did not preserve source semantics")
        return edges, {
            "apply": compact_result(applied),
            "verification": compact_result(verified),
        }
    finally:
        await discard_unsaved(bridge, scratch)


async def ensure_native_mapping(bridge: UnrealBridge, plan: AssetPlan) -> dict[str, Any]:
    if not plan.native_target or not plan.edit:
        return {"success": False, "status": "not_native"}

    edges, probe = await probe_native_edges(bridge, plan)
    validation_options = dict(plan.edit.get("validation", {}))
    validation_options["mode"] = "translation"
    validation_options["require_ready_to_run"] = False
    validation_options["allowed_removed_edges"] = edges
    baseline = plan.exact_target

    if await asset_exists(bridge, plan.native_target):
        listener_window = await ensure_native_listener_window(bridge, plan)
        if not listener_window.get("success"):
            return {
                "success": False,
                "status": "invalid_listener_window",
                "target": plan.native_target,
                "subtype": plan.subtype,
                "allowed_removed_edges": edges,
                "probe": probe,
                "listener_window": listener_window,
            }
        data_channel = await ensure_native_data_channel_bindings(bridge, plan)
        if not data_channel.get("success"):
            return {
                "success": False,
                "status": "invalid_data_channel_binding",
                "target": plan.native_target,
                "subtype": plan.subtype,
                "allowed_removed_edges": edges,
                "probe": probe,
                "listener_window": listener_window,
                "data_channel": data_channel,
            }
        verification = await compare(
            bridge,
            baseline,
            plan.native_target,
            validation_options,
        )
        return {
            "success": bool(verification.get("success")),
            "status": "verified_existing" if verification.get("success") else "invalid_existing",
            "target": plan.native_target,
            "subtype": plan.subtype,
            "allowed_removed_edges": edges,
            "probe": probe,
            "listener_window": listener_window,
            "data_channel": data_channel,
            "verification": compact_result(verification),
        }

    duplicate = await duplicate_unsaved(bridge, baseline, plan.native_target)
    if not duplicate.get("success"):
        return {"success": False, "status": "duplicate_failed", "result": compact_result(duplicate)}
    exact = await compare(
        bridge,
        baseline,
        plan.native_target,
        {"mode": "exact", "require_ready_to_run": False},
    )
    if not exact.get("success"):
        cleanup = await discard_unsaved(bridge, plan.native_target)
        return {
            "success": False,
            "status": "exact_clone_gate_failed",
            "verification": compact_result(exact),
            "cleanup": compact_result(cleanup),
        }

    real_edit = validation_with_edges(plan.edit, edges, probe=False)
    applied = await graph_edit(bridge, plan.native_target, real_edit, True)
    if not applied.get("success") or not applied.get("saved"):
        cleanup = await discard_unsaved(bridge, plan.native_target)
        return {
            "success": False,
            "status": "native_edit_or_save_failed",
            "apply": compact_result(applied),
            "cleanup": compact_result(cleanup),
            "allowed_removed_edges": edges,
        }

    listener_window = await ensure_native_listener_window(bridge, plan)
    if not listener_window.get("success"):
        return {
            "success": False,
            "status": "invalid_listener_window",
            "target": plan.native_target,
            "subtype": plan.subtype,
            "allowed_removed_edges": edges,
            "probe": probe,
            "apply": compact_result(applied),
            "listener_window": listener_window,
        }

    data_channel = await ensure_native_data_channel_bindings(bridge, plan)
    if not data_channel.get("success"):
        return {
            "success": False,
            "status": "invalid_data_channel_binding",
            "target": plan.native_target,
            "subtype": plan.subtype,
            "allowed_removed_edges": edges,
            "probe": probe,
            "apply": compact_result(applied),
            "listener_window": listener_window,
            "data_channel": data_channel,
        }

    verification = await compare(
        bridge,
        baseline,
        plan.native_target,
        validation_options,
    )
    return {
        "success": bool(verification.get("success")),
        "status": "created" if verification.get("success") else "post_save_verification_failed",
        "target": plan.native_target,
        "subtype": plan.subtype,
        "allowed_removed_edges": edges,
        "probe": probe,
        "apply": compact_result(applied),
        "listener_window": listener_window,
        "data_channel": data_channel,
        "verification": compact_result(verification),
    }


async def run(args: argparse.Namespace) -> int:
    with args.source_ir.open("r", encoding="utf-8") as handle:
        source_ir = json.load(handle)
    plans = build_plans(source_ir)
    if args.max_items:
        plans = plans[: args.max_items]

    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "massbattle.armyvfx.mapping_manifest.v1",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "source_ir": str(args.source_ir),
        "source_count": len(plans),
        "native_burst_count": sum(plan.mode == "native_burst" for plan in plans),
        "exact_component_pool_count": sum(
            plan.mode == "exact_component_pool" for plan in plans
        ),
        "native_subtype_range": [
            min((plan.subtype for plan in plans if plan.subtype is not None), default=None),
            max((plan.subtype for plan in plans if plan.subtype is not None), default=None),
        ],
        "assets": [manifest_record(plan) for plan in plans],
    }
    manifest_path = args.output_dir / "ArmyVFX_MassBattle_MappingManifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    print(
        f"ArmyVFX plan: {len(plans)} systems, "
        f"{manifest['native_burst_count']} native burst, "
        f"{manifest['exact_component_pool_count']} exact pool",
        flush=True,
    )
    for plan in plans:
        suffix = f" subtype={plan.subtype}" if plan.subtype is not None else ""
        print(f"  {plan.mode:20s} {plan.name}{suffix}", flush=True)
        if args.verbose and plan.mode != "native_burst":
            for reason in plan.reasons:
                print(f"    - {reason}", flush=True)

    if args.action == "plan":
        print(f"Manifest written: {manifest_path}", flush=True)
        return 0

    bridge = UnrealBridge(args.host, args.port)
    ping = await bridge.command("ping")
    if not ping.get("success"):
        raise CompileError(f"MassBattle bridge ping failed: {ping}")

    records: list[dict[str, Any]] = []
    failed = 0
    for index, plan in enumerate(plans, start=1):
        print(f"[{index}/{len(plans)}] {plan.name}: exact mapping", flush=True)
        exact = await ensure_exact_mapping(bridge, plan)
        record: dict[str, Any] = {
            "source": plan.source,
            "mode": plan.mode,
            "exact": exact,
        }
        if not exact.get("success"):
            failed += 1
            records.append(record)
            print(f"  FAILED exact: {exact.get('status')}", flush=True)
            if args.stop_on_error:
                break
            continue
        print(f"  exact: {exact.get('status')}", flush=True)

        if plan.mode == "native_burst":
            print(f"  native burst probe/apply (SubType {plan.subtype})", flush=True)
            try:
                native = await ensure_native_mapping(bridge, plan)
            except CompileError as exc:
                native = {"success": False, "status": "exception", "error": str(exc)}
            record["native"] = native
            if not native.get("success"):
                failed += 1
                print(f"  FAILED native: {native.get('status')} {native.get('error', '')}", flush=True)
                records.append(record)
                if args.stop_on_error:
                    break
                continue
            print(
                f"  native: {native.get('status')}, "
                f"declared edges={len(native.get('allowed_removed_edges', []))}",
                flush=True,
            )
        records.append(record)

    report = {
        "schema": "massbattle.armyvfx.mapping_report.v1",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "manifest": str(manifest_path),
        "requested_count": len(plans),
        "processed_count": len(records),
        "failed_count": failed,
        "success": failed == 0 and len(records) == len(plans),
        "records": records,
    }
    report_path = args.output_dir / "ArmyVFX_MassBattle_ConversionReport.json"
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"Report written: {report_path}", flush=True)
    print(
        f"Result: processed={len(records)}/{len(plans)}, failed={failed}",
        flush=True,
    )
    return 0 if report["success"] else 2


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action", choices=("plan", "apply"), default="plan")
    parser.add_argument("--source-ir", type=Path, default=DEFAULT_SOURCE_IR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--host", default=BRIDGE_HOST)
    parser.add_argument("--port", type=int, default=BRIDGE_PORT)
    parser.add_argument("--max-items", type=int, default=0)
    parser.add_argument("--stop-on-error", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        return asyncio.run(run(args))
    except (CompileError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
