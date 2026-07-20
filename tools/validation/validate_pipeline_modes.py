#!/usr/bin/env python3
"""Run the legacy path and four new pipeline modes against one PCAP set.

The authoritative compatibility gate is the retained/filtered sample count.
The four new modes are additionally compared by stable flow metadata and label;
flow_id and output order are deliberately not used.
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


MODES = ("legacy", "single_thread", "staged", "feature_parallel", "full_parallel")
NEW_MODES = MODES[1:]
FILTER_RE = re.compile(r"过滤统计:\s*保留\s*(\d+)\s*个样本,\s*过滤掉\s*(\d+)\s*个样本")
ACCURACY_RE = re.compile(r"^\s*accuracy\s+([0-9.]+)\s+\d+\s*$", re.MULTILINE)


@dataclass(frozen=True)
class Summary:
    retained: int
    filtered: int
    accuracy: str | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate legacy and four new KTAG pipeline modes")
    parser.add_argument("--demo", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument(
        "--pcap", required=True, action="append", type=Path,
        help="Repeat in label order, matching normal kdadk_demo usage")
    parser.add_argument("--feature-threads", type=int, default=4)
    parser.add_argument("--inference-threads", type=int, default=2)
    parser.add_argument(
        "--work-dir", type=Path, default=Path("phase8_validation"))
    return parser.parse_args()


def absolute_existing(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.exists():
        raise ValueError(f"{description} does not exist: {resolved}")
    return resolved


def build_command(
    demo: Path,
    config: Path,
    pcaps: list[Path],
    mode: str,
    output: Path,
    feature_threads: int,
    inference_threads: int,
) -> list[str]:
    command = [str(demo), "-r", str(config)]
    for pcap in pcaps:
        command.extend(("-p", str(pcap)))
    command.extend(("-m", "1", "--pipeline-mode", mode, "-c", str(output)))
    if mode in ("feature_parallel", "full_parallel"):
        command.extend(("-n", str(feature_threads)))
    if mode == "full_parallel":
        command.extend(("--inference-threads", str(inference_threads)))
    return command


def parse_summary(output: str, mode: str) -> Summary:
    filter_match = FILTER_RE.search(output)
    if filter_match is None:
        raise RuntimeError(f"{mode}: missing retained/filtered summary")
    accuracy_match = ACCURACY_RE.search(output)
    return Summary(
        retained=int(filter_match.group(1)),
        filtered=int(filter_match.group(2)),
        accuracy=accuracy_match.group(1) if accuracy_match else None,
    )


def prediction_signature(path: Path) -> list[tuple[str, ...]]:
    required = (
        "source_index",
        "flow_key",
        "first_packet_sequence",
        "last_packet_sequence",
        "label",
    )
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or ())
        missing = [field for field in required if field not in fields]
        if missing:
            raise RuntimeError(
                f"{path}: missing new-pipeline fields: {', '.join(missing)}")
        rows = [tuple(row[field] for field in required) for row in reader]
    rows.sort()
    return rows


def main() -> int:
    args = parse_args()
    if args.feature_threads < 1 or args.inference_threads < 1:
        print("thread counts must be positive", file=sys.stderr)
        return 2

    try:
        demo = absolute_existing(args.demo, "demo")
        config = absolute_existing(args.config, "config")
        pcaps = [absolute_existing(path, "pcap") for path in args.pcap]
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    work_dir = args.work_dir.expanduser().resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    summaries: dict[str, Summary] = {}
    outputs: dict[str, Path] = {}

    for mode in MODES:
        csv_path = work_dir / f"result_{mode}.csv"
        log_path = work_dir / f"{mode}.log"
        command = build_command(
            demo, config, pcaps, mode, csv_path,
            args.feature_threads, args.inference_threads)
        print(f"[{mode}] {' '.join(command)}", flush=True)
        completed = subprocess.run(
            command,
            cwd=work_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        log_path.write_text(completed.stdout, encoding="utf-8")
        if completed.returncode != 0:
            print(
                f"{mode}: failed with exit code {completed.returncode}; "
                f"see {log_path}", file=sys.stderr)
            return 1
        try:
            summaries[mode] = parse_summary(completed.stdout, mode)
        except RuntimeError as error:
            print(f"{error}; see {log_path}", file=sys.stderr)
            return 1
        outputs[mode] = csv_path

    legacy = summaries["legacy"]
    failed = False
    print("\nmode               retained   filtered   accuracy")
    print("-----------------  ---------  ---------  --------")
    for mode in MODES:
        summary = summaries[mode]
        print(
            f"{mode:17}  {summary.retained:9}  {summary.filtered:9}  "
            f"{summary.accuracy or '-'}")
        if (summary.retained, summary.filtered) != (
            legacy.retained, legacy.filtered
        ):
            print(f"ERROR: {mode} sample counts differ from legacy", file=sys.stderr)
            failed = True
        if (
            legacy.accuracy is not None
            and summary.accuracy is not None
            and summary.accuracy != legacy.accuracy
        ):
            print(f"ERROR: {mode} accuracy differs from legacy", file=sys.stderr)
            failed = True

    try:
        reference = prediction_signature(outputs["single_thread"])
        for mode in NEW_MODES[1:]:
            if prediction_signature(outputs[mode]) != reference:
                print(
                    f"ERROR: {mode} stable prediction records differ from "
                    "single_thread", file=sys.stderr)
                failed = True
    except (OSError, RuntimeError) as error:
        print(f"ERROR: could not compare new pipeline CSV: {error}", file=sys.stderr)
        failed = True

    if failed:
        print(f"\nValidation failed. Logs: {work_dir}", file=sys.stderr)
        return 1
    print(f"\nValidation passed. Logs and CSV files: {work_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
