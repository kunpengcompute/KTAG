#!/usr/bin/env python3
"""Convert the legacy YAML/ONNX/scaler files into a Schema-bound package."""

import argparse
import json
import shutil
from pathlib import Path

import yaml


FNV1A_OFFSET_BASIS = 14695981039346656037
FNV1A_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def _hash_byte(value, byte):
    return ((value ^ byte) * FNV1A_PRIME) & MASK64


def _hash_u32(value, number):
    for offset in range(4):
        value = _hash_byte(value, (number >> (offset * 8)) & 0xFF)
    return value


def _hash_string(value, text):
    encoded = text.encode("utf-8")
    value = _hash_u32(value, len(encoded))
    for byte in encoded:
        value = _hash_byte(value, byte)
    return value


def _legacy_descriptors():
    descriptors = [
        ("legacy.flow_id", "builtin.legacy.metadata", 1),
        ("network.src_ip", "builtin.legacy.tuple", 1),
        ("network.dst_ip", "builtin.legacy.tuple", 1),
        ("network.src_port", "builtin.legacy.tuple", 1),
        ("network.dst_port", "builtin.legacy.tuple", 1),
        ("network.protocol", "builtin.legacy.tuple", 1),
    ]
    for direction in ("send", "receive"):
        prefix = f"traffic.{direction}"
        provider = "builtin.legacy.traffic_statistics"
        descriptors.extend(
            [
                (f"{prefix}.byte_count", provider, 1),
                (f"{prefix}.packet_count", provider, 1),
            ]
        )
        for value_type in ("header", "payload", "interval"):
            type_prefix = f"{prefix}.{value_type}"
            for statistic in ("minimum", "maximum", "mean", "variance"):
                descriptors.append((f"{type_prefix}.{statistic}", provider, 1))
            for histogram in ("relative", "absolute", "normalized"):
                descriptors.append(
                    (f"{type_prefix}.histogram.{histogram}", provider, 16)
                )
    descriptors.append(
        ("rawbow.crc_histogram", "builtin.legacy.rawbow", 32)
    )
    return descriptors


def _legacy_schema_hash(descriptors):
    value = FNV1A_OFFSET_BASIS
    value = _hash_string(value, "ktag_legacy_v1")
    value = _hash_u32(value, 1)
    value = _hash_u32(value, len(descriptors))
    for index, (feature_id, provider_id, dimension) in enumerate(descriptors):
        value = _hash_u32(value, index)
        value = _hash_string(value, feature_id)
        value = _hash_string(value, provider_id)
        value = _hash_u32(value, 1)  # provider_version
        value = _hash_u32(value, 1)  # KTAG_FEATURE_FLOAT32
        value = _hash_u32(value, dimension)
    return value or 1


def _resolve(config_path, configured_path):
    path = Path(configured_path)
    if not path.is_absolute():
        path = config_path.parent / path
    return path.resolve()


def create_package(config_path, output_path):
    config_path = config_path.resolve()
    output_path = output_path.resolve()
    with config_path.open("r", encoding="utf-8") as stream:
        config = yaml.safe_load(stream)

    model_path = _resolve(config_path, config["model_path_onnx"])
    scaler_path = _resolve(config_path, config["scaler_path_json"])
    if not model_path.is_file():
        raise FileNotFoundError(f"ONNX model does not exist: {model_path}")
    if not scaler_path.is_file():
        raise FileNotFoundError(f"scaler does not exist: {scaler_path}")

    with scaler_path.open("r", encoding="utf-8") as stream:
        old_scaler = json.load(stream)
    mean = old_scaler.get("mean")
    scale = old_scaler.get("scale")
    if not isinstance(mean, list) or not isinstance(scale, list):
        raise ValueError("legacy scaler must contain mean and scale arrays")
    pipeline = config.get("pipeline")
    schema = pipeline.get("schema") if isinstance(pipeline, dict) else None
    if isinstance(schema, dict) and schema.get("name") not in (
            None, "ktag_legacy_v1"):
        raise ValueError(
            "create_legacy_package.py only accepts ktag_legacy_v1"
        )
    columns_to_remove = config.get("columns_to_remove", [])
    supported_removals = ([0, 1, 2], list(range(15)))
    if columns_to_remove not in supported_removals:
        raise ValueError(
            "this LegacyModelAdapter supports historical [0, 1, 2] or "
            "Schema CSV [0..14] removal contracts only"
        )

    descriptors = _legacy_descriptors()
    if sum(item[2] for item in descriptors) != 354:
        raise RuntimeError("legacy Schema descriptor generation is inconsistent")
    excluded = {"legacy.flow_id", "network.src_ip", "network.dst_ip"}
    input_features = [
        feature_id
        for feature_id, _, _ in descriptors
        if feature_id not in excluded
    ]
    input_value_count = sum(
        dimension
        for feature_id, _, dimension in descriptors
        if feature_id not in excluded
    )
    if len(mean) != input_value_count or len(scale) != input_value_count:
        raise ValueError(
            "legacy scaler dimension does not match the 351-value model input: "
            f"mean={len(mean)}, scale={len(scale)}"
        )

    schema_hash = _legacy_schema_hash(descriptors)
    output_path.mkdir(parents=True, exist_ok=True)
    shutil.copy2(model_path, output_path / "model.onnx")

    scaler = {
        "schema_hash": f"0x{schema_hash:016x}",
        "input_features": input_features,
        "mean": mean,
        "scale": scale,
    }
    manifest = {
        "model_name": model_path.stem,
        "model_version": "legacy-v1-adapter",
        "schema_hash": f"0x{schema_hash:016x}",
        "onnx_log_level": config.get("onnx_log_level", "WARNING"),
        "input_features": input_features,
        "preprocessing": {
            "minimum_total_packets": int(config.get("filter_packets", 0)),
            "packet_count_features": [
                "traffic.send.packet_count",
                "traffic.receive.packet_count",
            ],
            "missing_value_policy": "reject",
            "scaler": "scaler.json",
        },
        "model": "model.onnx",
    }
    with (output_path / "scaler.json").open("w", encoding="utf-8") as stream:
        json.dump(scaler, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    with (output_path / "manifest.json").open("w", encoding="utf-8") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write("\n")

    print(f"model package: {output_path}")
    print(f"legacy schema_hash: 0x{schema_hash:016x}")
    print(f"model input values: {input_value_count}")


def main():
    parser = argparse.ArgumentParser(
        description="Create a new Schema-bound package for the legacy model"
    )
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    create_package(arguments.config, arguments.output)


if __name__ == "__main__":
    main()
