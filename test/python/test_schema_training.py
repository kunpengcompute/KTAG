#
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
#

import csv
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

import numpy as np
import pandas as pd
from sklearn.preprocessing import StandardScaler
import yaml


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PYTHON_TOOLS = REPOSITORY_ROOT / 'tools' / 'demo' / 'python'
sys.path.insert(0, str(PYTHON_TOOLS))

import evaluation  # noqa: E402
import training  # noqa: E402
import utils  # noqa: E402


def _expanded_columns(features):
    columns = []
    for feature in features:
        feature_id = feature['feature_id']
        dimension = feature['dimension']
        if dimension == 1:
            columns.append(feature_id)
        else:
            columns.extend(
                f'{feature_id}[{index}]' for index in range(dimension))
    return columns


def _proxy_features():
    catalog = (
        REPOSITORY_ROOT / 'plugins' / 'proxy_features' /
        'proxy_feature_catalog.h').read_text(encoding='utf-8')
    matches = re.findall(
        r'\{"(proxy\.[^"]+)",\s*([0-9]+)U\}', catalog)
    if len(matches) != 68:
        raise RuntimeError('failed to read the 68 Proxy Features')
    return [
        {'feature_id': feature_id, 'dimension': int(dimension)}
        for feature_id, dimension in matches
    ]


def _write_schema_csv(
        path, features, label, split, rows_per_family=4, versions=(4, 6)):
    header = list(utils.META_COLUMNS) + _expanded_columns(features)
    rows = []
    for version in versions:
        for sample in range(rows_per_family):
            values = ['0'] * len(header)
            if version == 4:
                source = f'10.{label + 1}.{1 if split == "train" else 2}.{sample + 1}'
                destination = (
                    f'198.51.{label + 1}.{sample + 1}' if split == 'train'
                    else f'203.0.{label + 1}.{sample + 1}')
            else:
                source = (
                    f'2001:db8:{label + 1}:'
                    f'{1 if split == "train" else 2}::{sample + 1}')
                destination = (
                    f'2001:db8:100:{label + 1}::{sample + 1}'
                    if split == 'train' else
                    f'2001:db8:200:{label + 1}::{sample + 1}')
            assignments = {
                'meta.flow_id': label * 100 + version * 10 + sample,
                'meta.ip_version': version,
                'meta.src_ip': source,
                'meta.dst_ip': destination,
                'meta.src_port': 50000 + sample,
                'meta.dst_port': 443 if version == 4 else 8443,
                'meta.protocol': 6 if version == 4 else 17,
                'meta.flow_key': f'label-{label}-{split}-{version}-{sample}',
                'meta.ipv6_extension_packet_count': 0,
                'meta.ip_fragment_first_packet_count': 0,
                'meta.first_packet_sequence': 1,
                'meta.last_packet_sequence': 32,
                'legacy.flow_id': label * 100 + sample,
                'network.src_ip': 0 if version == 6 else 167772161 + label,
                'network.dst_ip': 0 if version == 6 else 3325256705 + label,
                'network.src_port': 50000 + sample,
                'network.dst_port': 443 if version == 4 else 8443,
                'network.protocol': 6 if version == 4 else 17,
                'traffic.send.packet_count': 8 + sample,
                'traffic.receive.packet_count': 8 + sample,
            }
            for name, value in assignments.items():
                if name in header:
                    values[header.index(name)] = str(value)
            preserved_zero_features = {
                'network.src_ip',
                'network.dst_ip',
            }
            for index in range(len(utils.META_COLUMNS), len(header)):
                if (values[index] == '0' and
                        header[index] not in preserved_zero_features):
                    values[index] = str(
                        label * 10.0 + version * 0.1 + sample * 0.01 +
                        (index % 7) * 0.001)
            rows.append(values)
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)
    return header


def _base_config(directory, schema_name, training_paths, evaluation_paths):
    proxy_schema = schema_name != 'ktag_legacy_v1'
    contract = utils.OFFICIAL_SCHEMA_CONTRACTS[schema_name]
    config = {
        'training_data_paths': training_paths,
        'evaluation_data_paths': evaluation_paths,
        'model_path_pkl': str(directory / 'model.pkl'),
        'scaler_path_pkl': str(directory / 'scaler.pkl'),
        'model_path_onnx': str(directory / 'model.onnx'),
        'scaler_path_json': str(directory / 'scaler.json'),
        'output_dir': str(directory / 'result'),
        'classification_report_file': str(directory / 'report.txt'),
        'predictions_detail_file': str(directory / 'predictions.csv'),
        'filter_packets': 16,
        'columns_to_remove': (
            list(range(15)) if schema_name == 'ktag_legacy_v1'
            else list(range(12))),
        'pipeline': {
            'enabled': True,
            'model_package_path': str(directory / 'model_package'),
            'schema': {
                'name': schema_name,
                'version': 1,
                'remove_features': (
                    [] if not proxy_schema else
                    sorted(contract['excluded_features'])),
                'add_features': (
                    [] if not proxy_schema else
                    [item['feature_id'] for item in _proxy_features()]),
            },
            'training': {
                'model_name': f'{schema_name}_test',
                'model_version': 'dualstack-v1',
                'data_version': 'synthetic-dualstack-v1',
            },
        },
    }
    return config


def _write_config(path, config):
    with path.open('w', encoding='utf-8') as stream:
        yaml.safe_dump(config, stream, sort_keys=False)


class SchemaTrainingTest(unittest.TestCase):
    def test_complete_proxy_config_matches_frozen_contract(self):
        config_path = (
            REPOSITORY_ROOT / 'tools' / 'demo' /
            'proxy_config.yaml')
        with config_path.open('r', encoding='utf-8') as stream:
            config = yaml.safe_load(stream)

        self.assertEqual(
            len(config['training_data_paths']),
            len(config['evaluation_data_paths']))
        self.assertGreaterEqual(len(config['training_data_paths']), 2)
        self.assertTrue(all(config['training_data_paths']))
        self.assertTrue(all(config['evaluation_data_paths']))
        self.assertEqual(list(range(12)), config['columns_to_remove'])

        pipeline = config['pipeline']
        schema = pipeline['schema']
        self.assertTrue(pipeline['enabled'])
        self.assertEqual('ktag_proxy_v1', schema['name'])
        self.assertEqual(1, schema['version'])
        self.assertEqual([
            'legacy.flow_id',
            'network.src_ip',
            'network.dst_ip',
        ], schema['remove_features'])
        self.assertEqual(
            [item['feature_id'] for item in _proxy_features()],
            schema['add_features'])

        contract = utils.OFFICIAL_SCHEMA_CONTRACTS['ktag_proxy_v1']
        self.assertEqual(118, contract['input_features'])
        self.assertEqual(619, contract['input_values'])
        self.assertEqual(
            '0xca1eae5086b41ee5', contract['schema_hash'])

    def test_three_class_application_train_evaluate_and_package(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            features = utils.legacy_schema_features()
            train_groups = []
            eval_groups = []
            for label in range(3):
                train_path = directory / f'app_{label}_train.csv'
                eval_path = directory / f'app_{label}_evaluation.csv'
                _write_schema_csv(train_path, features, label, 'train')
                _write_schema_csv(eval_path, features, label, 'evaluation')
                train_groups.append([str(train_path)])
                eval_groups.append([str(eval_path)])
            config = _base_config(
                directory, 'ktag_legacy_v1', train_groups, eval_groups)
            config_path = directory / 'config.yaml'
            _write_config(config_path, config)

            self.assertEqual(0, training.KDADK_Train(config_path))
            self.assertTrue((directory / 'result' /
                             'feature_importance.csv').is_file())
            importance = pd.read_csv(
                directory / 'result' / 'feature_importance.csv')
            self.assertIn('AddressFamilyEffectMean', importance.columns)
            self.assertIn('ImportanceFamilyBiasScore', importance.columns)
            manifest = json.loads(
                (directory / 'model_package' / 'manifest.json').read_text(
                    encoding='utf-8'))
            self.assertEqual('0xdd48882b53bebfa5', manifest['schema_hash'])
            self.assertEqual(
                'synthetic-dualstack-v1', manifest['data_version'])
            self.assertEqual(50, len(manifest['input_features']))
            self.assertEqual(
                351,
                sum(item['dimension'] for item in manifest['input_features']))
            self.assertNotIn(
                'legacy.flow_id',
                [item['feature_id'] for item in manifest['input_features']])

            self.assertEqual(0, evaluation.KDADK_Evaluation(config_path))
            details = pd.read_csv(directory / 'predictions.csv')
            self.assertIn('confidence', details.columns)
            self.assertEqual(24, len(details))
            report = (directory / 'report.txt').read_text(encoding='utf-8')
            self.assertIn('[overall] support=24', report)
            self.assertIn('[ipv4] support=12', report)
            self.assertIn('[ipv6] support=12', report)
            self.assertTrue(
                (directory / 'report_confusion_matrix.csv').is_file())
            self.assertTrue(
                (directory / 'report_ipv4_confusion_matrix.csv').is_file())
            self.assertTrue(
                (directory / 'report_ipv6_confusion_matrix.csv').is_file())

            train_frame = pd.read_csv(train_groups[0][0])
            ipv6 = train_frame['meta.ip_version'] == 6
            self.assertTrue(
                (train_frame.loc[ipv6, 'network.src_ip'] == 0).all())
            self.assertTrue(
                (train_frame.loc[ipv6, 'network.dst_ip'] == 0).all())

    def test_single_family_training_and_evaluation_are_supported(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            features = utils.legacy_schema_features()
            train_groups = []
            eval_groups = []
            for label in range(2):
                train_path = directory / f'app_{label}_train.csv'
                eval_path = directory / f'app_{label}_ipv4.csv'
                _write_schema_csv(train_path, features, label, 'train')
                _write_schema_csv(
                    eval_path, features, label, 'evaluation', versions=(4,))
                train_groups.append([str(train_path)])
                eval_groups.append([str(eval_path)])
            config = _base_config(
                directory, 'ktag_legacy_v1', train_groups, eval_groups)

            validated_train, validated_eval, _ = (
                utils.validate_dataset_config(config, directory))
            self.assertEqual(train_groups, validated_train)
            self.assertEqual(eval_groups, validated_eval)
            combined = utils.load_labeled_data(
                validated_eval, directory, randomize=False)
            prepared = utils.prepare_model_data(combined, config)
            sections = evaluation._family_sections(
                prepared['y'], prepared['y'].to_numpy(),
                prepared['metadata'], [0, 1])
            self.assertEqual(0, sections[2]['support'])
            self.assertIn('not evaluable', sections[2]['report'])
            with self.assertLogs(level='WARNING') as captured:
                training._log_family_support(prepared, 2)
            self.assertIn(
                '没有 IPv6 训练样本', '\n'.join(captured.output))

            ipv4_only_train = directory / 'app_0_ipv4_train.csv'
            _write_schema_csv(
                ipv4_only_train, features, 0, 'train', versions=(4,))
            config['training_data_paths'][0] = [str(ipv4_only_train)]
            with self.assertLogs(level='WARNING') as captured:
                validated_train, validated_eval, _ = (
                    utils.validate_dataset_config(config, directory))
            self.assertEqual(config['training_data_paths'], validated_train)
            self.assertEqual(config['evaluation_data_paths'], validated_eval)
            self.assertIn(
                '训练数据缺少 IPv6', '\n'.join(captured.output))

    def test_legacy_scaler_column_names_align_only_for_frozen_contract(self):
        columns = _expanded_columns(utils.legacy_model_features())
        values = pd.DataFrame(
            np.arange(702, dtype=float).reshape(2, 351), columns=columns)
        historical = values.copy()
        historical.columns = [f'legacy_{index}' for index in range(351)]
        scaler = StandardScaler().fit(historical)
        config = {
            'pipeline': {
                'schema': {'name': 'ktag_legacy_v1', 'version': 1},
            },
        }
        aligned = evaluation._validate_scaler_columns(
            scaler, values, config)
        self.assertEqual(list(scaler.feature_names_in_), list(aligned.columns))
        with self.assertRaisesRegex(ValueError, 'do not match'):
            evaluation._validate_scaler_columns(
                scaler, values, {
                    'pipeline': {
                        'schema': {'name': 'custom', 'version': 1},
                    },
                })

    def test_three_proxy_ablation_contracts_train_and_package(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            proxy_features = _proxy_features()
            cases = (
                ('proxy_endpoint_ablation_v1', 120, 621,
                 '0xd05c3d9b98a6062f'),
                ('ktag_proxy_v1', 118, 619, '0xca1eae5086b41ee5'),
                ('proxy_no_endpoint_ablation_v1', 116, 617,
                 '0xe307cb37ec0a70c8'),
            )
            for schema_name, feature_count, value_count, schema_hash in cases:
                directory = root / schema_name
                directory.mkdir()
                excluded = utils.OFFICIAL_SCHEMA_CONTRACTS[
                    schema_name]['excluded_features']
                features = [
                    item for item in utils.legacy_schema_features()
                    if item['feature_id'] not in excluded
                ] + proxy_features
                self.assertEqual(feature_count, len(features))
                self.assertEqual(
                    value_count,
                    sum(item['dimension'] for item in features))
                train_groups = []
                eval_groups = []
                for label in range(13):
                    train_path = directory / f'proxy_{label}_train.csv'
                    eval_path = directory / f'proxy_{label}_evaluation.csv'
                    _write_schema_csv(
                        train_path, features, label, 'train',
                        rows_per_family=1)
                    _write_schema_csv(
                        eval_path, features, label, 'evaluation',
                        rows_per_family=1)
                    train_groups.append([str(train_path)])
                    eval_groups.append([str(eval_path)])
                config = _base_config(
                    directory, schema_name, train_groups, eval_groups)
                config_path = directory / 'config.yaml'
                _write_config(config_path, config)

                self.assertEqual(0, training.KDADK_Train(config_path))
                self.assertEqual(0, evaluation.KDADK_Evaluation(config_path))
                manifest = json.loads(
                    (directory / 'model_package' / 'manifest.json').read_text(
                        encoding='utf-8'))
                self.assertEqual(schema_hash, manifest['schema_hash'])
                self.assertEqual(feature_count, len(manifest['input_features']))
                self.assertEqual(
                    value_count,
                    sum(item['dimension']
                        for item in manifest['input_features']))
                identifiers = [
                    item['feature_id'] for item in manifest['input_features']]
                self.assertNotIn('legacy.flow_id', identifiers)
                if schema_name == 'proxy_endpoint_ablation_v1':
                    self.assertIn('network.src_ip', identifiers)
                    self.assertIn('network.dst_port', identifiers)
                else:
                    self.assertNotIn('network.src_ip', identifiers)
                if schema_name == 'proxy_no_endpoint_ablation_v1':
                    self.assertNotIn('network.src_port', identifiers)
                    self.assertNotIn('network.dst_port', identifiers)

    def test_rejects_bad_feature_contract_and_keeps_label_order(self):
        frame = pd.DataFrame({
            'first': [1.0, 2.0, 3.0],
            'label': [0, 1, 2],
        })
        config = {'columns_to_remove': [], 'filter_packets': 0}
        prepared = utils.prepare_model_data(frame, config)
        self.assertEqual([0, 1, 2], prepared['y'].tolist())
        with self.assertRaisesRegex(ValueError, 'does not start at index 0'):
            utils.expanded_columns_to_features(['proxy.test[1]'])
        with self.assertRaisesRegex(ValueError, 'index/order mismatch'):
            utils.expanded_columns_to_features(
                ['proxy.test[0]', 'proxy.test[2]'])

        with tempfile.TemporaryDirectory() as temp:
            duplicate_path = Path(temp) / 'duplicate.csv'
            with duplicate_path.open(
                    'w', encoding='utf-8', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['duplicate', 'duplicate'])
                writer.writerow([1, 2])
            with self.assertRaisesRegex(ValueError, 'duplicate columns'):
                utils._read_csv_header(duplicate_path)

        application_columns = (
            list(utils.META_COLUMNS) +
            _expanded_columns(utils.legacy_schema_features()))
        bad_frame = pd.DataFrame(
            [[0.0] * (len(application_columns) - 1)],
            columns=application_columns[:-1])
        bad_frame['meta.ip_version'] = 4
        bad_frame['traffic.send.packet_count'] = 8
        bad_frame['traffic.receive.packet_count'] = 8
        bad_frame['label'] = 0
        bad_config = {
            'columns_to_remove': list(range(15)),
            'filter_packets': 16,
            'pipeline': {
                'schema': {'name': 'ktag_legacy_v1', 'version': 1},
            },
        }
        with self.assertRaisesRegex(ValueError, 'expected 351'):
            utils.prepare_model_data(bad_frame, bad_config)

    def test_old_application_csv_remains_compatible(self):
        columns = [
            'flow_id', 'src_ip', 'dst_ip', 'src_port', 'dst_port', 'protocol',
            'send_packet_nums', 'receive_packet_nums',
        ]
        columns.extend(f'legacy_{index}' for index in range(354 - len(columns)))
        frame = pd.DataFrame(
            [[0.0] * 354, [1.0] * 354], columns=columns)
        frame['send_packet_nums'] = 8
        frame['receive_packet_nums'] = 8
        frame['label'] = [0, 1]
        config = {
            'columns_to_remove': [0, 1, 2],
            'filter_packets': 16,
            'pipeline': {
                'schema': {'name': 'ktag_legacy_v1', 'version': 1},
            },
        }
        prepared = utils.prepare_model_data(frame, config)
        self.assertFalse(prepared['schema_csv'])
        self.assertEqual((2, 351), prepared['X'].shape)

        # Historical deployments often ran feature_filter.py in-place before
        # training while leaving columns_to_remove=[0,1,2] in the YAML.  The
        # resulting 351-column CSV must not be filtered a second time.
        prefiltered = frame.drop(
            columns=['flow_id', 'src_ip', 'dst_ip']).copy()
        prepared_prefiltered = utils.prepare_model_data(prefiltered, config)
        self.assertFalse(prepared_prefiltered['schema_csv'])
        self.assertEqual((2, 351), prepared_prefiltered['X'].shape)

    def test_confidence_zero_is_written_without_label_rewrite(self):
        class ZeroConfidenceModel:
            def predict(self, values):
                return np.array([0])

            def predict_proba(self, values):
                return np.array([[0.0, 0.0]])

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            result = evaluation.predict_new_data({
                'model': ZeroConfidenceModel(),
                'X_scaled': np.array([[0.0]]),
                'y': pd.Series([0]),
                'X_original': pd.DataFrame({'feature': [0.0]}),
                'metadata': pd.DataFrame({'meta.ip_version': [4]}),
                'raw_bow': None,
                'labels': [0, 1],
                'report_output_path': directory / 'report.txt',
                'prediction_output_path': directory / 'predictions.csv',
            })
            self.assertEqual(1, len(result['details']))
            self.assertEqual(0, result['details'].loc[0, 'predicted_label'])
            self.assertEqual(0.0, result['details'].loc[0, 'confidence'])


if __name__ == '__main__':
    unittest.main()
