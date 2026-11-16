# KTAG 工具集

`tools` 目录包含命令行 demo 以及从原独立工具仓库整合的数据准备工具。

## PCAP 推理的真实标签

`-r` 使用 `-p` 读取 PCAP 时，可用 `--pcap-labels` 逐文件指定真实标签：

```bash
output/ktag_demo -r tools/demo/proxy_config.yaml \
  -p /data/a.pcap -p /data/b.pcap -p /data/c.pcap \
  --pcap-labels 2,2,3 -c result/prediction.csv
```

三个标签分别对应三个 PCAP。重复标签的有效流合并统计；报告类别只取该列表的去重集合，
按数值升序显示，因此上例只显示 2、3。预测落入其他模型类别的错误样本仍参与统计。
未传该参数时按 PCAP 顺序假定真实标签为 0、1、2……，并打印提示；请确认顺序正确。
标签数量不符、格式错误、标签不属于模型时，在读包前报错。

模型包必须包含 `manifest.json` 的 `class_labels`。使用新版本执行 `-t` 会从实际训练
模型导出该字段；缺少时必须先重新导出，不能仅凭 YAML 分组数手工猜测类别。
该参数只用于单机 `-r/-p`，不支持训练、特征提取、网口 `-i` 或 UB 分布式组合。

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
sh /opt/KTAG/build.sh
```

生成的文件如下：

```text
/opt/KTAG/output/libktag_capture.so
/opt/KTAG/output/libktag_feature.so
/opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/ktag_demo
/opt/KTAG/output/plugins/libktag_example_feature_plugin.so
/opt/KTAG/output/plugins/libktag_proxy_features.so
/opt/KTAG/output/python/training.py
/opt/KTAG/output/python/evaluation.py
```

代理插件是可选 Feature Provider；不加载该插件时，现有应用 Schema 不会增加
`proxy.*` 特征。安装布局中，插件位于 KTAG 库目录下的 `plugins/` 子目录。

工具默认开启。在配置 KTAG 时也可显式选择：

```bash
cmake -S /opt/KTAG -B /opt/KTAG/build -DKTAG_BUILD_TOOLS=ON
cmake --build /opt/KTAG/build --target ktag_demo
```

如需仅编译库，设置 `KTAG_BUILD_TOOLS=OFF`。KTAG 现有的依赖项（libpcap、yaml-cpp 和 ONNX Runtime）同样适用于 demo。

可执行文件输出到 `/opt/KTAG/output/ktag_demo`。其 Python 辅助脚本在编译时会拷贝到 `/opt/KTAG/output/python`，因此训练和评估无需依赖原独立工具仓库的目录结构。

使用 `-t` 或 `-e` 前请先安装 Python 依赖：

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

命令示例：

```bash
# 应用分类
/opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/config.yaml
/opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/config.yaml

# 代理分类：先替换配置中的全部 /path/to CSV 路径
/opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/proxy_config.yaml
/opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/proxy_config.yaml
```

新的 pipeline 为可选启用。若 YAML 中设置
`pipeline.enabled: false`，则保留历史命令的调度和输出格式，推理仍调用公共 Inference API。应用任务使用
`/opt/KTAG/tools/demo/config.yaml`，代理任务使用
`/opt/KTAG/tools/demo/proxy_config.yaml`；其中的 `model_package_path` 必须指向与
Schema 匹配的模型包。插件 ABI 见《[API 参考](../docs/zh/api_reference.md)》，代理
Schema 的详细约定见《[用户指南](../docs/zh/user_guide.md)》。

历史入口同样要求 `pipeline.model_package_path`，路径相对于 YAML 文件解析。
`--pipeline-mode legacy` 保留 `-m 0/1/2` 的单流、批量和文件调度以及历史 CSV/JSON
格式；列选择、标准化和包数过滤统一由模型包控制，修改旧顶层 `columns_to_remove`、
`filter_packets` 或 `onnx_log_level` 后应重新生成模型包。已有 ONNX/Scaler 可转换：

```bash
python3 /opt/KTAG/tools/model_package/create_legacy_package.py \
  --config /opt/KTAG/tools/demo/config.yaml \
  --output /opt/KTAG/result/legacy_model_package
```

模型推理统一使用公共 `libktag_inference.so`，旧固定向量推理接口与独立兼容库已移除。
`ktag_legacy_types.h` 仅保留历史文件格式的数据类型；版本查询由独立的版本库目标提供。

同一份完整 YAML 可保持 `pipeline.enabled: false` 以默认走旧路径，并在某次调用时
临时启用某种拓扑：

```bash
/opt/KTAG/output/ktag_demo -r /opt/KTAG/tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode feature_parallel -n 4 -c result.csv

/opt/KTAG/output/ktag_demo -r /opt/KTAG/tools/demo/config.yaml -p sample.pcap \
  --pipeline-mode full_parallel -n 4 --inference-threads 2 \
  -c result.csv
```

## 应用与代理 Schema

应用分类和代理分类复用完全相同的 `pipeline.plugins`、`pipeline.schema`、
`remove_features` 和 `add_features` 配置结构，不增加任务类型枚举或另一套 YAML。

- `/opt/KTAG/tools/demo/config.yaml` 保持应用分类示例：`ktag_legacy_v1`，53 个 Feature
  Descriptor、354 个数值，Hash 为 `0xdd48882b53bebfa5`。
- `/opt/KTAG/tools/demo/proxy_config.yaml` 是代理 Schema 完整结构和两分类数据示例配置：加载
  `libktag_proxy_features.so`，组合 `ktag_proxy_v1`，118 个 Feature Descriptor、
  619 个数值，Hash 为 `0xca1eae5086b41ee5`；数字标签由当前启用的数据组顺序决定。
  当前文件实际启用两个 VMess 承载类别，只是增量训练示例；普通流量和其余代理类别
  需要在具备相应数据后作为新的外层组加入并重新训练。

生成 Schema CSV：

```bash
# 应用：CSV 为 12 个 meta.* 列 + 354 个 Schema 值
/opt/KTAG/output/ktag_demo -f application.pcap \
  --config /opt/KTAG/tools/demo/config.yaml -c application.csv

# 代理：CSV 为 12 个 meta.* 列 + 619 个 Schema 值
/opt/KTAG/output/ktag_demo -f proxy.pcap \
  --config /opt/KTAG/tools/demo/proxy_config.yaml -c proxy.csv
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
python3 /opt/KTAG/tools/data/dataset_preflight.py /path/to/config.yaml
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
/opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/proxy_config.yaml
/opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/proxy_config.yaml
/opt/KTAG/output/ktag_demo -r /opt/KTAG/tools/demo/proxy_config.yaml -p sample.pcap -c predictions.csv
```

训练阶段会在内存中执行 `filter_packets` 和 `columns_to_remove`，打印最终输入列，保存
ONNX、Scaler、Schema manifest，以及 `output_dir/feature_importance.csv`。正式应用和
代理 Schema 的 Feature ID/维度/顺序由 CSV 和冻结契约自动校验，不要求在 YAML 中
重复填写 `input_features`。

评估阶段始终保留 RandomForest 原始数字标签并调用 `predict_proba`；预测明细增加
`confidence`。配置中的 `classification_report_file` 同时包含 overall、IPv4、IPv6
报告及每标签 support，并在相同目录派生输出 overall、IPv4、IPv6 混淆矩阵 CSV。

完整操作顺序、标签语义、模型包结构、IPv6/QUIC 范围和已知限制参见
《[用户指南](../docs/zh/user_guide.md)》。
## DPDK 在线收包（可选）

在线推理的网口实时抓包默认走 libpcap。若需利用 DPDK 轮询（poll-mode）驱动与
零拷贝 mbuf 达到更高 PPS，可用可选的 `ktag_capture_dpdk` 后端。该功能不改变
`ktag_capture`（libpcap）核心，通过 demo 的捕获后端抽象切换。

### 前置条件

- 已安装 **DPDK 24.11 或更高版本**（可用 `pkg-config --modversion libdpdk` 确认；低于 24.11 时 CMake 配置阶段即报错，源码中另有编译期版本检查）。
- 网卡已绑定 DPDK 支持的驱动（如 vfio-pci），可用 `dpdk-devbind.py --status` 查看。
- 已配置大页（hugepages）。
- 以 `-DKTAG_ENABLE_DPDK=ON` 编译，可用一键脚本：

```bash
sh build.sh dpdk
```

或手动配置：

```bash
cmake -S . -B build -DKTAG_BUILD_TOOLS=ON -DKTAG_ENABLE_DPDK=ON
cmake --build build --target ktag_demo ktag_capture_dpdk
```

默认 `sh build.sh` 不带 DPDK（`KTAG_ENABLE_DPDK=OFF`）。

### 使用

在 pipeline 配置中设置 `pipeline.capture.backend: dpdk` 并填写 `capture.dpdk.*`
选项（见 `tools/demo/proxy_config.yaml` 的 `pipeline.capture` 段），然后：

```bash
# SP600/HiSilicon 网卡按 PCI 地址收包
output/ktag_demo -r config.yaml -i 0000:03:00.0 --dpdk -j result.json

# 或通过命令行指定 EAL 参数
output/ktag_demo -r config.yaml -i 0000:03:00.0 --dpdk \
  --dpdk-eal-args "-a 0000:03:00.0 --log-level=lib.eal:7" -j result.json
```

说明：

- `--dpdk` 只能与新的 pipeline 模式（`--pipeline-mode` 非 legacy）配合使用；
  `-i` 参数此时解释为 PCI 地址或 DPDK vdev 名。
- EAL 参数只生效于进程内第一次成功初始化（`rte_eal_init` 一次性）；CLI
  `--dpdk-eal-args` 优先于 YAML 中的 `capture.dpdk.eal_args`。
- 单线程模式（BORROWED）零拷贝指向 mbuf；并行模式（OWNED）复用
  `ktag_packet_make_owned` 拷入引用计数 buffer。

离线验证（无需真实流量，用 DPDK net_pcap vdev 把 pcap 文件模拟成网卡）：

```yaml
pipeline:
  capture:
    backend: dpdk
    dpdk:
      device_name: "net_pcap0"
      eal_args: "--vdev=net_pcap0,rx_pcap=/path/to/test.pcap --no-pci"
      rx_idle_eof_us: 500000   # 文件读完后 N 微秒空闲触发结束
```

此时 `-i net_pcap0 --dpdk` 即可回放 pcap 文件走完整 DPDK 收包路径。
