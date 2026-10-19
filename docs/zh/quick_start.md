# 快速上手指南

本文指导用户快速使用 `ktag_demo` 完成流量分类：使用已有模型推理，或从 PCAP 训练自己的模型。KTAG 源码根目录以 `/opt/KTAG` 为例，命令均使用绝对路径。配置文件的详细说明、训练步骤和参数含义见《[用户指南](user_guide.md)》。

## 前提条件

已完成 KTAG 编译（见《[编译指南](installation_guide.md)》），并确认产物存在：

```bash
ls -l /opt/KTAG/output/ktag_demo
```

仅使用已有模型推理时不需要 Python；要训练自己的模型（第四、五步），先安装依赖：

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

## 第一步：准备配置文件

建立独立工作目录，复制示例配置（应用分类使用 `config.yaml`，代理分类使用 `proxy_config.yaml`）：

```bash
mkdir -p /opt/KTAG/work/{pcap,csv,model,result}
cp /opt/KTAG/tools/demo/config.yaml /opt/KTAG/work/app.yaml
```

把待处理的 PCAP 文件放入 `/opt/KTAG/work/pcap/`。

## 第二步：PCAP 离线推理

确认 `/opt/KTAG/work/app.yaml` 中的 `pipeline.model_package_path` 指向已收到的模型包目录（配置修改方法见《[用户指南](user_guide.md)》），然后执行推理，输出 CSV：

```bash
/opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml \
  -p /opt/KTAG/work/pcap/sample.pcap \
  --pipeline-mode feature_parallel -n 4 \
  -c /opt/KTAG/work/result/prediction.csv
```

输出 JSON 使用 `-j`：

```bash
/opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml \
  -p /opt/KTAG/work/pcap/sample.pcap \
  -j /opt/KTAG/work/result/prediction.json
```

结果文件每行对应一条流，包含数字标签 `label`、置信度 `confidence` 和流元数据；`label` 与业务名称的对应关系由训练配置决定，详见《[用户指南](user_guide.md)》。

## 第三步：网口实时推理（可选）

列出可用网口：

```bash
/opt/KTAG/output/ktag_demo -l
```

对指定网口实时分类，使用 Ctrl+C 停止：

```bash
/opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml -i eth0 \
  --pipeline-mode full_parallel -n 4 --inference-threads 2 \
  -j /opt/KTAG/work/result/live_prediction.json
```

实时抓包需要相应权限，并确保采集点能看到目标的双向完整流量。

## 第四步：生成训练特征（可选，训练自己的模型时）

从每个 PCAP 生成一份特征 CSV（每个 PCAP 单独执行一次）：

```bash
/opt/KTAG/output/ktag_demo -f /opt/KTAG/work/pcap/video_train_1.pcap \
  --config /opt/KTAG/work/app.yaml \
  -c /opt/KTAG/work/csv/video_train_1.csv
```

## 第五步：训练、评估并使用新模型（可选）

1. 在 `/opt/KTAG/work/app.yaml` 中填写训练和评估数据路径。外层数组的顺序就是数字标签，训练和评估必须保持一致（详见《[用户指南](user_guide.md)》）。
2. 执行只读预检，确认数据无误：

```bash
python3 /opt/KTAG/tools/data/dataset_preflight.py /opt/KTAG/work/app.yaml
```

3. 训练并评估：

```bash
/opt/KTAG/output/ktag_demo -t /opt/KTAG/work/app.yaml
/opt/KTAG/output/ktag_demo -e /opt/KTAG/work/app.yaml
```

训练完成后，模型包生成在配置的 `pipeline.model_package_path` 目录。回到第二步，即可用新模型执行推理。

## 更多信息

- 配置文件、Schema 契约、训练流程与参数含义：《[用户指南](user_guide.md)》
- C API 接口：《[API 参考](api_reference.md)》
- 依赖安装与构建选项：《[编译指南](installation_guide.md)》
