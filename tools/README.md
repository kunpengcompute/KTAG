# KTAG 工具集

`tools` 目录包含命令行 demo 以及原先维护在 KDADK-TOOLS 中的数据准备工具。

## 目录结构

- `demo`：用于特征提取、训练、评估和在线推理的 C++ 示例程序。
- `demo/python`：demo 在模型训练和评估时调用的 Python 辅助脚本。
- `demo/config.yaml`：训练、评估和推理的示例配置文件。
- `demo/pipeline_config.example.yaml`：第 7 阶段的线程、队列、Schema 和插件配置示例。
- `data/feature_filter.py`：CSV 特征过滤工具。
- `capture` 和 `labeling`：流量抓包和打标签工具。

## 编译

推荐使用一键编译命令，会同时编译 KTAG 库和 demo：

```bash
sh build.sh
```

生成的文件如下：

```text
output/libktag_capture.so
output/libKDADK_feature_extract.so
output/libktag_inference.so
output/libKDADK_inference.so
output/kdadk_demo
output/plugins/libktag_example_feature_plugin.so
output/python/training.py
output/python/evaluation.py
```

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
output/kdadk_demo -t tools/demo/config.yaml
output/kdadk_demo -e tools/demo/config.yaml
python3 tools/data/feature_filter.py tools/demo/config.yaml /path/to/csv_data_folder
```

新的 pipeline 为可选启用。若 YAML 中缺少 `pipeline` 块或设置
`pipeline.enabled: false`，则保留旧推理路径。如需使用 Schema 化的单线程或多线程
pipeline，请拷贝 `tools/demo/pipeline_config.example.yaml`，选择对应模式，并将
`model_package_path` 指向第 6 阶段的模型包。队列、停止流程和插件 ABI 的详细行为
见 `docs/zh/demo_pipeline.md`。

同一份完整 YAML 可保持 `pipeline.enabled: false` 以默认走旧路径，并在某次调用时
临时启用某种拓扑：

```bash
output/kdadk_demo -r tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode feature_parallel -n 4 -c result.csv

output/kdadk_demo -r tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode full_parallel -n 4 --inference-threads 2 \
  -c result.csv
```
