import json
import sys
import unittest
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

import MassBattleMcpServer as server


class FakeConnection:
    def __init__(self):
        self.calls = []

    async def send_command(self, command, params=None):
        params = params or {}
        self.calls.append((command, params))
        return {"success": True, "command": command, "params": params}


class ActorUnitAuthoringTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.previous_connection = server._unreal_connection
        self.fake = FakeConnection()
        server._unreal_connection = self.fake

    async def asyncTearDown(self):
        server._unreal_connection = self.previous_connection

    async def test_inspection_forwards_actor_and_options(self):
        await server.editor_inspect_actor_assembly(
            "/Game/Actors/BP_Infantry",
            {"component_overrides": [{"component": "Weapon_R"}]},
        )

        command, params = self.fake.calls[-1]
        self.assertEqual(command, "MCP_EditorInspectActorAssembly")
        self.assertEqual(params["ActorPath"], "/Game/Actors/BP_Infantry")
        self.assertEqual(
            json.loads(params["OptionsJson"]),
            {"component_overrides": [{"component": "Weapon_R"}]},
        )

    async def test_plan_serializes_actor_recipe(self):
        spec = {
            "actor_class": "/Game/Actors/BP_Infantry",
            "unit_name": "Infantry_Rifle",
        }
        await server.editor_plan_create_vat_unit_from_actor(spec)

        command, params = self.fake.calls[-1]
        self.assertEqual(command, "MCP_EditorPlanCreateVatUnitFromActor")
        self.assertEqual(json.loads(params["SpecJson"]), spec)

    async def test_vat_animation_inspection_serializes_diagnostic_spec(self):
        spec = {
            "vat_data_asset": "/Game/Units/Gen/VAT_Infantry",
            "skeletal_mesh": "/Game/Units/Gen/SKM_Infantry",
        }
        await server.editor_inspect_vat_animation(spec)

        command, params = self.fake.calls[-1]
        self.assertEqual(command, "MCP_EditorInspectVatAnimation")
        self.assertEqual(json.loads(params["SpecJson"]), spec)

    async def test_apply_adds_compact_default_without_mutating_input(self):
        spec = {"actor_class": "/Game/Actors/BP_Infantry"}
        await server.editor_apply_create_vat_unit_from_actor(
            spec, save_assets=False, compact_response=False
        )

        command, params = self.fake.calls[-1]
        self.assertEqual(command, "MCP_EditorApplyCreateVatUnitFromActor")
        self.assertFalse(params["bSaveAssets"])
        self.assertEqual(
            json.loads(params["SpecJson"]),
            {"actor_class": "/Game/Actors/BP_Infantry", "compact_response": False},
        )
        self.assertNotIn("compact_response", spec)


if __name__ == "__main__":
    unittest.main()
