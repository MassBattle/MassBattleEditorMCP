"""Read back the packaged source demo in a fresh UE editor/commandlet."""
import unreal

path = '/MassBattleEditorMCP/Demo/TeamColorTank/BP_TankSourceDemo'
bp = unreal.load_asset(path)
assert bp
source = unreal.get_default_object(bp.generated_class())
assert source.preview_team_index == 2
assert not source.get_editor_property('preview_team_color')
assert source.team_mask and source.team_material
assert source.body.static_mesh and source.turret.static_mesh and source.barrel.static_mesh
assert source.body.get_material(1) and source.body.get_material(2)
subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actor = subsystem.spawn_actor_from_class(bp.generated_class(), unreal.Vector())
assert actor.muzzle.get_attach_parent() == actor.barrel_pivot
assert actor.barrel_pivot.get_attach_parent() == actor.turret_pivot
actor.apply_team_color()
material = actor.body.get_material(0)
assert material.get_scalar_parameter_value('TeamTintStrength') == 1
assert material.get_scalar_parameter_value('PreviewTeamIndex') == 2
assert material.get_texture_parameter_value('TeamMask') == source.team_mask
actor.show_original_color()
assert actor.body.get_material(0).get_scalar_parameter_value('TeamTintStrength') == 0
if hasattr(unreal, 'MBSTSingleTurretEditorLibrary'):
    authoring, message = unreal.MBSTSingleTurretEditorLibrary.add_transient_authoring_component(actor)
    assert authoring.resolve_turret_yaw_pivot() == actor.turret_pivot
    assert authoring.resolve_barrel_pitch_pivot() == actor.barrel_pivot
    assert authoring.resolve_muzzle() == actor.muzzle
before = actor.muzzle.get_world_location()
actor.turret_pivot.set_relative_rotation(unreal.Rotator(0, 35, 0), False, False)
assert (actor.muzzle.get_world_location() - before).length() > 1
subsystem.destroy_actor(actor)
unreal.log('SOURCE_PREVIEW_RELOAD_OK')
