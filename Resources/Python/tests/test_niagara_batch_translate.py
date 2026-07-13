import json
import sys
import unittest
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

import MassBattleMcpServer as server


SOURCE = "/Game/ArmyVFX/Niagara/Projectiles/NS_StartFlash_1.NS_StartFlash_1"
TARGET = "/Game/ArmyVFX/MassBattleMapped/NS_StartFlash_1_MB.NS_StartFlash_1_MB"


class FakeConnection:
    def __init__(self):
        self.calls = []

    async def send_command(self, command, params=None):
        params = params or {}
        self.calls.append((command, params))
        if command == "MCP_NiagaraReadSummary":
            if params.get("SystemPath") == SOURCE:
                return {"success": True, "ready_to_run": True}
            return {"success": False, "error": "not found"}
        if command == "MCP_EffectDuplicateAsset":
            return {"success": True, "asset_path": TARGET}
        if command == "MCP_NiagaraCompareSystems":
            return {"success": True, "exact_match": True, "source_preserved": True}
        if command == "MCP_NiagaraApplyGraphEdit":
            saved = bool(params.get("bSaveAssets"))
            return {"success": True, "validation_passed": True, "saved": saved}
        if command == "MCP_EffectDiscardUnsavedDuplicate":
            return {"success": True, "discarded": True}
        raise AssertionError(f"Unexpected command: {command}")


class TranslationFailureConnection(FakeConnection):
    def __init__(self):
        super().__init__()
        self.comparison_count = 0

    async def send_command(self, command, params=None):
        if command == "MCP_NiagaraCompareSystems":
            params = params or {}
            self.calls.append((command, params))
            self.comparison_count += 1
            if self.comparison_count == 2:
                return {"success": False, "source_preserved": False}
            return {"success": True, "exact_match": True}
        return await super().send_command(command, params)


def valid_manifest():
    return {
        "schema": "massbattle.niagara.translation_manifest.v1",
        "items": [
            {
                "id": "start_flash",
                "source_system_path": SOURCE,
                "new_asset_name": "NS_StartFlash_1_MB",
                "package_path": "/Game/ArmyVFX/MassBattleMapped",
                "edit": {"operations": []},
                "comparison": {"mode": "translation"},
            }
        ],
    }


class NiagaraBatchTranslateTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.previous_connection = server._unreal_connection
        self.fake = FakeConnection()
        server._unreal_connection = self.fake

    async def asyncTearDown(self):
        server._unreal_connection = self.previous_connection

    async def test_dry_run_only_reads_source_and_target(self):
        result = await server.niagara_batch_translate(valid_manifest(), apply=False)

        self.assertTrue(result["success"])
        self.assertFalse(result["mutated"])
        self.assertEqual(
            [command for command, _ in self.fake.calls],
            ["MCP_NiagaraReadSummary", "MCP_NiagaraReadSummary"],
        )

    async def test_apply_uses_exact_gate_compile_barrier_and_validated_save(self):
        result = await server.niagara_batch_translate(
            valid_manifest(), apply=True, save_assets=True
        )

        self.assertTrue(result["success"])
        self.assertEqual(result["processed_count"], 1)
        self.assertEqual(
            [command for command, _ in self.fake.calls],
            [
                "MCP_NiagaraReadSummary",
                "MCP_NiagaraReadSummary",
                "MCP_EffectDuplicateAsset",
                "MCP_NiagaraCompareSystems",
                "MCP_NiagaraApplyGraphEdit",
                "MCP_NiagaraCompareSystems",
                "MCP_NiagaraApplyGraphEdit",
            ],
        )
        exact_options = json.loads(self.fake.calls[3][1]["OptionsJson"])
        self.assertEqual(
            exact_options, {"mode": "exact", "require_ready_to_run": False}
        )
        self.assertFalse(self.fake.calls[4][1]["bSaveAssets"])
        self.assertTrue(self.fake.calls[6][1]["bSaveAssets"])

    async def test_invalid_manifest_is_rejected_without_editor_calls(self):
        manifest = valid_manifest()
        manifest["items"][0]["package_path"] = "/Engine/Unsafe"

        result = await server.niagara_batch_translate(manifest, apply=True)

        self.assertFalse(result["success"])
        self.assertFalse(result["mutated"])
        self.assertEqual(self.fake.calls, [])

    async def test_failed_translation_discards_only_the_unsaved_duplicate(self):
        self.fake = TranslationFailureConnection()
        server._unreal_connection = self.fake

        result = await server.niagara_batch_translate(
            valid_manifest(), apply=True, save_assets=True
        )

        self.assertFalse(result["success"])
        self.assertEqual(result["failed_count"], 1)
        self.assertTrue(result["results"][0]["cleanup"]["discarded"])
        self.assertEqual(
            self.fake.calls[-1],
            ("MCP_EffectDiscardUnsavedDuplicate", {"AssetPath": TARGET}),
        )


if __name__ == "__main__":
    unittest.main()
