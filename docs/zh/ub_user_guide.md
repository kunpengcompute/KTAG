# KTAG DPDK/UB 使用指导文档

## 1. 文档范围

本文用于测试已经完成部署的 KTAG DPDK/UB 环境，验证三台特征前端和一台推理后端之间的特征传输、共享内存访问、推理和结果落地是否正常。

本文不介绍 DPDK、UBS Engine、ubs-mem 或 OBMM 的安装部署，只介绍：

- 如何确认服务器具备 KTAG 所需的 DPDK/UB 运行环境；
- 如何准备四节点配置；
- 如何启动 3:1 测试；
- 如何判断测试成功以及定位常见问题。

当前 KTAG 通过 DPDK/UB 的 raw `lib/ub_ring` 接口传输数据，使用 `rte_ub_ring_*` 接口，不使用 `drivers/net/ub_ring` ethdev PMD，也不建立 KTAG 应用层 TCP 连接。

## 2. 当前版本和测试拓扑

### 2.1 版本要求

KTAG UB 适配按以下版本验证：

- DPDK/UB：`DPDK_ub-24.11/DPDK_ub`；
- 运行时库：`librte_ub_ring.so`（24.11 对应版本）；
- UB 依赖库：`libubsm_sdk.so`；
- KTAG 可执行文件：`output/ktag_demo`，必须使用启用 DPDK/UB 的构建版本。

>![](public_sys-resources/icon-note.gif) **说明：** 
>
>四台机器必须使用同一套 DPDK/UB 头文件、`librte_ub_ring.so` 和 `libubsm_sdk.so`。

### 2.2 目标拓扑

```text
computer01（Frontend/Provider） ─┐
computer02（Frontend/Provider） ─┼──> controller（Backend/User）
computer03（Frontend/Provider） ─┘          └─ 本地批量推理与结果落地
```

- `computer01`、`computer02`、`computer03`：Frontend/Provider，各自创建并持有自己的共享内存；
- `controller`：Backend/User，一次映射三个 Provider 的共享内存，接收三个前端的特征并推理；
- 推理结果不返回前端，只在 `controller` 本地输出 CSV/JSON 和统计结果；
- 后端处理较慢时，空 buffer 会逐渐耗尽，前端按阻塞策略等待，不主动丢弃 Feature。

>![](public_sys-resources/icon-note.gif) **说明：** 
>
>当前 DPDK/UB 版本仍需遵守节点名称约束：所有 Provider hostname 的字典序必须小于唯一 User hostname。`computer01/02/03 < controller`，因此可直接用于本指南。四台机器上的 `hostname`、YAML 节点列表和 `--node-id` 必须完全一致。

## 3. 环境自检

以下检查在四台机器上都执行。命令只用于确认环境，不涉及安装或修改 UB 服务。

### 3.1 检查主机名和节点解析

```bash
# 使用以下命令检查主机名
hostname
hostname -f
getent hosts computer01 computer02 computer03 controller
```

每台机器的 `hostname` 应分别为自己的节点名。若 `getent hosts` 无输出，应先由环境管理员配置管理网 DNS 或 `/etc/hosts`。KTAG 不建立应用级 TCP，但 UB/ubs-mem 仍需要管理网和底层控制面可达。

### 3.2 检查 DPDK/UB 版本和接口

```bash
# 使用以下命令检查 DPDK/UB 版本和接口
pkg-config --modversion libdpdk
pkg-config --cflags libdpdk
pkg-config --libs libdpdk | grep rte_ub_ring
```

`pkg-config --modversion` 应显示现场安装的 24.11 版本；库参数中应包含 `-lrte_ub_ring`。四台机器的版本输出应一致。

### 3.3 检查动态库和依赖

```bash
# 使用以下命令检查动态库和依赖
ldconfig -p | grep rte_ub_ring
ls -l /usr/lib64/librte_ub_ring.so*
readlink -f /usr/lib64/librte_ub_ring.so
ldd /usr/lib64/librte_ub_ring.so | grep -E 'ubsm|not found'
find /usr/local/ubs_mem/lib -name 'libubsm_sdk.so*' -print
```

运行 KTAG 前设置动态库路径，例如：

```bash
# 使用以下命令设置动态库路径
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:/path/to/KTAG/output:${LD_LIBRARY_PATH}
```

### 3.4 检查 KTAG 可执行文件

```bash
# 在仓库根目录启用 UB 传输并构建
KTAG_ENABLE_DPDK_UB=ON sh build.sh
# 使用以下命令检查 KTAG 可执行文件和必需插件
ls -l output/plugins/libktag_traffic_features.so
ldd output/ktag_demo | grep -E 'rte_ub_ring|ubsm_sdk|not found'
output/ktag_demo -h | grep -E 'distributed-role|node-id'
```

应能看到 `librte_ub_ring`，且不能有 `not found`；其 UB 依赖还应结合第 3.3 节检查。帮助信息中应包含 `--distributed-role` 和 `--node-id`，但帮助参数本身不能证明已启用 UB。`sh build.sh dpdk` 只启用 DPDK 收包后端，UB 传输需独立设置 `KTAG_ENABLE_DPDK_UB=ON`。

## 4. 四节点配置

### 4.1 配置文件分发原则

当前仓库的 `tools/demo/config.yaml` 已经包含 3:1 UB 配置：
`provider_nodes` 为 `computer01/02/03`，`user_nodes` 为 `controller`。
四台机器直接使用这一份文件，不要为每台机器修改 Provider/User 列表；本机角色通过启动命令中的 `--distributed-role` 和 `--node-id` 指定。

部署配置后在四台机器分别执行：

```bash
# 使用以下命令校验配置文件是否一致
sha256sum tools/demo/config.yaml
```

四个 SHA256 必须相同。

### 4.2 UB 配置示例

下面是 `tools/demo/config.yaml` 中与 3:1 UB 测试相关的核心配置。仓库中的文件已经包含这些内容，测试时只需要确认节点名称与现场 hostname 一致：

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
    threads: 1
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
      credits_per_provider: 256
      poll_us: 50
      eal_arguments: [ktag_demo, --no-huge, -l, "0"]

  plugins:
    - ../../output/plugins/libktag_traffic_features.so
  schema:
    name: ktag_legacy_v1
    version: 1
    remove_features: []
    add_features: []
```

`pipeline.enabled: true` 表示普通单机 Pipeline 默认启用；`distributed.ub_ring.enabled: false` 表示 UB 默认关闭。`--distributed-role frontend/backend` 会为本次运行启用对应的 UB 角色，因此测试命令必须同时指定该参数与 `--node-id`。Frontend 当前要求 `feature_parallel`，Backend 要求一个推理线程。

`tools/demo/proxy_config.yaml` 当前用于代理特征、训练和单机推理，文件中没有 `pipeline.distributed.ub_ring` 配置，因此本次 UB 3:1 验证直接使用 `config.yaml`。如果后续要验证代理模型通过 UB 传输，需要在代理配置中补充同一套 `distributed.ub_ring` 配置，并同时保证 Backend 的代理模型包和四台机器的 Schema 配置一致。

关键字段说明：

| 字段 | 说明 |
|---|---|
| `provider_nodes` | 三台 Frontend 的真实 hostname，四台机器必须完全一致且顺序一致 |
| `user_nodes` | 只能填写一个 Backend hostname |
| `region`、`shm_prefix` | 四台机器必须一致，用于发现和映射共享内存 |
| `provider_shm_size` | 每个 Provider 创建的共享内存大小，四台配置保持一致 |
| `queues_per_provider` | 当前实现固定为 `1` |
| `queue_depth` | Ring 深度，必须为 2 的幂；当前建议 `1024` |
| `data_element_size` | 应用可见 Feature Frame 最大载荷，需容纳当前 Schema 的编码帧（含元数据） |
| `credits_per_provider` | Backend 为每个 Provider 预投递的空 buffer 数量，必须小于 `queue_depth`；当前建议 `256` |
| `backpressure.policy` | 必须为 `block`，过载时阻塞，不主动丢弃 Feature |
| `inference.threads` | 当前 Backend 实现固定为 `1` |
| `model_package_path` | Backend 必须能访问匹配当前 Schema 的模型包；Frontend 只做特征提取，不执行推理 |

### 4.3 是否需要 `ground_truth_labels`

如果只是验证特征传输和基于已有模型的推理，不需要训练，也可以不配置 `ground_truth_labels`。此时每个 `-p` 文件仍携带 source index 用于来源关联，但不会被当作真实类别参与监督统计；Backend 会正常推理并落地结果。

只有在需要给 PCAP 指定真实类别、生成带监督统计的结果时才配置它。该字段放在 `pipeline.distributed.ub_ring` 下，必须覆盖全部 Provider；每个 Provider 的数组长度必须等于该 Provider 启动命令中的 `-p` 数量，例如：

```yaml
ground_truth_labels:
  computer01: [0, 1]
  computer02: [0, 1]
  computer03: [0, 1]
```

这表示每台 Frontend 都传入两个 PCAP，且两个 PCAP 的真实标签分别为 0 和 1。标签配置不是 UB 建链的必要条件。

## 5. 四节点测试流程

### 5.1 测试前准备

1. 四台机器部署相同版本的 `output/ktag_demo`、三层公共库、`output/plugins/libktag_traffic_features.so` 和配置文件；保留相对布局，复制配置后调整插件和模型包路径。
2. 三台 Frontend 各自准备 PCAP；PCAP 不要求内容相同，建议先使用已经通过单机基线验证的数据。
3. Backend 准备与 YAML Schema 匹配的模型包。
4. 停止上一轮遗留的 `ktag_demo` 进程。不要在进程仍运行时重复启动同一套 `region`/`shm_prefix`。
5. 四台机器均设置 `LD_LIBRARY_PATH`，并确认 `ldd` 无 `not found`。

### 5.2 启动三个 Frontend

建议先启动三个 Frontend，再启动 Backend。Frontend 会创建自己的 Provider 共享内存并等待 Backend 投递 CREDIT；Backend 会等待所有 Provider 共享内存就绪。

在 `computer01`：

```bash
# 在 computer01 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/ktag_demo \
  -r tools/demo/config.yaml \
  -p /data/computer01/class0.pcap \
  -p /data/computer01/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

在 `computer02`：

```bash
# 在 computer02 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/ktag_demo \
  -r tools/demo/config.yaml \
  -p /data/computer02/class0.pcap \
  -p /data/computer02/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

在 `computer03`：

```bash
# 在 computer03 上启动 Frontend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}

output/ktag_demo \
  -r tools/demo/config.yaml \
  -p /data/computer03/class0.pcap \
  -p /data/computer03/class1.pcap \
  --distributed-role frontend \
  --node-id "$(hostname)"
```

>![](public_sys-resources/icon-note.gif) **说明：** 
>
>每个 Frontend 的 `--node-id` 必须与本机 `hostname` 完全一致。`-p` 可以重复使用；如果使用实时网口采集，可将多个 `-p` 替换为 `-i <interface>`。

### 5.3 启动 Backend

确认三个 Frontend 已经启动并开始等待后，在 `controller` 执行：

```bash
# 在 controller 上启动 Backend 进程
cd /path/to/KTAG
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:$PWD/output:${LD_LIBRARY_PATH}
mkdir -p /data/result

output/ktag_demo \
  -r tools/demo/config.yaml \
  --distributed-role backend \
  --node-id "$(hostname)" \
  -c /data/result/ub-3to1.csv
```

Backend 不需要 `-p` 或 `-i`。它只从三个 Provider 的 UB Ring 接收 Feature，按 `batch_size`/`batch_timeout_us` 进行本地推理，并将结果写入 `ub-3to1.csv`。

PCAP 处理完成后，Frontend 会发送 DRAIN，Backend 等待三个 Provider 都完成尾批推理，再发送 DRAIN_ACK。正常情况下四个进程都会自行退出；实时网口模式下应先停止 Frontend，再等待 Backend 完成尾批处理。

## 6. 成功判定

### 6.1 Frontend 日志

每台 Frontend 都应出现类似信息：

```text
UB Frontend ready: node=computer0X, ...
UB Frontend drained: features=..., send_retries=..., empty_polls=...
```

`features` 表示该 Frontend 成功发送的 Feature 数量。`send_retries` 或 `empty_polls` 非零不一定是错误，过载测试中可能增加；不能出现初始化失败、协议校验失败或进程崩溃。

### 6.2 Backend 日志

Backend 应出现：

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

### 6.3 结果文件和计数

```bash
# 使用以下命令检查结果文件行数和内容
wc -l /data/result/ub-3to1.csv
head -n 5 /data/result/ub-3to1.csv
```

验收至少确认：

- 三台 Frontend 均出现 ready 和 drained；
- Backend 识别到 `providers=3`，且列出三个正确 hostname；
- 每个 Provider 的 `features` 与对应 Frontend 的发送数量能够对账；
- Backend 生成非空 CSV/JSON，推理结果和分类统计符合预期；
- 没有 `not found`、Schema/ruleset mismatch、队列异常、进程崩溃或主动丢弃 Feature 的错误日志。

## 7. 常见问题定位

| 现象 | 优先检查 |
|---|---|
| `libubsm_sdk.so => not found` | `LD_LIBRARY_PATH`、`ldconfig -p`、`ldd /usr/lib64/librte_ub_ring.so` |
| `librte_ub_ring.so` 不存在 | `ldconfig -p`、`pkg-config --libs libdpdk`，确认实际安装的是 24.11 运行库 |
| Backend 等待 Provider 超时 | 三台 Frontend 是否都启动、hostname 是否可解析、四台 `region`/`shm_prefix` 是否一致 |
| `--node-id does not belong...` 或 hostname 错误 | `hostname`、`provider_nodes`/`user_nodes` 和 `--node-id` 是否完全匹配 |
| Schema/ruleset/hash 不一致 | 四台配置 SHA256、模型包、Schema、规则配置是否一致 |
| Frontend 长时间等待 CREDIT | Backend 是否已启动，Provider/User 列表顺序是否一致，是否有旧进程占用同一共享内存 |
| 只启动一个或两个 Provider | Backend 会等待所有三个 Provider；检查三台 Frontend 的日志和进程状态 |
| 过载时吞吐下降、Frontend 等待 | 属于 `block` 背压的预期表现；应确认没有主动丢弃和最终计数不一致 |

如果怀疑仍在运行旧二进制，先检查构建产物、插件和依赖，再重新部署四台机器。

## 8. 测试边界

- 本测试验证的是“Frontend 特征提取 → UB 共享内存 → Backend 接收 → 推理 → Backend 落地”的链路，不验证模型训练过程。
- 默认使用已有模型包；不配置 `ground_truth_labels` 也可以完成链路验证。
- 本指南使用 3 个 Provider、1 个 User；代码支持多个 Provider 与唯一 User，节点总数不超过 256。当前每个 Provider 固定 1 个队列、Backend 固定 1 个推理线程。
- Backend 不把推理结果发送回 Frontend。
- 过载时允许阻塞，本文不把“处理速度下降”判定为 UB 丢包；需要重点区分主动丢弃、底层网卡丢包和正常背压等待。
