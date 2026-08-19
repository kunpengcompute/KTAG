#!/usr/bin/env python3
#
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
#
"""Read-only preflight for dual-stack Schema CSV datasets.

The tool deliberately consumes the existing config.yaml structure.  It does
not rewrite CSV files and does not create a dataset manifest or report file.
"""

import argparse
import csv
import hashlib
import ipaddress
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
import sys

import yaml


META_COLUMNS = (
    "meta.flow_id",
    "meta.ip_version",
    "meta.src_ip",
    "meta.dst_ip",
    "meta.src_port",
    "meta.dst_port",
    "meta.protocol",
    "meta.flow_key",
    "meta.ipv6_extension_packet_count",
    "meta.ip_fragment_first_packet_count",
    "meta.first_packet_sequence",
    "meta.last_packet_sequence",
)

FORBIDDEN_MODEL_COLUMNS = {
    "legacy.flow_id",
    "network.src_ip",
    "network.dst_ip",
}

SCHEMA_CONTRACTS = {
    "ktag_legacy_v1": {
        "version": 1,
        "schema_values": 354,
        "model_values": 351,
        "hash": "0xdd48882b53bebfa5",
        "required_groups": None,
        "proxy": False,
    },
    "proxy_endpoint_ablation_v1": {
        "version": 1,
        "schema_values": 621,
        "model_values": 621,
        "hash": "0xd05c3d9b98a6062f",
        "required_groups": None,
        "proxy": True,
        "forbidden": {"legacy.flow_id"},
        "required": {
            "network.src_ip",
            "network.dst_ip",
            "network.src_port",
            "network.dst_port",
        },
    },
    "ktag_proxy_v1": {
        "version": 1,
        "schema_values": 619,
        "model_values": 619,
        "hash": "0xca1eae5086b41ee5",
        "required_groups": None,
        "proxy": True,
        "forbidden": FORBIDDEN_MODEL_COLUMNS,
        "required": {
            "network.src_port",
            "network.dst_port",
        },
    },
    "proxy_no_endpoint_ablation_v1": {
        "version": 1,
        "schema_values": 617,
        "model_values": 617,
        "hash": "0xe307cb37ec0a70c8",
        "required_groups": None,
        "proxy": True,
        "forbidden": FORBIDDEN_MODEL_COLUMNS | {
            "network.src_port",
            "network.dst_port",
        },
        "required": set(),
    },
}


class PreflightError(RuntimeError):
    """Raised when configuration or input data cannot be inspected."""


@dataclass
class FileStats:
    path: Path
    rows: int = 0
    ip_versions: Counter = field(default_factory=Counter)
    protocols: Counter = field(default_factory=Counter)
    source_ips: set = field(default_factory=set)
    destination_ips: set = field(default_factory=set)
    destination_ports: set = field(default_factory=set)
    packet_buckets: Counter = field(default_factory=Counter)


@dataclass
class SplitStats:
    files: int = 0
    rows: int = 0
    ip_versions: Counter = field(default_factory=Counter)
    protocols: Counter = field(default_factory=Counter)
    source_ips: set = field(default_factory=set)
    destination_ips: set = field(default_factory=set)
    destination_ports: set = field(default_factory=set)
    packet_buckets: Counter = field(default_factory=Counter)

    def add(self, item):
        self.files += 1
        self.rows += item.rows
        self.ip_versions.update(item.ip_versions)
        self.protocols.update(item.protocols)
        self.source_ips.update(item.source_ips)
        self.destination_ips.update(item.destination_ips)
        self.destination_ports.update(item.destination_ports)
        self.packet_buckets.update(item.packet_buckets)


@dataclass
class PreflightResult:
    schema_name: str
    schema_version: int
    schema_hash: str
    header_sha256: str
    group_count: int
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    @property
    def ok(self):
        return not self.errors


def _resolve_path(config_path, configured_path):
    path = Path(configured_path).expanduser()
    if not path.is_absolute():
        path = config_path.parent / path
    return path.resolve()


def _read_config(config_path):
    try:
        with config_path.open("r", encoding="utf-8") as stream:
            config = yaml.safe_load(stream)
    except OSError as exc:
        raise PreflightError(f"cannot read config: {exc}") from exc
    except yaml.YAMLError as exc:
        raise PreflightError(f"invalid YAML: {exc}") from exc
    if not isinstance(config, dict):
        raise PreflightError("config root must be a mapping")
    return config


def _normalize_groups(config, key, config_path, errors):
    configured = config.get(key)
    if not isinstance(configured, list) or not configured:
        errors.append(f"{key} must be a non-empty two-dimensional list")
        return []

    groups = []
    all_paths = set()
    for label, group in enumerate(configured):
        if not isinstance(group, list) or not group:
            errors.append(f"{key}[{label}] must be a non-empty list")
            groups.append([])
            continue
        normalized = []
        for index, configured_path in enumerate(group):
            if not isinstance(configured_path, str) or not configured_path:
                errors.append(
                    f"{key}[{label}][{index}] must be a non-empty path")
                continue
            path = _resolve_path(config_path, configured_path)
            if path in all_paths:
                errors.append(f"duplicate CSV in {key}: {path}")
            all_paths.add(path)
            normalized.append(path)
        groups.append(normalized)
    return groups


def _schema_contract(config, errors):
    pipeline = config.get("pipeline")
    schema = pipeline.get("schema") if isinstance(pipeline, dict) else None
    if not isinstance(schema, dict):
        errors.append("pipeline.schema is required for Schema CSV preflight")
        return "", 0, None
    name = schema.get("name")
    version = schema.get("version")
    if not isinstance(name, str) or not name:
        errors.append("pipeline.schema.name must be a non-empty string")
        name = ""
    if not isinstance(version, int) or version <= 0:
        errors.append("pipeline.schema.version must be a positive integer")
        version = 0
    contract = SCHEMA_CONTRACTS.get(name)
    if contract is None:
        errors.append(
            "dataset preflight requires ktag_legacy_v1, ktag_proxy_v1 or "
            "one of the two frozen proxy ablation Schemas")
    elif version != contract["version"]:
        errors.append(
            f"{name} requires version {contract['version']}, got {version}")
    return name, version, contract


def _validate_columns_to_remove(config, header, contract, errors):
    configured = config.get("columns_to_remove")
    if not isinstance(configured, list):
        errors.append("columns_to_remove must be a list")
        return
    if (any(not isinstance(index, int) or isinstance(index, bool)
            for index in configured) or
            any(index < 0 or index >= len(header) for index in configured)):
        errors.append(
            f"columns_to_remove must contain indexes in [0, {len(header) - 1}]")
        return
    if len(set(configured)) != len(configured):
        errors.append("columns_to_remove contains duplicate indexes")
        return

    removed = {header[index] for index in configured}
    missing_meta = [column for column in META_COLUMNS if column not in removed]
    if missing_meta:
        errors.append(
            "columns_to_remove must exclude every meta.* column: " +
            ", ".join(missing_meta))

    if contract["schema_values"] == 354:
        missing = sorted(FORBIDDEN_MODEL_COLUMNS - removed)
        if missing:
            errors.append(
                "application model input must also exclude: " +
                ", ".join(missing))

    remaining = len(header) - len(configured)
    if remaining != contract["model_values"]:
        errors.append(
            "columns_to_remove leaves "
            f"{remaining} model values, expected {contract['model_values']}")


def _packet_bucket(total):
    if total < 16:
        return "<16"
    if total < 32:
        return "16-31"
    if total < 128:
        return "32-127"
    return ">=128"


def _read_integer(row, indexes, name, path, line_number, errors):
    try:
        return int(row[indexes[name]])
    except (ValueError, IndexError):
        errors.append(f"{path}:{line_number}: invalid integer in {name}")
        return None


def _inspect_csv(path, expected_header, contract, errors):
    stats = FileStats(path=path)
    if not path.is_file():
        errors.append(f"CSV does not exist: {path}")
        return stats, expected_header
    try:
        stream = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        errors.append(f"cannot read CSV {path}: {exc}")
        return stats, expected_header

    with stream:
        reader = csv.reader(stream)
        try:
            header = next(reader)
        except StopIteration:
            errors.append(f"CSV is empty: {path}")
            return stats, expected_header
        if len(set(header)) != len(header):
            errors.append(f"CSV header contains duplicate columns: {path}")
        if tuple(header[:len(META_COLUMNS)]) != META_COLUMNS:
            errors.append(
                f"CSV does not start with the 12 canonical meta.* columns: {path}")
        expected_total = len(META_COLUMNS) + contract["schema_values"]
        if len(header) != expected_total:
            errors.append(
                f"CSV has {len(header)} columns, expected {expected_total}: {path}")
        if expected_header is not None and header != expected_header:
            errors.append(f"CSV header/order differs from the first file: {path}")
        if expected_header is None:
            expected_header = header

        if contract.get("proxy"):
            present_forbidden = sorted(
                contract["forbidden"] & set(header))
            if present_forbidden:
                errors.append(
                    "proxy Schema CSV contains excluded columns: " +
                    ", ".join(present_forbidden))
            required = set(contract["required"]) | {
                "network.protocol",
                "traffic.send.packet_count",
                "traffic.receive.packet_count",
                "rawbow.crc_histogram[0]",
            }
            missing = sorted(required - set(header))
            if missing:
                errors.append(
                    "proxy Schema CSV is missing required columns: " +
                    ", ".join(missing))
            if not any(column.startswith("proxy.") for column in header):
                errors.append(f"proxy Schema CSV has no proxy.* columns: {path}")

        indexes = {name: index for index, name in enumerate(header)}
        required_meta = set(META_COLUMNS)
        if not required_meta.issubset(indexes):
            return stats, expected_header
        send_packets = indexes.get("traffic.send.packet_count")
        receive_packets = indexes.get("traffic.receive.packet_count")

        for line_number, row in enumerate(reader, 2):
            if not row or all(not value for value in row):
                continue
            if len(row) != len(header):
                errors.append(
                    f"{path}:{line_number}: row has {len(row)} values, "
                    f"expected {len(header)}")
                continue
            version = _read_integer(
                row, indexes, "meta.ip_version", path, line_number, errors)
            protocol = _read_integer(
                row, indexes, "meta.protocol", path, line_number, errors)
            destination_port = _read_integer(
                row, indexes, "meta.dst_port", path, line_number, errors)
            if version not in (4, 6):
                errors.append(
                    f"{path}:{line_number}: meta.ip_version must be 4 or 6")
                continue
            try:
                source = ipaddress.ip_address(row[indexes["meta.src_ip"]])
                destination = ipaddress.ip_address(
                    row[indexes["meta.dst_ip"]])
            except ValueError:
                errors.append(f"{path}:{line_number}: invalid IP address")
                continue
            if source.version != version or destination.version != version:
                errors.append(
                    f"{path}:{line_number}: IP address family disagrees with "
                    "meta.ip_version")
                continue

            stats.rows += 1
            stats.ip_versions[version] += 1
            if protocol is not None:
                stats.protocols[protocol] += 1
            stats.source_ips.add(str(source))
            stats.destination_ips.add(str(destination))
            if destination_port is not None:
                stats.destination_ports.add(destination_port)
            if send_packets is not None and receive_packets is not None:
                try:
                    total_packets = int(float(row[send_packets])) + int(
                        float(row[receive_packets]))
                    stats.packet_buckets[_packet_bucket(total_packets)] += 1
                except ValueError:
                    errors.append(
                        f"{path}:{line_number}: invalid packet-count Feature")
        if stats.rows == 0:
            errors.append(f"CSV contains no valid Feature rows: {path}")
    return stats, expected_header


def _parent_hints(paths):
    return sorted({path.parent.name for path in paths})


def _format_counter(counter):
    return ",".join(
        f"{key}:{counter[key]}" for key in sorted(counter, key=str)) or "none"


def validate_dataset(config_path, output=None):
    """Validate a configured dual-stack dataset without modifying it."""
    output = output or sys.stdout
    config_path = Path(config_path).expanduser().resolve()
    config = _read_config(config_path)
    errors = []
    warnings = []
    schema_name, schema_version, contract = _schema_contract(config, errors)
    training = _normalize_groups(
        config, "training_data_paths", config_path, errors)
    evaluation = _normalize_groups(
        config, "evaluation_data_paths", config_path, errors)

    if len(training) != len(evaluation):
        errors.append(
            "training_data_paths and evaluation_data_paths must contain the "
            "same number of label groups")
    group_count = min(len(training), len(evaluation))
    if contract is None:
        for message in errors:
            print(f"ERROR: {message}", file=output)
        print(f"Preflight FAILED ({len(errors)} errors)", file=output)
        return PreflightResult(
            schema_name, schema_version, "unknown", "", group_count, errors,
            warnings)
    required_groups = contract["required_groups"]
    if required_groups is not None and (
            len(training) != required_groups or
            len(evaluation) != required_groups):
        errors.append(
            f"{schema_name} reference dataset requires {required_groups} "
            "training and evaluation groups")

    training_paths = {path for group in training for path in group}
    evaluation_paths = {path for group in evaluation for path in group}
    duplicate_splits = sorted(training_paths & evaluation_paths)
    if duplicate_splits:
        errors.append(
            "the same CSV appears in training and evaluation: " +
            ", ".join(str(path) for path in duplicate_splits))

    expected_header = None
    group_stats = []
    all_training_destinations = set()
    all_evaluation_destinations = set()
    for label in range(group_count):
        train_stats = SplitStats()
        eval_stats = SplitStats()
        for path in training[label]:
            item, expected_header = _inspect_csv(
                path, expected_header, contract, errors)
            train_stats.add(item)
        for path in evaluation[label]:
            item, expected_header = _inspect_csv(
                path, expected_header, contract, errors)
            eval_stats.add(item)
        group_stats.append((train_stats, eval_stats))
        all_training_destinations.update(train_stats.destination_ips)
        all_evaluation_destinations.update(eval_stats.destination_ips)

        for split_name, stats in (
                ("train", train_stats), ("evaluation", eval_stats)):
            missing_versions = [
                str(version) for version in (4, 6)
                if stats.ip_versions[version] == 0
            ]
            if missing_versions:
                warnings.append(
                    f"label {label} {split_name} has no IPv" +
                    "/IPv".join(missing_versions) +
                    " samples; the dataset remains valid, but model quality "
                    "for the missing address family is not established")

        overlap = train_stats.destination_ips & eval_stats.destination_ips
        if overlap:
            warnings.append(
                f"label {label} destination/server IP overlaps train and "
                f"evaluation ({len(overlap)} addresses); preflight continues, "
                "but measured accuracy may overestimate cross-server "
                "generalization")
        if train_stats.files + eval_stats.files < 2:
            errors.append(f"label {label} must contain multiple capture CSVs")
        combined_sources = train_stats.source_ips | eval_stats.source_ips
        combined_destinations = (
            train_stats.destination_ips | eval_stats.destination_ips)
        combined_ports = (
            train_stats.destination_ports | eval_stats.destination_ports)
        if len(combined_sources) < 2:
            errors.append(
                f"label {label} exposes fewer than two client/source IPs")
        if len(combined_destinations) < 2:
            errors.append(
                f"label {label} exposes fewer than two server/destination IPs")
        if len(combined_ports) < 2:
            errors.append(
                f"label {label} exposes fewer than two destination ports")

        train_hints = _parent_hints(training[label])
        eval_hints = _parent_hints(evaluation[label])
        if (len(train_hints) == 1 and len(eval_hints) == 1 and
                train_hints != eval_hints):
            warnings.append(
                f"label {label} parent-directory hints differ: "
                f"train={train_hints[0]}, evaluation={eval_hints[0]}; "
                "confirm label ordering manually")

    global_overlap = (
        all_training_destinations & all_evaluation_destinations)
    if global_overlap:
        warnings.append(
            "destination/server IPs overlap between the complete training and "
            f"evaluation datasets ({len(global_overlap)} addresses); "
            "preflight continues with this generalization risk")

    if expected_header is not None:
        _validate_columns_to_remove(config, expected_header, contract, errors)
        header_sha256 = hashlib.sha256(
            ",".join(expected_header).encode("utf-8")).hexdigest()
    else:
        header_sha256 = ""

    filter_packets = config.get("filter_packets", 0)
    if (not isinstance(filter_packets, int) or isinstance(filter_packets, bool)
            or filter_packets < 0):
        errors.append("filter_packets must be a non-negative integer")

    print(
        f"Schema: {schema_name} v{schema_version} "
        f"(expected_hash={contract['hash']}, "
        f"schema_values={contract['schema_values']}, "
        f"model_values={contract['model_values']})",
        file=output)
    print(f"Label groups: {len(training)} train / {len(evaluation)} evaluation",
          file=output)
    if header_sha256:
        print(f"CSV header SHA256: {header_sha256}", file=output)
    for label, (train_stats, eval_stats) in enumerate(group_stats):
        for split_name, stats in (
                ("train", train_stats), ("evaluation", eval_stats)):
            print(
                f"label={label} split={split_name} files={stats.files} "
                f"rows={stats.rows} ip=[{_format_counter(stats.ip_versions)}] "
                f"protocol=[{_format_counter(stats.protocols)}] "
                f"servers={len(stats.destination_ips)} "
                f"ports={len(stats.destination_ports)} "
                f"packets=[{_format_counter(stats.packet_buckets)}]",
                file=output)
    warnings.append(
        "numeric label meaning is defined only by YAML outer-list order; "
        "confirm the train/evaluation business ordering manually")
    warnings.append(
        "Schema CSV does not embed Schema Hash; this check combines the frozen "
        "official config contract with an exact cross-file header comparison")
    warnings.append(
        "CSV cannot prove whether different IPv4/IPv6 addresses belong to the "
        "same physical dual-stack server; the data provider must confirm this "
        "isolation from capture records")
    for message in warnings:
        print(f"WARNING: {message}", file=output)
    for message in errors:
        print(f"ERROR: {message}", file=output)
    print(
        "Preflight PASSED" if not errors else
        f"Preflight FAILED ({len(errors)} errors)",
        file=output)

    return PreflightResult(
        schema_name=schema_name,
        schema_version=schema_version,
        schema_hash=contract["hash"],
        header_sha256=header_sha256,
        group_count=group_count,
        errors=errors,
        warnings=warnings,
    )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate dual-stack training/evaluation Schema CSV data")
    parser.add_argument(
        "config", help="existing KTAG YAML containing training/evaluation paths")
    args = parser.parse_args(argv)
    try:
        result = validate_dataset(args.config)
    except PreflightError as exc:
        print(f"Preflight FAILED: {exc}", file=sys.stderr)
        return 1
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
