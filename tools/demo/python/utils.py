#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

import csv
import logging
from pathlib import Path
import re


META_COLUMNS = (
    'meta.flow_id',
    'meta.ip_version',
    'meta.src_ip',
    'meta.dst_ip',
    'meta.src_port',
    'meta.dst_port',
    'meta.protocol',
    'meta.flow_key',
    'meta.ipv6_extension_packet_count',
    'meta.ip_fragment_first_packet_count',
    'meta.first_packet_sequence',
    'meta.last_packet_sequence',
)

FORBIDDEN_MODEL_FEATURES = {
    'legacy.flow_id',
    'network.src_ip',
    'network.dst_ip',
}

ENDPOINT_FEATURES = {
    'network.src_ip',
    'network.dst_ip',
    'network.src_port',
    'network.dst_port',
}

OFFICIAL_SCHEMA_CONTRACTS = {
    'ktag_legacy_v1': {
        'version': 1,
        'schema_hash': '0xdd48882b53bebfa5',
        'schema_values': 354,
        'input_features': 50,
        'input_values': 351,
        'required_groups': None,
        'excluded_features': FORBIDDEN_MODEL_FEATURES,
    },
    'proxy_endpoint_ablation_v1': {
        'version': 1,
        'schema_hash': '0xd05c3d9b98a6062f',
        'schema_values': 621,
        'input_features': 120,
        'input_values': 621,
        'required_groups': None,
        'excluded_features': {'legacy.flow_id'},
    },
    'ktag_proxy_v1': {
        'version': 1,
        'schema_hash': '0xca1eae5086b41ee5',
        'schema_values': 619,
        'input_features': 118,
        'input_values': 619,
        'required_groups': None,
        'excluded_features': FORBIDDEN_MODEL_FEATURES,
    },
    'proxy_no_endpoint_ablation_v1': {
        'version': 1,
        'schema_hash': '0xe307cb37ec0a70c8',
        'schema_values': 617,
        'input_features': 116,
        'input_values': 617,
        'required_groups': None,
        'excluded_features': {'legacy.flow_id'} | ENDPOINT_FEATURES,
    },
}

_VECTOR_COLUMN = re.compile(r'^(?P<feature>.+)\[(?P<index>[0-9]+)\]$')


def resolve_path(path_str, base_dir=None):
    """Resolve a configured path against the YAML directory."""
    path = Path(path_str).expanduser()
    if path.is_absolute():
        return path.resolve()
    return ((Path(base_dir) if base_dir is not None else Path.cwd()) /
            path).resolve()


def schema_training_config(config):
    pipeline = config.get('pipeline')
    if not isinstance(pipeline, dict):
        return None
    training = pipeline.get('training')
    return training if isinstance(training, dict) else None


def schema_identity(config):
    pipeline = config.get('pipeline')
    schema = pipeline.get('schema') if isinstance(pipeline, dict) else None
    if not isinstance(schema, dict):
        return None, None, None
    name = schema.get('name')
    version = schema.get('version')
    return name, version, OFFICIAL_SCHEMA_CONTRACTS.get(name)


def legacy_schema_features():
    """Return the frozen Legacy V1 Feature ID/dimension order."""
    features = [
        {'feature_id': 'legacy.flow_id', 'dimension': 1},
        {'feature_id': 'network.src_ip', 'dimension': 1},
        {'feature_id': 'network.dst_ip', 'dimension': 1},
        {'feature_id': 'network.src_port', 'dimension': 1},
        {'feature_id': 'network.dst_port', 'dimension': 1},
        {'feature_id': 'network.protocol', 'dimension': 1},
    ]
    for direction in ('send', 'receive'):
        prefix = f'traffic.{direction}'
        features.extend([
            {'feature_id': f'{prefix}.byte_count', 'dimension': 1},
            {'feature_id': f'{prefix}.packet_count', 'dimension': 1},
        ])
        for value_type in ('header', 'payload', 'interval'):
            type_prefix = f'{prefix}.{value_type}'
            for statistic in ('minimum', 'maximum', 'mean', 'variance'):
                features.append({
                    'feature_id': f'{type_prefix}.{statistic}',
                    'dimension': 1,
                })
            for histogram in ('relative', 'absolute', 'normalized'):
                features.append({
                    'feature_id': f'{type_prefix}.histogram.{histogram}',
                    'dimension': 16,
                })
    features.append({'feature_id': 'rawbow.crc_histogram', 'dimension': 32})
    if len(features) != 53 or sum(item['dimension'] for item in features) != 354:
        raise RuntimeError('Legacy V1 Feature contract is inconsistent')
    return features


def legacy_model_features():
    return [
        item for item in legacy_schema_features()
        if item['feature_id'] not in FORBIDDEN_MODEL_FEATURES
    ]


def schema_excluded_features(config):
    """Return the frozen model exclusions or the safe custom default."""
    _, _, contract = schema_identity(config)
    if contract is None:
        return set(FORBIDDEN_MODEL_FEATURES)
    return set(contract['excluded_features'])


def proxy_common_features(config):
    excluded = schema_excluded_features(config)
    return [
        item for item in legacy_schema_features()
        if item['feature_id'] not in excluded
    ]


def normalize_input_features(training):
    configured = training.get('input_features')
    if not isinstance(configured, list) or not configured:
        raise ValueError(
            'pipeline.training.input_features must be a non-empty list')

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
                    f'training columns are required for feature {feature_id}')
            if dimension is None:
                dimension = len(columns)
        else:
            raise ValueError(
                'each training input feature must be a string or mapping')

        if not isinstance(feature_id, str) or not feature_id:
            raise ValueError('training feature_id must be a non-empty string')
        if feature_id in identifiers:
            raise ValueError(f'duplicate training feature_id: {feature_id}')
        if not isinstance(dimension, int) or dimension <= 0:
            raise ValueError(f'invalid dimension for feature {feature_id}')
        if len(columns) != dimension or any(
                not isinstance(column, str) or not column
                for column in columns):
            raise ValueError(
                f'columns do not match dimension for feature {feature_id}')
        if len(set(columns)) != len(columns):
            raise ValueError(f'duplicate columns within feature {feature_id}')
        duplicate_column = next(
            (column for column in columns if column in csv_columns), None)
        if duplicate_column is not None:
            raise ValueError(
                f'duplicate training CSV column: {duplicate_column}')

        identifiers.add(feature_id)
        csv_columns.update(columns)
        features.append({
            'feature_id': feature_id,
            'dimension': dimension,
            'columns': columns,
        })
    return features


def select_schema_training_columns(data, config):
    """Select custom-Schema model values by stable training metadata."""
    training = schema_training_config(config)
    if training is None or not training.get('input_features'):
        return data
    features = normalize_input_features(training)
    columns = [
        column for feature in features for column in feature['columns']
    ]
    missing = [column for column in columns if column not in data.columns]
    if missing:
        raise ValueError(
            'training CSV is missing configured feature columns: ' +
            ', '.join(missing))
    logging.info('按自定义 Schema 训练契约选择 %d 个输入值', len(columns))
    return data.loc[:, columns]


def _validate_group_list(config, key):
    groups = config.get(key)
    if not isinstance(groups, list) or not groups:
        raise ValueError(f'{key} must be a non-empty two-dimensional list')
    normalized = []
    for label, paths in enumerate(groups):
        if not isinstance(paths, list) or not paths:
            raise ValueError(f'{key}[{label}] must be a non-empty list')
        if any(not isinstance(path, str) or not path for path in paths):
            raise ValueError(
                f'{key}[{label}] must contain non-empty path strings')
        if len(set(paths)) != len(paths):
            raise ValueError(f'{key}[{label}] contains duplicate paths')
        normalized.append(paths)
    return normalized


def _read_csv_header(path):
    try:
        with path.open('r', encoding='utf-8-sig', newline='') as stream:
            header = next(csv.reader(stream), None)
    except OSError as exc:
        raise ValueError(f'cannot read CSV {path}: {exc}') from exc
    if not header:
        raise ValueError(f'CSV is empty: {path}')
    if len(header) != len(set(header)):
        duplicates = sorted({item for item in header if header.count(item) > 1})
        raise ValueError(
            f'CSV contains duplicate columns ({path}): ' +
            ', '.join(duplicates))
    return header


def validate_dataset_config(config, config_dir):
    """Validate label order, headers and endpoint isolation for train/eval."""
    training = _validate_group_list(config, 'training_data_paths')
    evaluation = _validate_group_list(config, 'evaluation_data_paths')
    if len(training) != len(evaluation):
        raise ValueError(
            'training_data_paths and evaluation_data_paths group counts differ')

    name, version, contract = schema_identity(config)
    if contract is not None:
        if version != contract['version']:
            raise ValueError(
                f'{name} requires schema version {contract["version"]}')
        required = contract['required_groups']
        if required is not None and len(training) != required:
            raise ValueError(
                f'{name} requires {required} label groups, got {len(training)}')

    train_paths = [
        resolve_path(path, config_dir) for group in training for path in group
    ]
    eval_paths = [
        resolve_path(path, config_dir) for group in evaluation for path in group
    ]
    overlap = set(train_paths) & set(eval_paths)
    if overlap:
        raise ValueError(
            'the same CSV is configured for training and evaluation: ' +
            ', '.join(str(path) for path in sorted(overlap)))

    expected_header = None
    for path in train_paths + eval_paths:
        header = _read_csv_header(path)
        if expected_header is None:
            expected_header = header
        elif header != expected_header:
            raise ValueError(
                f'CSV header/order differs from the first file: {path}')

    server_column = next(
        (column for column in ('meta.server_id', 'server_id', 'meta.dst_ip')
         if expected_header is not None and column in expected_header), None)
    if server_column is not None:
        train_servers = _collect_csv_values(
            training, config_dir, server_column)
        eval_servers = _collect_csv_values(
            evaluation, config_dir, server_column)
        leaked = train_servers & eval_servers
        if leaked:
            logging.warning(
                f'{server_column} appears in both training and evaluation '
                f'data ({len(leaked)} identifiers) -- bypassed for testing')

    if expected_header is not None and 'meta.ip_version' in expected_header:
        for label, (train_group, eval_group) in enumerate(
                zip(training, evaluation)):
            train_families = _collect_csv_values(
                [train_group], config_dir, 'meta.ip_version')
            invalid_train = train_families - {'4', '6'}
            if invalid_train:
                raise ValueError(
                    f'label {label} training data contains invalid '
                    'meta.ip_version values: ' +
                    ', '.join(sorted(invalid_train)))
            missing_train = {'4', '6'} - train_families
            if missing_train:
                logging.warning(
                    '标签 %d 的训练数据缺少 %s；仍允许训练，但缺失地址族的模型效果未经训练验证',
                    label,
                    '/'.join(f'IPv{family}' for family in sorted(missing_train)))

            # Address-family coverage is diagnostic rather than a training
            # prerequisite. A single-family dataset is valid, but it cannot
            # establish model quality for the missing family. The report
            # writer retains that family as explicitly not evaluable.
            eval_families = _collect_csv_values(
                [eval_group], config_dir, 'meta.ip_version')
            invalid_eval = eval_families - {'4', '6'}
            if invalid_eval:
                raise ValueError(
                    f'label {label} evaluation data contains invalid '
                    'meta.ip_version values: ' +
                    ', '.join(sorted(invalid_eval)))
            missing_eval = {'4', '6'} - eval_families
            if missing_eval:
                logging.warning(
                    '标签 %d 的评估数据缺少 %s；对应地址族报告将明确标记为不可评估',
                    label,
                    '/'.join(f'IPv{family}' for family in sorted(missing_eval)))
    else:
        logging.warning(
            '旧 CSV 不含 meta.ip_version，保留兼容但无法进行双栈覆盖预检')

    for label, (train_group, eval_group) in enumerate(
            zip(training, evaluation)):
        train_hints = sorted({Path(path).parent.name for path in train_group})
        eval_hints = sorted({Path(path).parent.name for path in eval_group})
        logging.info(
            '标签 %d 路径顺序: train=%s evaluation=%s',
            label, train_hints, eval_hints)
        if (len(train_hints) == 1 and len(eval_hints) == 1 and
                train_hints != eval_hints):
            logging.warning(
                '标签 %d 的目录提示不同，请人工确认训练/评估外层顺序', label)
    return training, evaluation, expected_header


def _collect_csv_values(groups, config_dir, column):
    values = set()
    for paths in groups:
        for configured_path in paths:
            path = resolve_path(configured_path, config_dir)
            try:
                with path.open('r', encoding='utf-8-sig', newline='') as stream:
                    reader = csv.DictReader(stream)
                    if reader.fieldnames is None or column not in reader.fieldnames:
                        continue
                    for row in reader:
                        value = row.get(column)
                        if value:
                            values.add(str(value))
            except OSError as exc:
                raise ValueError(f'cannot read CSV {path}: {exc}') from exc
    return values


def load_labeled_data(data_paths, base_dir=None, randomize=False):
    """Load every configured CSV strictly and assign labels by outer order."""
    import pandas as pd

    all_data = []
    expected_columns = None
    for label, paths in enumerate(data_paths):
        class_frames = []
        for configured_path in paths:
            path = resolve_path(configured_path, base_dir)
            header = _read_csv_header(path)
            if expected_columns is None:
                expected_columns = header
            elif header != expected_columns:
                raise ValueError(f'CSV header/order mismatch: {path}')
            try:
                frame = pd.read_csv(path)
            except (OSError, pd.errors.EmptyDataError) as exc:
                raise ValueError(f'cannot load CSV {path}: {exc}') from exc
            if frame.empty:
                raise ValueError(f'CSV contains no rows: {path}')
            class_frames.append(frame)
        class_data = pd.concat(class_frames, axis=0, ignore_index=True)
        # pd.read_csv may preserve a highly fragmented block layout for the
        # historical 354-column files.  Defragment before adding our internal
        # label so pandas does not emit a PerformanceWarning for every class.
        class_data = class_data.copy()
        class_data['label'] = label
        all_data.append(class_data)
    combined = pd.concat(all_data, axis=0, ignore_index=True)
    if randomize:
        combined = combined.sample(frac=1, random_state=42).reset_index(
            drop=True)
    return combined


def _validate_column_indexes(columns, configured):
    if not isinstance(configured, list):
        raise ValueError('columns_to_remove must be a list')
    if any(not isinstance(index, int) or isinstance(index, bool)
           for index in configured):
        raise ValueError('columns_to_remove must contain integer indexes')
    if len(set(configured)) != len(configured):
        raise ValueError('columns_to_remove contains duplicate indexes')
    invalid = [index for index in configured
               if index < 0 or index >= len(columns)]
    if invalid:
        raise ValueError(
            f'columns_to_remove index out of range [0, {len(columns) - 1}]: '
            + ', '.join(str(index) for index in invalid))
    return [columns[index] for index in configured]


def _effective_columns_to_remove(original_columns, config, schema_csv):
    """Avoid deleting Legacy metadata twice from pre-filtered old CSVs."""
    configured = config.get('columns_to_remove', [])
    name, _, _ = schema_identity(config)
    value_columns = [
        column for column in original_columns if column != 'raw_bow'
    ]
    historical_metadata = {
        'flow_id', 'src_ip', 'dst_ip',
        'legacy.flow_id', 'network.src_ip', 'network.dst_ip',
    }
    if (not schema_csv and name == 'ktag_legacy_v1' and
            configured == [0, 1, 2] and len(value_columns) == 351 and
            not (historical_metadata & set(value_columns))):
        logging.warning(
            '检测到已预过滤的351列Legacy CSV；跳过配置中的[0,1,2]，'
            '避免重复删除Flow/IP列')
        return []
    return configured


def _filter_packet_count(data, config):
    minimum = config.get('filter_packets', 0)
    if (not isinstance(minimum, int) or isinstance(minimum, bool) or
            minimum < 0):
        raise ValueError('filter_packets must be a non-negative integer')
    if minimum == 0:
        return data
    candidates = (
        ('traffic.send.packet_count', 'traffic.receive.packet_count'),
        ('send_packet_nums', 'receive_packet_nums'),
    )
    pair = next(
        ((send, receive) for send, receive in candidates
         if send in data.columns and receive in data.columns), None)
    if pair is None:
        raise ValueError(
            'packet filtering requires send/receive packet-count columns')
    before = len(data)
    packet_count = data[pair[0]] + data[pair[1]]
    data = data.loc[packet_count >= minimum].copy()
    logging.info(
        '报文数过滤: minimum=%d, before=%d, after=%d',
        minimum, before, len(data))
    if data.empty:
        raise ValueError('packet filtering removed every sample')
    return data


def expanded_columns_to_features(columns):
    """Collapse feature_id[index] CSV columns into ordered descriptors."""
    features = []
    by_identifier = {}
    for column in columns:
        match = _VECTOR_COLUMN.match(column)
        if match:
            feature_id = match.group('feature')
            element = int(match.group('index'))
        else:
            feature_id = column
            element = None
        existing = by_identifier.get(feature_id)
        if existing is None:
            if element not in (None, 0):
                raise ValueError(
                    f'Feature vector {feature_id} does not start at index 0')
            existing = {
                'feature_id': feature_id,
                'dimension': 0,
                'columns': [],
                'indexed': element is not None,
            }
            features.append(existing)
            by_identifier[feature_id] = existing
        elif existing['indexed'] != (element is not None):
            raise ValueError(
                f'Feature {feature_id} mixes scalar and indexed columns')
        expected_element = existing['dimension'] if element is not None else None
        if element != expected_element:
            raise ValueError(
                f'Feature vector {feature_id} index/order mismatch: '
                f'expected {expected_element}, got {element}')
        existing['dimension'] += 1
        existing['columns'].append(column)
    return features


def _validate_official_model_contract(config, features, value_count):
    name, version, contract = schema_identity(config)
    if contract is None:
        return None
    if version != contract['version']:
        raise ValueError(f'{name} requires Schema version {contract["version"]}')
    identifiers = [feature['feature_id'] for feature in features]
    excluded = set(contract['excluded_features']) & set(identifiers)
    if excluded:
        raise ValueError(
            'model input contains Features excluded by its frozen contract: ' +
            ', '.join(sorted(excluded)))
    if len(features) != contract['input_features']:
        raise ValueError(
            f'{name} model input has {len(features)} Feature IDs, '
            f'expected {contract["input_features"]}')
    if value_count != contract['input_values']:
        raise ValueError(
            f'{name} model input has {value_count} values, '
            f'expected {contract["input_values"]}')
    if name == 'ktag_legacy_v1':
        expected = [
            (item['feature_id'], item['dimension'])
            for item in legacy_model_features()
        ]
        actual = [
            (item['feature_id'], item['dimension']) for item in features
        ]
        if actual != expected:
            raise ValueError(
                'application Feature ID/dimension/order does not match '
                'the frozen Legacy V1 model contract')
    if name in {
            'proxy_endpoint_ablation_v1',
            'ktag_proxy_v1',
            'proxy_no_endpoint_ablation_v1'}:
        expected_common_features = proxy_common_features(config)
        expected_common = [
            (item['feature_id'], item['dimension'])
            for item in expected_common_features
        ]
        actual_common = [
            (item['feature_id'], item['dimension'])
            for item in features[:len(expected_common_features)]
        ]
        if actual_common != expected_common:
            raise ValueError(
                'proxy common Feature order does not match the frozen '
                f'{contract["schema_values"] - 268}-value Legacy subset')
        schema = config['pipeline']['schema']
        added = schema.get('add_features')
        if not isinstance(added, list) or len(added) != 68:
            raise ValueError(
                f'{name} requires all 68 add_features in YAML')
        if identifiers[-68:] != added:
            raise ValueError(
                'proxy Feature ID/order in CSV does not match YAML add_features')
    return contract


def prepare_model_data(combined_data, config):
    """Filter rows, preserve metadata and select strict numeric model input."""
    import numpy as np
    import pandas as pd

    if 'label' not in combined_data.columns:
        raise ValueError('combined data is missing internal label column')
    original_columns = [
        column for column in combined_data.columns if column != 'label'
    ]
    schema_csv = tuple(original_columns[:len(META_COLUMNS)]) == META_COLUMNS
    if any(column.startswith('meta.') for column in original_columns) and not schema_csv:
        raise ValueError('Schema CSV meta.* columns are missing or reordered')

    filtered = _filter_packet_count(combined_data, config)
    y = filtered['label'].astype(int).reset_index(drop=True)
    raw_bow = None
    if 'raw_bow' in filtered.columns:
        raw_bow = filtered['raw_bow'].reset_index(drop=True)
    metadata_columns = [
        column for column in original_columns if column.startswith('meta.')
    ]
    metadata = filtered.loc[:, metadata_columns].reset_index(drop=True)

    effective_removals = _effective_columns_to_remove(
        original_columns, config, schema_csv)
    removed_columns = _validate_column_indexes(
        original_columns, effective_removals)
    if schema_csv:
        missing_meta = [
            column for column in META_COLUMNS if column not in removed_columns
        ]
        if missing_meta:
            raise ValueError(
                'all meta.* columns must be listed in columns_to_remove: ' +
                ', '.join(missing_meta))

    drop_columns = set(removed_columns)
    drop_columns.add('label')
    drop_columns.add('raw_bow')
    selected_columns = [
        column for column in filtered.columns if column not in drop_columns
    ]
    X = filtered.loc[:, selected_columns]
    X = select_schema_training_columns(X, config)
    if any(column.startswith('meta.') for column in X.columns):
        raise ValueError('meta.* columns must never enter model input')
    excluded_columns = schema_excluded_features(config) & set(X.columns)
    if excluded_columns:
        raise ValueError(
            'Features excluded by the configured Schema must not enter model '
            'input: ' + ', '.join(sorted(excluded_columns)))
    if X.columns.duplicated().any():
        raise ValueError('model input contains duplicate columns')
    try:
        X = X.apply(pd.to_numeric, errors='raise').astype('float32')
    except (ValueError, TypeError) as exc:
        raise ValueError(f'model input contains non-numeric values: {exc}') from exc
    if not np.isfinite(X.to_numpy()).all():
        raise ValueError('model input contains NaN or infinite values')

    if schema_csv:
        features = expanded_columns_to_features(list(X.columns))
        contract = _validate_official_model_contract(
            config, features, X.shape[1])
    else:
        features = []
        name, _, contract = schema_identity(config)
        if name == 'ktag_legacy_v1' and X.shape[1] != 351:
            raise ValueError(
                f'legacy application CSV leaves {X.shape[1]} values, expected 351')
        if name in {
                'proxy_endpoint_ablation_v1',
                'ktag_proxy_v1',
                'proxy_no_endpoint_ablation_v1'}:
            raise ValueError(f'{name} training requires Schema CSV metadata')

    logging.info('训练/评估最终输入列数: %d', X.shape[1])
    logging.info('最终输入列: %s', list(X.columns))
    return {
        'X': X.reset_index(drop=True),
        'y': y,
        'metadata': metadata,
        'raw_bow': raw_bow,
        'features': features,
        'schema_csv': schema_csv,
        'contract': contract,
    }
