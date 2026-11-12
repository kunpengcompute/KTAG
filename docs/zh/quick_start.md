# 快速入门

## 介绍

本文指导用户快速使用 `ktag_demo` 完成流量分类：使用已有模型推理，或从PCAP（网络报文捕获文件）训练自己的模型。KTAG源码根目录以 `/opt/KTAG` 为例，命令均使用绝对路径。配置文件的详细说明、训练步骤和参数含义见《[用户指南](user_guide.md)》。

## 环境准备

按照《[安装指南](installation_guide.md#快速安装)》完成KTAG编译。环境要求如下，其他依赖见[检查安装环境](installation_guide.md#检查安装环境)。

- Linux/AArch64，配套系统和编译器见[版本配套表](release_notes.md#ZH-CN_TOPIC_0000002518245458)。
- 支持C++17的GCC/G++。
- CMake 3.16.3或更高版本。

确认以下产物存在。

```bash
ls -l /opt/KTAG/output/ktag_demo \
  /opt/KTAG/output/plugins/libktag_traffic_features.so
```

仅使用已有模型推理时不需要Python；要训练自己的模型（[第四步](#第四步生成训练特征可选)、[第五步](#第五步训练评估并使用新模型可选)），先安装依赖。

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

## 快速开始

以下步骤以应用流量分类为例，先使用已有模型推理，再按需体验网口实时分类和模型训练。命令中的PCAP文件名、网口名和模型包路径请按实际情况替换。

### 1. 准备配置文件

   建立独立工作目录，复制示例配置（应用分类使用 `config.yaml`，代理分类使用 `proxy_config.yaml`）。

   ```bash
   mkdir -p /opt/KTAG/work/{pcap,csv,model,result}
   cp /opt/KTAG/tools/demo/config.yaml /opt/KTAG/work/app.yaml
   sed -i 's|../../output/plugins/|/opt/KTAG/output/plugins/|g; s|../../result|/opt/KTAG/result|g' \
     /opt/KTAG/work/app.yaml
   ```

   上述替换将示例中的插件和结果路径改为绝对路径，避免配置复制到 `work/` 后解析到其他目录。代理配置也应加载Traffic和Proxy两个插件，完整配置见《[用户指南](user_guide.md)》。把待处理的PCAP文件放入 `/opt/KTAG/work/pcap/`。

### 2. PCAP 离线推理

1. 确认 `/opt/KTAG/work/app.yaml` 中的 `pipeline.model_package_path` 指向已有应用模型包，建议使用绝对路径。配置修改方法见《[用户指南](user_guide.md#配置文件)》。然后执行推理，输出CSV文件。

   ```bash
   /opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml \
     -p /opt/KTAG/work/pcap/sample.pcap \
     --pipeline-mode feature_parallel -n 4 \
     -c /opt/KTAG/work/result/prediction.csv
   ```

2. 输出JSON（JavaScript对象表示法）文件使用 `-j`。

   ```bash
   /opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml \
     -p /opt/KTAG/work/pcap/sample.pcap \
     -j /opt/KTAG/work/result/prediction.json
   ```

**参数说明**

`-r` 指定推理配置，`-p` 指定输入PCAP，`-c` / `-j` 指定CSV / JSON结果文件。

| 参数 | 示例中使用的含义与约束 |
| :--- | :--- |
| --pipeline-mode | 可选 legacy、single_thread、staged、feature_parallel、full_parallel，各模式见[模式表](user_guide.md#pipeline-mode-与线程配置)；省略时使用配置中的调度方式 |
| -n | Feature线程数，十进制正整数 1..4294967295；省略时使用YAML，缺省值为4，并受模式归一化约束；single_thread/staged只允许显式指定1 |
| --inference-threads | Inference线程数，同一正整数范围；省略时使用YAML，缺省值为1；single_thread/staged/feature_parallel只允许显式指定1 |

上述范围是参数解析范围，实际线程数应结合CPU和内存配置。`legacy` 不接受这两个线程参数。

**结果说明**

结果文件每行对应一条流，包含数字标签 `label`、置信度 `confidence` 和流元数据；`label` 与业务名称的对应关系由训练配置决定，详见《[用户指南](user_guide.md)》。

### 3. 网口实时推理（可选）

执行前确认当前账号有网口抓包权限；可由root运行，或由管理员按抓包模式授予所需网络能力。采集点应能看到目标的双向完整流量。

1. 列出可用网口。

   ```bash
   /opt/KTAG/output/ktag_demo -l
   ```

2. 对指定网口实时分类，使用Ctrl+C停止。

   ```bash
   /opt/KTAG/output/ktag_demo -r /opt/KTAG/work/app.yaml -i eth0 \
     --pipeline-mode full_parallel -n 4 --inference-threads 2 \
     -j /opt/KTAG/work/result/live_prediction.json
   ```

**参数说明**

`-l` 列出网口；`-i eth0` 中的 `eth0` 应替换为列表中的实际接口名，保持名称大小写。`--inference-threads` 的范围与模式约束见上一节参数表。

**结果说明**

实时推理结果写入 `/opt/KTAG/work/result/live_prediction.json`。


### 4. 生成训练特征（可选）

   从每个PCAP生成一份特征CSV（每个PCAP单独执行一次）。

   ```bash
   /opt/KTAG/output/ktag_demo -f /opt/KTAG/work/pcap/video_train_1.pcap \
     --config /opt/KTAG/work/app.yaml \
     -c /opt/KTAG/work/csv/video_train_1.csv
   ```

**参数说明**

`-f` 指定待提取特征的PCAP，`--config` 指定配置文件，`-c` 指定特征CSV输出路径。

**结果说明**

命令退出码为0，且 `/opt/KTAG/work/csv/video_train_1.csv` 存在并包含表头，表示本例特征生成成功；后续训练还需有有效数据行。

### 5. 训练、评估并使用新模型（可选）

1. 在 `/opt/KTAG/work/app.yaml` 中填写训练和评估数据路径。外层数组的顺序就是数字标签，训练和评估必须保持一致（详见《[用户指南](user_guide.md)）》。
2. 执行只读预检。退出码为0且显示 `Preflight PASSED` 表示通过；`WARNING` 仍需结合采集记录核对。

   ```bash
   python3 /opt/KTAG/tools/data/dataset_preflight.py /opt/KTAG/work/app.yaml
   ```

3. 训练并评估。

   ```bash
   /opt/KTAG/output/ktag_demo -t /opt/KTAG/work/app.yaml
   /opt/KTAG/output/ktag_demo -e /opt/KTAG/work/app.yaml
   ```

**参数说明**

`-t` 使用指定配置执行训练，`-e` 使用指定配置执行评估。

**结果说明**

训练命令退出码为0后，检查 `pipeline.model_package_path` 下的 `manifest.json`、`model.onnx` 和 `scaler.json`。评估命令退出码为0并生成配置指定的分类报告表示评估流程完成。回到[第二步](#第二步pcap-离线推理)，即可用新模型执行推理。

## 更多功能

- 配置文件、Schema契约、训练流程与参数含义：《[用户指南](user_guide.md)》
- C API接口：《[API 参考](api_reference.md)》
- 依赖安装与构建选项：《[编译指南](installation_guide.md)》

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布。 |
