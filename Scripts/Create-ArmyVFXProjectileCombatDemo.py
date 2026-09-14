"""Create the ArmyVFX 500-unit projectile combat acceptance level.

The level contains only presentation infrastructure and Batch FX renderer actors.
MassUnitInHere spawners are intentionally placed afterwards through LandmarkSystemMCP
so that the complete acceptance path remains auditable.
"""

from __future__ import annotations

import json
from pathlib import Path

import unreal


LEVEL_PATH = (
    "/Game/ArmyVFX/WinyunqRelease/ProjectileCombat/Demo/"
    "L_ArmyVFX_ProjectileCombat_500"
)
VALIDATION_PATH = Path(
    "D:/UE5Project/Winyunq/Saved/MassBattleEditorMCP/Validation/"
    "ArmyVFX_ProjectileCombat_LevelCreate.json"
)

RENDERER_CLASSES = (
    # Built-in MassBattle projectile bodies/tracers used by Bullet and CannonBall.
    "/MassBattle/Demo/Projectile/Batched/Bullet/Renderer/"
    "BP_FxRenderer_Bullet.BP_FxRenderer_Bullet_C",
    "/MassBattle/Demo/Projectile/Batched/CannonBall/Renderer/"
    "BP_FxRenderer_CannonBall.BP_FxRenderer_CannonBall_C",
    # ArmyVFX source-faithful MassBattleFrame Batch FX used by the combat configs.
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_RocketEngine1_Batch.BP_FxRenderer_RocketEngine1_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_RocketStart_Batch.BP_FxRenderer_RocketStart_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_ArtySplash_Batch.BP_FxRenderer_ArtySplash_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_TankSplash_Batch.BP_FxRenderer_TankSplash_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_MuzzleFlashArty_Batch.BP_FxRenderer_MuzzleFlashArty_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_MuzzleFlashTankMainGun_Batch."
    "BP_FxRenderer_MuzzleFlashTankMainGun_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_MuzzleFlashAPC_Batch.BP_FxRenderer_MuzzleFlashAPC_Batch_C",
    "/Game/ArmyVFX/WinyunqRelease/Renderers/"
    "BP_FxRenderer_ShellsAPC_Batch.BP_FxRenderer_ShellsAPC_Batch_C",
)


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def label(actor, name, folder):
    actor.set_actor_label(name)
    actor.set_folder_path(folder)
    return actor


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


def spawn_asset(actor_subsystem, asset_path, name, location, scale, folder):
    asset = require(unreal.load_asset(asset_path), f"Asset not found: {asset_path}")
    actor = require(
        actor_subsystem.spawn_actor_from_object(asset, location, unreal.Rotator()),
        f"Failed to spawn asset: {asset_path}",
    )
    actor.set_actor_scale3d(scale)
    return label(actor, name, folder)


def repair_existing_presentation():
    """Repair deterministic presentation transforms without touching combat actors."""
    world = require(
        unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH),
        f"Failed to load existing map: {LEVEL_PATH}",
    )
    actor_subsystem = require(
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem),
        "EditorActorSubsystem is unavailable",
    )
    actors = actor_subsystem.get_all_level_actors()
    by_label = {actor.get_actor_label(): actor for actor in actors}

    camera = require(
        by_label.get("ProjectileCombat_OverviewCamera"),
        "Existing level is missing ProjectileCombat_OverviewCamera",
    )
    camera.set_actor_location(unreal.Vector(0.0, -10500.0, 7200.0), False, False)
    camera.set_actor_rotation(
        unreal.Rotator(pitch=-34.0, yaw=90.0, roll=0.0),
        False,
    )

    player_start = require(
        by_label.get("ProjectileCombat_PlayerStart"),
        "Existing level is missing ProjectileCombat_PlayerStart",
    )
    player_start.set_actor_location(unreal.Vector(0.0, -10000.0, 6500.0), False, False)
    player_start.set_actor_rotation(
        unreal.Rotator(pitch=-32.0, yaw=90.0, roll=0.0),
        False,
    )

    directional = require(
        by_label.get("ProjectileCombat_DirectionalLight"),
        "Existing level is missing ProjectileCombat_DirectionalLight",
    )
    directional.set_actor_rotation(
        unreal.Rotator(pitch=-48.0, yaw=-35.0, roll=0.0),
        False,
    )

    require(
        unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH),
        f"Failed to save repaired map: {LEVEL_PATH}",
    )
    result = {
        "success": True,
        "operation": "repair_existing_presentation",
        "level": LEVEL_PATH,
        "camera_rotation": {"pitch": -34.0, "yaw": 90.0, "roll": 0.0},
        "player_start_rotation": {"pitch": -32.0, "yaw": 90.0, "roll": 0.0},
        "directional_light_rotation": {"pitch": -48.0, "yaw": -35.0, "roll": 0.0},
        "actor_count": len(actors),
    }
    VALIDATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    VALIDATION_PATH.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    unreal.log(f"[ArmyVFXProjectileCombat] {json.dumps(result, ensure_ascii=False)}")


def main():
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        repair_existing_presentation()
        return

    level_folder = "/Game/ArmyVFX/WinyunqRelease/ProjectileCombat/Demo"
    if not unreal.EditorAssetLibrary.does_directory_exist(level_folder):
        require(
            unreal.EditorAssetLibrary.make_directory(level_folder),
            f"Failed to create {level_folder}",
        )

    world = require(
        unreal.EditorLoadingAndSavingUtils.new_blank_map(False),
        "Failed to create blank map",
    )
    actor_subsystem = require(
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem),
        "EditorActorSubsystem is unavailable",
    )
    world_settings = require(world.get_world_settings(), "WorldSettings is unavailable")
    world_settings.set_editor_property("default_game_mode", unreal.GameModeBase.static_class())

    floor = spawn_asset(
        actor_subsystem,
        "/Engine/BasicShapes/Cube.Cube",
        "ProjectileCombat_Floor",
        unreal.Vector(0.0, 0.0, -55.0),
        unreal.Vector(180.0, 180.0, 0.5),
        "ArmyVFXProjectileCombat/Stage",
    )
    floor_component = floor.get_component_by_class(unreal.StaticMeshComponent)
    grid_material = unreal.load_asset(
        "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"
    )
    if floor_component and grid_material:
        floor_component.set_material(0, grid_material)

    renderer_labels = []
    for index, class_path in enumerate(RENDERER_CLASSES):
        short_name = class_path.rsplit("/", 1)[-1].split(".", 1)[0]
        renderer = spawn_class(
            actor_subsystem,
            class_path,
            f"Renderer_{index:02d}_{short_name}",
            unreal.Vector(0.0, 0.0, -500.0 - index * 10.0),
            folder="ArmyVFXProjectileCombat/BatchRenderers",
        )
        renderer_labels.append(renderer.get_actor_label())

    directional = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.DirectionalLight,
                unreal.Vector(0.0, 0.0, 6000.0),
                unreal.Rotator(pitch=-48.0, yaw=-35.0, roll=0.0),
            ),
            "Failed to spawn DirectionalLight",
        ),
        "ProjectileCombat_DirectionalLight",
        "ArmyVFXProjectileCombat/Lighting",
    )
    directional_component = directional.get_component_by_class(
        unreal.DirectionalLightComponent
    )
    if directional_component:
        directional_component.set_editor_property("intensity", 5.0)

    skylight = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.SkyLight,
                unreal.Vector(0.0, 0.0, 3000.0),
            ),
            "Failed to spawn SkyLight",
        ),
        "ProjectileCombat_SkyLight",
        "ArmyVFXProjectileCombat/Lighting",
    )
    skylight_component = skylight.get_component_by_class(unreal.SkyLightComponent)
    if skylight_component:
        skylight_component.set_editor_property("intensity", 1.2)

    label(
        require(
            actor_subsystem.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector()),
            "Failed to spawn SkyAtmosphere",
        ),
        "ProjectileCombat_SkyAtmosphere",
        "ArmyVFXProjectileCombat/Lighting",
    )

    camera = label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.CameraActor,
                unreal.Vector(0.0, -10500.0, 7200.0),
                unreal.Rotator(pitch=-34.0, yaw=90.0, roll=0.0),
            ),
            "Failed to spawn CameraActor",
        ),
        "ProjectileCombat_OverviewCamera",
        "ArmyVFXProjectileCombat/Camera",
    )
    camera_component = camera.get_component_by_class(unreal.CameraComponent)
    if camera_component:
        camera_component.set_editor_property("field_of_view", 65.0)
    try:
        camera.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    except Exception as exc:
        unreal.log_warning(f"Could not auto-activate overview camera: {exc}")

    label(
        require(
            actor_subsystem.spawn_actor_from_class(
                unreal.PlayerStart,
                unreal.Vector(0.0, -10000.0, 6500.0),
                unreal.Rotator(pitch=-32.0, yaw=90.0, roll=0.0),
            ),
            "Failed to spawn PlayerStart",
        ),
        "ProjectileCombat_PlayerStart",
        "ArmyVFXProjectileCombat/Camera",
    )

    require(
        unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL_PATH),
        f"Failed to save map: {LEVEL_PATH}",
    )
    require(
        unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH),
        "Saved level is not visible to the Asset Registry",
    )

    actors = actor_subsystem.get_all_level_actors()
    labels = sorted(actor.get_actor_label() for actor in actors)
    result = {
        "success": True,
        "level": LEVEL_PATH,
        "actor_count_before_landmark_placement": len(actors),
        "renderer_count": len(renderer_labels),
        "renderer_actors": renderer_labels,
        "actors": labels,
        "next_step": "Place four 125-unit MassUnitInHere armies through LandmarkSystemMCP.",
    }
    VALIDATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    VALIDATION_PATH.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    unreal.log(f"[ArmyVFXProjectileCombat] {json.dumps(result, ensure_ascii=False)}")


if __name__ == "__main__":
    main()
