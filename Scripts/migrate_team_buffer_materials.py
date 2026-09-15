"""Replace the obsolete texture lookup in the known team-color materials."""
import unreal, json
from pathlib import Path
root = Path(unreal.Paths.project_dir()).resolve()
me = unreal.MaterialEditingLibrary
ea = unreal.EditorAssetLibrary
report = json.loads((root / 'Saved/AutoTeamMask/applied.json').read_text())
paths = [row['material'] for row in report['compiled']]
paths.append('/MassBattleEditorMCP/Demo/TeamColorTank/M_SourceTeamColor')
for path in paths:
    mat = unreal.load_asset(path)
    nodes = me.get_material_expressions(mat)
    lookup = next(n for n in nodes if n.get_editor_property('desc') in ('Team palette texel center', 'Minimap team buffer'))
    has_preview = any(str(i.get_editor_property('input_name')) == 'Preview' for i in lookup.get_editor_property('inputs'))
    team = '(Preview>=0?Preview:Team)' if has_preview else 'Team'
    lookup.set_editor_property('code', 'uint i=(uint)max(0.0,floor('+team+'+0.5));\nreturn i<Scene.MassBattleTeamColors.Count ? float4(Scene.MassBattleTeamColors.Colors[i].rgb,1) : float4(0,0,0,0);')
    lookup.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    lookup.set_editor_property('desc', 'Minimap team buffer')
    tint = next(n for n in nodes if n.get_editor_property('desc') == 'Generic team paint')
    tint.set_editor_property('code', 'float light=dot(Base,float3(0.2126,0.7152,0.0722));\nreturn lerp(Base,TeamColor.rgb*light,saturate(Strength)*saturate(Mask)*TeamColor.a);')
    assert me.connect_material_expressions(lookup, '', tint, 'TeamColor')
    sample = next((n for n in nodes if n.get_editor_property('desc') == 'Common team palette'), None)
    if sample: me.delete_material_expression(mat, sample)
    errors = list(me.recompile_material(mat))
    assert not errors, (path, errors)
    assert ea.save_loaded_asset(mat, False)
unreal.log('TEAM_BUFFER_MATERIALS_MIGRATED')

bp = unreal.load_asset('/MassBattleEditorMCP/Demo/TeamColorTank/BP_TankSourceDemo')
source = unreal.get_default_object(bp.generated_class())
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
ea.save_loaded_asset(bp, False)
mi = unreal.load_asset('/MassBattleEditorMCP/Demo/TeamColorTank/MI_TankTeamPreview')
values = mi.get_editor_property('texture_parameter_values')
mi.set_editor_property('texture_parameter_values', [v for v in values if str(v.parameter_info.name) != 'TeamPalette'])
me.update_material_instance(mi)
ea.save_loaded_asset(mi, False)
unreal.log('TEAM_BUFFER_MIGRATION_COMPLETE')
