# KTAG 工具集

`tools` 目录包含命令行 demo 以及原先维护在 KDADK-TOOLS 中的数据准备工具。

## 目录结构

- `demo`：用于特征提取、训练、评估和在线推理的 C++ 示例程序。
- `demo/python`：demo 在模型训练和评估时调用的 Python 辅助脚本。
- `demo/config.yaml`：现有应用分类的完整训练、评估和推理示例。
- `demo/proxy_config.yaml`：`ktag_proxy_v1` 的训练、评估、特征提取和推理配置；
  当前启用两个 VMess 承载类别，13 类只是数据齐备后的参考训练规划。
- `data/feature_filter.py`：CSV 特征过滤工具。
- `data/dataset_preflight.py`：只读检查双栈 Schema CSV 和现有训练/评估配置。
- `capture` 和 `labeling`：流量抓包和打标签工具。

## 编译

推荐使用一键编译命令，会同时编译 KTAG 库和 demo：

```bash
sh build.sh
```

生成的文件如下：

```text
output/libktag_capture.so
output/libktag_feature.so
output/libKDADK_feature_extract.so
output/libktag_inference.so
output/libKDADK_inference.so
output/kdadk_demo
output/plugins/libktag_example_feature_plugin.so
output/plugins/libktag_proxy_features.so
output/python/training.py
output/python/evaluation.py
```

代理插件是可选 Feature Provider；不加载该插件时，现有应用 Schema 不会增加
`proxy.*` 特征。安装布局中，插件位于 KTAG 库目录下的 `plugins/` 子目录。

工具默认开启。在配置 KTAG 时也可显式选择：

```bash
cmake -S . -B build -DKTAG_BUILD_TOOLS=ON
cmake --build build --target kdadk_demo
```

如需仅编译库，设置 `KTAG_BUILD_TOOLS=OFF`。KTAG 现有的依赖项（libpcap、yaml-cpp 和 ONNX Runtime）同样适用于 demo。

可执行文件输出到 `output/kdadk_demo`。其 Python 辅助脚本在编译时会拷贝到 `output/python`，因此训练和评估无需依赖旧的 KDADK-TOOLS 目录结构。

使用 `-t` 或 `-e` 前请先安装 Python 依赖：

```bash
python3 -m pip install -r tools/demo/python/packages.txt
```

命令示例：

```bash
# 应用分类
output/kdadk_demo -t tools/demo/config.yaml
output/kdadk_demo -e tools/demo/config.yaml

# 代理分类：先替换配置中的全部 /path/to CSV 路径
output/kdadk_demo -t tools/demo/proxy_config.yaml
output/kdadk_demo -e tools/demo/proxy_config.yaml
```

新的 pipeline 为可选启用。若 YAML 中缺少 `pipeline` 块或设置
`pipeline.enabled: false`，则保留旧推理路径。应用任务使用 `tools/demo/config.yaml`，
代理任务使用 `tools/demo/proxy_config.yaml`；其中的 `model_package_path` 必须指向与
Schema 匹配的模型包。队列、停止流程、插件 ABI 与
代理 Schema 的详细约定见 `docs/zh/proxy_protocol_identification_design.md`。

同一份完整 YAML 可保持 `pipeline.enabled: false` 以默认走旧路径，并在某次调用时
临时启用某种拓扑：

```bash
output/kdadk_demo -r tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode feature_parallel -n 4 -c result.csv

output/kdadk_demo -r tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode full_parallel -n 4 --inference-threads 2 \
  -c result.csv
```

## 应用与代理 Schema

应用分类和代理分类复用完全相同的 `pipeline.plugins`、`pipeline.schema`、
`remove_features` 和 `add_features` 配置结构，不增加任务类型枚举或另一套 YAML。

- `tools/demo/config.yaml` 保持应用分类示例：`ktag_legacy_v1`，53 个 Feature
  Descriptor、354 个数值，Hash 为 `0xdd48882b53bebfa5`。
- `tools/demo/proxy_config.yaml` 是代理 Schema 完整结构和两分类数据示例配置：加载
  `libktag_proxy_features.so`，组合 `ktag_proxy_v1`，118 个 Feature Descriptor、
  619 个数值，Hash 为 `0xca1eae5086b41ee5`；数字标签由当前启用的数据组顺序决定。
  当前文件实际启用两个 VMess 承载类别，只是增量训练示例；普通流量和其余代理类别
  需要在具备相应数据后作为新的外层组加入并重新训练。

生成 Schema CSV：

```bash
# 应用：CSV 为 12 个 meta.* 列 + 354 个 Schema 值
output/kdadk_demo -f application.pcap \
  --config tools/demo/config.yaml -c application.csv

# 代理：CSV 为 12 个 meta.* 列 + 619 个 Schema 值
output/kdadk_demo -f proxy.pcap \
  --config tools/demo/proxy_config.yaml -c proxy.csv
```

程序启动时会输出实际 Schema Name、Version、Feature Count、Value Count 和 Hash。
正式名称是冻结契约：调整 Feature ID、顺序或维度时必须使用新的 Schema
名称/版本和匹配的模型包。`ktag_proxy_v1` 不允许包含 `legacy.flow_id`、
`network.src_ip`、`network.dst_ip`，但保留端口、协议号、流统计、RawBoW CRC
以及全部 68 个 `proxy.*` Feature Descriptor。

YAML 不需要重复填写模型 `input_features`；该清单属于模型包 manifest，并按冻结
Schema 顺序保存。内部端点增强或无端口消融实验同样只修改这套 Schema 配置，但必须
使用独立的 Schema Name、Version 和 Hash。

## 双栈数据预检

每个 PCAP 单独生成 Schema CSV，并把路径按原有二维列表填写到同一份配置的
`training_data_paths` 和 `evaluation_data_paths` 后，执行只读预检：

```bash
python3 tools/data/dataset_preflight.py /path/to/config.yaml
```

预检不会修改 CSV，也不会生成数据 Manifest 或报告文件。它检查训练/评估组数、
代理任务数据组非空及训练/评估组数一致性、CSV 列头与列数，统计每个标签的 IPv4/IPv6 覆盖、TCP/UDP 和包数
分布、服务器/端口数量、相同服务器 IP 是否跨训练与评估，以及最终模型输入是否仍含
`meta.*`、Flow ID 或 IP 字段。

IPv4/IPv6 覆盖只产生诊断信息：仅含 IPv4 或仅含 IPv6 的数据集仍可训练；缺失地址族
会输出警告，表示该模型在该地址族上的效果未经训练和评估验证。

现代 Schema CSV 的 `columns_to_remove` 应为：

- 应用 `ktag_legacy_v1`：删除前 12 个 `meta.*` 列以及随后 3 个 Legacy Flow/IP
  特征，即 `[0..14]`，最终模型输入为 351 个数值。
- 代理 `ktag_proxy_v1`：Schema 本身已排除 Flow/IP，只删除前 12 个 `meta.*` 列，
  即 `[0..11]`，最终模型输入为 619 个数值。

现有 `feature_filter.py` 会原地覆盖 CSV。双栈数据的 `meta.ip_version` 后续还需要用于
分层评估，因此不要对唯一一份原始 Schema CSV 执行原地过滤。不同 IPv4/IPv6 地址
是否属于同一台物理双栈服务器无法仅凭 CSV 推断，仍须由数据提供方依据采集记录确认。

训练和评估继续使用现有命令：

```bash
output/kdadk_demo -t /path/to/proxy_config.yaml
output/kdadk_demo -e /path/to/proxy_config.yaml
output/kdadk_demo -r /path/to/proxy_config.yaml -p sample.pcap -c predictions.csv
```

训练阶段会在内存中执行 `filter_packets` 和 `columns_to_remove`，打印最终输入列，保存
ONNX、Scaler、Schema manifest，以及 `output_dir/feature_importance.csv`。正式应用和
代理 Schema 的 Feature ID/维度/顺序由 CSV 和冻结契约自动校验，不要求在 YAML 中
重复填写 `input_features`。

评估阶段始终保留 RandomForest 原始数字标签并调用 `predict_proba`；预测明细增加
`confidence`。配置中的 `classification_report_file` 同时包含 overall、IPv4、IPv6
报告及每标签 support，并在相同目录派生输出 overall、IPv4、IPv6 混淆矩阵 CSV。

完整操作顺序、标签语义、模型包结构、IPv6/QUIC 范围和当前已知限制参见
[`docs/zh/demo_pipeline.md`](../docs/zh/demo_pipeline.md)。
