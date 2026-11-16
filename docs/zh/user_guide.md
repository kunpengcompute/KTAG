<a id="用户指南"></a>

# KTAG用户指南

本文介绍 `ktag_demo` 的完整使用方法：功能范围、安装与构建、流量分类场景、工具操作维护、常用操作、常见问题和故障排除。KTAG源码根目录以 `/opt/KTAG` 为例，命令均使用绝对路径。快速上手见《[快速入门](quick_start.md)》。

## KTAG 简介

KTAG采用Capture、Feature、Inference三层架构，对IPv4/IPv6 TCP、UDP
流量进行组流、特征提取和模型推理。应用分类与代理分类是两个独立运行模式：一次
运行只加载一套Schema和一套模型，不会先执行应用分类再自动执行代理分类。

KTAG提供两套正式特征契约。

| 用途 | 配置示例 | Schema | Schema数值 | 模型输入 |
|---|---|---|---:|---:|
| 应用分类 | /opt/KTAG/tools/demo/config.yaml | ktag_legacy_v1 v1 | 354 | 351 |
| 代理分类 | /opt/KTAG/tools/demo/proxy_config.yaml | ktag_proxy_v1 v1 | 619 | 619 |

应用Schema保留原354维契约。训练时排除 `legacy.flow_id`、
`network.src_ip` 和 `network.dst_ip`，所以随机森林实际接收351个数值。代理
Schema已在组合阶段排除这三个Feature，并在351维通用特征基础上增加268维
代理专用特征，因此Schema和模型输入均为619个数值。

`proxy_config.yaml` 是四分类示例：标签0/1为SSR/auth-aes128-md5和SSR/auth-aes128-sha1，标签2/3为VMess/WebSocket/TLS和VMess/XHTTP(H2)/TLS。若要识别普通流量或其余代理形态，必须把对应流量作为独立外层数据组参与训练；KTAG不在模型之前提供自动判断“普通流/VPN流”的规则引擎。

本期“VPN识别”实际指应用代理或加密代理流量分类，包括Shadowsocks、
ShadowsocksR、VMess、VLESS、Trojan、Hysteria2及其约定承载形态，不包含
PPTP、L2TP、IPsec等传统隧道VPN。

## IPv4/IPv6 支持边界

现代Capture/Feature Pipeline支持IPv4/IPv6 TCP、UDP，使用完整128位IPv6地址
组成Flow Key。IPv4与IPv6连接分别形成流，不进行跨地址族会话合并。DNS A和
AAAA记录可写入双栈Domain Store，用于相应流的域名关联。

IPv6地址只作为Flow Key、CSV/推理元数据和审计信息。正式代理Schema不输入源、
目的IP；应用 `ktag_legacy_v1` 为保持354维兼容，在IPv6流中把两个Legacy IP
Feature填0，真实IPv6地址仍保存在 `meta.*` 中。

## 代理专用特征与 QUIC 范围

代理插件注册三个Provider。

| Provider | 主要信息 |
|---|---|
| Sequence/Burst | 有方向包长、相对时间、TCP Flags、Burst和方向切换 |
| TCP Signature | 有界TCP Prefix、重传/乱序/缺口、TLS ClientHello和HTTP/WebSocket/H2/gRPC迹象 |
| QUIC | 长短头、Version、CID、Token、Header Type、报文序列、首字节分布和熵 |

QUIC解析是轻量级结构解析，支持识别QUIC v1、v2、Version Negotiation和部分长短
Header结构，不执行QUIC解密、完整连接状态机、TLS-over-QUIC重组或HTTP/3内容
解析。

Hysteria2与普通HTTP/3的区分属于统计模型分类，不是确定性协议签名。启用
Salamander等混淆后，标准QUIC Header可能无法解析；解析失败计数、包长、方向、
时序和熵仍可作为模型输入。

## 约束与限制

- 示例代理配置启用两个SSR类别与两个VMess承载类别；识别其他类别需要准备对应数据并重新训练。
- KTAG不内置 `unknown` 标签或confidence阈值；输出为模型数字标签。
- 要区分普通流量和代理流量，训练数据中必须存在具有明确数字标签的普通流量类别。
- 服务器地址跨训练/评估重叠只告警，需要数据侧人工判断是否会造成泄漏。
- IPv4-only或IPv6-only数据可以训练，但不能据此声明缺失地址族的模型效果。

# 安装

编译依赖和详细构建方法见《[编译指南](installation_guide.md)》。

## 构建与检查

**操作场景**

构建KTAG，并确认 `ktag_demo`、公共动态库和插件产物可用。

**前提条件**

已按《[编译指南](installation_guide.md)》准备构建环境。

**操作步骤**

1. 执行构建命令。

   ```bash
   bash /opt/KTAG/build.sh
   ```

   与应用/代理分类有关的主要产物如下。

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

   `ktag_demo` 的全部推理入口（包括 `--pipeline-mode legacy`）统一使用 `libktag_capture.so`、
   `libktag_feature.so` 和 `libktag_inference.so`。应用分类加载Traffic插件，代理分类同时加载
   Traffic与Proxy插件；二者均由默认构建生成。旧固定向量提取、推理接口及独立兼容库已移除。

2. 检查产物并查看帮助信息。

   ```bash
   ls -l /opt/KTAG/output/ktag_demo /opt/KTAG/output/libktag_capture.so \
     /opt/KTAG/output/libktag_feature.so /opt/KTAG/output/libktag_inference.so
   /opt/KTAG/output/ktag_demo -h
   ```

## 安装训练和评估依赖

**操作场景**

训练和评估前安装Python依赖；仅推理不需要此操作。

**操作步骤**

1. 安装Python依赖。

   ```bash
   python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
   ```

# 流量分类场景

## 训练、评估和部署前提

训练、评估和部署必须使用同一Schema、Feature生命周期及数据处理规则。

## 配置文件

**操作场景**

为应用分类或代理分类准备配置文件和数据标签。

**操作步骤**

1. 根据应用分类或代理分类任务，复制对应的示例YAML。
2. 按下一节的统一配置结构填写训练、评估文件列表，并核对数字标签与业务类别。
3. 按[应用Schema配置](#应用-schema-配置)或[代理Schema配置](#代理-schema-配置)核对插件、Schema和列删除规则。
4. 按[路径解析规则](#路径解析规则)调整复制后的相对路径。
5. 完成CSV生成后执行[数据预检](#数据预检)，确认通过再训练。

**配置说明**

### 统一配置结构说明

应用任务使用 `/opt/KTAG/tools/demo/config.yaml`，代理任务使用
`/opt/KTAG/tools/demo/proxy_config.yaml`。两者都沿用以下现有字段，不需要另一种配置格式。

```yaml
training_data_paths:
  - [同一标签的训练CSV-1, 同一标签的训练CSV-2]
  - [下一标签的训练CSV-1, 下一标签的训练CSV-2]

evaluation_data_paths:
  - [标签0的评估CSV]
  - [标签1的评估CSV]
```

外层列表的位置就是数字标签：第一个数据组为标签0，第二个数据组为标签1，依此
类推。训练和评估的外层组数、顺序必须一致。同一标签中的IPv4和IPv6 CSV应放在
同一个内层列表中，不能因为地址族不同而新建标签。

代码和推理引擎只理解数字标签，不理解 `bilibili`、`VMess/WebSocket/TLS` 等业务
名称。业务侧必须保存本次训练使用的数据组顺序；改变外层顺序后，数字标签含义也会
随之改变。

规划数据与标签时，每个类别准备独立PCAP，按采集批次、终端、时间段或服务器划分
训练集和评估集。

不要让同一PCAP、会话或服务器流量跨集合泄漏，否则评估准确率
可能虚高。IPv4、IPv6的同类数据可置于同一个内层列表；仅含单一地址族也可训练，
但另一个地址族将未经验证。


同一CSV同时出现在训练集和评估集会阻止训练。训练集和评估集出现相同服务器地址
只会产生告警并继续执行，因为单靠CSV无法可靠判断NAT、负载均衡或双栈地址是否
属于同一物理服务器；这类重叠可能使评估结果高估跨服务器泛化能力，需要数据提供方
结合采集记录人工确认。

常用运行字段的缺省值如下；示例YAML可覆盖它们。训练与推理应保持特征契约一致。

| 字段 | 缺省值 | 取值与作用 |
| :--- | :--- | :--- |
| pipeline.enabled | false | 布尔值；CLI显式指定新模式可为本次运行启用Pipeline |
| pipeline.mode | single_thread | YAML只接受 single_thread、staged、feature_parallel、full_parallel；含义及线程约束见[模式表](#pipeline-mode-与线程配置) |
| pipeline.feature.threads | 4 | 正整数 1..4294967295，Feature线程数；最终值受模式约束 |
| pipeline.inference.threads | 1 | 同一正整数范围，Inference线程数；最终值受模式约束 |
| pipeline.feature.queue_capacity | 65536 | 同一正整数范围，报文队列可容纳的元素数 |
| pipeline.inference.queue_capacity | 4096 | 同一正整数范围，Feature记录队列可容纳的元素数 |
| pipeline.feature.lifecycle_mode | legacy | legacy 保持历史流结束语义；modern 启用空闲超时和UDP报文上限；详见[生命周期说明](#pipeline-mode-与线程配置) |

`legacy` 是CLI的 `--pipeline-mode` 特殊选项，不是 `pipeline.mode` 的YAML枚举值。以上数值范围表示解析范围，不代表部署时可以无成本使用任意大值。

### 应用 Schema 配置

应用配置通过Traffic插件提供冻结特征，关键内容为。

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

该Schema是冻结契约：53个Feature Descriptor、354个数值、Hash
`0xdd48882b53bebfa5`。不要在相同名称和版本下改变Feature、顺序或维度。

新生成的应用Schema CSV包含12个 `meta.*` 列和354个Schema特征列，共
366列。训练时使用。

```yaml
columns_to_remove: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
```

即删除12个元数据列以及Schema开头的Flow ID、源IP、目的IP三列，最终输入
351个数值。旧式CSV没有12个 `meta.*` 列，必须沿用其原有删除规则；不能只看
总列数混用新旧配置。

### 代理 Schema 配置

代理配置需要加载插件，并使用冻结的代理Schema。

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

`ktag_proxy_v1` 包含118个Feature Descriptor、619个数值、Hash
`0xca1eae5086b41ee5`。代理Schema CSV包含12个 `meta.*` 列和619个Schema
特征列，共631列。由于Flow ID和IP已在Schema组合阶段排除，训练时只删除前
12个元数据列。

```yaml
columns_to_remove: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
```

不要把622维Traffic与Proxy Feature全集当作正式代理Schema。622只表示可选择
全集；正式 `ktag_proxy_v1` 固定为619维。

### 路径解析规则

配置中的相对路径均相对于配置文件所在目录解析，而不是相对于终端当前目录。建议在
配置中统一使用绝对路径，避免路径歧义。复制YAML到新目录后，必须同步调整
`pipeline.plugins`、`pipeline.model_package_path` 和训练/评估输入输出等相对路径。

## 生成 Schema 特征 CSV

**操作场景**

从PCAP生成应用分类或代理分类使用的Schema特征CSV。

**操作步骤**

1. 根据分类任务，选择以下一种命令生成CSV。

   **应用分类**

   ```bash
   /opt/KTAG/output/ktag_demo \
     -f /path/to/application.pcap \
     --config /opt/KTAG/tools/demo/config.yaml \
     -c /path/to/application.csv
   ```

   **代理分类**

   ```bash
   /opt/KTAG/output/ktag_demo \
     -f /path/to/proxy.pcap \
     --config /opt/KTAG/tools/demo/proxy_config.yaml \
     -c /path/to/proxy.csv
   ```

   `-f --config` 每次只接受一个PCAP，输出格式必须使用 `-c *.csv`，不能同时使用
   `-j` 或 `-w`。需要处理多个PCAP时，应逐个执行并分别生成CSV。

2. 检查启动日志和CSV列数。

   启动日志会打印Schema Name、Version、Feature Count、Value Count和Hash。应用
   输出应显示354个值，CSV总列数为366；代理输出应显示619个值，CSV总列数为
   631。这里的“Schema数值数”与“CSV总列数”不是同一个概念。

   不要调整CSV列顺序，也不要用 `feature_filter.py` 覆盖唯一的原始CSV。
   `meta.ip_version` 等元数据还要用于分层评估。

## 数据预检、训练与评估

### 数据预检

**操作场景**

检查训练和评估数据的配置、列布局与覆盖情况。

**前提条件**

已填写 `training_data_paths` 和 `evaluation_data_paths`。

**操作步骤**

1. 执行只读预检。

   ```bash
   python3 /opt/KTAG/tools/data/dataset_preflight.py /opt/KTAG/tools/demo/proxy_config.yaml
   ```

   预期结果：预检不会修改CSV，主要检查。

   - 训练/评估数据组数量与顺序。
   - CSV列头、顺序和Schema维度。
   - 同一CSV是否跨训练/评估复用。
   - IPv4/IPv6、TCP/UDP、报文数、服务器和端口覆盖。
   - `meta.*`、Flow ID和地址字段是否可能进入模型。
   - 训练与评估服务器地址是否重叠。

2. 根据报告处理ERROR和WARNING。

   修复预检报告中的ERROR后再训练；地址重叠WARNING需要结合采集记录判断是否
   存在泄漏。

   IPv4-only、IPv6-only和混合数据都允许训练。缺少某个地址族只输出告警，不阻止
   训练；告警表示该模型没有通过对应地址族数据建立或验证效果。

### 训练

**操作场景**

使用应用或代理配置训练模型并生成部署产物。

**前提条件**

已完成数据预检，并处理报告中的ERROR。

**操作步骤**

1. 根据分类任务，选择对应命令训练模型。

   ```bash
   # 应用模型
   /opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/config.yaml

   # 代理模型
   /opt/KTAG/output/ktag_demo -t /opt/KTAG/tools/demo/proxy_config.yaml
   ```

2. 检查训练产物和模型包。

   预期结果：训练使用随机森林和StandardScaler，并输出PKL、ONNX、Scaler、
   `feature_importance.csv`。配置了 `pipeline.model_package_path` 时，还会生成
   公共Inference API使用的模型包。

   ```text
   model_package_path/
   ├── manifest.json
   ├── model.onnx
   └── scaler.json
   ```

   模型包记录Schema Hash、Feature ID/维度/顺序、过滤条件和Scaler参数。在线加载
   时会校验这些内容；应用模型包和代理模型包不能交叉加载。校验失败时，应使用同一
   训练产物中的完整模型包，而不是替换单个文件或手工调整CSV列。

### 评估

**操作场景**

使用独立评估数据检查模型分类效果。

**前提条件**

已完成模型训练，且训练和评估数据组顺序一致。

**操作步骤**

1. 根据分类任务，执行对应评估命令。

   ```bash
   /opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/config.yaml
   /opt/KTAG/output/ktag_demo -e /opt/KTAG/tools/demo/proxy_config.yaml
   ```

   预期结果：评估使用 `evaluation_data_paths`，生成整体Classification Report、预测明细、整体
   混淆矩阵；现代Schema CSV含 `meta.ip_version` 时，还会生成IPv4、IPv6分层报告
   和分层混淆矩阵。缺少某地址族时，对应部分明确标记为不可评估。

2. 检查分类指标并使用第三批PCAP核对效果。

   应同时查看每类precision、recall、F1和support，不能只看overall accuracy。
   评估通过后，使用未参与训练和评估的第三批PCAP再执行离线推理，核对标签映射、
   结果数量及业务表现，再上线实时网口。

## PCAP 与网口推理

### 推理参数说明

| 参数 | 类型、默认值与约束 |
| :--- | :--- |
| -r | YAML配置路径，必填；相对路径按启动时的当前目录定位配置文件 |
| -p | 输入PCAP路径，可重复；每个文件内的有效流参与处理 |
| -c / -j | CSV / JSON输出路径，二者互斥，扩展名须匹配；PCAP推理必须指定一种 |
| --pipeline-mode | CLI可选 legacy、single_thread、staged、feature_parallel、full_parallel；省略时按YAML及 pipeline.enabled 选择 |
| -n / --feature-threads | Feature线程数，十进制正整数 1..4294967295；省略时沿用YAML，缺省值为4，并按模式归一化 |
| --inference-threads | Inference线程数，同一正整数范围；省略时沿用YAML，缺省值为1，并按模式归一化 |
| -i | 网口名，按 -l 返回的实际名称填写并保留大小写；本节命令使用libpcap网口采集 |

`single_thread` / `staged` 最终使用1个Feature、1个Inference线程，显式CLI线程数也只能为1。`feature_parallel` 的Inference固定为1；`full_parallel` 可分别设置两类线程数。`legacy` 不接受新Pipeline线程参数。

### 应用分类

**操作场景**

使用应用模型对PCAP执行推理。

**背景信息**

`config.yaml` 默认启用 `feature_parallel`。显式指定Pipeline Mode可以为本次运行选择
其他调度方式。

即使YAML的 `pipeline.enabled: false`，也可通过 `--pipeline-mode` 仅为本次调用
启用新Pipeline。

`--pipeline-mode legacy` 只保留应用分类的历史调度和CSV/JSON格式，推理同样调用
公共Inference API，并且必须设置 `pipeline.model_package_path`。

该路径相对于YAML
文件解析；`-m 0/1/2` 分别保留单流、批量、文件往返模式。旧ONNX/Scaler可通过
`tools/model_package/create_legacy_package.py` 转换；列选择、标准化和包数过滤均以
生成的模型包为准，修改旧YAML顶层预处理字段后需要重新生成模型包。

**操作步骤**

1. 使用应用配置执行推理。

   ```bash
   /opt/KTAG/output/ktag_demo \
     -r /opt/KTAG/tools/demo/config.yaml \
     -p /path/to/application.pcap \
     --pipeline-mode feature_parallel \
     -n 4 \
     -c /path/to/application_result.csv
   ```

### 代理分类

**操作场景**

使用代理模型对PCAP执行推理。

**背景信息**

`proxy_config.yaml` 默认启用新Pipeline，并从
`pipeline.model_package_path` 加载代理模型包。

**操作步骤**

1. 使用代理配置执行推理。

   ```bash
   /opt/KTAG/output/ktag_demo \
     -r /opt/KTAG/tools/demo/proxy_config.yaml \
     -p /path/to/proxy.pcap \
     --pipeline-mode feature_parallel \
     -n 4 \
     -c /path/to/proxy_result.csv
   ```

**参数说明**

`-p` 可重复指定多个PCAP。PCAP输入必须使用 `-c` 或 `-j` 指定结果文件；前者
输出CSV，后者输出JSON，二者互斥且扩展名必须匹配。CSV/JSON推理结果包含数字
标签、confidence、地址族、第一包方向的源/目的地址与端口、规范化Flow Key和报文
序号。Flow ID只用于结果关联，不进入模型。

### 网口实时推理

**操作场景**

采集网口流量并实时执行模型推理。

**背景信息**

网口模式未指定 `-c/-j` 时，结果只打印到控制台。确认镜像口、旁路点或主机网口能
看到目标的双向完整报文；单向或截断流会降低特征完整度和分类效果。

**前提条件**

先确认账号具有网口抓包权限；可由root运行，或由管理员按抓包模式配置网络能力。`-i` 应填写下面 `-l` 命令返回的接口名，保持大小写；接口必须存在且对当前账号可访问。

**操作步骤**

1. 列出可用接口。

   ```bash
   /opt/KTAG/output/ktag_demo -l
   ```

2. 选择接口并启动实时推理。

   ```bash
   /opt/KTAG/output/ktag_demo \
     -r /opt/KTAG/tools/demo/proxy_config.yaml \
     -i eth0 \
     --pipeline-mode full_parallel \
     -n 4 \
     --inference-threads 2 \
     -j /path/to/proxy_live_result.json
   ```

# 工具操作维护

## 程序、动态库和插件维护

**操作场景**

复制运行产物或维护Python辅助脚本目录。

**操作步骤**

1. 保持程序、动态库和插件的相对布局。

   构建树的 `ktag_demo` 已设置运行时库搜索路径。若将程序、动态库或插件复制到其他目录，请保留其相对布局，或配置 `LD_LIBRARY_PATH`，并确保配置中的Traffic、Proxy插件路径可被加载。

2. 保留训练和评估使用的Python辅助脚本目录。

   编译时Python辅助脚本会复制到 `/opt/KTAG/output/python/`；请不要单独删除该目录。

## 运行资源和日志维护

**操作场景**

维护长期运行的网口推理任务。

**操作步骤**

1. 看管CPU、内存、磁盘与结果文件增长。
2. 由外部日志轮转机制管理输出文件。

# 常用操作

## Pipeline Mode 与线程配置

**操作场景**

选择Pipeline调度方式并配置Feature、Inference线程。

**背景信息**

| 模式 | 说明 | 线程约束 |
|---|---|---|
| legacy | 保留历史调度和输出格式，推理改用公共Inference API | 需要应用模型包，不接受新Pipeline线程参数 |
| single_thread | Capture、Feature、Inference串行执行 | Feature/Inference均为1 |
| staged | 分阶段队列执行 | Feature/Inference均为1 |
| feature_parallel | 多Feature Worker，单Inference Worker | -n 可大于1，Inference为1 |
| full_parallel | Feature和Inference均可并行 | 同时设置两类线程数 |

**操作步骤**

1. 需要显式设置两类线程数时，执行以下命令。

   ```bash
   /opt/KTAG/output/ktag_demo -r /opt/KTAG/tools/demo/proxy_config.yaml \
     -p /path/to/proxy.pcap \
     --pipeline-mode full_parallel -n 4 --inference-threads 2 \
     -c /path/to/prediction.csv
   ```

   `-m 0/1/2` 只控制 `legacy` 调度；四种Schema Pipeline Mode使用各自的线程与批次配置。
   性能统计保留计算耗时、CPU累计计算量及端到端总耗时/吞吐量，不再输出有效吞吐量或最慢线程经过耗时。

2. 固定模型、配置和线程数完成验收，再逐步调优。

   先以 `single_thread` 或固定线程数完成准确性验收，再逐步调优。相同模型、配置、
   PCAP和线程数下，输出应可重复。改变线程数可能改变内部流分配和浮点累计顺序，
   因此最终统计或准确率可能出现很小差异；同一验收批次应固定模型、配置和线程数。

   `pipeline.feature.lifecycle_mode` 有两种取值。

   - `legacy`：复现旧固定向量的流结束语义，适合现有应用模型兼容。
   - `modern`：启用超时、UDP报文上限等现代Feature Context行为，代理配置使用该
     模式。训练特征提取与在线推理必须使用一致的Schema和生命周期配置。

## 结果解读

**操作场景**

查看分类标签及置信度的含义。

**操作步骤**

1. 查看推理结果中的字段。

   推理结果包含数字 `label`、`confidence`、IPv4/IPv6流元数据、首包方向的地址端口、
   规范化Flow Key和包序号等。

2. 核对数字标签与业务类别的映射。

   `label` 是训练数据外层列表的位置（从0开始），不是自动得出的业务名称；交付模型
   时务必保留“数字标签—业务名称”映射表。

3. 解读置信度。

   `confidence` 是模型对当前预测标签的概率，`0` 为合法值，`-1` 表示没有可用概率
   输出。它不是整体准确率，也不会自动将低置信度流标为 `unknown`；阈值与未知类策略
   应通过离线验证后由业务侧定义。推理引擎不使用confidence阈值生成 `unknown`。

## 命令速查

| 目标 | 命令 |
|---|---|
| 帮助/网口 | /opt/KTAG/output/ktag_demo -h / /opt/KTAG/output/ktag_demo -l |
| 历史格式特征CSV | /opt/KTAG/output/ktag_demo -f input.pcap -c feature.csv |
| 历史格式特征JSON（含rawbow） | /opt/KTAG/output/ktag_demo -f input.pcap -j feature.json -w |
| Schema CSV | /opt/KTAG/output/ktag_demo -f input.pcap --config task.yaml -c schema.csv |
| 训练/评估 | /opt/KTAG/output/ktag_demo -t task.yaml / /opt/KTAG/output/ktag_demo -e task.yaml |
| PCAP/网口推理 | /opt/KTAG/output/ktag_demo -r task.yaml -p input.pcap -c result.csv / /opt/KTAG/output/ktag_demo -r task.yaml -i eth0 -j result.json |

不带 `--config` 的 `-f` 通过公共Feature API和Traffic插件提取特征，再转换为历史
固定向量CSV/JSON格式，可附带rawbow，主要用于人工打标核验；与正式Schema CSV的
列布局不同。此入口默认查找 `libktag_feature.so` 同级 `plugins/` 中的Traffic插件，
也可通过 `KTAG_LEGACY_FEATURE_PLUGIN` 指定插件路径。

`-w` 输出RawBoW诊断文本：由域名和HTTP URL提取的Token文本及对应特征，供人工打标核验使用。文本注解不进入模型输入；不要与Schema中的RawBoW数值直方图混淆。

`KTAG_LEGACY_FEATURE_PLUGIN` 是可选的共享库路径覆盖。未设置或为空时采用上述默认位置；推荐绝对路径，也可使用含目录的相对路径，如 `./plugins/libktag_traffic_features.so`，相对于启动进程的当前目录解析。文件须可读、ABI匹配且依赖可加载。

```bash
export KTAG_LEGACY_FEATURE_PLUGIN=/opt/KTAG/output/plugins/libktag_traffic_features.so
printf '%s\n' "$KTAG_LEGACY_FEATURE_PLUGIN"
test -r "$KTAG_LEGACY_FEATURE_PLUGIN"
ldd "$KTAG_LEGACY_FEATURE_PLUGIN"
```

export对当前Shell及后续子进程立即生效；需要持久化时将export写入 `~/.bashrc`，再执行 `source ~/.bashrc`。预期输出所设路径、文件可读且 `ldd` 无 `not found`；最终加载结果以ktag_demo的插件加载日志为准。

# 常见问题

## 代理 CSV 为什么不是 619 列？

### 问题

代理CSV的总列数与619不一致，是否属于输出异常？

### 回答

619是Schema特征数。Schema CSV还包含12个 `meta.*` 列，因此总列数应为631；应用CSV对应为12 + 354 = 366列。

## 训练提示旧 CSV 不含 `meta.ip_version`，是否会阻止训练？

### 问题

训练时提示旧CSV没有地址族元数据。

### 回答

这是兼容告警，不阻止旧IPv4 CSV训练。由于旧CSV没有地址族元数据，训练工具无法生成可靠的IPv4/IPv6分层统计。

## 普通流量为什么被判为代理类？

### 问题

使用示例代理模型时，普通流量也被输出为代理类别。

### 回答

示例模型没有普通流量类别；将其作为独立外层数据组加入并重新训练。

## Classification Report 中的数字如何对应业务类别？

### 问题

Classification Report中只有数字，没有应用名或代理名。

### 回答

数字来自 `training_data_paths` 外层顺序。KTAG不在模型或推理代码中硬编码应用名、代理名；请使用采集和训练记录维护数字到业务类别的映射。

# 故障排除

## 共享库或插件加载失败

**问题描述**

找不到共享库，或插件加载失败。

**对系统的影响**

无法正常执行特征提取或推理。

**可能原因**

运行产物布局、共享库搜索路径或插件路径不匹配。

**操作步骤**

1. 检查 `/opt/KTAG/output/`、`/opt/KTAG/output/plugins/` 布局。
2. 确认 `LD_LIBRARY_PATH` 和配置中的插件路径。

## Schema 特征提取参数错误

**问题描述**

`-f --config` 报参数错误。

**对系统的影响**

无法生成Schema特征CSV。

**可能原因**

输入数量、输出格式或参数组合不符合约束。

**操作步骤**

1. 每次只能指定一个 `-f` 输入。
2. 使用 `-c *.csv` 输出，不要配合 `-j`、`-w` 或 `legacy`。

## 模型包或 Schema 校验失败

**问题描述**

加载时模型包或Schema校验失败。

**对系统的影响**

无法正常加载模型进行推理。

**可能原因**

YAML、插件、Feature生命周期和模型包不属于同一训练契约，或混用了应用与代理模型。

**操作步骤**

1. 检查配置中的Schema Name、Version、Feature顺序和 `pipeline.model_package_path`。
2. 确认YAML、插件、Feature生命周期和模型包来自同一训练契约，勿混用应用与代理模型。
3. 不能在冻结名称下修改Feature后继续加载旧模型。

## 训练列数或列头不匹配

**问题描述**

训练时提示列数或列头不匹配。

**对系统的影响**

训练无法按预期读取特征数据。

**可能原因**

CSV的Schema、列布局或删除规则不一致。

**操作步骤**

1. 确认所有CSV均由相同YAML的 `-f --config` 生成。
2. 检查 `columns_to_remove`。

## 实时无结果或结果很少

**问题描述**

实时推理没有结果，或结果数量很少。

**对系统的影响**

分类结果数量不符合预期。

**可能原因**

网口、抓包权限、镜像方向、输出位置或双向流完整性需要核对。

**操作步骤**

1. 检查网口、抓包权限、镜像方向和输出位置。
2. 确认是否采集到完整双向流。

## 评估结果高于现场效果

**问题描述**

离线评估效果高于现场业务效果。

**对系统的影响**

离线评估可能高估实际分类效果。

**可能原因**

可能存在PCAP、会话或服务器泄漏，或数据与现场流量分布不同。

**操作步骤**

1. 排查PCAP、会话和服务器泄漏。
2. 加入贴近现场的流量、终端和时间段数据。

## PCAP 尾部损坏

**问题描述**

PCAP尾部损坏，但损坏位置之前存在完整报文。

**对系统的影响**

损坏位置之后的数据无法恢复，输出可能只包含此前成功处理的结果。

**可能原因**

输入PCAP尾部数据损坏。

**操作步骤**

1. 核对Pipeline的WARNING；Pipeline会保留此前成功处理的流，并在EOF清理阶段输出已有结果。
2. 训练前替换或重新采集损坏文件。

# 附录

## 术语和缩略语

| 术语或缩略语 | 说明 |
|---|---|
| Capture | 采集报文的公共API层。 |
| Feature | 组流和提取特征的公共API层。 |
| Inference | 加载模型包并执行推理的公共API层。 |
| Schema | 定义特征ID、顺序、维度等内容的特征契约。 |
| Flow Key | 标识流的规范化键。 |

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布。 |
