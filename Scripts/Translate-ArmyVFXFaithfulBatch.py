#!/usr/bin/env python3
"""Complete and validate the 27 ArmyVFX -> MassBattle Batch FX mappings.

This is an acceptance compiler, not a template copier.  Every target keeps the
source emitters/renderers/materials and replaces only the per-instance scheduler
with the MassBattle Burst NDC and/or Attached-array controller.

The existing 24 WinyunqRelease targets are repaired and revalidated.  The three
missing SpawnPerUnit targets are generated from exact source duplicates.  Their
fixed-rate lowering is valid only for the explicit same-speed-per-SubType
runtime invariant recorded in the manifest and enforced by the Demo.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


SCRIPT_PATH = Path(__file__).resolve()
PLUGIN_ROOT = SCRIPT_PATH.parent.parent
PROJECT_ROOT = PLUGIN_ROOT.parent.parent
SOURCE_IR = (
    PROJECT_ROOT
    / "Saved"
    / "MassBattleEditorMCP"
    / "Validation"
    / "ArmyVFX_Niagara_SourceIR.json"
)
OUTPUT_DIR = PROJECT_ROOT / "Saved" / "MassBattleEditorMCP" / "Validation"

HOST = os.environ.get("MASSBATTLE_UNREAL_HOST", "127.0.0.1")
PORT = int(os.environ.get("MASSBATTLE_UNREAL_PORT", "55258"))
CONNECT_TIMEOUT = float(os.environ.get("MASSBATTLE_CONNECT_TIMEOUT", "10"))
COMMAND_TIMEOUT = float(os.environ.get("MASSBATTLE_COMMAND_TIMEOUT", "1200"))

EFFECT_ROOT = "/Game/ArmyVFX/WinyunqRelease/Effects"
RENDERER_ROOT = "/Game/ArmyVFX/WinyunqRelease/Renderers"
ATTACHED_TEMPLATE = (
    "/MassBattle/Demo/Agent/FighterJet/JetTrail/"
    "NS_FxRenderer_JetTrail.NS_FxRenderer_JetTrail"
)
ATTACHED_TEMPLATE_EMITTER = "Core"
SPAWN_FROM_OTHER = (
    "/Niagara/Modules/AttributeReader/"
    "SpawnParticlesFromOtherEmitter.SpawnParticlesFromOtherEmitter"
)
MB_SAMPLE_FROM_OTHER = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NM_SampleParticlesFromOtherEmitter.NM_SampleParticlesFromOtherEmitter"
)
NDC_BURST = (
    "/MassBattle/Core/FxRenderer/NS_Modules/"
    "NDC_BurstFx.NDC_BurstFx"
)
RENDERER_TEMPLATE_CLASS = (
    "/MassBattle/Core/FxRenderer/"
    "BP_FxRendererTemplate.BP_FxRendererTemplate_C"
)

SCHEDULER_FUNCTIONS = {
    "SpawnBurst_Instantaneous",
    "SpawnRate",
    "SpawnPerUnit",
}

RENDERER_NAMES = {
    "NS_Expl_Tank_1": "BP_FxRenderer_ExplTank_Batch",
    "NS_Expl_Tank_Tower_1": "BP_FxRenderer_ExplTankTower_Batch",
    "NS_Fire_Tank_1": "BP_FxRenderer_FireTank_Batch",
    "NS_Tank_FireShells_1": "BP_FxRenderer_TankFireShells_Batch",
    "NS_AA_Gun_1": "BP_FxRenderer_AAGun_Batch",
    "NS_AA_SplashGround_1": "BP_FxRenderer_AASplash_Batch",
    "NS_Arty_SplashGround_1": "BP_FxRenderer_ArtySplash_Batch",
    "NS_Heli_SplashGround_1": "BP_FxRenderer_HeliSplash_Batch",
    "NS_Tank_SplashGround_1": "BP_FxRenderer_TankSplash_Batch",
    "NS_Jet_countermeasures": "BP_FxRenderer_JetCountermeasures_Batch",
    "NS_Jet_Fire_Continuous": "BP_FxRenderer_JetFireContinuous_Batch",
    "NS_Jet_Trails": "BP_FxRenderer_JetTrails74_Batch",
    "NS_MuzzleFlash_APC_1": "BP_FxRenderer_MuzzleFlashAPC_Batch",
    "NS_MuzzleFlash_Arty_1": "BP_FxRenderer_MuzzleFlashArty_Batch",
    "NS_MuzzleFlash_SPG_1": "BP_FxRenderer_MuzzleFlashSPG_Batch",
    "NS_MuzzleFlash_Tank_Maingun_1": "BP_FxRenderer_MuzzleFlashTankMainGun_Batch",
    "NS_MuzzleFlash_Tank_Mashingun_1": "BP_FxRenderer_MuzzleFlashTankMG_Batch",
    "NS_Rocket_Engine_1": "BP_FxRenderer_RocketEngine1_Batch",
    "NS_Rocket_Engine_2": "BP_FxRenderer_RocketEngine2_Batch",
    "NS_Rocket_Smoke_1": "BP_FxRenderer_RocketSmoke1_75_V2_Batch",
    "NS_Rocket_Smoke_2": "BP_FxRenderer_RocketSmoke2_76_V2_Batch",
    "NS_Rocket_Start": "BP_FxRenderer_RocketStart_Batch",
    "NS_StartFlash_1": "BP_FxRenderer_StartFlash_Batch",
    "NS_Shells_APC_1": "BP_FxRenderer_ShellsAPC_Batch",
    "NS_Shells_Arty_1": "BP_FxRenderer_ShellsArty_Batch",
    "NS_Shells_Tank_Mashingun_1": "BP_FxRenderer_ShellsTankMG_Batch",
    "NS_SmokeScreen_APC": "BP_FxRenderer_SmokeScreenAPC_Batch",
}


@dataclass(frozen=True)
class RecipeStep:
    channel: str
    style: int
    delay: float
    life_span: float
    reason: str


@dataclass(frozen=True)
class EffectSpec:
    source_name: str
    category: str
    subtype: int
    recipe: tuple[RecipeStep, ...]
    reference_speed: float | None = None
    reference_rate: float | None = None

    @property
    def source(self) -> str:
        return (
            f"/Game/ArmyVFX/Niagara/{self.category}/{self.source_name}."
            f"{self.source_name}"
        )

    @property
    def stem(self) -> str:
        return self.source_name.removeprefix("NS_")

    @property
    def target_name(self) -> str:
        return f"NS_MB_{self.stem}_Batch"

    @property
    def target(self) -> str:
        return f"{EFFECT_ROOT}/{self.target_name}.{self.target_name}"

    @property
    def renderer_name(self) -> str:
        return RENDERER_NAMES[self.source_name]

    @property
    def renderer_class(self) -> str:
        return (
            f"{RENDERER_ROOT}/{self.renderer_name}."
            f"{self.renderer_name}_C"
        )

    @property
    def generated_spawn_per_unit(self) -> bool:
        return self.source_name in {
            "NS_Jet_Trails",
            "NS_Rocket_Smoke_1",
            "NS_Rocket_Smoke_2",
        }


def burst(life: float = 5.0) -> tuple[RecipeStep, ...]:
    return (RecipeStep("Burst", 0, 0.0, life, "MassBattle NDC one-shot"),)


def attached(life: float) -> tuple[RecipeStep, ...]:
    return (
        RecipeStep(
            "Attached",
            0,
            0.0,
            life,
            "MassBattle persistent-array controller",
        ),
    )


def hybrid(life: float) -> tuple[RecipeStep, ...]:
    return (
        RecipeStep("Burst", 0, 0.0, max(life, 1.0), "Immediate authored layers"),
        RecipeStep(
            "Attached",
            0,
            0.0,
            life,
            "Authored SpawnRate/curve layers",
        ),
    )


SPECS: tuple[EffectSpec, ...] = (
    EffectSpec(
        "NS_Expl_Tank_1",
        "Destroyed",
        97,
        (
            RecipeStep("Burst", 0, 0.0, 5.0, "Immediate explosion layers"),
            RecipeStep(
                "Burst",
                1,
                0.3,
                5.0,
                "Source smoke_after Spawn Time = 0.3 s",
            ),
        ),
    ),
    EffectSpec("NS_Expl_Tank_Tower_1", "Destroyed", 98, attached(10.0)),
    EffectSpec("NS_Fire_Tank_1", "Destroyed", 89, attached(8.0)),
    EffectSpec("NS_Tank_FireShells_1", "Destroyed", 90, attached(3.0)),
    EffectSpec("NS_AA_Gun_1", "Environment", 91, attached(8.0)),
    EffectSpec("NS_AA_SplashGround_1", "Environment", 70, attached(1.0)),
    EffectSpec("NS_Arty_SplashGround_1", "Environment", 80, burst()),
    EffectSpec("NS_Heli_SplashGround_1", "Environment", 88, attached(8.0)),
    EffectSpec("NS_Tank_SplashGround_1", "Environment", 81, burst()),
    EffectSpec("NS_Jet_countermeasures", "Jet", 82, burst()),
    EffectSpec("NS_Jet_Fire_Continuous", "Jet", 92, attached(8.0)),
    EffectSpec(
        "NS_Jet_Trails",
        "Jet",
        74,
        attached(8.0),
        reference_speed=6000.0,
        reference_rate=6.0,
    ),
    EffectSpec("NS_MuzzleFlash_APC_1", "MuzzleFlash", 95, hybrid(0.1)),
    EffectSpec("NS_MuzzleFlash_Arty_1", "MuzzleFlash", 71, hybrid(1.0)),
    EffectSpec("NS_MuzzleFlash_SPG_1", "MuzzleFlash", 72, hybrid(1.0)),
    EffectSpec("NS_MuzzleFlash_Tank_Maingun_1", "MuzzleFlash", 73, hybrid(1.0)),
    EffectSpec("NS_MuzzleFlash_Tank_Mashingun_1", "MuzzleFlash", 96, hybrid(0.1)),
    EffectSpec("NS_Rocket_Engine_1", "Projectiles", 93, attached(8.0)),
    EffectSpec("NS_Rocket_Engine_2", "Projectiles", 94, attached(8.0)),
    EffectSpec(
        "NS_Rocket_Smoke_1",
        "Projectiles",
        75,
        attached(8.0),
        reference_speed=2200.0,
        reference_rate=22.0,
    ),
    EffectSpec(
        "NS_Rocket_Smoke_2",
        "Projectiles",
        76,
        attached(8.0),
        reference_speed=2200.0,
        reference_rate=22.0,
    ),
    EffectSpec("NS_Rocket_Start", "Projectiles", 99, attached(0.2)),
    EffectSpec("NS_StartFlash_1", "Projectiles", 83, burst()),
    EffectSpec("NS_Shells_APC_1", "Shells", 84, burst()),
    EffectSpec("NS_Shells_Arty_1", "Shells", 85, burst()),
    EffectSpec("NS_Shells_Tank_Mashingun_1", "Shells", 86, burst()),
    EffectSpec("NS_SmokeScreen_APC", "SmokeScreen", 87, burst()),
)


class TranslationError(RuntimeError):
    pass


class Bridge:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port

    async def command(
        self, command: str, params: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=CONNECT_TIMEOUT,
            )
        except Exception as exc:
            raise TranslationError(
                f"Cannot connect to Unreal bridge {self.host}:{self.port}: {exc}"
            ) from exc
        try:
            writer.write(
                json.dumps(
                    {"command": command, "params": params or {}},
                    ensure_ascii=False,
                ).encode("utf-8")
                + b"\0"
            )
            await writer.drain()
            if writer.can_write_eof():
                writer.write_eof()
            chunks: list[bytes] = []
            while True:
                chunk = await asyncio.wait_for(
                    reader.read(65536), timeout=COMMAND_TIMEOUT
                )
                if not chunk:
                    break
                marker = chunk.find(b"\0")
                if marker >= 0:
                    chunks.append(chunk[:marker])
                    break
                chunks.append(chunk)
            if not chunks:
                raise TranslationError(f"{command}: Unreal returned no response")
            result = json.loads(b"".join(chunks).decode("utf-8"))
            if not isinstance(result, dict):
                raise TranslationError(f"{command}: response is not an object")
            return result
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


def graphs(payload: dict[str, Any]) -> list[dict[str, Any]]:
    return payload.get("graphs", payload.get("graph", {}).get("graphs", []))


def modules(payload: dict[str, Any]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    return [
        (graph, node)
        for graph in graphs(payload)
        for node in graph.get("nodes", [])
        if node.get("kind") == "function_call" and node.get("is_stack_module")
    ]


def module_key(graph: dict[str, Any], node: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(graph.get("emitter", "")),
        str(graph.get("script_usage", "")),
        str(node.get("node_guid", "")),
    )


def dynamic_guids(value: Any) -> list[str]:
    result: list[str] = []

    def visit(item: Any) -> None:
        if isinstance(item, dict):
            guid = item.get("dynamic_input_node_guid")
            if guid:
                result.append(str(guid))
            for child in item.get("dynamic_inputs", []):
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return result


def scheduler_guids(source_graph: dict[str, Any]) -> list[str]:
    values: list[str] = []
    for _graph, node in modules(source_graph):
        if node.get("function_name") not in SCHEDULER_FUNCTIONS:
            continue
        values.append(str(node["node_guid"]))
        values.extend(dynamic_guids(node.get("stack_inputs", [])))
    return sorted(set(filter(None, values)))


def stack_input(node: dict[str, Any], name: str) -> dict[str, Any] | None:
    normalized = name.replace(" ", "").lower()
    return next(
        (
            item
            for item in node.get("stack_inputs", [])
            if str(item.get("name", "")).replace(" ", "").lower() == normalized
        ),
        None,
    )


def controller_previous_position_assignments(
    payload: dict[str, Any], script_usage: str
) -> list[dict[str, Any]]:
    """Find controller assignments required by attribute-reader position sampling."""
    result: list[dict[str, Any]] = []
    for graph in graphs(payload):
        if (
            graph.get("emitter") != "MB_BatchController_Attached"
            or graph.get("script_usage") != script_usage
        ):
            continue
        for node in graph.get("nodes", []):
            if not str(node.get("node_class", "")).endswith("NiagaraNodeAssignment"):
                continue
            value = stack_input(node, "Particles.Previous.Position")
            if value and (
                value.get("value_mode") == "linked_parameter"
                and value.get("linked_parameter") == "Particles.Position"
            ):
                result.append(node)
    return result


def previous_position_insert_operations(script_usage: str) -> list[dict[str, Any]]:
    operation_id = (
        "controller_previous_position_spawn"
        if script_usage == "ParticleSpawnScript"
        else "controller_previous_position_update"
    )
    return [
        {
            "id": operation_id,
            "op": "insert_assignment",
            "graph": {
                "scope": "emitter",
                "emitter": "MB_BatchController_Attached",
                "script_usage": script_usage,
            },
            # Spawn stores the position after the array-driven transform module;
            # Update stores it immediately before that module changes Position.
            "stack_index": -1 if script_usage == "ParticleSpawnScript" else 1,
            "targets": [
                {"name": "Particles.Previous.Position", "type": "position"}
            ],
        },
        {
            "op": "set_stack_input",
            "node": {"operation": operation_id},
            "input": "Particles.Previous.Position",
            "value": {
                "mode": "linked_parameter",
                "parameter": "Particles.Position",
            },
        },
    ]


async def ensure_controller_previous_position(
    bridge: Bridge, spec: EffectSpec
) -> dict[str, Any]:
    """Make SpawnPerUnit controller history explicit and disable retry duplicates."""
    target_graph = await read_graph(bridge, spec.target)
    operations: list[dict[str, Any]] = []
    allowed_state_change_guids: list[str] = []
    before: dict[str, dict[str, int]] = {}

    for usage in ("ParticleSpawnScript", "ParticleUpdateScript"):
        matches = controller_previous_position_assignments(target_graph, usage)
        active = [node for node in matches if node.get("stack_enabled", True)]
        before[usage] = {"total": len(matches), "active": len(active)}
        if not active:
            if matches:
                operations.append(
                    {
                        "op": "set_module_enabled",
                        "node": matches[0]["reference"],
                        "enabled": True,
                    }
                )
                allowed_state_change_guids.append(str(matches[0]["node_guid"]))
                active = matches[:1]
            else:
                operations.extend(previous_position_insert_operations(usage))
        for duplicate in active[1:]:
            operations.append(
                {
                    "op": "set_module_enabled",
                    "node": duplicate["reference"],
                    "enabled": False,
                }
            )
            allowed_state_change_guids.append(str(duplicate["node_guid"]))

    apply_result: dict[str, Any] = {"success": True, "saved": True, "skipped": True}
    if operations:
        apply_result = await bridge.command(
            "MCP_NiagaraApplyGraphEdit",
            {
                "SystemPath": spec.target,
                "EditJson": json.dumps(
                    {
                        "operations": operations,
                        "validation": {
                            "require_ready_to_run": False,
                            "allow_removed_edges": True,
                            "allowed_scheduler_node_guids": sorted(
                                set(allowed_state_change_guids)
                            ),
                        },
                    }
                ),
                "bSaveAssets": True,
            },
        )

    ready = await wait_until_ready(bridge, spec.target)
    final_graph = await read_graph(bridge, spec.target)
    after: dict[str, dict[str, int]] = {}
    valid = True
    for usage in ("ParticleSpawnScript", "ParticleUpdateScript"):
        matches = controller_previous_position_assignments(final_graph, usage)
        active = [node for node in matches if node.get("stack_enabled", True)]
        after[usage] = {"total": len(matches), "active": len(active)}
        valid = valid and len(active) == 1

    final_save = await save_validated_target(bridge, spec.target) if valid else {
        "success": False,
        "saved": False,
        "error": "controller history is invalid",
    }
    success = bool(
        apply_result.get("success")
        and ready.get("success")
        and valid
        and final_save.get("success")
        and final_save.get("saved")
    )
    return {
        "success": success,
        "stage": "verified" if success else "controller_history_invalid",
        "operation_count": len(operations),
        "before": before,
        "after": after,
        "apply": apply_result,
        "ready": ready,
        "save": final_save,
    }


def input_value(item: dict[str, Any]) -> dict[str, Any]:
    mode = item.get("value_mode")
    if mode == "linked_parameter":
        return {"mode": "linked_parameter", "parameter": item["linked_parameter"]}
    if mode == "local":
        return {"mode": "local", "value_text": item["value_text"]}
    if mode == "dynamic_input":
        return {"mode": "dynamic_input", "asset": item["dynamic_input_script"]}
    raise TranslationError(
        f"Cannot round-trip stack input {item.get('name')}: mode={mode}"
    )


def rapid_iteration_name(signature: str) -> str:
    fields = signature.split("|")
    return fields[-3] if len(fields) >= 3 else ""


async def read_graph(bridge: Bridge, system: str) -> dict[str, Any]:
    result = await bridge.command(
        "MCP_NiagaraReadGraph",
        {"SystemPath": system, "SelectorJson": "{}"},
    )
    if not result.get("success"):
        raise TranslationError(f"Read graph failed for {system}: {result}")
    return result


async def compile_unsaved(bridge: Bridge, system: str) -> dict[str, Any]:
    return await bridge.command(
        "MCP_NiagaraApplyGraphEdit",
        {
            "SystemPath": system,
            "EditJson": json.dumps(
                {
                    "operations": [],
                    "validation": {"require_ready_to_run": True},
                }
            ),
            "bSaveAssets": False,
        },
    )


async def wait_until_ready(
    bridge: Bridge, system: str, timeout_seconds: float = 30.0
) -> dict[str, Any]:
    """Wait for Niagara's deferred CPU/GPU compile work to settle."""
    deadline = asyncio.get_running_loop().time() + timeout_seconds
    last: dict[str, Any] = {}
    while asyncio.get_running_loop().time() < deadline:
        last = await bridge.command(
            "MCP_NiagaraReadSummary",
            {"SystemPath": system, "OptionsJson": '{"include_modules":false}'},
        )
        if last.get("success") and last.get("ready_to_run"):
            return {"success": True, "ready_to_run": True, "summary": last}
        await asyncio.sleep(0.5)
    return {
        "success": False,
        "ready_to_run": bool(last.get("ready_to_run")),
        "summary": last,
        "error": f"Niagara did not become ready within {timeout_seconds:.1f}s",
    }


async def asset_exists(bridge: Bridge, object_path: str) -> bool:
    name = object_path.rsplit("/", 1)[-1].split(".", 1)[0]
    result = await bridge.command(
        "MCP_NiagaraQuery",
        {
            "QueryJson": json.dumps(
                {"path": object_path.rsplit("/", 1)[0], "query": name, "limit": 10}
            )
        },
    )
    return any(item.get("object_path") == object_path for item in result.get("systems", []))


async def derive_comparison_policy(
    bridge: Bridge,
    source: str,
    target: str,
    scheduler_nodes: Iterable[str],
) -> tuple[dict[str, Any], dict[str, Any]]:
    policy: dict[str, Any] = {
        "mode": "translation",
        "require_ready_to_run": True,
        "allow_removed_edges": True,
        "allowed_system_setting_changes": ["fixed_bounds"],
        "allowed_scheduler_node_guids": sorted(set(scheduler_nodes)),
    }
    # The probe exists only to derive the exact set of scheduler-edge removals.
    # Runtime readiness is enforced by the final comparison after deferred
    # Niagara diagnostics/compilation have settled.
    probe_policy = {**policy, "require_ready_to_run": False}
    probe = await bridge.command(
        "MCP_NiagaraCompareSystems",
        {
            "SourceSystemPath": source,
            "TargetSystemPath": target,
            "OptionsJson": json.dumps(probe_policy),
        },
    )
    preservation = probe.get("preservation", {})
    policy["allowed_rapid_iteration_parameter_changes"] = sorted(
        {
            rapid_iteration_name(item)
            for item in preservation.get("missing_rapid_iteration_values", [])
            if rapid_iteration_name(item)
        }
    )
    policy["allowed_removed_edges"] = preservation.get("removed_edges", [])
    policy.pop("allow_removed_edges", None)
    await wait_until_ready(bridge, source)
    await wait_until_ready(bridge, target)
    final = await bridge.command(
        "MCP_NiagaraCompareSystems",
        {
            "SourceSystemPath": source,
            "TargetSystemPath": target,
            "OptionsJson": json.dumps(policy),
        },
    )
    return policy, final


async def save_validated_target(bridge: Bridge, target: str) -> dict[str, Any]:
    return await bridge.command(
        "MCP_NiagaraApplyGraphEdit",
        {
            "SystemPath": target,
            "EditJson": json.dumps(
                {
                    "operations": [],
                    "validation": {"require_ready_to_run": True},
                }
            ),
            "bSaveAssets": True,
        },
    )


async def repair_existing(
    bridge: Bridge, spec: EffectSpec
) -> dict[str, Any]:
    source_graph = await read_graph(bridge, spec.source)
    target_graph = await read_graph(bridge, spec.target)
    source_modules = modules(source_graph)
    target_by_key = {module_key(graph, node): node for graph, node in modules(target_graph)}
    scheduler_nodes = scheduler_guids(source_graph)
    operations: list[dict[str, Any]] = []

    for graph, source_node in source_modules:
        if source_node.get("function_name") not in SCHEDULER_FUNCTIONS:
            continue
        key = module_key(graph, source_node)
        target_node = target_by_key.get(key)
        if not target_node:
            raise TranslationError(f"{spec.source_name}: source scheduler missing in target: {key}")

        # Older hybrid targets used FLT_MAX Spawn Time to suppress source bursts.
        # Restore the exact source pin and disable the scheduler node instead.
        if source_node.get("function_name") == "SpawnBurst_Instantaneous":
            source_time = stack_input(source_node, "Spawn Time")
            target_time = stack_input(target_node, "Spawn Time")
            if source_time and target_time and (
                source_time.get("value_text") != target_time.get("value_text")
                or source_time.get("value_mode") != target_time.get("value_mode")
            ):
                operations.append(
                    {
                        "op": "set_stack_input",
                        "node": target_node["reference"],
                        "input": "Spawn Time",
                        "value": input_value(source_time),
                    }
                )

        if target_node.get("stack_enabled", True):
            operations.append(
                {
                    "op": "set_module_enabled",
                    "node": target_node["reference"],
                    "enabled": False,
                }
            )

    repair_barrier: dict[str, Any] = {"success": True, "skipped": True}
    if operations:
        # The old target is intentionally being repaired to the source signature,
        # so this first in-memory barrier may report the removed old FLT_MAX value.
        # Nothing is saved until source-vs-target comparison succeeds below.
        repair_barrier = await bridge.command(
            "MCP_NiagaraApplyGraphEdit",
            {
                "SystemPath": spec.target,
                "EditJson": json.dumps(
                    {
                        "operations": operations,
                        "validation": {
                            "require_ready_to_run": True,
                            "allow_removed_edges": True,
                            "allowed_scheduler_node_guids": scheduler_nodes,
                        },
                    }
                ),
                "bSaveAssets": False,
            },
        )

    source_compile = await compile_unsaved(bridge, spec.source)
    target_compile = await compile_unsaved(bridge, spec.target)
    source_ready = await wait_until_ready(bridge, spec.source)
    target_ready = await wait_until_ready(bridge, spec.target)
    policy, comparison = await derive_comparison_policy(
        bridge, spec.source, spec.target, scheduler_nodes
    )
    if not comparison.get("success"):
        return {
            "success": False,
            "stage": "translation_compare",
            "repair_barrier": repair_barrier,
            "source_compile": source_compile,
            "target_compile": target_compile,
            "source_ready": source_ready,
            "target_ready": target_ready,
            "policy": policy,
            "comparison": comparison,
        }

    save = await save_validated_target(bridge, spec.target)
    return {
        "success": bool(save.get("success") and save.get("saved")),
        "stage": "saved" if save.get("saved") else "save_failed",
        "repair_operation_count": len(operations),
        "repair_barrier": repair_barrier,
        "source_ready": source_ready,
        "target_ready": target_ready,
        "policy": policy,
        "comparison": comparison,
        "save": save,
    }


def attached_data_interfaces() -> list[dict[str, Any]]:
    return [
        {
            "op": "add_user_data_interface",
            "name": "User.LocationArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayFloat3",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.OrientationArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayQuat",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.ScaleArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayFloat3",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.IsHiddenArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayBool",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.PositionArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayPosition",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.StyleArray_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayInt32",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.NiagaraIDIndex_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayInt32",
        },
        {
            "op": "add_user_data_interface",
            "name": "User.NiagaraIDAcquireTag_Attached",
            "data_interface_class": "/Script/Niagara.NiagaraDataInterfaceArrayInt32",
        },
    ]


async def ensure_runtime_interface_parameters(
    bridge: Bridge, spec: EffectSpec
) -> dict[str, Any]:
    """Declare the full MassBattle renderer ABI, including unused array inputs.

    The runtime uploads Attached arrays to every renderer component, including a
    Burst-only system.  Unused user data interfaces are therefore required to
    avoid per-frame OverrideParameter warnings; they do not alter source visual
    emitters or make a Burst recipe Attached.
    """
    summary = await bridge.command(
        "MCP_NiagaraReadSummary",
        {"SystemPath": spec.target, "OptionsJson": '{"include_modules":false}'},
    )
    existing = {str(item.get("name")) for item in summary.get("user_parameters", [])}
    required = attached_data_interfaces()
    missing = [item for item in required if item["name"] not in existing]
    apply_result: dict[str, Any] = {"success": True, "saved": True, "skipped": True}
    if missing:
        apply_result = await bridge.command(
            "MCP_NiagaraApplyGraphEdit",
            {
                "SystemPath": spec.target,
                "EditJson": json.dumps(
                    {
                        "operations": missing,
                        "validation": {
                            "require_ready_to_run": False,
                            "allow_removed_edges": True,
                        },
                    }
                ),
                "bSaveAssets": True,
            },
        )
    ready = await wait_until_ready(bridge, spec.target)
    verified = await bridge.command(
        "MCP_NiagaraReadSummary",
        {"SystemPath": spec.target, "OptionsJson": '{"include_modules":false}'},
    )
    final_names = {
        str(item.get("name")) for item in verified.get("user_parameters", [])
    }
    still_missing = [item["name"] for item in required if item["name"] not in final_names]
    success = bool(
        apply_result.get("success")
        and (not missing or apply_result.get("saved"))
        and ready.get("success")
        and not still_missing
    )
    return {
        "success": success,
        "stage": "verified" if success else "runtime_interface_invalid",
        "added": [item["name"] for item in missing],
        "still_missing": still_missing,
        "apply": apply_result,
        "ready": ready,
    }


def make_spawn_per_unit_edit(
    spec: EffectSpec, source_graph: dict[str, Any]
) -> tuple[dict[str, Any], list[str]]:
    scheduler_nodes = scheduler_guids(source_graph)
    visual_schedulers = [
        (graph, node)
        for graph, node in modules(source_graph)
        if node.get("function_name") == "SpawnPerUnit"
    ]
    if len(visual_schedulers) != 1:
        raise TranslationError(
            f"{spec.source_name}: expected one SpawnPerUnit, got {len(visual_schedulers)}"
        )
    graph, scheduler = visual_schedulers[0]
    emitter = str(graph["emitter"])
    update_modules = [
        node
        for item_graph, node in modules(source_graph)
        if item_graph.get("emitter") == emitter
        and item_graph.get("script_usage") == "EmitterUpdateScript"
    ]
    scheduler_index = next(
        index
        for index, node in enumerate(update_modules)
        if node.get("node_guid") == scheduler.get("node_guid")
    )
    particle_modules = [
        node
        for item_graph, node in modules(source_graph)
        if item_graph.get("emitter") == emitter
        and item_graph.get("script_usage") == "ParticleSpawnScript"
    ]
    initialize_index = next(
        index
        for index, node in enumerate(particle_modules)
        if node.get("function_name") in {"InitializeRibbon", "InitializeParticle"}
    )
    is_ribbon = any(
        node.get("function_name") == "InitializeRibbon" for node in particle_modules
    )

    emitter_update = {
        "scope": "emitter",
        "emitter": emitter,
        "script_usage": "EmitterUpdateScript",
    }
    particle_spawn = {
        "scope": "emitter",
        "emitter": emitter,
        "script_usage": "ParticleSpawnScript",
    }
    operations = attached_data_interfaces()
    operations.extend(
        [
            {
                "op": "add_user_parameter",
                "name": "User.MB_ReferenceSpawnRate",
                "value_type": "float",
                "default": spec.reference_rate,
            },
            {
                "op": "copy_emitter_from_system",
                "source_system": ATTACHED_TEMPLATE,
                "source_emitter": ATTACHED_TEMPLATE_EMITTER,
                "new_emitter_name": "MB_BatchController_Attached",
            },
            *previous_position_insert_operations("ParticleSpawnScript"),
            *previous_position_insert_operations("ParticleUpdateScript"),
            {
                "id": "spawn_from_batch_controller",
                "op": "insert_module",
                "graph": emitter_update,
                "module_asset": SPAWN_FROM_OTHER,
                "stack_index": scheduler_index,
            },
            {
                "op": "set_stack_inputs",
                "node": {"operation": "spawn_from_batch_controller"},
                "inputs": [
                    {
                        "input": "Attribute Reader",
                        "value": {
                            "mode": "data_interface",
                            "properties": {
                                "EmitterBinding": (
                                    '(BindingMode=Other,'
                                    'EmitterName="MB_BatchController_Attached")'
                                )
                            },
                        },
                    },
                    {
                        "input": "Spawn Rate",
                        "value": {
                            "mode": "linked_parameter",
                            "parameter": "User.MB_ReferenceSpawnRate",
                        },
                    },
                    {
                        "input": "Calculate Spawn Rate Per Particle",
                        "value": {"mode": "local", "value_text": "(Value=-1)"},
                    },
                    {
                        "input": "Spawn Rate Per Particle Cap",
                        "value": {
                            "mode": "local",
                            "value_text": "(Value=100000.000000)",
                        },
                    },
                    {
                        "input": "Spawning Enabled",
                        "value": {"mode": "local", "value_text": "(Value=-1)"},
                    },
                ],
            },
            {
                "op": "set_module_enabled",
                "node": scheduler["reference"],
                "enabled": False,
            },
            {
                "id": "sample_batch_controller",
                "op": "insert_module",
                "graph": particle_spawn,
                "module_asset": MB_SAMPLE_FROM_OTHER,
                "stack_index": initialize_index + 1,
            },
            {
                "op": "set_stack_inputs",
                "node": {"operation": "sample_batch_controller"},
                "inputs": [
                    {
                        "input": "Emitter Sampling Mode",
                        "value": {"mode": "local", "value_text": "(Value=1)"},
                    },
                    {
                        "input": "Transform Data Option",
                        "value": {"mode": "local", "value_text": "(Value=0)"},
                    },
                    {
                        "input": "Reject Invalid Particle For Ribbons",
                        "value": {
                            "mode": "local",
                            "value_text": "(Value=-1)" if is_ribbon else "(Value=0)",
                        },
                    },
                    {
                        "input": "Position Sampling",
                        "value": {"mode": "local", "value_text": "(Value=1)"},
                    },
                    {
                        "input": "Switch on ENiagara_RibbonIDSampling",
                        "value": {
                            "mode": "local",
                            "value_text": "(Value=1)" if is_ribbon else "(Value=0)",
                        },
                    },
                    {
                        "input": "NiagaraIDIndexArray",
                        "value": {
                            "mode": "linked_parameter",
                            "parameter": "User.NiagaraIDIndex_Attached",
                        },
                    },
                    {
                        "input": "NiagaraIDAcquireTagArray",
                        "value": {
                            "mode": "linked_parameter",
                            "parameter": "User.NiagaraIDAcquireTag_Attached",
                        },
                    },
                ],
            },
        ]
    )
    return (
        {
            "operations": operations,
            "validation": {
                "require_ready_to_run": True,
                "require_no_warnings": False,
                "allow_removed_edges": True,
                "allowed_scheduler_node_guids": scheduler_nodes,
            },
        },
        scheduler_nodes,
    )


async def create_spawn_per_unit(
    bridge: Bridge, spec: EffectSpec
) -> dict[str, Any]:
    source_graph = await read_graph(bridge, spec.source)
    edit, scheduler_nodes = make_spawn_per_unit_edit(spec, source_graph)
    package_path, object_part = spec.target.rsplit("/", 1)
    new_name = object_part.split(".", 1)[0]
    duplicate = await bridge.command(
        "MCP_EffectDuplicateAsset",
        {
            "SourceAssetPath": spec.source,
            "NewAssetName": new_name,
            "PackagePath": package_path,
            "bSaveAssets": False,
        },
    )
    if not duplicate.get("success"):
        return {"success": False, "stage": "duplicate", "duplicate": duplicate}
    exact = await bridge.command(
        "MCP_NiagaraCompareSystems",
        {
            "SourceSystemPath": spec.source,
            "TargetSystemPath": spec.target,
            "OptionsJson": json.dumps(
                {"mode": "exact", "require_ready_to_run": False}
            ),
        },
    )
    if not exact.get("success"):
        await bridge.command(
            "MCP_EffectDiscardUnsavedDuplicate", {"AssetPath": spec.target}
        )
        return {"success": False, "stage": "exact_clone", "exact": exact}

    graph_edit = await bridge.command(
        "MCP_NiagaraApplyGraphEdit",
        {
            "SystemPath": spec.target,
            "EditJson": json.dumps(edit),
            "bSaveAssets": False,
        },
    )
    # Remove only the copied controller's reference renderer.  All source
    # renderer identities/materials remain untouched.
    remove_renderer = await bridge.command(
        "MCP_NiagaraDelete",
        {
            "SystemPath": spec.target,
            "DeleteJson": json.dumps(
                {
                    "type": "renderer",
                    "emitter": "MB_BatchController_Attached",
                    "renderer_index": 0,
                }
            ),
            "bSaveAssets": False,
        },
    )
    bounds = await bridge.command(
        "MCP_NiagaraMergeWrite",
        {
            "SystemPath": spec.target,
            "PatchJson": json.dumps(
                {
                    "patches": [
                        {
                            "target": "system",
                            "property": "bFixedBounds",
                            "value_text": "True",
                        },
                        {
                            "target": "system",
                            "property": "FixedBounds",
                            "value_text": (
                                "(Min=(X=-100000000.000000,Y=-100000000.000000,"
                                "Z=-100000000.000000),Max=(X=100000000.000000,"
                                "Y=100000000.000000,Z=100000000.000000),IsValid=1)"
                            ),
                        },
                    ]
                }
            ),
            "bSaveAssets": False,
        },
    )
    source_compile = await compile_unsaved(bridge, spec.source)
    target_compile = await compile_unsaved(bridge, spec.target)
    source_ready = await wait_until_ready(bridge, spec.source)
    target_ready = await wait_until_ready(bridge, spec.target)
    policy, comparison = await derive_comparison_policy(
        bridge, spec.source, spec.target, scheduler_nodes
    )
    if not comparison.get("success"):
        cleanup = await bridge.command(
            "MCP_EffectDiscardUnsavedDuplicate", {"AssetPath": spec.target}
        )
        return {
            "success": False,
            "stage": "translation_compare",
            "graph_edit": graph_edit,
            "remove_renderer": remove_renderer,
            "bounds": bounds,
            "source_compile": source_compile,
            "target_compile": target_compile,
            "source_ready": source_ready,
            "target_ready": target_ready,
            "policy": policy,
            "comparison": comparison,
            "cleanup": cleanup,
        }
    save = await save_validated_target(bridge, spec.target)
    return {
        "success": bool(save.get("success") and save.get("saved")),
        "stage": "saved" if save.get("saved") else "save_failed",
        "graph_edit": graph_edit,
        "remove_renderer": remove_renderer,
        "bounds": bounds,
        "source_ready": source_ready,
        "target_ready": target_ready,
        "policy": policy,
        "comparison": comparison,
        "save": save,
    }


async def ensure_renderer(bridge: Bridge, spec: EffectSpec) -> dict[str, Any]:
    read = await bridge.command(
        "MCP_BatchFxReadRendererDefaults", {"TargetClassPath": spec.renderer_class}
    )
    duplicate: dict[str, Any] | None = None
    if not read.get("success"):
        for attempt in range(3):
            duplicate = await bridge.command(
                "MCP_DuplicateClassAsset",
                {
                    "SourceClassPath": RENDERER_TEMPLATE_CLASS,
                    "NewClassName": spec.renderer_name,
                    "PackagePath": RENDERER_ROOT,
                },
            )
            if duplicate.get("success"):
                break
            # AssetTools can still be releasing the Niagara package compiled in
            # the immediately preceding command.  Re-read before a short retry
            # because DuplicateClassAsset can finish even when its first result
            # reports nullptr.
            await asyncio.sleep(0.25 * (attempt + 1))
            read = await bridge.command(
                "MCP_BatchFxReadRendererDefaults",
                {"TargetClassPath": spec.renderer_class},
            )
            if read.get("success"):
                duplicate = {"success": True, "status": "created_on_previous_attempt"}
                break
        if not duplicate or not duplicate.get("success"):
            return {"success": False, "stage": "duplicate", "duplicate": duplicate}
    write = await bridge.command(
        "MCP_BatchFxSetRendererDefaults",
        {
            "TargetClassPath": spec.renderer_class,
            "NiagaraSystemPath": spec.target,
            "NdcBurstFxPath": NDC_BURST,
            "SubType": spec.subtype,
            "RenderBatchSize": 2048,
            "PoolingCooldown": 3.0,
            "bSaveAssets": True,
        },
    )
    verify = await bridge.command(
        "MCP_BatchFxReadRendererDefaults", {"TargetClassPath": spec.renderer_class}
    )
    defaults = verify.get("defaults", verify)
    verified = bool(
        verify.get("success")
        and str(defaults.get("niagara_system", defaults.get("NiagaraSystemAsset", "")))
        in {spec.target, spec.target.split(".", 1)[0]}
        and int(defaults.get("subtype", defaults.get("SubType", -1))) == spec.subtype
    )
    return {
        "success": bool(write.get("success") and verified),
        "stage": "verified" if verified else "verify_failed",
        "duplicate": duplicate,
        "write": write,
        "verify": verify,
    }


def manifest() -> dict[str, Any]:
    return {
        "schema": "massbattle.armyvfx.faithful_batch_manifest.v2",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "source_root": "/Game/ArmyVFX/Niagara",
        "source_system_count": len(SPECS),
        "batch_target_count": len(SPECS),
        "invariants": {
            "batch_equation": "BatchE([C0..Cn]) == [E(C0)..E(Cn)]",
            "source_visual_graph_policy": (
                "Preserve source emitters/renderers/materials/curves/events/sim targets; "
                "replace scheduler ABI only"
            ),
            "spawn_per_unit": (
                "JetTrail controllers use 6000 uu/s and RocketSmoke controllers use "
                "2200 uu/s; all controllers sharing one SubType must use the same speed."
            ),
        },
        "items": [
            {
                **asdict(spec),
                "source": spec.source,
                "target": spec.target,
                "renderer_class": spec.renderer_class,
                "generated_spawn_per_unit": spec.generated_spawn_per_unit,
                "recipe": [asdict(step) for step in spec.recipe],
            }
            for spec in SPECS
        ],
    }


async def run(args: argparse.Namespace) -> int:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    manifest_path = OUTPUT_DIR / "ArmyVFX_FaithfulBatch_Manifest.json"
    manifest_path.write_text(
        json.dumps(manifest(), ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"ArmyVFX faithful plan: {len(SPECS)} source systems", flush=True)
    print(f"Manifest: {manifest_path}", flush=True)
    if args.action == "plan":
        return 0

    bridge = Bridge(args.host, args.port)
    ping = await bridge.command("ping")
    if not ping.get("success"):
        raise TranslationError(f"Unreal bridge ping failed: {ping}")

    records: list[dict[str, Any]] = []
    failed = 0
    selected = [spec for spec in SPECS if not args.only or spec.source_name in args.only]
    for index, spec in enumerate(selected, start=1):
        print(f"[{index}/{len(selected)}] {spec.source_name}", flush=True)
        exists = await asset_exists(bridge, spec.target)
        if spec.generated_spawn_per_unit and not exists:
            conversion = await create_spawn_per_unit(bridge, spec)
        else:
            conversion = await repair_existing(bridge, spec)
        if conversion.get("success") and spec.generated_spawn_per_unit:
            controller_history = await ensure_controller_previous_position(bridge, spec)
            conversion["controller_previous_position"] = controller_history
            if not controller_history.get("success"):
                conversion["success"] = False
                conversion["stage"] = "controller_history"
        if conversion.get("success"):
            runtime_interface = await ensure_runtime_interface_parameters(bridge, spec)
            conversion["runtime_interface"] = runtime_interface
            if not runtime_interface.get("success"):
                conversion["success"] = False
                conversion["stage"] = "runtime_interface"
        renderer: dict[str, Any] = {"success": False, "stage": "skipped"}
        if conversion.get("success"):
            renderer = await ensure_renderer(bridge, spec)
        success = bool(conversion.get("success") and renderer.get("success"))
        records.append(
            {
                "source_name": spec.source_name,
                "source": spec.source,
                "target": spec.target,
                "subtype": spec.subtype,
                "recipe": [asdict(step) for step in spec.recipe],
                "conversion": conversion,
                "renderer": renderer,
                "success": success,
            }
        )
        if not success:
            failed += 1
            print(
                f"  FAILED conversion={conversion.get('stage')} "
                f"renderer={renderer.get('stage')}",
                flush=True,
            )
            if args.stop_on_error:
                break
        else:
            print(
                f"  PASS SubType={spec.subtype} recipe="
                f"{'+'.join(step.channel for step in spec.recipe)}",
                flush=True,
            )

    report = {
        "schema": "massbattle.armyvfx.faithful_batch_report.v2",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "requested_count": len(selected),
        "processed_count": len(records),
        "passed_count": sum(record["success"] for record in records),
        "failed_count": failed,
        "success": failed == 0 and len(records) == len(selected),
        "records": records,
    }
    report_path = OUTPUT_DIR / "ArmyVFX_FaithfulBatch_Report.json"
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        f"Result: {report['passed_count']}/{report['requested_count']} passed; "
        f"report={report_path}",
        flush=True,
    )
    return 0 if report["success"] else 2


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action", choices=("plan", "apply"), default="plan")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--stop-on-error", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        return asyncio.run(run(parse_args(argv or sys.argv[1:])))
    except (OSError, json.JSONDecodeError, TranslationError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
