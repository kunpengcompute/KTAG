#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

from datetime import datetime
import logging
from pathlib import Path
import sys

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
)
import yaml

from utils import (
    load_labeled_data,
    prepare_model_data,
    resolve_path,
    schema_identity,
    validate_dataset_config,
)


logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s')


def load_and_prepare_data(
        data_paths, scaler_path, config, base_dir=None):
    """Compatibility wrapper returning the historical four values."""
    combined = load_labeled_data(data_paths, base_dir, randomize=False)
    prepared = prepare_model_data(combined, config)
    scaler = joblib.load(scaler_path)
    X = _validate_scaler_columns(scaler, prepared['X'], config)
    X_scaled = scaler.transform(X)
    return (
        X_scaled,
        prepared['y'],
        prepared['X'],
        prepared['raw_bow'],
    )


def _validate_scaler_columns(scaler, X, config=None):
    if len(getattr(scaler, 'mean_', [])) != X.shape[1]:
        raise ValueError(
            'scaler dimension does not match evaluation input: '
            f'{len(getattr(scaler, "mean_", []))} != {X.shape[1]}')
    if hasattr(scaler, 'feature_names_in_'):
        expected = list(scaler.feature_names_in_)
        actual = list(X.columns)
        if expected != actual:
            name, _, contract = schema_identity(config or {})
            if (name != 'ktag_legacy_v1' or contract is None or
                    X.shape[1] != contract['input_values'] or
                    len(expected) != contract['input_values']):
                raise ValueError(
                    'evaluation Feature columns/order do not match trained '
                    'scaler')
            # Historical application CSVs and modern Schema CSVs carry
            # different column names for the same frozen positional Legacy
            # V1 contract.  Both sides have already passed the 351-value
            # contract check, so renaming here allows an old IPv4 model and a
            # new dual-stack model to be evaluated on the same regenerated
            # IPv4 test set without weakening any custom/proxy Schema check.
            X = X.copy()
            X.columns = expected
            logging.warning(
                'ktag_legacy_v1 Scaler 使用历史列名；已按冻结的351维顺序'
                '对齐评估输入，用于新旧应用模型同集比较')
    return X


def _matrix_path(report_path, section):
    stem = report_path.stem
    suffix = '' if section == 'overall' else f'_{section}'
    return report_path.with_name(
        f'{stem}{suffix}_confusion_matrix.csv')


def _write_confusion_matrix(path, matrix, labels):
    path.parent.mkdir(parents=True, exist_ok=True)
    frame = pd.DataFrame(
        matrix,
        index=[f'true_{label}' for label in labels],
        columns=[f'predicted_{label}' for label in labels],
    )
    frame.to_csv(path, encoding='utf-8')
    logging.info('Confusion Matrix 已保存到: %s', path)


def _build_report_section(name, y_true, y_pred, labels):
    if len(y_true) == 0:
        return {
            'name': name,
            'support': 0,
            'report': 'No samples; this address family is not evaluable.',
            'accuracy': None,
            'matrix': np.zeros((len(labels), len(labels)), dtype=int),
        }
    return {
        'name': name,
        'support': len(y_true),
        'report': classification_report(
            y_true,
            y_pred,
            labels=labels,
            digits=4,
            zero_division=0,
        ),
        'accuracy': accuracy_score(y_true, y_pred),
        'matrix': confusion_matrix(y_true, y_pred, labels=labels),
    }


def _family_sections(y, y_pred, metadata, labels):
    sections = [_build_report_section('overall', y, y_pred, labels)]
    if 'meta.ip_version' not in metadata.columns:
        logging.warning(
            '旧 CSV 不含 meta.ip_version，仅输出整体 Classification Report')
        return sections
    try:
        versions = pd.to_numeric(
            metadata['meta.ip_version'], errors='raise').astype(int)
    except (ValueError, TypeError) as exc:
        raise ValueError('meta.ip_version contains invalid values') from exc
    invalid = sorted(set(versions) - {4, 6})
    if invalid:
        raise ValueError(
            'meta.ip_version contains values other than 4/6: ' +
            ', '.join(str(value) for value in invalid))
    for version, name in ((4, 'ipv4'), (6, 'ipv6')):
        mask = versions.to_numpy() == version
        sections.append(_build_report_section(
            name,
            y.to_numpy()[mask],
            y_pred[mask],
            labels,
        ))
    if sections[0]['support'] != sections[1]['support'] + sections[2]['support']:
        raise ValueError('IPv4/IPv6 support does not sum to overall support')
    return sections


def _write_report(report_path, sections, labels, y, metadata):
    report_path.parent.mkdir(parents=True, exist_ok=True)
    with report_path.open('w', encoding='utf-8') as stream:
        stream.write('=' * 80 + '\n')
        stream.write('Classification Report\n')
        stream.write(
            f'Generated at: {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}\n')
        stream.write('=' * 80 + '\n')
        for section in sections:
            stream.write(f'\n[{section["name"]}] support={section["support"]}\n')
            stream.write(section['report'])
            if section['accuracy'] is not None:
                stream.write(f'\nAccuracy: {section["accuracy"]:.4f}\n')
            stream.write('Confusion Matrix:\n')
            stream.write(np.array2string(section['matrix']) + '\n')

        stream.write('\n[per-label address-family support]\n')
        if 'meta.ip_version' in metadata.columns:
            versions = pd.to_numeric(metadata['meta.ip_version']).astype(int)
            for label in labels:
                label_mask = y.to_numpy() == label
                ipv4 = int(np.count_nonzero(
                    label_mask & (versions.to_numpy() == 4)))
                ipv6 = int(np.count_nonzero(
                    label_mask & (versions.to_numpy() == 6)))
                stream.write(
                    f'label={label} IPv4={ipv4} IPv6={ipv6} '
                    f'total={ipv4 + ipv6}\n')
        else:
            for label in labels:
                support = int(np.count_nonzero(y.to_numpy() == label))
                stream.write(
                    f'label={label} IPv4=unavailable IPv6=unavailable '
                    f'total={support}\n')
        stream.write('=' * 80 + '\n')
    logging.info('Classification Report 已保存到: %s', report_path)


def predict_new_data(params):
    """Predict labels and always preserve the model probability/confidence."""
    model = params['model']
    X_scaled = params['X_scaled']
    y = params['y'].reset_index(drop=True)
    X_original = params['X_original'].reset_index(drop=True)
    metadata = params.get('metadata')
    if metadata is None:
        metadata = pd.DataFrame(index=X_original.index)
    else:
        metadata = metadata.reset_index(drop=True)
    raw_bow = params.get('raw_bow')
    report_path = Path(params['report_output_path'])
    prediction_path = Path(params['prediction_output_path'])
    labels = list(params.get('labels', sorted(set(y.tolist()))))

    if not hasattr(model, 'predict_proba'):
        raise ValueError('evaluation model must provide predict_proba')
    y_pred = np.asarray(model.predict(X_scaled))
    probabilities = np.asarray(model.predict_proba(X_scaled))
    if (probabilities.ndim != 2 or probabilities.shape[0] != len(y) or
            probabilities.shape[1] == 0):
        raise ValueError('predict_proba returned an invalid probability matrix')
    confidence = np.max(probabilities, axis=1)
    if (not np.isfinite(probabilities).all() or
            np.any(probabilities < 0.0) or np.any(probabilities > 1.0)):
        raise ValueError('predict_proba returned invalid probability values')

    sections = _family_sections(y, y_pred, metadata, labels)
    for section in sections:
        logging.info(
            '%s evaluation support=%d%s\n%s',
            section['name'],
            section['support'],
            ('' if section['accuracy'] is None else
             f', accuracy={section["accuracy"]:.4f}'),
            section['report'])
        _write_confusion_matrix(
            _matrix_path(report_path, section['name']),
            section['matrix'],
            labels,
        )
    _write_report(report_path, sections, labels, y, metadata)

    prediction_path.parent.mkdir(parents=True, exist_ok=True)
    result = pd.concat([metadata, X_original], axis=1)
    if raw_bow is not None:
        result['raw_bow'] = raw_bow.reset_index(drop=True)
    result['predicted_label'] = y_pred
    result['true_label'] = y.to_numpy()
    result['confidence'] = confidence
    result.to_csv(prediction_path, index=False, encoding='utf-8')
    logging.info('Prediction details 已保存到: %s', prediction_path)
    logging.info('Total samples: %d', len(result))
    return {
        'predictions': y_pred,
        'confidence': confidence,
        'sections': sections,
        'details': result,
    }


def KTAG_Evaluation(config_file):
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
        if ('anomaly_detect_switch' in config or
                'anomaly_detect_limit' in config):
            logging.warning(
                'anomaly threshold label rewriting is disabled; '
                'the RandomForest label is preserved')

        combined = load_labeled_data(
            evaluation_groups, config_dir, randomize=False)
        prepared = prepare_model_data(combined, config)
        scaler_path = resolve_path(config['scaler_path_pkl'], config_dir)
        model_path = resolve_path(config['model_path_pkl'], config_dir)
        scaler = joblib.load(scaler_path)
        X = _validate_scaler_columns(scaler, prepared['X'], config)
        X_scaled = scaler.transform(X)
        model = joblib.load(model_path)

        result = predict_new_data({
            'model': model,
            'X_scaled': X_scaled,
            'y': prepared['y'],
            'X_original': X,
            'metadata': prepared['metadata'],
            'raw_bow': prepared['raw_bow'],
            'labels': list(range(len(training_groups))),
            'report_output_path': resolve_path(
                config['classification_report_file'], config_dir),
            'prediction_output_path': resolve_path(
                config['predictions_detail_file'], config_dir),
        })
        logging.info(
            '评估完成: labels=%d, input_values=%d, samples=%d',
            len(training_groups), prepared['X'].shape[1],
            len(result['predictions']))
        return 0
    except Exception as exc:
        logging.exception('Evaluation failed: %s', exc)
        return -1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        logging.info('Usage: python3 evaluation.py <path_to_config>')
        sys.exit(1)
    sys.exit(0 if KTAG_Evaluation(sys.argv[1]) == 0 else 1)
