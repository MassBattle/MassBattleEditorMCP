import sys
from pathlib import Path


PYTHON_DIR = Path(__file__).resolve().parents[1]
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

import MassBattleMcpServer as server


def test_ue57_does_not_register_unsupported_batch_tools():
    tool_names = {tool.name for tool in server.mcp._tool_manager.list_tools()}

    assert not any(name.startswith("niagara_") for name in tool_names)
    assert "batch_fx_read_renderer_defaults" not in tool_names
    assert "batch_fx_set_renderer_defaults" not in tool_names


def test_ue57_keeps_generic_effect_asset_tools():
    tool_names = {tool.name for tool in server.mcp._tool_manager.list_tools()}

    assert "effect_asset_query" in tool_names
    assert "effect_asset_read_summary" in tool_names
    assert "effect_duplicate_asset" in tool_names
    assert "effect_discard_unsaved_duplicate" in tool_names
