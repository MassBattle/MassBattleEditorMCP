"""Run in UE Python; uses an isolated /Game/__MCPSourceActorTest fixture."""
import json
import unreal

API = unreal.MassBattleUnitMCPApi
ROOT = '/Game/__MCPSourceActorTest'
SOURCE = ROOT + '/BP_Source'
OUTPUT = ROOT + '/DA_Unit'
MOVED = ROOT + '/Moved/DA_Unit'


def result(text):
    value = json.loads(text)
    assert value.get('success'), value
    return value


def read(path):
    return result(API.mcp_unit_get(path, '{"object":"Health","include_defaults":true}'))['Data']['Health']


def update(source):
    source.update()
    return result(source.get_editor_property('last_report'))


if not unreal.EditorAssetLibrary.does_asset_exist(SOURCE):
    result(API.mcp_unit_create(json.dumps({
        'asset_type': 'source_actor', 'asset_name': 'BP_Source', 'package_path': ROOT,
        'export_path': OUTPUT, 'unit_data': {'Health': {'Current': 100, 'Maximum': 300}}
    }), True))
    bp = unreal.load_asset(SOURCE)
    source = unreal.get_default_object(bp.generated_class())
    assert source.get_editor_property('exported_unit') is None
    assert update(source)['created']
    assert read(OUTPUT)['Current'] == 100
    result(API.mcp_unit_merge_update(OUTPUT, '{"Health":{"Current":200}}', True))
    assert not update(source)['applied_diff']
    assert read(OUTPUT)['Current'] == 200
    result(API.mcp_unit_merge_update(SOURCE, '{"Health":{"Maximum":350}}', True))
    assert read(OUTPUT)['Current'] == 200
    assert read(OUTPUT)['Maximum'] == 350
    result(API.mcp_unit_merge_update(SOURCE, json.dumps({'export_path': MOVED}), True))
    assert source.get_editor_property('exported_unit').get_path_name() == MOVED + '.DA_Unit'
    assert read(MOVED)['Current'] == 200
    listing = result(API.mcp_unit_list(json.dumps({'roots': [ROOT], 'load_fields': False})))
    paths = [row['ObjectPath'] for row in listing['units']]
    assert SOURCE + '.BP_Source' in paths and MOVED + '.DA_Unit' in paths, paths
    unreal.log('SOURCE_ACTOR_PHASE1_OK: create, diff, external edit preservation, move, list')
else:
    # Run again in a fresh editor/commandlet to check persisted source baselines.
    bp = unreal.load_asset(SOURCE)
    source = unreal.get_default_object(bp.generated_class())
    assert not update(source)['applied_diff']
    assert read(MOVED)['Current'] == 200
    dependencies = unreal.AssetRegistryHelpers.get_asset_registry().get_dependencies(
        MOVED, unreal.AssetRegistryDependencyOptions(include_hard_package_references=True))
    assert SOURCE not in [str(path) for path in dependencies], dependencies
    result(API.mcp_unit_merge_update(SOURCE, '{"Health":{"Current":120}}', True))
    assert read(MOVED)['Current'] == 120
    # External ownership must block destruction of the pair.
    guard_path = ROOT + '/BP_Guard'
    result(API.mcp_unit_create(json.dumps({'asset_type': 'source_actor', 'unit_path': guard_path}), True))
    guard_bp = unreal.load_asset(guard_path)
    assert unreal.BlueprintEditorLibrary.add_member_variable(guard_bp, 'UnitReference',
        unreal.BlueprintEditorLibrary.get_object_reference_type(unreal.MassBattleAgentConfigDataAsset.static_class()))
    unreal.BlueprintEditorLibrary.compile_blueprint(guard_bp)
    guard = unreal.get_default_object(guard_bp.generated_class())
    guard.set_editor_property('UnitReference', source.get_editor_property('exported_unit'))
    unreal.EditorAssetLibrary.save_loaded_asset(guard_bp, only_if_is_dirty=False)
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT], True)
    plan = result(API.mcp_unit_delete(SOURCE, '{"mode":"hard","dry_run":true}'))
    assert not plan['applicable'], plan
    guard.set_editor_property('UnitReference', None)
    unreal.EditorAssetLibrary.save_loaded_asset(guard_bp, only_if_is_dirty=False)
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT], True)
    del guard, guard_bp
    result(API.mcp_unit_delete(guard_path, '{"mode":"hard","dry_run":false}'))
    del source, bp
    deleted = result(API.mcp_unit_delete(SOURCE, '{"mode":"hard","dry_run":false}'))
    assert deleted['deleted_count'] == 2, deleted
    assert not unreal.EditorAssetLibrary.does_asset_exist(SOURCE)
    assert not unreal.EditorAssetLibrary.does_asset_exist(MOVED)
    unreal.log('SOURCE_ACTOR_PHASE2_OK: reload, changed source wins, referencers, destroy pair')
