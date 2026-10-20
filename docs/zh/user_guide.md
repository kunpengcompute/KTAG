# 用户指南

本文介绍 `ktag_demo` 的完整使用方法：功能范围、构建产物、配置文件、特征 CSV 生成、模型训练与评估、推理运行模式、支持边界和常见问题。KTAG 源码根目录以 `/opt/KTAG` 为例，命令均使用绝对路径。快速上手见《[快速上手指南](quick_start.md)》。

## 1. 功能范围

KTAG 采用 Capture、Feature、Inference 三层架构，对 IPv4/IPv6 TCP、UDP
流量进行组流、特征提取和模型推理。应用分类与代理分类是两个独立运行模式：一次
运行只加载一套 Schema 和一套模型，不会先执行应用分类再自动执行代理分类。

KTAG 提供两套正式特征契约：

| 用途 | 配置示例 | Schema | Schema 数值 | 模型输入 |
|---|---|---|---:|---:|
| 应用分类 | `/opt/KTAG/tools/demo/config.yaml` | `ktag_legacy_v1` v1 | 354 | 351 |
| 代理分类 | `/opt/KTAG/tools/demo/proxy_config.yaml` | `ktag_proxy_v1` v1 | 619 | 619 |

应用 Schema 保留原 354 维契约。训练时排除 `legacy.flow_id`、
`network.src_ip` 和 `network.dst_ip`，所以随机森林实际接收 351 个数值。代理
Schema 已在组合阶段排除这三个 Feature，并在 351 维通用特征基础上增加 268 维
代理专用特征，因此 Schema 和模型输入均为 619 个数值。

> `proxy_config.yaml` 是可运行的两分类示例：标签 0 为 VMess/WebSocket/TLS，
> 标签 1 为 VMess/XHTTP(H2)/TLS。若要识别普通流量或其余代理形态，必须把
> 对应流量作为独立外层数据组参与训练；KTAG 不在模型之前提供自动判断
> “普通流/VPN 流”的规则引擎。

本期“VPN 识别”实际指应用代理或加密代理流量分类，包括 Shadowsocks、
ShadowsocksR、VMess、VLESS、Trojan、Hysteria2 及其约定承载形态，不包含
PPTP、L2TP、IPsec 等传统隧道 VPN。

## 2. 构建与产物

编译依赖和构建方法见《[编译指南](installation_guide.md)》。构建命令：

```bash
sh /opt/KTAG/build.sh
```

与应用/代理分类有关的主要产物如下：

```text
/opt/KTAG/output/ktag_demo
/opt/KTAG/output/libktag_capture.so
/opt/KTAG/output/libktag_feature.so
/opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/plugins/libktag_traffic_features.so
/opt/KTAG/output/plugins/libktag_proxy_features.so
/opt/KTAG/output/python/training.py
/opt/KTAG/output/python/evaluation.py
```

Demo 的全部推理入口（包括 `--pipeline-mode legacy`）统一使用 `libktag_capture.so`、
`libktag_feature.so` 和 `libktag_inference.so`。应用分类加载 Traffic 插件，代理分类同时加载
Traffic 与 Proxy 插件；二者均由默认构建生成。旧固定向量提取、推理接口及独立兼容库已移除。

确认产物存在：

```bash
ls -l /opt/KTAG/output/ktag_demo /opt/KTAG/output/libktag_capture.so \
  /opt/KTAG/output/libktag_feature.so /opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/ktag_demo -h
```

构建树的 `ktag_demo` 已设置运行时库搜索路径。若将程序、动态库或插件复制到其他目录，请保留其相对布局，或配置 `LD_LIBRARY_PATH`，并确保配置中的 Traffic、Proxy 插件路径可被加载。

训练和评估前安装 Python 依赖（仅推理不需要）：

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

编译时 Python 辅助脚本会复制到 `/opt/KTAG/output/python/`；请不要单独删除该目录。

## 3. 配置文件

### 3.1 应用与代理使用同一种配置结构

应用任务使用 `/opt/KTAG/tools/demo/config.yaml`，代理任务使用
`/opt/KTAG/tools/demo/proxy_config.yaml`。两者都沿用以下现有字段，不需要另一种配置格式：

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

规划数据与标签时，每个类别准备独立 PCAP，按采集批次、终端、时间段或服务器划分
训练集和评估集。不要让同一 PCAP、会话或服务器流量跨集合泄漏，否则评估准确率
可能虚高。IPv4、IPv6 的同类数据可置于同一个内层列表；仅含单一地址族也可训练，
但另一个地址族将未经验证。

同一 CSV 同时出现在训练集和评估集会阻止训练。训练集和评估集出现相同服务器地址
只会产生告警并继续执行，因为单靠 CSV 无法可靠判断 NAT、负载均衡或双栈地址是否
属于同一物理服务器；这类重叠可能使评估结果高估跨服务器泛化能力，需要数据提供方
结合采集记录人工确认。

### 3.2 应用 Schema 配置

应用配置通过 Traffic 插件提供冻结特征，关键内容为：

```yaml
pipeline:
  enabled: true
  mode: feature_parallel
  model_package_path: /opt/KTAG/result/legacy_model_package
  plugins:
    - /opt/KTAG/output/plugins/libktag_traffic_features.so
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
  model_package_path: /opt/KTAG/result/proxy_two_class_v1_model
  plugins:
    - /opt/KTAG/output/plugins/libktag_traffic_features.so
    - /opt/KTAG/output/plugins/libktag_proxy_features.so
  schema:
    name: ktag_proxy_v1
    version: 1
    remove_features:
      - legacy.flow_id
      - network.src_ip
      - network.dst_ip
    add_features:
      # 完整顺序见 /opt/KTAG/tools/demo/proxy_config.yaml
```

`ktag_proxy_v1` 包含 118 个 Feature Descriptor、619 个数值、Hash
`0xca1eae5086b41ee5`。代理 Schema CSV 包含 12 个 `meta.*` 列和 619 个 Schema
特征列，共 631 列。由于 Flow ID 和 IP 已在 Schema 组合阶段排除，训练时只删除前
12 个元数据列：

```yaml
columns_to_remove: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
```

不要把 622 维 Traffic 与 Proxy Feature 全集当作正式代理 Schema。622 只表示可选择
全集；正式 `ktag_proxy_v1` 固定为 619 维。

### 3.4 路径解析规则

配置中的相对路径均相对于配置文件所在目录解析，而不是相对于终端当前目录。建议在
配置中统一使用绝对路径，避免路径歧义。复制 YAML 到新目录后，必须同步调整
`pipeline.plugins`、`pipeline.model_package_path` 和训练/评估输入输出等相对路径。

## 4. 生成 Schema 特征 CSV

应用分类：

```bash
/opt/KTAG/output/ktag_demo \
  -f /path/to/application.pcap \
  --config /opt/KTAG/tools/demo/config.yaml \
  -c /path/to/application.csv
```

代理分类：

```bash
/opt/KTAG/output/ktag_demo \
  -f /path/to/proxy.pcap \
  --config /opt/KTAG/tools/demo/proxy_config.yaml \
  -c /path/to/proxy.csv
```

`-f --config` 每次只接受一个 PCAP，输出格式必须使用 `-c *.csv`，不能同时使用
`-j` 或 `-w`。需要处理多个 PCAP 时，应逐个执行并分别生成 CSV。

启动日志会打印 Schema Name、Version、Feature Count、Value Count 和 Hash。应用
输出应显示 354 个值，CSV 总列数为 366；代理输出应显示 619 个值，CSV 总列数为
631。这里的“Schema 数值数”与“CSV 总列数”不是同一个概念。

不要调整 CSV 列顺序，也不要用 `feature_filter.py` 覆盖唯一的原始 CSV；
`meta.ip_version` 等元数据还要用于分层评估。

若 PCAP 尾部损坏但前面已经包含完整报文，Pipeline 会保留此前成功处理的流，
打印一条 WARNING，并在 EOF 清理阶段输出已有结果。损坏位置之后的数据无法恢复；
训练前仍建议替换或重新采集损坏文件。

## 5. 数据预检、训练与评估

训练、评估和部署必须使用同一 Schema、Feature 生命周期及数据处理规则。

### 5.1 数据预检

填写 `training_data_paths` 和 `evaluation_data_paths` 后执行只读预检：

```bash
python3 /opt/KTAG/tools/data/dataset_preflight.py /opt/KTAG/tools/demo/proxy_config.yaml
```

预检不会修改 CSV，主要检查：

- 训练/评估数据组数量与顺序；
- CSV 列头、顺序和 Schema 维度；
- 同一 CSV 是否跨训练/评估复用；
- IPv4/IPv6、TCP/UDP、报文数、服务器和端口覆盖；
- `meta.*`、Flow ID 和地址字段是否可能进入模型；
- 训练与评估服务器地址是否重叠。

修复预检报告中的 ERROR 后再训练；地址重叠 WARNING 需要结合采集记录判断是否
存在泄漏。

IPv4-only、IPv6-only 和混合数据都允许训练。缺少某个地址族只输出告警，不阻止
训练；告警表示该模型没有通过对应地址族数据建立或验证效果。

### 5.2 训练

```bash
# 应用模型
/opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/config.yaml

# 代理模型
/opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/proxy_config.yaml
```

训练使用随机森林和 StandardScaler，并输出 PKL、ONNX、Scaler、
`feature_importance.csv`。配置了 `pipeline.model_package_path` 时，还会生成
公共 Inference API 使用的模型包：

```text
model_package_path/
├── manifest.json
├── model.onnx
└── scaler.json
```

模型包记录 Schema Hash、Feature ID/维度/顺序、过滤条件和 Scaler 参数。在线加载
时会校验这些内容；应用模型包和代理模型包不能交叉加载。校验失败时，应使用同一
训练产物中的完整模型包，而不是替换单个文件或手工调整 CSV 列。

### 5.3 评估

```bash
/opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/config.yaml
/opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/proxy_config.yaml
```

评估使用 `evaluation_data_paths`，生成整体 Classification Report、预测明细、整体
混淆矩阵；现代 Schema CSV 含 `meta.ip_version` 时，还会生成 IPv4、IPv6 分层报告
和分层混淆矩阵。缺少某地址族时，对应部分明确标记为不可评估。

应同时查看每类 precision、recall、F1 和 support，不能只看 overall accuracy。
评估通过后，使用未参与训练和评估的第三批 PCAP 再执行离线推理，核对标签映射、
结果数量及业务表现，再上线实时网口。

### 5.4 结果解读

推理结果包含数字 `label`、`confidence`、IPv4/IPv6 流元数据、首包方向的地址端口、
规范化 Flow Key 和包序号等。

`label` 是训练数据外层列表的位置（从 0 开始），不是自动得出的业务名称；交付模型
时务必保留“数字标签—业务名称”映射表。

`confidence` 是模型对当前预测标签的概率，`0` 为合法值，`-1` 表示没有可用概率
输出。它不是整体准确率，也不会自动将低置信度流标为 `unknown`；阈值与未知类策略
应通过离线验证后由业务侧定义。推理引擎不使用 confidence 阈值生成 `unknown`。

## 6. PCAP 与网口推理

### 6.1 应用分类

`config.yaml` 默认启用 `feature_parallel`。显式指定 Pipeline Mode 可以为本次运行选择
其他调度方式：

```bash
/opt/KTAG/output/ktag_demo \
  -r /opt/KTAG/tools/demo/config.yaml \
  -p /path/to/application.pcap \
  --pipeline-mode feature_parallel \
  -n 4 \
  -c /path/to/application_result.csv
```

即使 YAML 的 `pipeline.enabled: false`，也可通过 `--pipeline-mode` 仅为本次调用
启用新 Pipeline。

`--pipeline-mode legacy` 只保留应用分类的历史调度和 CSV/JSON 格式，推理同样调用
公共 Inference API，并且必须设置 `pipeline.model_package_path`。该路径相对于 YAML
文件解析；`-m 0/1/2` 分别保留单流、批量、文件往返模式。旧 ONNX/Scaler 可通过
`tools/model_package/create_legacy_package.py` 转换；列选择、标准化和包数过滤均以
生成的模型包为准，修改旧 YAML 顶层预处理字段后需要重新生成模型包。

### 6.2 代理分类

`proxy_config.yaml` 默认启用新 Pipeline，并从
`pipeline.model_package_path` 加载代理模型包：

```bash
/opt/KTAG/output/ktag_demo \
  -r /opt/KTAG/tools/demo/proxy_config.yaml \
  -p /path/to/proxy.pcap \
  --pipeline-mode feature_parallel \
  -n 4 \
  -c /path/to/proxy_result.csv
```

`-p` 可重复指定多个 PCAP。PCAP 输入必须使用 `-c` 或 `-j` 指定结果文件；前者
输出 CSV，后者输出 JSON，二者互斥且扩展名必须匹配。CSV/JSON 推理结果包含数字
标签、confidence、地址族、第一包方向的源/目的地址与端口、规范化 Flow Key 和报文
序号。Flow ID 只用于结果关联，不进入模型。

### 6.3 网口实时推理

先列出可用接口：

```bash
/opt/KTAG/output/ktag_demo -l
```

实时抓包需要具备相应权限，按现场安全规范使用有抓包权限的账号或授予最小必要能力：

```bash
/opt/KTAG/output/ktag_demo \
  -r /opt/KTAG/tools/demo/proxy_config.yaml \
  -i eth0 \
  --pipeline-mode full_parallel \
  -n 4 \
  --inference-threads 2 \
  -j /path/to/proxy_live_result.json
```

网口模式未指定 `-c/-j` 时，结果只打印到控制台。确认镜像口、旁路点或主机网口能
看到目标的双向完整报文；单向或截断流会降低特征完整度和分类效果。长期运行时应
监控 CPU、内存、磁盘与结果文件增长，并由外部日志轮转机制管理输出文件。

## 7. Pipeline Mode 与线程配置

| 模式 | 说明 | 线程约束 |
|---|---|---|
| `legacy` | 保留历史调度和输出格式，推理改用公共 Inference API | 需要应用模型包，不接受新 Pipeline 线程参数 |
| `single_thread` | Capture、Feature、Inference 串行执行 | Feature/Inference 均为 1 |
| `staged` | 分阶段队列执行 | Feature/Inference 均为 1 |
| `feature_parallel` | 多 Feature Worker，单 Inference Worker | `-n` 可大于 1，Inference 为 1 |
| `full_parallel` | Feature 和 Inference 均可并行 | 同时设置两类线程数 |

需要显式设置两类线程数时：

```bash
/opt/KTAG/output/ktag_demo -r /opt/KTAG/tools/demo/proxy_config.yaml \
  -p /path/to/proxy.pcap \
  --pipeline-mode full_parallel -n 4 --inference-threads 2 \
  -c /path/to/prediction.csv
```

`-m 0/1/2` 只控制 `legacy` 调度；四种 Schema Pipeline Mode 使用各自的线程与批次配置。
性能统计保留计算耗时、CPU 累计计算量及端到端总耗时/吞吐量，不再输出有效吞吐量或最慢线程经过耗时。

先以 `single_thread` 或固定线程数完成准确性验收，再逐步调优。相同模型、配置、
PCAP 和线程数下，输出应可重复。改变线程数可能改变内部流分配和浮点累计顺序，
因此最终统计或准确率可能出现很小差异；同一验收批次应固定模型、配置和线程数。

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
时序和熵仍可作为模型输入。

## 10. 功能限制

- 示例代理配置只启用两个 VMess 承载类别；识别其他类别需要准备对应数据并重新训练。
- KTAG 不内置 `unknown` 标签或 confidence 阈值；输出为模型数字标签。
- 要区分普通流量和代理流量，训练数据中必须存在具有明确数字标签的普通流量类别。
- 服务器地址跨训练/评估重叠只告警，需要数据侧人工判断是否会造成泄漏。
- IPv4-only 或 IPv6-only 数据可以训练，但不能据此声明缺失地址族的模型效果。

## 11. 命令速查

| 目标 | 命令 |
|---|---|
| 帮助/网口 | `/opt/KTAG/output/ktag_demo -h` / `/opt/KTAG/output/ktag_demo -l` |
| 历史格式特征 CSV | `/opt/KTAG/output/ktag_demo -f input.pcap -c feature.csv` |
| 历史格式特征 JSON（含 rawbow） | `/opt/KTAG/output/ktag_demo -f input.pcap -j feature.json -w` |
| Schema CSV | `/opt/KTAG/output/ktag_demo -f input.pcap --config task.yaml -c schema.csv` |
| 训练/评估 | `/opt/KTAG/output/ktag_demo -t task.yaml` / `/opt/KTAG/output/ktag_demo -e task.yaml` |
| PCAP/网口推理 | `/opt/KTAG/output/ktag_demo -r task.yaml -p input.pcap -c result.csv` / `/opt/KTAG/output/ktag_demo -r task.yaml -i eth0 -j result.json` |

不带 `--config` 的 `-f` 通过公共 Feature API 和 Traffic 插件提取特征，再转换为历史
固定向量 CSV/JSON 格式，可附带 rawbow，主要用于人工打标核验；与正式 Schema CSV 的
列布局不同。此入口默认查找 `libktag_feature.so` 同级 `plugins/` 中的 Traffic 插件，
也可通过 `KTAG_LEGACY_FEATURE_PLUGIN` 指定插件路径。

## 12. 常见问题

| 现象 | 排查方向 |
|---|---|
| 找不到共享库或插件加载失败 | 检查 `/opt/KTAG/output/`、`/opt/KTAG/output/plugins/` 布局，确认 `LD_LIBRARY_PATH` 和插件路径。 |
| `-f --config` 报参数错误 | 每次只能一个 `-f`，使用 `-c *.csv`，不要配合 `-j`、`-w`、`legacy`。 |
| 模型包/Schema 校验失败 | YAML、插件、Feature 生命周期和模型包必须来自同一训练契约；勿混用应用与代理模型。检查配置中的 Schema Name、Version、Feature 顺序和 `pipeline.model_package_path`；不能在冻结名称下修改 Feature 后继续加载旧模型。 |
| 训练提示列数或列头不匹配 | 确认所有 CSV 均由相同 YAML 的 `-f --config` 生成，并检查 `columns_to_remove`。 |
| 代理 CSV 不是 619 列 | 619 是 Schema 特征数。Schema CSV 还包含 12 个 `meta.*` 列，因此总列数应为 631；应用 CSV 对应为 12 + 354 = 366 列。 |
| 训练提示旧 CSV 不含 `meta.ip_version` | 这是兼容告警，不阻止旧 IPv4 CSV 训练。由于旧 CSV 没有地址族元数据，训练工具无法生成可靠的 IPv4/IPv6 分层统计。 |
| 实时无结果或结果很少 | 检查网口、抓包权限、镜像方向、输出位置及是否采集到完整双向流。 |
| 评估高于现场效果 | 排查 PCAP/会话/服务器泄漏；加入贴近现场的流量、终端和时间段数据。 |
| 普通流量被判为代理类 | 示例模型没有普通流量类别；将其作为独立外层数据组加入并重新训练。 |

Classification Report 中只有数字时：数字来自 `training_data_paths` 外层顺序。
KTAG 不在模型或推理代码中硬编码应用名、代理名；请使用采集和训练记录维护数字到
业务类别的映射。
