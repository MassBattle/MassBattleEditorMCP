"""Create a clean ArmyVFX NDC-only SpawnBatch acceptance level."""

from __future__ import annotations

import json
from pathlib import Path

import unreal


LEVEL_PATH = "/Game/ArmyVFX/MassBattleConverted/Demo/L_ArmyVFX_NDC_Batch_Demo"
DEMO_CLASS = "/Script/MassBattleEditorMCP.MassBattleArmyVFXDemoActor"
VALIDATION_PATH = Path(
    "D:/UE5Project/Winyunq/Saved/MassBattleEditorMCP/Validation/"
    "ArmyVFX_NDC_Batch_Demo_Create.json"
)


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def label(actor, name, folder):
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    return actor


def spawn_class(actor_subsystem, class_path, name, location, rotation=None, folder="ArmyVFXBatchDemo"):
    actor_class = require(unreal.load_class(None, class_path), f"Class not found: {class_path}")
    actor = require(
        actor_subsystem.spawn_actor_from_class(
            actor_class,
            location,
            rotation or unreal.Rotator(),
        ),
        f"Failed to spawn: {class_path}",
    )
    return label(actor, name, folder)


def main():
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        raise RuntimeError(f"Refusing to overwrite existing acceptance level: {LEVEL_PATH}")

    demo_folder = "/Game/ArmyVFX/MassBattleConverted/Demo"
    if not unreal.EditorAssetLibrary.does_directory_exist(demo_folder):
        require(unreal.EditorAssetLibrary.make_directory(demo_folder), "Failed to create Demo folder")

    world = require(unreal.EditorLoadingAndSavingUtils.new_blank_map(False), "Failed to create blank map")
    actors = require(
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem),
        "EditorActorSubsystem is unavailable",
    )
    world_settings = require(world.get_world_settings(), "WorldSettings is unavailable")
    world_settings.set_editor_property("default_game_mode", unreal.GameModeBase.static_class())

    # This is the only gameplay/effect actor in the level. It creates the eight
    # ArmyVFX NDC renderers and calls SpawnBatch once per effect with four contexts.
    harness = spawn_class(
        actors,
        DEMO_CLASS,
        "ArmyVFX_NDC_SpawnBatch_Actor",
        unreal.Vector(),
        folder="ArmyVFXBatchDemo/EffectActor",
    )
    harness.set_editor_property("auto_play", False)
    harness.set_editor_property("native_only", True)
    harness.set_editor_property("auto_play_all_native", True)
    harness.set_editor_property("initial_delay", 3.0)
    harness.set_editor_property("native_all_loop_interval", 8.0)
    harness.set_editor_property("native_grid_side", 2)
    harness.set_editor_property("native_grid_spacing", 220.0)
    harness.set_editor_property("native_center", unreal.Vector(300.0, 0.0, 160.0))

    cube = require(unreal.load_asset("/Engine/BasicShapes/Cube.Cube"), "Engine cube is missing")
    floor = require(
        actors.spawn_actor_from_object(cube, unreal.Vector(0.0, 0.0, -70.0), unreal.Rotator()),
        "Failed to spawn floor",
    )
    label(floor, "NDC_Batch_Demo_Floor", "ArmyVFXBatchDemo/Stage")
    floor.set_actor_scale3d(unreal.Vector(30.0, 18.0, 0.12))
    floor_component = floor.get_component_by_class(unreal.StaticMeshComponent)
    grid_material = unreal.load_asset("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")
    if floor_component and grid_material:
        floor_component.set_material(0, grid_material)

    directional = require(
        actors.spawn_actor_from_class(
            unreal.DirectionalLight,
            unreal.Vector(0.0, 0.0, 1400.0),
            unreal.Rotator(-48.0, -32.0, 0.0),
        ),
        "Failed to spawn DirectionalLight",
    )
    label(directional, "NDC_Batch_DirectionalLight", "ArmyVFXBatchDemo/Lighting")
    directional_component = directional.get_component_by_class(unreal.DirectionalLightComponent)
    if directional_component:
        directional_component.set_editor_property("intensity", 5.0)

    skylight = require(
        actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0.0, 0.0, 700.0)),
        "Failed to spawn SkyLight",
    )
    label(skylight, "NDC_Batch_SkyLight", "ArmyVFXBatchDemo/Lighting")
    skylight_component = skylight.get_component_by_class(unreal.SkyLightComponent)
    if skylight_component:
        skylight_component.set_editor_property("intensity", 1.25)

    atmosphere = require(
        actors.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector()),
        "Failed to spawn SkyAtmosphere",
    )
    label(atmosphere, "NDC_Batch_SkyAtmosphere", "ArmyVFXBatchDemo/Lighting")

    require(
        unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH),
        f"Failed to save Demo map: {LEVEL_PATH}",
    )

    level_actors = actors.get_all_level_actors()
    labels = sorted(actor.get_actor_label() for actor in level_actors)
    required_labels = {
        "ArmyVFX_NDC_SpawnBatch_Actor",
        "NDC_Batch_Demo_Floor",
        "NDC_Batch_DirectionalLight",
        "NDC_Batch_SkyLight",
        "NDC_Batch_SkyAtmosphere",
    }
    missing = sorted(required_labels - set(labels))
    require(not missing, f"NDC batch Demo is missing actors: {missing}")

    result = {
        "success": True,
        "level": LEVEL_PATH,
        "effect_actor": "ArmyVFX_NDC_SpawnBatch_Actor",
        "effect_actor_class": DEMO_CLASS,
        "actor_count": len(level_actors),
        "actors": labels,
        "source_or_exact_preview_actors": 0,
        "native_effect_count": 8,
        "contexts_per_effect": 4,
        "spawn_batch_calls_per_effect": 1,
    }
    VALIDATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    VALIDATION_PATH.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log(f"[ArmyVFXNDCBatchDemoCreate] {json.dumps(result, ensure_ascii=False)}")


if __name__ == "__main__":
    main()
