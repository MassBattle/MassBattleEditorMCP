"""Bind offline dominant-colour masks through the existing MassBattle DP0.W ABI.

Run in the editor after regenerating generic models. No runtime unit updates.
Unit materials read the existing minimap TeamColorsBuffer through Scene uniforms (UE 5.8).
"""
import json
import shutil
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
OUT = ROOT / 'Saved/AutoTeamMask'
SHARED = '/Game/Unit/Shared/Materials/TeamColor'
ME = unreal.MaterialEditingLibrary
EA = unreal.EditorAssetLibrary




def backup(asset):
    package = asset.get_path_name().split('.')[0]
    if package.startswith('/Game/'):
        source = ROOT / 'Content' / (package[6:] + '.uasset')
        dest = OUT / 'Before' / (package[6:] + '.uasset')
        if source.exists() and not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)


def load(path):
    obj = unreal.load_asset(path)
    assert obj, path
    return obj



def node(mat, cls, label):
    found = next((x for x in ME.get_material_expressions(mat) if x.get_editor_property('desc') == label), None)
    if found:
        return found
    obj = ME.create_material_expression(mat, cls)
    obj.set_editor_property('desc', label)
    return obj


def custom(mat, label, names, code, output):
    obj = node(mat, unreal.MaterialExpressionCustom, label)
    if [str(x.get_editor_property('input_name')) for x in obj.get_editor_property('inputs')] != names:
        inputs = []
        for name in names:
            entry = unreal.CustomInput()
            entry.set_editor_property('input_name', name)
            inputs.append(entry)
        obj.set_editor_property('inputs', inputs)
    obj.set_editor_property('code', code)
    obj.set_editor_property('output_type', output)
    return obj


def connect(source, output, target, pin):
    assert ME.connect_material_expressions(source, output, target, pin), (source, output, target, pin)


def configure_base(mat, texture, mask_texture):
    backup(mat)
    expressions = ME.get_material_expressions(mat)
    decoded = next((x for x in expressions if isinstance(x, unreal.MaterialExpressionCustom)
                    and 'MassBattle_UnpackDP0W' in x.get_editor_property('code')), None)
    if decoded and decoded.get_editor_property('desc') != 'MassBattle existing team decode':
        team_output = 'Team' if 'Team' in ME.get_material_expression_output_names(decoded) else ''
    else:
        dp = node(mat, unreal.MaterialExpressionDynamicParameter, 'MassBattle existing packed DP0')
        dp.set_editor_property('parameter_index', 0)
        decoded = custom(mat, 'MassBattle existing team decode', ['Packed'],
            'float Team,Dissolve,LODIndex,DrawLOD,BeingSelect,Selected;\n'
            'MassBattle_UnpackDP0W(Packed,Team,Dissolve,LODIndex,DrawLOD,BeingSelect,Selected);\nreturn Team;',
            unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        decoded.set_editor_property('include_file_paths', ['/MassBattle/MassBattle_MaterialDecode.ush'])
        connect(dp, ME.get_material_expression_output_names(dp)[3], decoded, 'Packed')
        team_output = ''
    preview = node(mat, unreal.MaterialExpressionScalarParameter, 'Source preview team index')
    preview.set_editor_property('parameter_name', 'PreviewTeamIndex')
    preview.set_editor_property('default_value', -1.0)
    sample = custom(mat, 'Minimap team buffer', ['Team', 'Preview'],
        'uint i=(uint)max(0.0,floor((Preview>=0?Preview:Team)+0.5));\n'
        'return i<Scene.MassBattleTeamColors.Count ? float4(Scene.MassBattleTeamColors.Colors[i].rgb,1) : float4(0,0,0,0);',
        unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    connect(decoded, team_output, sample, 'Team')
    connect(preview, '', sample, 'Preview')
    strength = node(mat, unreal.MaterialExpressionScalarParameter, 'Team paint strength')
    strength.set_editor_property('parameter_name', 'TeamTintStrength')
    strength.set_editor_property('default_value', 1.0)
    mask = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, 'Automatic dominant colour mask')
    mask.set_editor_property('parameter_name', 'TeamMask')
    mask.set_editor_property('texture', mask_texture)
    mask.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    mask.set_editor_property('const_coordinate', 0)
    tint = custom(mat, 'Generic team paint', ['Base', 'TeamColor', 'Strength', 'Mask'],
        'float light=dot(Base,float3(0.2126,0.7152,0.0722));\n'
        'return lerp(Base,TeamColor.rgb*light,saturate(Strength)*saturate(Mask)*TeamColor.a);',
        unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    connect(mask, 'R', tint, 'Mask')
    connect(sample, '', tint, 'TeamColor')
    connect(strength, '', tint, 'Strength')
    # Reuse the VAT switch at its existing position, preserving all other outputs.
    switch = next((x for x in expressions if isinstance(x, unreal.MaterialExpressionStaticSwitchParameter)
                   and str(x.get_editor_property('parameter_name')) == 'UseTeamColorTint'), None)
    direct = not mat.get_editor_property('use_material_attributes')
    if direct:
        base = ME.get_material_property_input_node(mat, unreal.MaterialProperty.MP_BASE_COLOR)
        output = ME.get_material_property_input_node_output_name(mat, unreal.MaterialProperty.MP_BASE_COLOR)
        if switch is None:
            switch = node(mat, unreal.MaterialExpressionStaticSwitchParameter, 'Generic team color switch')
            switch.set_editor_property('parameter_name', 'UseTeamColorTint')
        if base != switch:
            connect(base, output, tint, 'Base')
            connect(base, output, switch, 'False')
        connect(tint, '', switch, 'True')
        switch.set_editor_property('default_value', True)
        assert ME.connect_material_property(switch, '', unreal.MaterialProperty.MP_BASE_COLOR)
    else:
        assert switch, mat.get_path_name()
        base = ME.get_inputs_for_material_expression(mat, switch)[1]
        connect(base, '', tint, 'Base')
        connect(tint, '', switch, 'True')
        switch.set_editor_property('default_value', True)
    errors = list(ME.recompile_material(mat))
    assert not errors, (mat.get_path_name(), errors)
    assert EA.save_loaded_asset(mat, False)
    return {'material': mat.get_path_name(), 'compiler_errors': errors, 'saved': True}


def apply_masks(manifest):
    data = json.loads(Path(manifest).read_text(encoding='utf-8-sig'))
    OUT.mkdir(parents=True, exist_ok=True)
    lookup = None
    textures = {}
    for row in data['textures']:
        task = unreal.AssetImportTask()
        task.filename = str(ROOT / row['mask'])
        task.destination_path = SHARED + '/Masks'
        task.destination_name = row['id'] + '_TeamMask'
        task.automated = True
        task.replace_existing = True
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        tex = load(task.destination_path + '/' + task.destination_name)
        tex.set_editor_property('srgb', False)
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_GRAYSCALE)
        assert EA.save_loaded_asset(tex, False)
        textures[row['id']] = tex
    # Resolve only the supplied skin material references. No runtime unit changes.
    compiled, roots, bindings = [], {}, []
    for row in data['bindings']:
        mi = load(row['material'])
        base = mi.parent
        while isinstance(base, unreal.MaterialInstanceConstant):
            base = base.parent
        path = base.get_path_name().split('.')[0]
        if path not in roots:
            if path.startswith('/Game/'):
                target = base
            else:
                name = 'M_AutoTeam_' + base.get_name()
                target = unreal.load_asset(SHARED + '/' + name) or unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, SHARED, base)
            compiled.append(configure_base(target, lookup, textures[row['texture_id']]))
            roots[path] = target
        target = roots[path]
        # Plugin parents are directly inherited here; preserve project MI chains.
        if not path.startswith('/Game/'):
            assert isinstance(mi.parent, unreal.Material), mi.get_path_name()
            mi.set_editor_property('parent', target)
        ME.set_material_instance_texture_parameter_value(mi, 'TeamMask', textures[row['texture_id']])
        for assoc in (unreal.MaterialParameterAssociation.GLOBAL_PARAMETER, unreal.MaterialParameterAssociation.LAYER_PARAMETER):
            ME.set_material_instance_static_switch_parameter_value(mi, 'UseTeamColorTint', True, assoc, True)
        ME.update_material_instance(mi)
        assert EA.save_loaded_asset(mi, False)
        assert ME.get_material_instance_texture_parameter_value(mi, 'TeamMask') == textures[row['texture_id']]
        bindings.append(dict(material=mi.get_path_name(), mask=textures[row['texture_id']].get_path_name()))
    result = dict(compiled=compiled, bindings=bindings)
    (OUT / 'applied.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('Automatic masks bound:', len(bindings), 'skin materials')
    return result
