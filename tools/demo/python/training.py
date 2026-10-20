#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

import json
import logging
from pathlib import Path
import shutil
import sys

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.preprocessing import StandardScaler
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType
import yaml

from utils import (
    FORBIDDEN_MODEL_FEATURES,
    legacy_model_features,
    load_labeled_data,
    normalize_input_features,
    prepare_model_data,
    resolve_path,
    schema_identity,
    schema_training_config,
    validate_dataset_config,
)


logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s')


def create_directory(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    logging.info('结果保存路径: %s', path)


def load_and_prepare_data(data_paths, base_dir=None, config=None):
    """Compatibility wrapper; labels still follow outer-list order."""
    combined = load_labeled_data(data_paths, base_dir, randomize=True)
    if config is not None:
        prepared = prepare_model_data(combined, config)
        return prepared['X'], prepared['y']
    y = combined['label'].astype(int)
    X = combined.drop(columns=['label'])
    if 'raw_bow' in X.columns:
        X = X.drop(columns=['raw_bow'])
    return X, y


def train_random_forest(X, y, random_state=42):
    """Train the reference RandomForest with deterministic sampling."""
    scaler = StandardScaler().set_output(transform='pandas')
    X_scaled = scaler.fit_transform(X)
    model = RandomForestClassifier(
        n_estimators=100,
        max_depth=10,
        min_samples_split=5,
        min_samples_leaf=2,
        random_state=random_state,
        oob_score=True,
    )
    model.fit(X_scaled, y)
    logging.info('OOB Score: %s', model.oob_score_)
    importance = pd.DataFrame({
        'Feature': X_scaled.columns,
        'Importance': model.feature_importances_,
    }).sort_values('Importance', ascending=False)
    logging.info('Top 30 features:\n%s', importance.head(30))
    return model, scaler


def _add_address_family_audit(frame, prepared, scaler):
    """Add per-label IPv4/IPv6 shift diagnostics to importance output."""
    if prepared is None or scaler is None:
        return frame
    metadata = prepared['metadata']
    if 'meta.ip_version' not in metadata.columns:
        return frame

    versions = pd.to_numeric(
        metadata['meta.ip_version'], errors='raise').astype(int).to_numpy()
    X = prepared['X']
    y = prepared['y'].to_numpy()
    scales = np.asarray(scaler.scale_, dtype=float)
    if scales.shape != (X.shape[1],):
        raise ValueError(
            'trained scaler dimension does not match family-bias audit input')
    safe_scales = np.where(scales > 0.0, scales, 1.0)
    signed_gaps = []
    normalized_gaps = []
    for label in sorted(set(y.tolist())):
        ipv4_mask = (y == label) & (versions == 4)
        ipv6_mask = (y == label) & (versions == 6)
        if not np.any(ipv4_mask) or not np.any(ipv6_mask):
            logging.warning(
                'label=%d 过滤后缺少一个地址族，无法计算该标签的特征偏差',
                label)
            continue
        ipv4_mean = X.loc[ipv4_mask].mean(axis=0).to_numpy(dtype=float)
        ipv6_mean = X.loc[ipv6_mask].mean(axis=0).to_numpy(dtype=float)
        gap = ipv6_mean - ipv4_mean
        signed_gaps.append(gap)
        normalized_gaps.append(np.abs(gap) / safe_scales)

    if not normalized_gaps:
        return frame
    signed = np.vstack(signed_gaps)
    normalized = np.vstack(normalized_gaps)
    audit = pd.DataFrame({
        'Feature': list(X.columns),
        'IPv6MinusIPv4Mean': np.mean(signed, axis=0),
        'AddressFamilyEffectMean': np.mean(normalized, axis=0),
        'AddressFamilyEffectMax': np.max(normalized, axis=0),
    })
    frame = frame.merge(audit, on='Feature', how='left', validate='one_to_one')
    frame['ImportanceFamilyBiasScore'] = (
        frame['Importance'] * frame['AddressFamilyEffectMean'])
    logging.info(
        '按“模型重要性 × 地址族均值偏移”排序的 Top 30 特征:\n%s',
        frame.sort_values(
            'ImportanceFamilyBiasScore', ascending=False).head(30))
    return frame


def _save_feature_importance(
        model, columns, config, config_dir, prepared=None, scaler=None):
    output_dir = resolve_path(config['output_dir'], config_dir)
    configured = config.get('feature_importance_file')
    path = (resolve_path(configured, config_dir) if configured else
            output_dir / 'feature_importance.csv')
    path.parent.mkdir(parents=True, exist_ok=True)
    frame = pd.DataFrame({
        'Feature': list(columns),
        'Importance': model.feature_importances_,
    })
    frame = _add_address_family_audit(frame, prepared, scaler)
    frame = frame.sort_values('Importance', ascending=False)
    frame.to_csv(path, index=False, encoding='utf-8')
    logging.info('Feature Importance 已保存到: %s', path)


def _format_schema_hash(value):
    try:
        parsed = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError('pipeline.training.schema_hash is required') from exc
    if parsed <= 0 or parsed > ((1 << 64) - 1):
        raise ValueError(
            'pipeline.training.schema_hash must be a non-zero uint64')
    return f'0x{parsed:016x}'


def _package_feature_contract(config, prepared):
    name, _, contract = schema_identity(config)
    training = schema_training_config(config)
    if contract is not None:
        features = (prepared['features'] if prepared['schema_csv'] else
                    legacy_model_features())
        return contract['schema_hash'], features
    if training is None:
        raise ValueError(
            'custom Schema packaging requires pipeline.training metadata')
    return (
        _format_schema_hash(training.get('schema_hash')),
        normalize_input_features(training),
    )


def create_pipeline_package(config, config_path, scaler, prepared):
    """Create a complete Schema-bound model package for official/custom data."""
    pipeline = config.get('pipeline')
    if not isinstance(pipeline, dict) or not pipeline.get('model_package_path'):
        logging.info('未配置 pipeline.model_package_path，跳过模型包生成')
        return None

    schema_hash, features = _package_feature_contract(config, prepared)
    input_feature_ids = [feature['feature_id'] for feature in features]
    input_value_count = sum(feature['dimension'] for feature in features)
    if input_value_count != prepared['X'].shape[1]:
        raise ValueError(
            'model package Feature dimensions do not match trained columns: '
            f'{input_value_count} != {prepared["X"].shape[1]}')
    if (len(scaler.mean_) != input_value_count or
            len(scaler.scale_) != input_value_count):
        raise ValueError('trained scaler dimension does not match model input')
    excluded_features = set(
        (prepared.get('contract') or {}).get(
            'excluded_features', FORBIDDEN_MODEL_FEATURES))
    forbidden = excluded_features & set(input_feature_ids)
    if forbidden:
        raise ValueError(
            'model package contains Features excluded by its Schema: ' +
            ', '.join(sorted(forbidden)))

    output_path = resolve_path(
        pipeline['model_package_path'], config_path.parent)
    model_path = resolve_path(config['model_path_onnx'], config_path.parent)
    output_path.mkdir(parents=True, exist_ok=True)
    packaged_model_path = output_path / 'model.onnx'
    if model_path.resolve() != packaged_model_path.resolve():
        shutil.copy2(model_path, packaged_model_path)

    training = schema_training_config(config) or {}
    minimum_packets = int(
        training.get('minimum_total_packets', config.get('filter_packets', 0)))
    if minimum_packets < 0:
        raise ValueError('minimum_total_packets must not be negative')
    packet_count_features = training.get('packet_count_features', [
        'traffic.send.packet_count',
        'traffic.receive.packet_count',
    ])
    if (not isinstance(packet_count_features, list) or
            any(not isinstance(item, str) or not item
                for item in packet_count_features) or
            len(set(packet_count_features)) != len(packet_count_features)):
        raise ValueError(
            'packet_count_features must contain unique non-empty Feature IDs')
    if minimum_packets > 0 and not packet_count_features:
        raise ValueError(
            'packet_count_features are required when filtering is enabled')
    missing_packet_features = (
        set(packet_count_features) - set(input_feature_ids))
    if missing_packet_features:
        raise ValueError(
            'packet-count Features are absent from model input: ' +
            ', '.join(sorted(missing_packet_features)))

    model_name = training.get('model_name', model_path.stem)
    if not isinstance(model_name, str) or not model_name:
        raise ValueError('model_name must be a non-empty string')
    model_version = str(training.get('model_version', '1'))
    if not model_version.strip():
        raise ValueError('model_version must be a non-empty string')
    input_manifest = [
        {
            'feature_id': feature['feature_id'],
            'dimension': feature['dimension'],
        }
        for feature in features
    ]
    scaler_document = {
        'schema_hash': schema_hash,
        'input_features': input_feature_ids,
        'mean': scaler.mean_.tolist(),
        'scale': scaler.scale_.tolist(),
    }
    manifest = {
        'model_name': model_name,
        'model_version': model_version,
        'schema_hash': schema_hash,
        'onnx_log_level': config.get('onnx_log_level', 'WARNING'),
        'input_features': input_manifest,
        'preprocessing': {
            'minimum_total_packets': minimum_packets,
            'packet_count_features': packet_count_features,
            'missing_value_policy': 'reject',
            'scaler': 'scaler.json',
        },
        'model': 'model.onnx',
    }
    if 'data_version' in training:
        data_version = str(training['data_version'])
        if not data_version.strip():
            raise ValueError('data_version must be a non-empty string')
        manifest['data_version'] = data_version
    with (output_path / 'scaler.json').open(
            'w', encoding='utf-8') as stream:
        json.dump(scaler_document, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    with (output_path / 'manifest.json').open(
            'w', encoding='utf-8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    logging.info(
        'Schema 模型包已生成: %s (hash=%s, input_features=%d, '
        'input_values=%d)',
        output_path, schema_hash, len(features), input_value_count)
    return output_path


def _log_family_support(prepared, group_count):
    metadata = prepared['metadata']
    if 'meta.ip_version' not in metadata.columns:
        logging.warning('旧 CSV 无地址族元数据，仅能训练整体模型')
        return
    for label in range(group_count):
        mask = prepared['y'] == label
        support = metadata.loc[mask, 'meta.ip_version'].value_counts()
        ipv4 = int(support.get(4, 0))
        ipv6 = int(support.get(6, 0))
        logging.info(
            'label=%d support: IPv4=%d, IPv6=%d',
            label, ipv4, ipv6)
        if ipv4 == 0 or ipv6 == 0:
            missing = '/'.join(
                family for family, support_count in (
                    ('IPv4', ipv4), ('IPv6', ipv6))
                if support_count == 0)
            logging.warning(
                '标签 %d 在报文数过滤后没有 %s 训练样本；仍继续训练，'
                '但该地址族的模型效果未经训练验证',
                label,
                missing)


def KTAG_Train(config_file):
    logging.info('Loaded config file: %s', config_file)
    config_path = Path(config_file).expanduser().resolve()
    config_dir = config_path.parent
    try:
        with config_path.open('r', encoding='utf-8') as stream:
            config = yaml.safe_load(stream)
        if not isinstance(config, dict):
            raise ValueError('config root must be a mapping')
        training_groups, evaluation_groups, _ = validate_dataset_config(
            config, config_dir)
        combined = load_labeled_data(
            training_groups, config_dir, randomize=True)
        prepared = prepare_model_data(combined, config)
        _log_family_support(prepared, len(training_groups))

        output_dir = resolve_path(config['output_dir'], config_dir)
        create_directory(output_dir)
        model_path_pkl = resolve_path(config['model_path_pkl'], config_dir)
        model_path_onnx = resolve_path(config['model_path_onnx'], config_dir)
        scaler_path_pkl = resolve_path(config['scaler_path_pkl'], config_dir)
        scaler_path_json = resolve_path(config['scaler_path_json'], config_dir)
        for path in (
                model_path_pkl, model_path_onnx,
                scaler_path_pkl, scaler_path_json):
            path.parent.mkdir(parents=True, exist_ok=True)

        logging.info('Start training the model...')
        model, scaler = train_random_forest(prepared['X'], prepared['y'])
        joblib.dump(model, model_path_pkl)
        logging.info('Model saved as %s', model_path_pkl)

        initial_type = [
            ('float_input', FloatTensorType([None, prepared['X'].shape[1]]))
        ]
        onnx_model = convert_sklearn(
            model,
            initial_types=initial_type,
            options={id(model): {'zipmap': False}},
        )
        with model_path_onnx.open('wb') as stream:
            stream.write(onnx_model.SerializeToString())
        logging.info('ONNX model saved as %s', model_path_onnx)

        joblib.dump(scaler, scaler_path_pkl)
        logging.info('Scaler saved as %s', scaler_path_pkl)
        scaler_params = {
            'mean': scaler.mean_.tolist(),
            'scale': scaler.scale_.tolist(),
            'feature_names': (
                scaler.feature_names_in_.tolist()
                if hasattr(scaler, 'feature_names_in_') else []),
        }
        with scaler_path_json.open('w', encoding='utf-8') as stream:
            json.dump(scaler_params, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        logging.info('StandardScaler 参数已导出到 %s', scaler_path_json)

        _save_feature_importance(
            model, prepared['X'].columns, config, config_dir,
            prepared, scaler)
        create_pipeline_package(config, config_path, scaler, prepared)
        logging.info(
            '训练完成: labels=%d, evaluation_groups=%d, input_values=%d',
            len(training_groups), len(evaluation_groups),
            prepared['X'].shape[1])
        return 0
    except Exception as exc:
        logging.exception('Training failed: %s', exc)
        return -1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        logging.info('Usage: python3 training.py <path_to_config>')
        sys.exit(1)
    sys.exit(0 if KTAG_Train(sys.argv[1]) == 0 else 1)
