"""Create the clean 27/27 ArmyVFX MassBattleFrame Batch FX acceptance level.

The level contains one batch-only acceptance harness and 27 explicitly placed
AMassBattleFxRenderer Blueprint actors.  It contains no source Niagara actors,
no exact-copy preview components, and no unbatched effect actors.
"""

from __future__ import annotations

import json
from pathlib import Path

import unreal


LEVEL_PATH = "/Game/ArmyVFX/WinyunqRelease/Demo/L_ArmyVFX_FaithfulBatch_27"
DEMO_CLASS = "/Script/MassBattleEditorMCP.MassBattleArmyVFXDemoActor"
VALIDATION_PATH = Path(
    "D:/UE5Project/Winyunq/Saved/MassBattleEditorMCP/Validation/"
    "ArmyVFX_FaithfulBatch_Demo_Create.json"
)

RENDERERS = (
    (97, "BP_FxRenderer_ExplTank_Batch"),
    (98, "BP_FxRenderer_ExplTankTower_Batch"),
    (89, "BP_FxRenderer_FireTank_Batch"),
    (90, "BP_FxRenderer_TankFireShells_Batch"),
    (91, "BP_FxRenderer_AAGun_Batch"),
    (70, "BP_FxRenderer_AASplash_Batch"),
    (80, "BP_FxRenderer_ArtySplash_Batch"),
    (88, "BP_FxRenderer_HeliSplash_Batch"),
    (81, "BP_FxRenderer_TankSplash_Batch"),
    (82, "BP_FxRenderer_JetCountermeasures_Batch"),
    (92, "BP_FxRenderer_JetFireContinuous_Batch"),
    (74, "BP_FxRenderer_JetTrails74_Batch"),
    (95, "BP_FxRenderer_MuzzleFlashAPC_Batch"),
    (71, "BP_FxRenderer_MuzzleFlashArty_Batch"),
    (72, "BP_FxRenderer_MuzzleFlashSPG_Batch"),
    (73, "BP_FxRenderer_MuzzleFlashTankMainGun_Batch"),
    (96, "BP_FxRenderer_MuzzleFlashTankMG_Batch"),
    (93, "BP_FxRenderer_RocketEngine1_Batch"),
    (94, "BP_FxRenderer_RocketEngine2_Batch"),
    (75, "BP_FxRenderer_RocketSmoke1_75_V2_Batch"),
    (76, "BP_FxRenderer_RocketSmoke2_76_V2_Batch"),
    (99, "BP_FxRenderer_RocketStart_Batch"),
    (83, "BP_FxRenderer_StartFlash_Batch"),
    (84, "BP_FxRenderer_ShellsAPC_Batch"),
    (85, "BP_FxRenderer_ShellsArty_Batch"),
    (86, "BP_FxRenderer_ShellsTankMG_Batch"),
    (87, "BP_FxRenderer_SmokeScreenAPC_Batch"),
)


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def label(actor, name, folder):
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    return actor


def renderer_class_path(name: str) -> str:
    return (
        f"/Game/ArmyVFX/WinyunqRelease/Renderers/{name}."
        f"{name}_C"
    )


def spawn_class(actor_subsystem, class_path, name, location, rotation=None, folder=""):
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
        raise RuntimeError(
            f"Refusing to overwrite existing acceptance level: {LEVEL_PATH}. "
            "Delete it explicitly before rebuilding."
        )

    folder = "/Game/ArmyVFX/WinyunqRelease/Demo"
    if not unreal.EditorAssetLibrary.does_directory_exist(folder):
        require(unreal.EditorAssetLibrary.make_directory(folder), f"Failed to create {folder}")

    world = require(unreal.EditorLoadingAndSavingUtils.new_blank_map(False), "Failed to create blank map")
    actors = require(
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem),
        "EditorActorSubsystem unavailable",
    )
    world_settings = require(world.get_world_settings(), "WorldSettings unavailable")
    world_settings.set_editor_property("default_game_mode", unreal.GameModeBase.static_class())

    demo = spawn_class(
        actors,
        DEMO_CLASS,
        "ArmyVFX_FaithfulBatch_27_Harness",
        unreal.Vector(),
        folder="ArmyVFXFaithfulBatch/Acceptance",
    )
    demo.set_editor_property("warm_up_renderers", True)
    demo.set_editor_property("auto_play_all", True)
    demo.set_editor_property("initial_delay", 3.0)
    demo.set_editor_property("replay_interval", 12.0)
    demo.set_editor_property("column_spacing", 1250.0)
    demo.set_editor_property("row_spacing", 1050.0)

    renderer_records = []
    for index, (expected_subtype, name) in enumerate(RENDERERS):
        class_path = renderer_class_path(name)
        renderer = spawn_class(
            actors,
            class_path,
            f"Renderer_{index + 1:02d}_SubType_{expected_subtype}_{name}",
            unreal.Vector(2100.0, 0.0, -2500.0 - index * 10.0),
            folder="ArmyVFXFaithfulBatch/PlacedRenderers",
        )
        actual_subtype = int(renderer.get_editor_property("sub_type"))
        system = renderer.get_editor_property("niagara_system_asset")
        require(
            actual_subtype == expected_subtype,
            f"{name}: expected SubType {expected_subtype}, got {actual_subtype}",
        )
        require(system, f"{name}: NiagaraSystemAsset is empty")
        renderer_records.append(
            {
                "label": renderer.get_actor_label(),
                "class": class_path,
                "subtype": actual_subtype,
                "system": system.get_path_name(),
            }
        )

    cube = require(unreal.load_asset("/Engine/BasicShapes/Cube.Cube"), "Engine cube missing")
    floor = require(
        actors.spawn_actor_from_object(cube, unreal.Vector(2100.0, 0.0, -70.0), unreal.Rotator()),
        "Failed to spawn floor",
    )
    label(floor, "FaithfulBatch_Gallery_Floor", "ArmyVFXFaithfulBatch/Stage")
    floor.set_actor_scale3d(unreal.Vector(80.0, 80.0, 0.2))
    floor_component = floor.get_component_by_class(unreal.StaticMeshComponent)
    grid_material = unreal.load_asset("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")
    if floor_component and grid_material:
        floor_component.set_material(0, grid_material)

    directional = require(
        actors.spawn_actor_from_class(
            unreal.DirectionalLight,
            unreal.Vector(2100.0, 0.0, 5000.0),
            unreal.Rotator(pitch=-48.0, yaw=-35.0, roll=0.0),
        ),
        "Failed to spawn DirectionalLight",
    )
    label(directional, "FaithfulBatch_DirectionalLight", "ArmyVFXFaithfulBatch/Lighting")
    directional_component = directional.get_component_by_class(unreal.DirectionalLightComponent)
    if directional_component:
        directional_component.set_editor_property("intensity", 5.0)

    skylight = require(
        actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(2100.0, 0.0, 2500.0)),
        "Failed to spawn SkyLight",
    )
    label(skylight, "FaithfulBatch_SkyLight", "ArmyVFXFaithfulBatch/Lighting")
    skylight_component = skylight.get_component_by_class(unreal.SkyLightComponent)
    if skylight_component:
        skylight_component.set_editor_property("intensity", 1.3)

    atmosphere = require(
        actors.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector()),
        "Failed to spawn SkyAtmosphere",
    )
    label(atmosphere, "FaithfulBatch_SkyAtmosphere", "ArmyVFXFaithfulBatch/Lighting")

    require(
        unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH),
        f"Failed to save map: {LEVEL_PATH}",
    )
    require(
        unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH),
        "Saved acceptance level is not visible to the Asset Registry",
    )

    level_actors = actors.get_all_level_actors()
    labels = sorted(actor.get_actor_label() for actor in level_actors)
    result = {
        "success": True,
        "level": LEVEL_PATH,
        "effect_entrypoints": 27,
        "placed_renderer_count": len(renderer_records),
        "source_niagara_actor_count": 0,
        "unbatched_effect_actor_count": 0,
        "demo_harness": demo.get_actor_label(),
        "renderers": renderer_records,
        "actor_count": len(level_actors),
        "actors": labels,
    }
    VALIDATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    VALIDATION_PATH.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log(f"[ArmyVFXFaithfulBatchDemoCreate] {json.dumps(result, ensure_ascii=False)}")


if __name__ == "__main__":
    main()
