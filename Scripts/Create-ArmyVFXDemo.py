"""Create the non-destructive ArmyVFX source-faithful mapping Demo level."""

from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


LEVEL_PATH = "/Game/ArmyVFX/MassBattleConverted/Demo/L_ArmyVFX_AllEffects_Demo"
DEMO_CLASS = "/Script/MassBattleEditorMCP.MassBattleArmyVFXDemoActor"
VALIDATION_PATH = Path(
    "D:/UE5Project/Winyunq/Saved/MassBattleEditorMCP/Validation/"
    "ArmyVFX_DemoLevel_Create.json"
)


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def label(actor, name, folder):
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    return actor


def spawn_class(actor_subsystem, class_path, name, location, rotation=None, folder="ArmyVFXDemo"):
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


def spawn_asset(
    actor_subsystem,
    asset_path,
    name,
    location,
    rotation=None,
    scale=None,
    folder="ArmyVFXDemo/Stage",
):
    asset = require(unreal.load_asset(asset_path), f"Asset not found: {asset_path}")
    actor = require(
        actor_subsystem.spawn_actor_from_object(
            asset,
            location,
            rotation or unreal.Rotator(),
        ),
        f"Failed to spawn asset: {asset_path}",
    )
    actor.set_actor_scale3d(scale or unreal.Vector(1.0, 1.0, 1.0))
    return label(actor, name, folder)


def main():
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        raise RuntimeError(f"Refusing to overwrite existing Demo level: {LEVEL_PATH}")

    demo_folder = "/Game/ArmyVFX/MassBattleConverted/Demo"
    if not unreal.EditorAssetLibrary.does_directory_exist(demo_folder):
        require(unreal.EditorAssetLibrary.make_directory(demo_folder), "Failed to create Demo folder")

    world = require(unreal.EditorLoadingAndSavingUtils.new_blank_map(False), "Failed to create blank map")
    actor_subsystem = require(
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem),
        "EditorActorSubsystem is unavailable",
    )
    world_settings = require(world.get_world_settings(), "WorldSettings is unavailable")
    world_settings.set_editor_property("default_game_mode", unreal.GameModeBase.static_class())

    harness = spawn_class(
        actor_subsystem,
        DEMO_CLASS,
        "ArmyVFX_DemoHarness_27_SourceExact_8Native",
        unreal.Vector(),
        folder="ArmyVFXDemo/Harness",
    )
    harness.set_editor_property("auto_play", True)
    harness.set_editor_property("initial_delay", 2.0)
    harness.set_editor_property("cycle_interval", 7.0)
    harness.set_editor_property("current_effect_index", 0)
    harness.set_editor_property("native_grid_side", 2)
    harness.set_editor_property("native_grid_spacing", 260.0)

    floor = spawn_asset(
        actor_subsystem,
        "/Engine/BasicShapes/Cube.Cube",
        "DemoFloor",
        unreal.Vector(0.0, 0.0, -65.0),
        scale=unreal.Vector(45.0, 32.0, 0.12),
    )
    floor_component = floor.get_component_by_class(unreal.StaticMeshComponent)
    grid_material = unreal.load_asset(
        "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"
    )
    if floor_component and grid_material:
        floor_component.set_material(0, grid_material)

    for lane_name, y in (("Source", -900.0), ("Exact", 0.0), ("Native", 900.0)):
        pad = spawn_asset(
            actor_subsystem,
            "/Engine/BasicShapes/Cube.Cube",
            f"StagePad_{lane_name}",
            unreal.Vector(250.0, y, -5.0),
            scale=unreal.Vector(17.0, 6.5, 0.035),
        )
        pad_component = pad.get_component_by_class(unreal.StaticMeshComponent)
        if pad_component and grid_material:
            pad_component.set_material(0, grid_material)

        spawn_asset(
            actor_subsystem,
            "/Game/ArmyVFX/Mesh/SM_Tank.SM_Tank",
            f"ReferenceTank_{lane_name}",
            unreal.Vector(-80.0, y, 0.0),
            scale=unreal.Vector(0.34, 0.34, 0.34),
            folder="ArmyVFXDemo/References",
        )

    directional = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.DirectionalLight,
                unreal.Vector(0.0, 0.0, 1400.0),
                unreal.Rotator(-48.0, -32.0, 0.0),
            ),
            "Failed to spawn DirectionalLight",
        ),
        "DirectionalLight_Demo",
        "ArmyVFXDemo/Lighting",
    )
    directional_component = directional.get_component_by_class(unreal.DirectionalLightComponent)
    if directional_component:
        directional_component.set_editor_property("intensity", 5.0)

    skylight = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.SkyLight,
                unreal.Vector(0.0, 0.0, 700.0),
            ),
            "Failed to spawn SkyLight",
        ),
        "SkyLight_Demo",
        "ArmyVFXDemo/Lighting",
    )
    skylight_component = skylight.get_component_by_class(unreal.SkyLightComponent)
    if skylight_component:
        skylight_component.set_editor_property("intensity", 1.25)

    label(
        require(
            actor_subsystem.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector()),
            "Failed to spawn SkyAtmosphere",
        ),
        "SkyAtmosphere_Demo",
        "ArmyVFXDemo/Lighting",
    )
    fog = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.ExponentialHeightFog,
                unreal.Vector(0.0, 0.0, -100.0),
            ),
            "Failed to spawn ExponentialHeightFog",
        ),
        "HeightFog_Demo",
        "ArmyVFXDemo/Lighting",
    )
    fog_component = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    if fog_component:
        fog_component.set_editor_property("fog_density", 0.006)

    require(
        unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH),
        f"Failed to save Demo map: {LEVEL_PATH}",
    )
    require(
        unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH),
        "Saved Demo map is not visible to the Asset Registry",
    )

    actors = actor_subsystem.get_all_level_actors()
    labels = sorted(actor.get_actor_label() for actor in actors)
    required_labels = {
        harness.get_actor_label(),
        "DemoFloor",
        "StagePad_Source",
        "StagePad_Exact",
        "StagePad_Native",
        "ReferenceTank_Source",
        "ReferenceTank_Exact",
        "ReferenceTank_Native",
        "DirectionalLight_Demo",
        "SkyLight_Demo",
        "SkyAtmosphere_Demo",
    }
    missing = sorted(required_labels - set(labels))
    require(not missing, f"Demo map is missing actors: {missing}")

    result = {
        "success": True,
        "level": LEVEL_PATH,
        "demo_class": DEMO_CLASS,
        "actor_count": len(actors),
        "actors": labels,
        "effect_count": 27,
        "native_count": 8,
        "controls": {
            "previous": "Left/A",
            "next": "Right/D",
            "replay": "Space/R",
            "all_native": "N",
        },
    }
    VALIDATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    VALIDATION_PATH.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    unreal.log(f"[ArmyVFXDemoCreate] {json.dumps(result, ensure_ascii=False)}")


if __name__ == "__main__":
    main()
