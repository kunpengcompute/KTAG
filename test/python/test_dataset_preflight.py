#
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
#

import csv
import importlib.util
from io import StringIO
from pathlib import Path
import tempfile
import unittest

import yaml


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY_ROOT / "tools" / "data" / "dataset_preflight.py"
SPEC = importlib.util.spec_from_file_location("dataset_preflight", MODULE_PATH)
dataset_preflight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dataset_preflight)


def _schema_header(schema_name):
    if schema_name == "ktag_legacy_v1":
        value_count = 354
        values = [
            "legacy.flow_id",
            "network.src_ip",
            "network.dst_ip",
            "network.src_port",
            "network.dst_port",
            "network.protocol",
            "traffic.send.packet_count",
            "traffic.receive.packet_count",
            "rawbow.crc_histogram[0]",
        ]
    else:
        contract = dataset_preflight.SCHEMA_CONTRACTS[schema_name]
        value_count = contract["schema_values"]
        values = []
        if schema_name == "proxy_endpoint_ablation_v1":
            values.extend(["network.src_ip", "network.dst_ip"])
        if schema_name != "proxy_no_endpoint_ablation_v1":
            values.extend(["network.src_port", "network.dst_port"])
        values.extend([
            "network.protocol",
            "traffic.send.packet_count",
            "traffic.receive.packet_count",
            "rawbow.crc_histogram[0]",
            "proxy.sequence.signed_payload_length[0]",
        ])
    values.extend(
        f"test.padding[{index}]"
        for index in range(value_count - len(values)))
    return list(dataset_preflight.META_COLUMNS) + values


def _write_csv(path, header, label, split, include_ipv6=True,
               overlap_destination=False):
    rows = []
    versions = (4, 6) if include_ipv6 else (4,)
    for version in versions:
        values = ["0"] * len(header)
        if version == 4:
            source = f"10.{label + 1}.{1 if split == 'train' else 2}.1"
            if overlap_destination:
                destination = f"198.51.100.{label + 1}"
            elif split == "train":
                destination = f"198.51.100.{label + 1}"
            else:
                destination = f"203.0.113.{label + 1}"
            port = 443
            protocol = 6
        else:
            source = f"2001:db8:{label + 1}:{1 if split == 'train' else 2}::1"
            if overlap_destination:
                destination = f"2001:db8:100:{label + 1}::1"
            elif split == "train":
                destination = f"2001:db8:100:{label + 1}::1"
            else:
                destination = f"2001:db8:200:{label + 1}::1"
            port = 8443
            protocol = 17
        assignments = {
            "meta.flow_id": str(label * 10 + version),
            "meta.ip_version": str(version),
            "meta.src_ip": source,
            "meta.dst_ip": destination,
            "meta.src_port": "50000",
            "meta.dst_port": str(port),
            "meta.protocol": str(protocol),
            "meta.flow_key": "test-flow",
            "meta.ipv6_extension_packet_count": "0",
            "meta.ip_fragment_first_packet_count": "0",
            "meta.first_packet_sequence": "1",
            "meta.last_packet_sequence": "2",
            "traffic.send.packet_count": "8",
            "traffic.receive.packet_count": "8",
        }
        for name, value in assignments.items():
            values[header.index(name)] = value
        rows.append(values)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)


def _write_config(directory, schema_name, groups, columns_to_remove):
    training = []
    evaluation = []
    header = _schema_header(schema_name)
    for label in range(groups):
        train_path = directory / f"label_{label}_train.csv"
        eval_path = directory / f"label_{label}_evaluation.csv"
        _write_csv(train_path, header, label, "train")
        _write_csv(eval_path, header, label, "evaluation")
        training.append([str(train_path)])
        evaluation.append([str(eval_path)])
    config = {
        "training_data_paths": training,
        "evaluation_data_paths": evaluation,
        "columns_to_remove": columns_to_remove,
        "filter_packets": 16,
        "pipeline": {
            "schema": {
                "name": schema_name,
                "version": 1,
            },
        },
    }
    config_path = directory / "config.yaml"
    with config_path.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(config, stream, sort_keys=False)
    return config_path, header


class DatasetPreflightTest(unittest.TestCase):
    def test_accepts_application_dual_stack_schema_csv(self):
        with tempfile.TemporaryDirectory() as temp:
            config_path, _ = _write_config(
                Path(temp), "ktag_legacy_v1", 1, list(range(15)))
            output = StringIO()
            result = dataset_preflight.validate_dataset(config_path, output)
            self.assertTrue(result.ok, "\n".join(result.errors))
            self.assertEqual("0xdd48882b53bebfa5", result.schema_hash)
            self.assertIn("Preflight PASSED", output.getvalue())

    def test_accepts_thirteen_proxy_dual_stack_groups(self):
        with tempfile.TemporaryDirectory() as temp:
            config_path, _ = _write_config(
                Path(temp), "ktag_proxy_v1", 13, list(range(12)))
            result = dataset_preflight.validate_dataset(
                config_path, StringIO())
            self.assertTrue(result.ok, "\n".join(result.errors))
            self.assertEqual(13, result.group_count)
            self.assertEqual("0xca1eae5086b41ee5", result.schema_hash)

    def test_accepts_incremental_two_group_proxy_dataset(self):
        with tempfile.TemporaryDirectory() as temp:
            config_path, _ = _write_config(
                Path(temp), "ktag_proxy_v1", 2, list(range(12)))
            result = dataset_preflight.validate_dataset(
                config_path, StringIO())
            self.assertTrue(result.ok, "\n".join(result.errors))
            self.assertEqual(2, result.group_count)

    def test_accepts_frozen_proxy_ablation_schemas(self):
        cases = (
            ("proxy_endpoint_ablation_v1", "0xd05c3d9b98a6062f"),
            ("proxy_no_endpoint_ablation_v1", "0xe307cb37ec0a70c8"),
        )
        for schema_name, schema_hash in cases:
            with self.subTest(schema=schema_name):
                with tempfile.TemporaryDirectory() as temp:
                    config_path, _ = _write_config(
                        Path(temp), schema_name, 13, list(range(12)))
                    result = dataset_preflight.validate_dataset(
                        config_path, StringIO())
                    self.assertTrue(result.ok, "\n".join(result.errors))
                    self.assertEqual(schema_hash, result.schema_hash)

    def test_warns_missing_ipv6_and_server_overlap_without_rejecting(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            config_path, header = _write_config(
                directory, "ktag_legacy_v1", 1, list(range(15)))
            _write_csv(
                directory / "label_0_evaluation.csv",
                header,
                0,
                "evaluation",
                include_ipv6=False,
                overlap_destination=True)
            result = dataset_preflight.validate_dataset(
                config_path, StringIO())
            self.assertTrue(result.ok, "\n".join(result.errors))
            self.assertIn(
                "evaluation has no IPv6 samples", "\n".join(result.warnings))
            self.assertNotIn(
                "evaluation has no IPv6 samples", "\n".join(result.errors))
            self.assertIn(
                "overlaps train and evaluation", "\n".join(result.warnings))
            self.assertNotIn(
                "overlaps train and evaluation", "\n".join(result.errors))

    def test_rejects_same_csv_in_training_and_evaluation(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            config_path, _ = _write_config(
                directory, "ktag_legacy_v1", 1, list(range(15)))
            with config_path.open("r", encoding="utf-8") as stream:
                config = yaml.safe_load(stream)
            config["evaluation_data_paths"] = config["training_data_paths"]
            with config_path.open("w", encoding="utf-8") as stream:
                yaml.safe_dump(config, stream, sort_keys=False)

            result = dataset_preflight.validate_dataset(
                config_path, StringIO())
            self.assertFalse(result.ok)
            self.assertIn(
                "the same CSV appears in training and evaluation",
                "\n".join(result.errors))

    def test_rejects_metadata_left_in_model_input(self):
        with tempfile.TemporaryDirectory() as temp:
            config_path, _ = _write_config(
                Path(temp), "ktag_proxy_v1", 13, [0, 1, 2])
            result = dataset_preflight.validate_dataset(
                config_path, StringIO())
            self.assertFalse(result.ok)
            self.assertIn(
                "columns_to_remove must exclude every meta.* column",
                "\n".join(result.errors))


if __name__ == "__main__":
    unittest.main()
