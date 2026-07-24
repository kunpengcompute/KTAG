#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

from pathlib import Path
import logging

def resolve_path(path_str, base_dir=None):
    """
    解析路径，支持相对路径和绝对路径

    Args:
        path_str: 路径字符串
        base_dir: 相对路径的基准目录，默认为脚本所在目录

    Returns:
        Path: 解析后的绝对路径
    """
    path = Path(path_str)

    # 如果已经是绝对路径，直接返回
    if path.is_absolute():
        return path.resolve()

    # 如果是相对路径，基于 base_dir 解析
    if base_dir is None:
        base_dir = Path.cwd()
    else:
        base_dir = Path(base_dir).expanduser()
    return (base_dir / path).resolve()


def schema_training_config(config):
    pipeline = config.get('pipeline')
    if not isinstance(pipeline, dict):
        return None
    training = pipeline.get('training')
    return training if isinstance(training, dict) else None


def normalize_input_features(training):
    configured = training.get('input_features')
    if not isinstance(configured, list) or not configured:
        raise ValueError(
            "pipeline.training.input_features must be a non-empty list")

    features = []
    identifiers = set()
    csv_columns = set()
    for item in configured:
        if isinstance(item, str):
            feature_id = item
            dimension = 1
            columns = [item]
        elif isinstance(item, dict):
            feature_id = item.get('feature_id')
            columns = item.get('columns')
            dimension = item.get('dimension')
            if not isinstance(columns, list) or not columns:
                raise ValueError(
                    f"training columns are required for feature {feature_id}")
            if dimension is None:
                dimension = len(columns)
        else:
            raise ValueError(
                "each training input feature must be a string or mapping")

        if not isinstance(feature_id, str) or not feature_id:
            raise ValueError("training feature_id must be a non-empty string")
        if feature_id in identifiers:
            raise ValueError(f"duplicate training feature_id: {feature_id}")
        if not isinstance(dimension, int) or dimension <= 0:
            raise ValueError(f"invalid dimension for feature {feature_id}")
        if len(columns) != dimension or any(
                not isinstance(column, str) or not column for column in columns):
            raise ValueError(
                f"columns do not match dimension for feature {feature_id}")
        if len(set(columns)) != len(columns):
            raise ValueError(
                f"duplicate columns within feature {feature_id}")
        duplicate_column = next(
            (column for column in columns if column in csv_columns), None)
        if duplicate_column is not None:
            raise ValueError(f"duplicate training CSV column: {duplicate_column}")

        identifiers.add(feature_id)
        csv_columns.update(columns)
        features.append({
            'feature_id': feature_id,
            'dimension': dimension,
            'columns': columns,
        })
    return features


def select_schema_training_columns(data, config):
    """Select model input values by stable feature ID training metadata."""
    training = schema_training_config(config)
    if training is None:
        return data
    features = normalize_input_features(training)
    columns = [
        column for feature in features for column in feature['columns']
    ]
    missing = [column for column in columns if column not in data.columns]
    if missing:
        raise ValueError(
            "training CSV is missing configured feature columns: " +
            ", ".join(missing))
    logging.info("按 Schema 训练契约选择 %d 个输入值", len(columns))
    return data.loc[:, columns]
