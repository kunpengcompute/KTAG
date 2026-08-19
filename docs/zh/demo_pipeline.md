# 应用分类与代理分类使用指南

## 1. 功能范围与当前状态

KTAG 采用 Capture、Feature、Inference 三层架构，对 IPv4/IPv6 TCP、UDP
流量进行组流、特征提取和模型推理。应用分类与代理分类是两个独立运行模式：一次
运行只加载一套 Schema 和一套模型，不会先执行应用分类再自动执行代理分类。

当前仓库提供两套正式特征契约：

| 用途 | 配置示例 | Schema | Schema 数值 | 模型输入 |
|---|---|---|---:|---:|
| 应用分类 | `tools/demo/config.yaml` | `ktag_legacy_v1` v1 | 354 | 351 |
| 代理分类 | `tools/demo/proxy_config.yaml` | `ktag_proxy_v1` v1 | 619 | 619 |

应用 Schema 保留原 354 维契约。训练时排除 `legacy.flow_id`、
`network.src_ip` 和 `network.dst_ip`，所以随机森林实际接收 351 个数值。代理
Schema 已在组合阶段排除这三个 Feature，并在 351 维通用特征基础上增加 268 维
代理专用特征，因此 Schema 和模型输入均为 619 个数值。

> 当前 `proxy_config.yaml` 是真实可运行的两分类示例：标签 0 为
> VMess/WebSocket/TLS，标签 1 为 VMess/XHTTP(H2)/TLS。仓库已经具备扩展代理
> 数据组、训练和推理的代码能力，但没有随仓库交付“普通流量 + 全部代理形态”的
> 13 类训练数据和正式模型。若要识别普通流量，必须把普通流量作为一个独立外层数据
> 组参与训练；当前实现没有额外的规则引擎在模型之前自动判断“普通流/VPN 流”。

本期“VPN 识别”实际指应用代理或加密代理流量分类，包括 Shadowsocks、
ShadowsocksR、VMess、VLESS、Trojan、Hysteria2 及其约定承载形态，不包含
PPTP、L2TP、IPsec 等传统隧道 VPN。

## 2. 构建与产物

在仓库根目录执行：

```bash
sh build.sh
```

与应用/代理分类有关的主要产物如下：

```text
output/kdadk_demo
output/libktag_capture.so
output/libktag_feature.so
output/libktag_inference.so
output/libKDADK_feature_extract.so
output/libKDADK_inference.so
output/plugins/libktag_proxy_features.so
output/python/training.py
output/python/evaluation.py
```

其中 `libKDADK_feature_extract.so` 和 `libKDADK_inference.so` 是旧固定向量接口的
兼容库；新 Pipeline 使用 `libktag_capture.so`、`libktag_feature.so`、
`libktag_inference.so`。代理分类还需要
`output/plugins/libktag_proxy_features.so`。

训练和评估前安装 Python 依赖：

```bash
python3 -m pip install -r tools/demo/python/packages.txt
```

配置中的相对路径均相对于配置文件所在目录解析。例如从仓库根目录使用
`tools/demo/proxy_config.yaml` 时，其中的 `../../result` 会按
`tools/demo/` 所在位置解析，而不是按当前终端目录解析。

## 3. 配置文件

### 3.1 应用与代理使用同一种配置结构

应用任务使用 `tools/demo/config.yaml`，代理任务使用
`tools/demo/proxy_config.yaml`。两者都沿用以下现有字段，不需要另一种配置格式：

```yaml
training_data_paths:
  - [同一标签的训练CSV-1, 同一标签的训练CSV-2]
  - [下一标签的训练CSV-1, 下一标签的训练CSV-2]

evaluation_data_paths:
  - [标签0的评估CSV]
  - [标签1的评估CSV]
```

外层列表的位置就是数字标签：第一个数据组为标签 0，第二个数据组为标签 1，依此
类推。训练和评估的外层组数、顺序必须一致。同一标签中的 IPv4 和 IPv6 CSV 应放在
同一个内层列表中，不能因为地址族不同而新建标签。

代码和推理引擎只理解数字标签，不理解 `bilibili`、`VMess/WebSocket/TLS` 等业务
名称。业务侧必须保存本次训练使用的数据组顺序；改变外层顺序后，数字标签含义也会
随之改变。

同一 CSV 同时出现在训练集和评估集会阻止训练。训练集和评估集出现相同服务器地址
只会产生告警并继续执行，因为单靠 CSV 无法可靠判断 NAT、负载均衡或双栈地址是否
属于同一物理服务器；这类重叠可能使评估结果高估跨服务器泛化能力，需要数据提供方
结合采集记录人工确认。

### 3.2 应用 Schema 配置

应用配置的关键内容为：

```yaml
pipeline:
  schema:
    name: ktag_legacy_v1
    version: 1
    remove_features: []
    add_features: []
```

该 Schema 是冻结契约：53 个 Feature Descriptor、354 个数值、Hash
`0xdd48882b53bebfa5`。不要在相同名称和版本下改变 Feature、顺序或维度。

新生成的应用 Schema CSV 包含 12 个 `meta.*` 列和 354 个 Schema 特征列，共
366 列。训练时使用：

```yaml
columns_to_remove: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
```

即删除 12 个元数据列以及 Schema 开头的 Flow ID、源 IP、目的 IP 三列，最终输入
351 个数值。旧式 CSV 没有 12 个 `meta.*` 列，必须沿用其原有删除规则；不能只看
总列数混用新旧配置。

### 3.3 代理 Schema 配置

代理配置需要加载插件，并使用冻结的代理 Schema：

```yaml
pipeline:
  enabled: true
  model_package_path: ../../result/proxy_two_class_v1_model
  plugins:
    - ../../output/plugins/libktag_proxy_features.so
  schema:
    name: ktag_proxy_v1
    version: 1
    remove_features:
      - legacy.flow_id
      - network.src_ip
      - network.dst_ip
    add_features:
      # 完整顺序见 tools/demo/proxy_config.yaml
```

`ktag_proxy_v1` 包含 118 个 Feature Descriptor、619 个数值、Hash
`0xca1eae5086b41ee5`。代理 Schema CSV 包含 12 个 `meta.*` 列和 619 个 Schema
特征列，共 631 列。由于 Flow ID 和 IP 已在 Schema 组合阶段排除，训练时只删除前
12 个元数据列：

```yaml
columns_to_remove: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
```

不要把 622 维 Built-in 与代理 Feature 全集当作正式代理 Schema。622 只表示可选择
全集；正式 `ktag_proxy_v1` 固定为 619 维。

## 4. 生成 Schema 特征 CSV

应用分类：

```bash
cd output
./kdadk_demo \
  -f /path/to/application.pcap \
  --config ../tools/demo/config.yaml \
  -c /path/to/application.csv
```

代理分类：

```bash
cd output
./kdadk_demo \
  -f /path/to/proxy.pcap \
  --config ../tools/demo/proxy_config.yaml \
  -c /path/to/proxy.csv
```

`-f --config` 每次只接受一个 PCAP，输出格式必须使用 `-c *.csv`，不能同时使用
`-j` 或 `-w`。需要处理多个 PCAP 时，应逐个执行并分别生成 CSV。

启动日志会打印 Schema Name、Version、Feature Count、Value Count 和 Hash。应用
输出应显示 354 个值，CSV 总列数为 366；代理输出应显示 619 个值，CSV 总列数为
631。这里的“Schema 数值数”与“CSV 总列数”不是同一个概念。

若 PCAP 尾部损坏但前面已经包含完整报文，当前 Pipeline 会保留此前成功处理的流，
打印一条 WARNING，并在 EOF 清理阶段输出已有结果。损坏位置之后的数据无法恢复；
训练前仍建议替换或重新采集损坏文件。

## 5. 数据预检、训练与评估

### 5.1 数据预检

填写 `training_data_paths` 和 `evaluation_data_paths` 后执行只读预检：

```bash
python3 tools/data/dataset_preflight.py tools/demo/proxy_config.yaml
```

预检不会修改 CSV，主要检查：

- 训练/评估数据组数量与顺序；
- CSV 列头、顺序和 Schema 维度；
- 同一 CSV 是否跨训练/评估复用；
- IPv4/IPv6、TCP/UDP、报文数、服务器和端口覆盖；
- `meta.*`、Flow ID 和地址字段是否可能进入模型；
- 训练与评估服务器地址是否重叠。

IPv4-only、IPv6-only 和混合数据都允许训练。缺少某个地址族只输出告警，不阻止
训练；告警表示该模型没有通过对应地址族数据建立或验证效果。

### 5.2 训练

```bash
cd output

# 应用模型
./kdadk_demo -t ../tools/demo/config.yaml

# 代理模型
./kdadk_demo -t ../tools/demo/proxy_config.yaml
```

训练使用随机森林和 StandardScaler，并输出 PKL、ONNX、Scaler、
`feature_importance.csv`。配置了 `pipeline.model_package_path` 时，还会生成新
Inference API 使用的模型包：

```text
model_package_path/
├── manifest.json
├── model.onnx
└── scaler.json
```

模型包记录 Schema Hash、Feature ID/维度/顺序、过滤条件和 Scaler 参数。在线加载
时会校验这些内容；应用模型包和代理模型包不能交叉加载。

### 5.3 评估

```bash
cd output
./kdadk_demo -e ../tools/demo/config.yaml
./kdadk_demo -e ../tools/demo/proxy_config.yaml
```

评估使用 `evaluation_data_paths`，生成整体 Classification Report、预测明细、整体
混淆矩阵；现代 Schema CSV 含 `meta.ip_version` 时，还会生成 IPv4、IPv6 分层报告
和分层混淆矩阵。缺少某地址族时，对应部分明确标记为不可评估。

Classification Report 中的 accuracy 是整个评估数据集的正确比例；推理结果中的
confidence 是单条流在模型所选标签上的概率。两者含义不同。confidence 为 0 仍是
合法概率，不会删除或改写数字标签；值为 -1 表示模型没有可用概率输出，但数字标签
仍然有效。当前实现不使用 confidence 阈值生成 `unknown`。

## 6. PCAP 与网口推理

### 6.1 应用分类

`config.yaml` 默认保留旧推理路径。显式指定新 Pipeline Mode 会为本次运行启用新
Pipeline：

```bash
cd output
./kdadk_demo \
  -r ../tools/demo/config.yaml \
  -p /path/to/application.pcap \
  --pipeline-mode feature_parallel \
  -n 4 \
  -m 1 \
  -c application_result.csv
```

### 6.2 代理分类

`proxy_config.yaml` 默认启用新 Pipeline，并从
`pipeline.model_package_path` 加载代理模型包：

```bash
cd output
./kdadk_demo \
  -r ../tools/demo/proxy_config.yaml \
  -p /path/to/proxy.pcap \
  --pipeline-mode feature_parallel \
  -n 4 \
  -m 1 \
  -c proxy_result.csv
```

可重复使用 `-p` 输入多个 PCAP。CSV/JSON 推理结果包含数字标签、confidence、地址
族、第一包方向的源/目的地址与端口、规范化 Flow Key 和报文序号。Flow ID 只用于
结果关联，不进入模型。

网口实时推理示例：

```bash
cd output
./kdadk_demo \
  -r ../tools/demo/proxy_config.yaml \
  -i eth0 \
  --pipeline-mode full_parallel \
  -n 4 \
  --inference-threads 2 \
  -j proxy_live_result.json
```

网口模式未指定 `-c/-j` 时，结果只打印到控制台。

## 7. Pipeline Mode

| 模式 | 说明 | 线程约束 |
|---|---|---|
| `legacy` | 保留旧在线推理路径 | 不接受新 Pipeline 线程参数 |
| `single_thread` | Capture、Feature、Inference 串行执行 | Feature/Inference 均为 1 |
| `staged` | 分阶段队列执行 | Feature/Inference 均为 1 |
| `feature_parallel` | 多 Feature Worker，单 Inference Worker | `-n` 可大于 1 |
| `full_parallel` | Feature 和 Inference 均可并行 | 同时设置两类线程数 |

相同模型、配置、PCAP 和线程数下，输出应可重复。改变线程数可能改变内部流分配和
浮点累计顺序，因此最终统计或准确率可能出现很小差异；业务验收应固定线程配置。

`pipeline.feature.lifecycle_mode` 有两种取值：

- `legacy`：复现旧固定向量的流结束语义，适合现有应用模型兼容。
- `modern`：启用超时、UDP 报文上限等现代 Feature Context 行为，代理配置使用该
  模式。训练特征提取与在线推理必须使用一致的 Schema 和生命周期配置。

## 8. IPv4/IPv6 支持边界

现代 Capture/Feature Pipeline 支持 IPv4/IPv6 TCP、UDP，使用完整 128 位 IPv6 地址
组成 Flow Key。IPv4 与 IPv6 连接分别形成流，不进行跨地址族会话合并。DNS A 和
AAAA 记录可写入双栈 Domain Store，用于相应流的域名关联。

IPv6 地址只作为 Flow Key、CSV/推理元数据和审计信息。正式代理 Schema 不输入源、
目的 IP；应用 `ktag_legacy_v1` 为保持 354 维兼容，在 IPv6 流中把两个 Legacy IP
Feature 填 0，真实 IPv6 地址仍保存在 `meta.*` 中。

支持的 IPv6 Extension Header 包括 Hop-by-Hop、Routing、Destination Options、
Fragment 和 AH，单包最多遍历 8 个。当前限制如下：

- 不执行 IPv4/IPv6 分片重组；非首分片不能组成五元组时跳过该报文；
- 不支持 IPv6 Jumbogram；
- 不解析 ESP 加密内容；
- 未支持的 Next Header 或畸形报文按包拒绝，不应中止后续流量处理；
- “代码能够处理 IPv6”不等于已有模型具备 IPv6 分类效果。只有使用相应地址族数据
  训练并独立评估后，才能声明该地址族的模型效果。

## 9. 代理专用特征与 QUIC 范围

代理插件注册三个 Provider：

| Provider | 主要信息 |
|---|---|
| Sequence/Burst | 有方向包长、相对时间、TCP Flags、Burst 和方向切换 |
| TCP Signature | 有界 TCP Prefix、重传/乱序/缺口、TLS ClientHello 和 HTTP/WebSocket/H2/gRPC 迹象 |
| QUIC | 长短头、Version、CID、Token、Header Type、报文序列、首字节分布和熵 |

QUIC 解析是轻量级结构解析，支持识别 QUIC v1、v2、Version Negotiation 和部分长短
Header 结构，不执行 QUIC 解密、完整连接状态机、TLS-over-QUIC 重组或 HTTP/3 内容
解析。

Hysteria2 与普通 HTTP/3 的区分属于统计模型分类，不是确定性协议签名。启用
Salamander 等混淆后，标准 QUIC Header 可能无法解析；解析失败计数、包长、方向、
时序和熵仍可作为模型输入，但不能保证仅凭单条特征准确识别 Hysteria2。

TLS 1.3 ECH 下的视频/文本内容分类不在本期代理协议分类范围内。

## 10. 当前已知限制

- 仓库示例代理配置只启用两个 VMess 承载类别，未随仓库交付 13 类正式数据和模型。
- 当前没有固定的 `unknown` 标签或 confidence 阈值；输出一定是模型数字标签。
- 要区分普通流量和代理流量，训练数据中必须存在具有明确数字标签的普通流量类别。
- 服务器地址跨训练/评估重叠只告警，需要数据侧人工判断是否会造成泄漏。
- IPv4-only 或 IPv6-only 数据可以训练，但不能据此声明缺失地址族的模型效果。
- 完整双栈 Fuzz、ASan/UBSan、全量性能及资源对比属于后续独立验收工作，当前文档不
  将其表述为已完成结论。
- 旧兼容推理路径与新 Pipeline 的性能不同；当前尚未给出代理插件的正式吞吐基线。

## 11. 常见问题

### 11.1 提示模型包与 Schema 不匹配

检查配置中的 Schema Name、Version、Feature 顺序和
`pipeline.model_package_path`。应用与代理模型包不能混用，也不能在冻结名称下修改
Feature 后继续加载旧模型。

### 11.2 代理 CSV 不是 619 列

619 是 Schema 特征数。Schema CSV 还包含 12 个 `meta.*` 列，因此总列数应为
631。应用 CSV 对应为 12 + 354 = 366 列。

### 11.3 训练提示旧 CSV 不含 `meta.ip_version`

这是兼容告警，不阻止旧 IPv4 CSV 训练。由于旧 CSV 没有地址族元数据，训练工具无法
生成可靠的 IPv4/IPv6 分层统计。

### 11.4 找不到代理插件

确认已经执行完整构建，并检查配置中的插件相对路径：

```bash
ls -l output/plugins/libktag_proxy_features.so
```

### 11.5 Classification Report 中为什么只有数字

数字来自 `training_data_paths` 外层顺序。KTAG 不在模型或推理代码中硬编码应用名、
代理名；请使用采集和训练记录维护数字到业务类别的映射。
