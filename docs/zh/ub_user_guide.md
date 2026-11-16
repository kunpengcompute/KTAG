<a id="ktag-dpdkub-使用指导文档"></a>

# KTAG DPDK/UB使用指导文档

**文档导航**

- [简介](#简介)
- [快速入门](quick_start.md)
  - [四节点测试流程](#四节点测试流程)
- [安装指南](installation_guide.md)
- [当前版本和测试拓扑](#当前版本和测试拓扑)
  - [版本要求](#版本要求)
  - [目标拓扑](#目标拓扑)
- [环境自检](#环境自检)
- [四节点配置](#四节点配置)
  - [配置文件分发原则](#配置文件分发原则)
  - [UB 配置示例](#ub-配置示例)
  - [是否需要 ground_truth_labels](#是否需要-ground_truth_labels)
- [成功判定](#成功判定)
- [FAQ](#faq)
- [附录](#附录)
  - [测试边界](#测试边界)

---

## 简介

本文用于测试已经完成部署的KTAG DPDK/UB环境，验证3台特征前端和1台推理后端之间的特征传输、共享内存访问、推理和结果落地是否正常。

先完成DPDK/UB、UBS Engine、ubs-mem和OBMM的配套部署。KTAG自身的源码、编译依赖和产物获取见《[安装指南](installation_guide.md#准备源码)》。通用[DPDK安装文档](https://doc.dpdk.org/guides-24.11/linux_gsg/index.html)只说明DPDK基础环境，不替代现场DPDK/UB扩展及UB组件的部署说明。

本文介绍以下验证任务。

- 如何确认服务器具备KTAG所需的DPDK/UB运行环境。
- 如何准备四节点配置。
- 如何启动3:1测试。
- 如何判断测试成功以及定位常见问题。

当前KTAG通过DPDK/UB的raw `lib/ub_ring` 接口传输Feature Frame，使用 `rte_ub_ring_*` 接口。该接口直接提供共享内存缓冲区和队列，匹配“前端提取特征、后端批量推理”的数据通路；本实现不使用UB ethdev PMD，也不建立应用层TCP连接。

Backend先向各Frontend投递CREDIT空缓冲区；Frontend填入特征后发送，Backend接收、推理并归还缓冲区。结束时Frontend发送DRAIN，Backend完成尾批后回复DRAIN_ACK。这些消息用于缓冲区所有权和结束同步；本文不据此宣称相对socket的性能提升数值。

## 当前版本和测试拓扑

### 版本要求

本指南对应以下现场配套；请保持同一套版本，不能仅以“DPDK 24.11”判断普通DPDK包已包含UB扩展。KTAG基础系统、CPU和编译器配套见[版本配套表](release_notes.md#ZH-CN_TOPIC_0000002518245458)。

- DPDK/UB：`DPDK_ub-24.11/DPDK_ub`。
- 运行时库：`librte_ub_ring.so`（24.11对应版本）。
- UB依赖库：`libubsm_sdk.so`。
- KTAG可执行文件：`output/KFlowLens`，必须使用启用DPDK/UB的构建版本。

> ![](public_sys-resources/icon-note.gif) **说明。**
>
> 4台机器必须使用同一套DPDK/UB头文件、`librte_ub_ring.so` 和 `libubsm_sdk.so`。

### 运行约束

| 项目 | 当前实现约束 |
| :--- | :--- |
| 节点 | 多个Provider、唯一User，总节点数不超过256；本文为3:1 |
| 名称 | hostname、YAML和 --node-id 完全一致；Provider名称字典序小于User名称 |
| 配置 | 4台机器保持Provider/User列表、顺序、region、shm_prefix、Schema及规则一致 |
| 队列 | 每Provider支持多个队列；queue_depth为至少2的2次幂；每队列credits为 1..min(queue_depth-1,1023) |
| 执行 | Frontend使用feature_parallel；Backend每个worker独占一个队列，inference.threads必须等于Provider数×queues_per_provider；背压策略必须为block |
| 结果 | 只在Backend落地结果，不回传Frontend；不在运行中单独重启Backend恢复原有会话 |

单机与UB流程的选择如下。

| 项目 | 单机Pipeline | UB分布式 |
| :--- | :--- | :--- |
| 用途 | 单机基线验证或部署 | 已有配套UB环境下，验证多前端向唯一后端传送特征 |
| 数据流 | 同一进程采集、提取、推理并输出 | Frontend采集/提取，Backend接收/推理并输出 |
| 启动切换 | 不传分布式角色参数，使用本机PCAP或网口 | 同一拓扑配置，分别传入角色与匹配的node-id |

移回单机时，关闭YAML中的UB开关并移除分布式角色参数，再核对本地模型及输入；两种方式的性能差异需现场测量。

### 目标拓扑

```text
computer01(Frontend/Provider) ─┐
computer02(Frontend/Provider) ─┼──> controller(Backend/User)
computer03(Frontend/Provider) ─┘          └─ 本地批量推理与结果落地
```

- `computer01`、`computer02`、`computer03`：Frontend/Provider，各自创建并持有自己的共享内存。
- `controller`：Backend/User，一次映射3个Provider的共享内存，接收3个前端的特征并推理。
- 推理结果不返回前端，只在 `controller` 本地输出CSV/JSON和统计结果。
- 后端处理较慢时，空buffer会逐渐耗尽，前端按阻塞策略等待，不主动丢弃Feature。

> ![](public_sys-resources/icon-note.gif) **说明。**
>
> 当前DPDK/UB版本仍需遵守节点名称约束：所有Provider hostname的字典序必须小于唯一User hostname。`computer01/02/03 < controller`，因此可直接用于本指南。4台机器上的 `hostname`、YAML节点列表和 `--node-id` 必须完全一致。

## 环境自检

以下检查在4台机器上都执行。命令只用于确认环境，不涉及安装或修改UB服务。

先取得KTAG源码并进入实际仓库根目录。后文的 `tools/demo/config.yaml`、`output/` 均相对此目录；`/path/to/KTAG` 是占位符，应替换为实际路径，例如 `/opt/KTAG`。

### 检查主机名和节点解析

```bash
# 使用以下命令检查主机名
hostname
hostname -f
getent hosts computer01 computer02 computer03 controller
```

每台机器的 `hostname` 应分别为自己的节点名。若 `getent hosts` 无输出，应先由环境管理员配置管理网DNS或 `/etc/hosts`。KTAG不建立应用级TCP，但UB/ubs-mem仍需要管理网和底层控制面可达。

### 检查 DPDK/UB 版本和接口

```bash
# 使用以下命令检查 DPDK/UB 版本和接口
pkg-config --modversion libdpdk
pkg-config --cflags libdpdk
pkg-config --libs libdpdk | grep rte_ub_ring
```

`pkg-config --modversion` 应显示现场安装的24.11版本；库参数中应包含 `-lrte_ub_ring`。4台机器的版本输出应一致。

### 检查动态库和依赖

```bash
# 使用以下命令检查动态库和依赖
ldconfig -p | grep rte_ub_ring
ls -l /usr/lib64/librte_ub_ring.so*
readlink -f /usr/lib64/librte_ub_ring.so
ldd /usr/lib64/librte_ub_ring.so | grep -E 'ubsm|not found'
find /usr/local/ubs_mem/lib -name 'libubsm_sdk.so*' -print
```

运行KTAG前设置动态库路径，例如。

```bash
# 使用以下命令设置动态库路径
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:/path/to/KTAG/output:${LD_LIBRARY_PATH}
```

上例export在当前Shell及其子进程立即生效，4台机器都需设置，并按本机实际路径调整。需要持久化时可写入 `~/.bashrc`，再执行 `source ~/.bashrc`。使用 `printf '%s\n' "$LD_LIBRARY_PATH"` 查看路径；最终以 `ldd` 无 `not found` 为依赖可解析的判据。

### 检查 KTAG 可执行文件

```bash
# 在仓库根目录启用 UB 传输并构建
KTAG_ENABLE_DPDK_UB=ON bash build.sh
# 使用以下命令检查 KTAG 可执行文件和必需插件
ls -l output/plugins/libktag_traffic_features.so
ldd output/KFlowLens | grep -E 'rte_ub_ring|ubsm_sdk|not found'
output/KFlowLens -h | grep -E 'distributed-role|node-id'
```

应能看到 `librte_ub_ring`，且不能有 `not found`；UB依赖解析结果见[检查动态库和依赖](#检查动态库和依赖)。帮助信息中应包含 `--distributed-role` 和 `--node-id`，但帮助参数本身不能证明已启用UB。`bash build.sh dpdk` 只启用DPDK收包后端，UB传输需独立设置 `KTAG_ENABLE_DPDK_UB=ON`。

## 四节点配置

### 配置文件分发原则

当前仓库的 `tools/demo/config.yaml` 已经包含3:1 UB配置。
`provider_nodes` 为 `computer01/02/03`，`user_nodes` 为 `controller`。
4台机器直接使用这一份文件，不要为每台机器修改Provider/User列表；本机角色通过启动命令中的 `--distributed-role` 和 `--node-id` 指定。

部署配置后在4台机器分别执行。

```bash
# 使用以下命令校验配置文件是否一致
sha256sum tools/demo/config.yaml
```

4个SHA256必须相同。`libktag_traffic_features.so` 是通用流量特征提取插件，应与KFlowLens、公共库配套部署；输入为组流后的报文信息，输出为当前Schema的特征。

### UB 配置示例

下面是3:1 UB测试的核心配置。`tools/demo/config.yaml` 默认兼顾单机使用，启用3:1 UB前须将 `inference.threads` 改为3，并按实际CPU拓扑设置绑核列表。4台机器使用同一份配置。另有多RX、多sender的1:1示例 `tools/demo/config-ub-1to1.yaml`，也已启用普通流量特征插件。

```yaml
pipeline:
  enabled: true
  mode: feature_parallel
  model_package_path: ../../result/legacy_model_package

  feature:
    threads: 4
    queue_capacity: 65536
    lifecycle_mode: legacy

  inference:
    threads: 3
    batch_size: 256
    batch_timeout_us: 1000
    queue_capacity: 4096
    intra_op_threads: 1
    inter_op_threads: 1

  backpressure:
    policy: block

  distributed:
    ub_ring:
      enabled: false
      provider_nodes: [computer01, computer02, computer03]
      user_nodes: [controller]
      region: ktag_ub_region
      shm_prefix: ktag_feature
      provider_shm_size: 268435456
      queues_per_provider: 1
      queue_depth: 1024
      data_element_size: 4096
      credits_per_queue: 64
      backend_worker_cpus: "0-2"
      poll_us: 50
      eal_arguments: [KFlowLens, --no-huge, -l, "0"]

  plugins:
    - ../../output/plugins/libktag_traffic_features.so
  schema:
    name: ktag_legacy_v1
    version: 1
    remove_features: []
    add_features: []
```

Backend的模型包由本项目训练生成：按[训练步骤](user_guide.md#训练)准备应用Schema CSV，再使用同一配置执行 `KFlowLens -t`，产物写入 `pipeline.model_package_path`。也可部署已有且契约一致的交付模型包。示例路径表示部署位置，不表示仓库必然附带预训练模型。

`pipeline.enabled: true` 表示普通单机Pipeline默认启用；`distributed.ub_ring.enabled: false` 表示UB默认关闭。`--distributed-role frontend/backend` 会为本次运行启用对应的UB角色，因此测试命令必须同时指定该参数与 `--node-id`。Frontend要求 `feature_parallel`，Backend线程数为Provider数×每Provider队列数，且显式配置同等数量的 `backend_worker_cpus`。UB场景不能再通过命令行覆盖线程数或Pipeline模式。

`tools/demo/proxy_config.yaml` 当前用于代理特征、训练和单机推理，文件中没有 `pipeline.distributed.ub_ring` 配置，因此本次UB 3:1验证直接使用 `config.yaml`。如果后续要验证代理模型通过UB传输，需要在代理配置中补充同一套 `distributed.ub_ring` 配置，并同时保证Backend的代理模型包和4台机器的Schema配置一致。

关键字段说明。

| 字段 | 说明 |
| --- | --- |
| provider_nodes | 3台Frontend的真实hostname，4台机器必须完全一致且顺序一致 |
| user_nodes | 只能填写一个Backend hostname |
| region、shm_prefix | 4台机器必须一致，用于发现和映射共享内存 |
| provider_shm_size | 每个Provider创建的共享内存，单位字节；默认268435456（256MiB），必须大于0且不超过本机size_t范围；按内存和队列占用配置 |
| queues_per_provider | 每Provider队列数；Backend worker总数为Provider数×该值 |
| queue_depth | Ring深度，必须为2的幂；当前建议 1024 |
| data_element_size | 应用可见Feature Frame最大载荷，单位字节，默认4096；需容纳当前Schema编码帧及元数据，启动时校验 |
| credits_per_queue | Backend为每个队列预投递的空buffer数量，范围1..min(queue_depth-1,1023)，默认64；旧credits_per_provider键已停用 |
| backend_worker_cpus | Backend逐worker绑核的CPU列表，数量必须等于inference.threads，例如"0-2" |
| sender_threads / sender_worker_cpus | Frontend发送线程数量及可选绑核列表；sender不超过每Provider队列数，各sender独占分配给自己的队列 |
| backpressure.policy | 必须为 block，过载时阻塞，不主动丢弃Feature |
| inference.threads | 必须在YAML中显式配置为Provider数×queues_per_provider；每个worker独占队列和推理Context |
| model_package_path | Backend必须能访问匹配当前Schema的模型包；Frontend只做特征提取 |
| poll_us | 空轮询退避间隔，单位微秒，默认50；uint32范围，0时让出线程调度，不表示超时时间 |
| eal_arguments | 有序字符串列表；省略时为 [KFlowLens, --no-huge, -l, "0"]，首项为程序名； --no-huge 是此UB传输初始化的默认参数，不代表DPDK收包后端无需hugepage；现场调整需遵守配套DPDK/UB要求 |
| feature.threads / feature.queue_capacity | 默认4 / 65536，分别为Feature线程数 / 报文队列元素数，均为正uint32；按实际CPU和内存选取 |
| inference.queue_capacity | Feature记录队列容量，单位为记录数，默认4096，正uint32 |
| inference.batch_size / batch_timeout_us | 默认256条 / 1000微秒，均为正uint32；后者为批量等待时间，不是建链超时 |
| inference.intra_op_threads / inter_op_threads | 单个ONNX Runtime Session内部算子内 / 算子间线程数，默认均为1；0使用ONNX Runtime默认，非零值不超过2147483647；不是Backend的Inference Worker数量 |
| feature.lifecycle_mode | legacy（默认）保持历史流结束语义；modern 启用超时及UDP上限；前后端及训练应使用一致契约 |
| schema.name / schema.version | 本例为 ktag_legacy_v1 / 1；代理使用 ktag_proxy_v1 / 1，须与模型包匹配 |
| schema.remove_features / add_features | Feature ID列表，默认均为空；正式Schema的允许组合见[Schema配置](user_guide.md#配置文件)，不能随意增删后复用旧模型 |

本节字段均位于 `pipeline` 下；UB专用字段位于 `pipeline.distributed.ub_ring`。uint32的解析上限为4294967295，实际取值还受上表约束和资源条件限制。UB Frontend只允许 `feature_parallel`，单机模式的完整取值见[模式表](user_guide.md#pipeline-mode-与线程配置)。

### 是否需要 `ground_truth_labels`

如果只是验证特征传输和基于已有模型的推理，不需要训练，也可以不配置 `ground_truth_labels`。此时每个 `-p` 文件仍携带source index用于来源关联，但不会被当作真实类别参与监督统计；Backend会正常推理并落地结果。

只有在需要给PCAP指定真实类别、生成带监督统计的结果时才配置它。该字段放在 `pipeline.distributed.ub_ring` 下，必须覆盖全部Provider；每个Provider的数组长度必须等于该Provider启动命令中的 `-p` 数量，例如。

```yaml
ground_truth_labels:
  computer01: [0, 1]
  computer02: [0, 1]
  computer03: [0, 1]
```

这表示每台Frontend都传入两个PCAP，且两个PCAP的真实标签分别为0和1。标签配置不是UB建链的必要条件。

## 四节点测试流程

### 测试前准备

1. 4台机器部署相同版本的 `output/KFlowLens`、三层公共库、Traffic插件和配置文件，保留相对布局。
2. 调整复制后的插件、模型包路径，并按[配置文件分发原则](#配置文件分发原则)核对一致性。
3. 3台Frontend各自准备PCAP；PCAP不要求内容相同，建议先使用已经通过单机基线验证的数据。
4. Backend准备与YAML Schema匹配的模型包。
5. 停止上一轮遗留的 `KFlowLens` 进程。不要在进程仍运行时重复启动同一套 `region`/`shm_prefix`。
6. 在4台机器设置 `LD_LIBRARY_PATH`。
7. 分别检查 `ldd output/KFlowLens`，确认无 `not found`。

停止旧任务前，可用 `ps -eo pid,args | grep '[k]tag_demo'` 核对本轮任务PID，再以 `kill -INT PID` 请求停止（将PID替换为对应任务的实际值）。用 `ps -p PID -o pid,args` 确认退出；不要按进程名批量终止其他任务。UB等待循环可能无法立即响应，未退出时先保留日志，由现场管理员协调本轮全部节点停止。

### 启动3个 Frontend

建议先启动3个Frontend，再启动Backend。Frontend会创建自己的Provider共享内存并等待Backend投递CREDIT；Backend会等待所有Provider共享内存就绪。

在 `computer01`。

```bash
# 在 computer01 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/KFlowLens \
  -r tools/demo/config.yaml \
  -p /data/computer01/class0.pcap \
  -p /data/computer01/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

在 `computer02`。

```bash
# 在 computer02 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/KFlowLens \
  -r tools/demo/config.yaml \
  -p /data/computer02/class0.pcap \
  -p /data/computer02/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

在 `computer03`。

```bash
# 在 computer03 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/KFlowLens \
  -r tools/demo/config.yaml \
  -p /data/computer03/class0.pcap \
  -p /data/computer03/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

> ![](public_sys-resources/icon-note.gif) **说明。**
>
> 启动Backend前，在各Frontend另一个终端执行 `ps -eo pid,args | grep '[k]tag_demo'`，确认本轮进程仍在且没有初始化错误。此时可能仍在等待底层共享内存或CREDIT，尚未出现 `UB Frontend ready` 属正常现象；该日志在收到首个CREDIT后才输出。
>
> 每个Frontend的 `--node-id` 必须与本机 `hostname` 完全一致。`-p` 可以重复使用；如果使用实时网口采集，可将多个 `-p` 替换为 `-i <interface>`。接口名用 `output/KFlowLens -l` 列出，保持名称大小写并准备抓包权限。此处只切换采集输入，UB拓扑不变；长期网口采集需持续看管背压和资源，停止时先请求Frontend结束，再等待Backend尾批完成。DPDK收包还需单独启用并配置其设备。

### 启动 Backend

确认3个Frontend已经启动并开始等待后，在 `controller` 执行。

```bash
# 在 controller 上启动 Backend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}
mkdir -p /data/result

output/KFlowLens \
  -r tools/demo/config.yaml \
  --distributed-role backend \
  --node-id "$(hostname)" \
  -c /data/result/ub-3to1.csv
```

启动后的即时判据：Backend显示 `UB Backend ready: providers=3` 并列出3个正确hostname，且各Frontend均显示 `UB Frontend ready: node=...`。不同节点日志的先后顺序不作为判据。完整字段见[成功判定](#成功判定)。如果某一端始终等待，先核对全部节点进程、配套版本、配置SHA256和Schema/模型包。

Backend不需要 `-p` 或 `-i`。它只从3个Provider的UB Ring接收Feature，按 `batch_size`/`batch_timeout_us` 进行本地推理，并将结果写入 `ub-3to1.csv`。

PCAP处理完成后，Frontend会发送DRAIN，Backend等待3个Provider都完成尾批推理，再发送DRAIN_ACK。正常情况下4个进程都会自行退出；实时网口模式下应先停止Frontend，再等待Backend完成尾批处理。

## 成功判定

### Frontend 日志

每台Frontend都应出现类似信息。

```text
UB Frontend ready: node=computer0X, ...
UB Frontend drained: features=..., send_retries=..., empty_polls=...
```

`features` 表示该Frontend成功发送的Feature数量。`send_retries` 或 `empty_polls` 非零不一定是错误，过载测试中可能增加；不能出现初始化失败、协议校验失败或进程崩溃。

### Backend 日志

Backend应出现。

```text
UB Backend ready: providers=3, credits/provider=256, ...
  Provider: hostname=computer01, ...
  Provider: hostname=computer02, ...
  Provider: hostname=computer03, ...
UB Backend drained: features=..., batches=..., ...
  Provider drained: hostname=computer01, features=..., last_sequence=...
  Provider drained: hostname=computer02, features=..., last_sequence=...
  Provider drained: hostname=computer03, features=..., last_sequence=...
```

### 结果文件和计数

```bash
# 使用以下命令检查结果文件行数和内容
wc -l /data/result/ub-3to1.csv
head -n 5 /data/result/ub-3to1.csv
```

验收至少确认。

- 3台Frontend均出现ready和drained。
- Backend识别到 `providers=3`，且列出3个正确hostname。
- 每个Provider的 `features` 与对应Frontend的发送数量能够对账。
- Backend生成非空CSV/JSON，推理结果和分类统计符合预期。
- 没有 `not found`、Schema/ruleset mismatch、队列异常、进程崩溃或主动丢弃Feature的错误日志。

## FAQ

### 等待、背压与恢复

KTAG自身的CREDIT、空buffer及发送重试循环没有配置固定秒数的等待超时；`poll_us` 只控制退避。底层 `rte_ub_ring_provider_init` / `user_init` 的超时和共享内存清理由现场配套库决定，不能把长时间等待一概判断为已丢包。

先检查Backend是否仍在处理及计数是否增长；Backend较慢时，Frontend等待是block背压的正常表现。确认瓶颈后可调优Backend的模型内部线程或批量参数；如调整credits，须保持 `credits < queue_depth` 且不超过1023。本文不提供未经验证的性能最优值。

进程异常后，保留4台日志和计数，协调停止本轮全部节点，确认无残留进程后再按现场UB/ubs-mem运维规程清理残留资源并整组重启。不要在已有Frontend运行时只重启Backend继续原会话；当前协议会校验本轮epoch。恢复前后的结果需分开验收。

### 现象与检查项

| 现象 | 优先检查 |
| --- | --- |
| libubsm_sdk.so => not found | LD_LIBRARY_PATH、ldconfig -p、ldd /usr/lib64/librte_ub_ring.so |
| librte_ub_ring.so 不存在 | ldconfig -p、pkg-config --libs libdpdk，确认实际安装的是24.11运行库 |
| Backend初始化超时或持续等待Provider | 3台Frontend是否都启动、hostname是否可解析、4台 region/shm_prefix 是否一致 |
| --node-id does not belong... 或hostname错误 | hostname、provider_nodes/user_nodes 和 --node-id 是否完全匹配 |
| UB hostname order invalid | 错误中列出的Provider名称必须按区分大小写的字典序小于User名称；核对真实hostname和拓扑配置 |
| Schema/ruleset/hash不一致 | 4台配置SHA256、模型包、Schema、规则配置是否一致 |
| Frontend长时间等待CREDIT | Backend是否已启动，Provider/User列表顺序是否一致，是否有旧进程占用同一共享内存 |
| 只启动1个或2个Provider | Backend会等待所有3个Provider；检查3台Frontend的日志和进程状态 |
| 过载时吞吐下降、Frontend等待 | 属于 block 背压的预期表现；应确认未主动丢弃，并在正常结束后确认最终计数一致 |

如果怀疑仍在运行旧二进制，先检查构建产物、插件和依赖，再重新部署4台机器。

## 附录

### 测试边界

- 本测试验证的是“Frontend特征提取 → UB共享内存 → Backend接收 → 推理 → Backend落地”的链路，不验证模型训练过程。
- 默认使用已有模型包；不配置 `ground_truth_labels` 也可以完成链路验证。
- 本指南使用3个Provider、1个User；代码支持多个Provider与唯一User，节点总数不超过256。支持每Provider多队列及Backend多worker独占队列推理。
- Backend不把推理结果发送回Frontend。
- 过载时允许阻塞，本文不把“处理速度下降”判定为UB丢包；需要重点区分主动丢弃、底层网卡丢包和正常背压等待。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布。 |
