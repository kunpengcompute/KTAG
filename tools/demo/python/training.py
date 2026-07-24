#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

import logging
from typing import List
from sklearn.ensemble import RandomForestClassifier
from sklearn.preprocessing import StandardScaler
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType
import yaml
import joblib
import json
import pandas as pd
import sys
import os
import subprocess
import shutil
from pathlib import Path
from utils import (
    normalize_input_features,
    resolve_path,
    schema_training_config,
    select_schema_training_columns,
)

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')


def create_directory(path):
    """创建目录（如果不存在）"""
    if not os.path.exists(path):
        os.makedirs(path)
        logging.info(f"创建目录: {path}")
    else:
        logging.info(f"结果保存路径: {path}")

def _load_and_concat(file_paths: List[str], base_dir=None):
    """Load and concatenate multiple CSV files"""
    dfs = []
    for file_path in file_paths:
        try:
            df = pd.read_csv(resolve_path(file_path, base_dir))
            dfs.append(df)
        except FileNotFoundError:
            logging.warning(f"{file_path} file not found. Skipping this file.")
            continue
        except PermissionError:
            logging.warning(f"No permission to load {file_path}. Skipping this file.")
            continue
        except pd.errors.EmptyDataError:
            logging.warning(f"File '{file_path}' is empty. Skipping this file.")
            continue
        except Exception as e:
            logging.warning(f"Error occurred while reading {file_path}: {str(e)}. Skipping this file.")
            continue
    if not dfs:
        raise ValueError("No data was successfully loaded from any of the provided CSV files.")

    return pd.concat(dfs, axis=0, ignore_index=True)


def load_and_prepare_data(data_paths: List[List[str]], base_dir=None):
    """
     Load and prepare the data, and add labels to the dataset.
    """
    labels = [i for i in range(len(data_paths))]

    all_data = []
    for paths, label in zip(data_paths, labels):
        class_data = _load_and_concat(paths, base_dir)
        class_data['label'] = label
        all_data.append(class_data)


    combined_data = pd.concat(all_data, axis=0)
    combined_data = combined_data.sample(frac=1).reset_index(drop=True)

    # Separate features and labels
    X = combined_data.drop('label', axis=1)
    y = combined_data['label']
    if 'raw_bow' in combined_data.columns:
        raw_bow = combined_data['raw_bow']
        X = combined_data.drop(['label', 'raw_bow'], axis=1)
    else:
        X = combined_data.drop('label', axis=1)

    return X, y


def train_random_forest(X, y, random_state=42):
    """
    Train a Random Forest Classifier

    args:
        X: Feature data
        y: labels
        random_state: Random Seed

    return:
        Trained model
    """
    # Data standardization (returns DataFrame instead of NumPy array)
    scaler = StandardScaler().set_output(transform="pandas")
    X_scaled = scaler.fit_transform(X)

    # Initialize the Random Forest Classifier
    rf = RandomForestClassifier(
        n_estimators=100,
        max_depth=10,
        min_samples_split=5,
        min_samples_leaf=2,
        random_state=random_state,
        oob_score=True
    )

    # Train the model
    rf.fit(X_scaled, y)
    logging.info("OOB Score: %s", rf.oob_score_)

    # print feature importance
    feature_importance = rf.feature_importances_
    features_df = pd.DataFrame({
        'Feature': X_scaled.columns,
        'Importance': feature_importance
    }).sort_values('Importance', ascending=False)
    logging.info("Top 30 features:\n%s", features_df.head(30))
    return rf, scaler


def _format_schema_hash(value):
    if isinstance(value, str):
        parsed = int(value, 0)
    elif isinstance(value, int):
        parsed = value
    else:
        raise ValueError("pipeline.training.schema_hash is required")
    if parsed <= 0 or parsed > ((1 << 64) - 1):
        raise ValueError("pipeline.training.schema_hash must be a non-zero uint64")
    return f"0x{parsed:016x}"


def create_schema_pipeline_package(config, config_path, scaler):
    """Package a custom Schema model without any fixed column indexes."""
    pipeline = config['pipeline']
    training = schema_training_config(config)
    features = normalize_input_features(training)
    schema_hash = _format_schema_hash(training.get('schema_hash'))
    output_path = resolve_path(pipeline['model_package_path'], config_path.parent)
    model_path = resolve_path(config['model_path_onnx'], config_path.parent)
    output_path.mkdir(parents=True, exist_ok=True)
    packaged_model_path = output_path / 'model.onnx'
    if model_path.resolve() != packaged_model_path.resolve():
        shutil.copy2(model_path, packaged_model_path)

    input_feature_ids = [feature['feature_id'] for feature in features]
    input_value_count = sum(feature['dimension'] for feature in features)
    if len(scaler.mean_) != input_value_count or len(scaler.scale_) != input_value_count:
        raise ValueError("trained scaler dimension does not match Schema contract")

    scaler_document = {
        'schema_hash': schema_hash,
        'input_features': input_feature_ids,
        'mean': scaler.mean_.tolist(),
        'scale': scaler.scale_.tolist(),
    }
    minimum_packets = int(
        training.get('minimum_total_packets', config.get('filter_packets', 0)))
    packet_count_features = training.get('packet_count_features', [])
    if minimum_packets < 0:
        raise ValueError("minimum_total_packets must not be negative")
    if minimum_packets > 0 and (
            not isinstance(packet_count_features, list) or
            not packet_count_features):
        raise ValueError(
            "packet_count_features are required when packet filtering is enabled")
    if (not isinstance(packet_count_features, list) or
            any(not isinstance(item, str) or not item
                for item in packet_count_features) or
            len(set(packet_count_features)) != len(packet_count_features)):
        raise ValueError(
            "packet_count_features must contain unique non-empty feature IDs")

    model_name = training.get('model_name', model_path.stem)
    if not isinstance(model_name, str) or not model_name:
        raise ValueError("pipeline.training.model_name must be a non-empty string")

    manifest = {
        'model_name': model_name,
        'model_version': str(training.get('model_version', '1')),
        'schema_hash': schema_hash,
        'onnx_log_level': config.get('onnx_log_level', 'WARNING'),
        'input_features': [
            {
                'feature_id': feature['feature_id'],
                'dimension': feature['dimension'],
            }
            for feature in features
        ],
        'preprocessing': {
            'minimum_total_packets': minimum_packets,
            'packet_count_features': packet_count_features,
            'missing_value_policy': 'reject',
            'scaler': 'scaler.json',
        },
        'model': 'model.onnx',
    }
    with (output_path / 'scaler.json').open('w', encoding='utf-8') as stream:
        json.dump(scaler_document, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    with (output_path / 'manifest.json').open('w', encoding='utf-8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    logging.info(
        "Schema 模型包已生成: %s (hash=%s, input_values=%d)",
        output_path, schema_hash, input_value_count)


def create_pipeline_package(config, config_path, scaler):
    """Create either a custom Schema package or the Legacy V1 adapter package."""
    pipeline = config.get('pipeline')
    if not isinstance(pipeline, dict) or not pipeline.get('model_package_path'):
        logging.info("未配置 pipeline.model_package_path，跳过模型包生成")
        return

    if schema_training_config(config) is not None:
        create_schema_pipeline_package(config, config_path, scaler)
        return

    schema = pipeline.get('schema') or {}
    if (schema.get('feature_order') or schema.get('remove_features') or
            schema.get('add_features')):
        logging.warning(
            "当前训练脚本只自动打包完整 Legacy V1 Schema；自定义 Schema "
            "需要按 feature ID 生成 manifest/scaler")
        return

    converter = Path(__file__).resolve().with_name('create_legacy_package.py')
    if not converter.is_file():
        raise FileNotFoundError(f"模型包转换脚本不存在: {converter}")
    output_path = resolve_path(pipeline['model_package_path'], config_path.parent)
    logging.info("生成 Schema 模型包: %s", output_path)
    subprocess.run(
        [
            sys.executable,
            str(converter),
            '--config',
            str(config_path),
            '--output',
            str(output_path),
        ],
        check=True,
    )


def KDADK_Train(config_file):
    logging.info("Loaded config file : %s", config_file)
    config_path = Path(config_file).expanduser().resolve()
    config_dir = config_path.parent
    # File Path and Labels- read from config.yaml
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            config = yaml.safe_load(f)
        data_paths = config['training_data_paths']
    except PermissionError:
        logging.error("No permission to read the config.yaml file. Please check the file permissions.")
        return
    except yaml.YAMLError:
        logging.error("The YAML file format is incorrect.")
        return
    except KeyError as e:
        logging.error(f"Missing necessary configuration items - {str(e)}")
        return
    except Exception as e:
        logging.error(f"An unexpected error occurred while reading the configuration file. - {str(e)}")
        return

    X, y = load_and_prepare_data(data_paths, config_dir)
    X = select_schema_training_columns(X, config)
    logging.info("Start training the model...")
    rf_model, scaler = train_random_forest(X, y)

    # Save the model
    output_dir = resolve_path(config['output_dir'], config_dir)
    create_directory(output_dir)

    # 拼接相对路径
    model_path_pkl = resolve_path(config['model_path_pkl'], config_dir)
    try:
        joblib.dump(rf_model, model_path_pkl)
    except PermissionError:
        logging.error("No permission to dump the model_classifier.pkl. Please check the file permissions.")
        return
    except IOError as e:
        logging.error(f"{str(e)}")
        return
    except Exception as e:
        logging.error(f"An unexpected error occurred while saving the model. {str(e)}")
        return

    logging.info(f"Model saved as {model_path_pkl}")

    # Save in ONNX format
    initial_type = [('float_input', FloatTensorType([None, X.shape[1]]))]
    onnx_model = convert_sklearn(rf_model, initial_types=initial_type, options={id(rf_model): {'zipmap': False}})
    model_path_onnx = resolve_path(config['model_path_onnx'], config_dir)

    with open(model_path_onnx, "wb") as f:
        f.write(onnx_model.SerializeToString())
    logging.info(f"Model saved as {model_path_onnx}")

    # Save the scaler
    # Save the standardizer for subsequent predictions.
    scaler_path_pkl = resolve_path(config['scaler_path_pkl'], config_dir)

    try:
        joblib.dump(scaler, scaler_path_pkl)
    except PermissionError as e:
        raise PermissionError("No permission to dump the scaler.pkl. Please check the file permissions.") from e
    except IOError as e:
        raise IOError(f"IO Error while saving the model.: {str(e)}") from e
    except Exception as e:
        raise RuntimeError(f"Error: An unexpected error occurred while saving the model. {str(e)}") from e
    logging.info(f"Scaler saved as {scaler_path_pkl}")

    # 提取参数
    scaler_params = {
        'mean': scaler.mean_.tolist(),
        'scale': scaler.scale_.tolist(),
        'feature_names': scaler.feature_names_in_.tolist() if hasattr(scaler, 'feature_names_in_') else []
    }

    # 保存为 JSON 文件（C++ 易于读取的格式）
    scaler_path_json = resolve_path(config['scaler_path_json'], config_dir)

    with open(scaler_path_json, 'w') as f:
        json.dump(scaler_params, f, indent=4)

    logging.info(f"StandardScaler 参数已导出到 {scaler_path_json}")
    create_pipeline_package(config, config_path, scaler)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        logging.info("Usage: python3 training.py <path_to_config>")
        sys.exit(1)
    config_file = sys.argv[1]
    KDADK_Train(config_file)
