"""Unreal editor entry: export effective BaseColor once per texture, not per team/unit."""
import hashlib
import json
from pathlib import Path
import unreal


def export_materials(materials, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    me = unreal.MaterialEditingLibrary
    textures, bindings = {}, []
    for material in dict.fromkeys(materials):
        mi = unreal.load_asset(material) if isinstance(material, str) else material
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            raise TypeError('Select material instances: ' + str(mi))
        texture = None
        for parameter in ('BaseColorTex', 'BaseColorTexture', 'baseColorTexture'):
            for association in (unreal.MaterialParameterAssociation.GLOBAL_PARAMETER,
                                unreal.MaterialParameterAssociation.LAYER_PARAMETER):
                candidate = me.get_material_instance_texture_parameter_value(mi, parameter, association)
                if candidate and candidate.get_path_name().startswith('/Game/'):
                    texture = candidate
                    break
            if texture:
                break
        assert texture, 'No project BaseColor found: ' + mi.get_path_name() + ' parameters=' + str(mi.get_editor_property('texture_parameter_values'))
        path = texture.get_path_name()
        key = texture.get_name() + '_' + hashlib.sha256(path.encode()).hexdigest()[:10]
        if key not in textures:
            filename = output / (key + '.tga')
            task = unreal.AssetExportTask()
            task.object = texture
            task.filename = str(filename)
            task.automated = True
            task.prompt = False
            task.replace_identical = True
            task.exporter = unreal.TextureExporterTGA()
            assert unreal.Exporter.run_asset_export_task(task), path
            textures[key] = dict(id=key, texture=path, source=str(filename))
        bindings.append(dict(material=mi.get_path_name(), texture_id=key, base_parameter=parameter))
    result = dict(textures=list(textures.values()), bindings=bindings)
    (output / 'basecolors.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def export_selected(output):
    materials = []
    for asset in unreal.EditorUtilityLibrary.get_selected_assets():
        if isinstance(asset, unreal.StaticMesh):
            materials.extend(slot.material_interface for slot in asset.static_materials)
        elif isinstance(asset, unreal.SkeletalMesh):
            materials.extend(slot.material_interface for slot in asset.materials)
        else:
            materials.append(asset)
    if not materials:
        raise ValueError('Select a mesh or skin material instance first')
    return export_materials(materials, output)
