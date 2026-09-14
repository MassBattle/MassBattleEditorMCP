#!/usr/bin/env python3
"""Summarize ArmyVFX Demo PIE log segments into a machine-readable gate."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DIAGNOSTIC_MARKER = "[ArmyVFXDemoValidation]"
TRIGGER_RE = re.compile(r"\[ArmyVFXDemo\] Triggered (\d+)/27 ([^\s]+)")
FIELD_RE = re.compile(r"(\w+)=([^\s]+)")
DI_ERROR_RE = re.compile(
    r"Error initializing data interface(?:s)?", re.IGNORECASE
)


def parse_segment(value: str) -> tuple[Path, int]:
    path_text, separator, offset_text = value.rpartition(":")
    if not separator or not offset_text.isdigit():
        raise argparse.ArgumentTypeError(
            "segment must be an absolute log path followed by :BYTE_OFFSET"
        )
    path = Path(path_text)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"log does not exist: {path}")
    return path, int(offset_text)


def read_segment(path: Path, offset: int) -> str:
    with path.open("rb") as handle:
        handle.seek(offset)
        return handle.read().decode("utf-8", errors="replace")


def integer(fields: dict[str, str], name: str, default: int = 0) -> int:
    try:
        return int(fields.get(name, default))
    except (TypeError, ValueError):
        return default


def true_value(fields: dict[str, str], name: str) -> bool:
    return fields.get(name, "").lower() == "true"


def valid_bool(fields: dict[str, str], name: str) -> bool:
    return fields.get(name, "").lower() == "true/valid"


def valid_int(fields: dict[str, str], name: str, expected: int) -> bool:
    return fields.get(name, "") == f"{expected}/valid"


def summarize(args: argparse.Namespace) -> dict[str, Any]:
    diagnostics: dict[int, list[dict[str, str]]] = defaultdict(list)
    triggered_indices: set[int] = set()
    segments: list[dict[str, Any]] = []
    di_errors: list[str] = []

    for path, offset in args.segment:
        text = read_segment(path, offset)
        lines = text.splitlines()
        segment_diagnostics = 0
        segment_triggers = 0
        for line in lines:
            trigger = TRIGGER_RE.search(line)
            if trigger:
                triggered_indices.add(int(trigger.group(1)) - 1)
                segment_triggers += 1
            if DIAGNOSTIC_MARKER in line:
                fields = dict(FIELD_RE.findall(line.split(DIAGNOSTIC_MARKER, 1)[1]))
                index = integer(fields, "Index", -1)
                if index >= 0:
                    diagnostics[index].append(fields)
                    segment_diagnostics += 1
            if DI_ERROR_RE.search(line):
                di_errors.append(line)
        segments.append(
            {
                "path": str(path),
                "byte_offset": offset,
                "bytes_read": len(text.encode("utf-8")),
                "trigger_lines": segment_triggers,
                "diagnostic_lines": segment_diagnostics,
            }
        )

    assets: list[dict[str, Any]] = []
    native_assets: list[dict[str, Any]] = []
    for index in range(27):
        records = diagnostics.get(index, [])
        first = records[0] if records else {}
        name = first.get("Name", f"Index{index}")
        mode = first.get("Mode", "unknown")
        subtype = integer(first, "SubType", -1)
        source_active = any(true_value(record, "SourceActive") for record in records)
        exact_active = any(true_value(record, "ExactActive") for record in records)
        max_particles = max(
            (integer(record, "ActiveParticles") for record in records),
            default=0,
        )
        max_active_batches = max(
            (integer(record, "ActiveBatchComponents") for record in records),
            default=0,
        )
        max_system_age = max(
            (
                float(record.get("SystemAge", -1.0))
                for record in records
                if record.get("SystemAge")
            ),
            default=-1.0,
        )
        native_handshake = any(
            valid_bool(record, "EnableShort")
            and valid_bool(record, "EnableFull")
            and valid_int(record, "RuntimeSubTypeShort", subtype)
            and valid_int(record, "RuntimeSubTypeFull", subtype)
            for record in records
        )
        native_pass = True
        if mode == "native_burst":
            native_pass = (
                max_particles > 0
                and max_active_batches > 0
                and native_handshake
                and not di_errors
            )
        runtime_pass = (
            index in triggered_indices
            and bool(records)
            and source_active
            and exact_active
            and native_pass
        )
        record: dict[str, Any] = {
            "index": index,
            "display_index": index + 1,
            "name": name,
            "mode": mode,
            "subtype": subtype if subtype >= 0 else None,
            "triggered": index in triggered_indices,
            "diagnostic_sample_count": len(records),
            "source_ever_active": source_active,
            "exact_ever_active": exact_active,
            "max_active_particles": max_particles,
            "max_active_batch_components": max_active_batches,
            "max_system_age": max_system_age,
            "native_parameter_handshake": native_handshake,
            "runtime_pass": runtime_pass,
        }
        assets.append(record)
        if mode == "native_burst":
            native_assets.append(record)

    structural: dict[str, Any] = {}
    if args.structural_report:
        structural_report = json.loads(
            args.structural_report.read_text(encoding="utf-8")
        )
        structural = {
            "path": str(args.structural_report),
            "success": bool(structural_report.get("success")),
            "requested_count": structural_report.get("requested_count"),
            "processed_count": structural_report.get("processed_count"),
            "failed_count": structural_report.get("failed_count"),
        }

    success = (
        len(triggered_indices) == 27
        and len(diagnostics) == 27
        and len(native_assets) == 8
        and all(asset["runtime_pass"] for asset in assets)
        and not di_errors
        and (not structural or structural.get("success") is True)
    )
    return {
        "schema": "massbattle.armyvfx.demo_runtime_validation.v1",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "success": success,
        "demo_level": args.demo_level,
        "screenshot": str(args.screenshot) if args.screenshot else None,
        "segments": segments,
        "structural_conversion": structural,
        "unique_triggered_count": len(triggered_indices),
        "diagnosed_effect_count": len(diagnostics),
        "native_effect_count": len(native_assets),
        "data_interface_error_count": len(di_errors),
        "data_interface_errors": di_errors[:20],
        "runtime_pass_count": sum(asset["runtime_pass"] for asset in assets),
        "native_runtime_pass_count": sum(
            asset["runtime_pass"] for asset in native_assets
        ),
        "assets": assets,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--segment",
        action="append",
        type=parse_segment,
        required=True,
        help="Absolute log path and byte offset, e.g. D:\\Project\\Saved\\Logs\\Game.log:1234",
    )
    parser.add_argument("--structural-report", type=Path)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument(
        "--demo-level",
        default="/Game/ArmyVFX/MassBattleConverted/Demo/L_ArmyVFX_AllEffects_Demo",
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = summarize(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps({
        "success": report["success"],
        "output": str(args.output),
        "runtime_pass_count": report["runtime_pass_count"],
        "native_runtime_pass_count": report["native_runtime_pass_count"],
        "data_interface_error_count": report["data_interface_error_count"],
    }, ensure_ascii=False))
    return 0 if report["success"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
