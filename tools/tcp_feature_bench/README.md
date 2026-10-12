# TCP 特征提取、压缩与推理用例

两台 Linux 服务器共用一条普通 100G 以太网直连链路：后端保存 PCAP 并打流，前端实时收包、提取、压缩，通过 TCP 回传后端解压和推理。不依赖 DPDK/UB。

```text
后端 PCAP → tcpreplay → 原始以太网报文 → 前端 libpcap
前端实时收包 → 4 个按流分配的特征 worker → 组批 / Zstd 压缩
前端 TCP 回传 → 后端接收 / 解压 / 校验 → KTAG 推理
```

**实现性能计时和吞吐统计，不设性能达标门槛。** 10:1 是目标，记录实际压缩比；压缩降低吞吐不属于功能失败。成功要求输入包数、传输特征数和推理计数闭合，没有已知抓包丢失或协议错误。

## 构建

两端需要现有 KTAG 环境：ONNX Runtime、yaml-cpp、libpcap，以及新增的 libzstd 开发包（Zstd 1.4.0 或更新）。后端另需 tcpreplay，可选 taskset。本实现用 libpcap 生成仅替换两处 MAC 的临时回放文件，无需 tcprewrite。

Debian/Ubuntu 开发依赖示例：

```bash
sudo apt-get install cmake g++ libpcap-dev libzstd-dev libyaml-cpp-dev tcpreplay
```

在 KTAG 根目录构建，ONNX 路径使用现场已有安装：

```bash
cmake -S . -B build/tcp-bench \
  -DONNXRUNTIME_ROOT=/opt/onnxruntime \
  -DKTAG_BUILD_TOOLS=OFF -DKTAG_BUILD_TCP_FEATURE_BENCH=ON \
  -DKTAG_BUILD_EXAMPLES=OFF -DKTAG_BUILD_PROXY_FEATURES=OFF \
  -DKTAG_ENABLE_DPDK=OFF -DKTAG_ENABLE_DPDK_UB=OFF
cmake --build build/tcp-bench --target ktag_tcp_bench ktag_tcp_wire_test ktag_tcp_config_test ktag_tcp_transport_test -j
ctest --test-dir build/tcp-bench/tools/tcp_feature_bench --output-on-failure
```

产物为 `output/ktag_tcp_bench`。开关默认关闭，链接现代 capture/feature/inference API，复用旧 demo 的纯二进制帧编码。前后端采用同一可执行文件，前端不需要模型包，但仍需可加载 ONNX Runtime 动态库。

## 数据和网卡

- PCAP 和现有 UB 模型包放在后端。首版采用 `ktag_legacy_v1`、354 个 float32 值、内置 Provider；自定义 Schema、增删特征及插件明确拒绝。
- `--config` 支持本目录 YAML，也支持现有 `tools/demo/config-ub-1to1.yaml` 的特征、Schema 和推理参数。配置中的模型相对路径按配置文件目录解析；CLI 路径按当前工作目录解析。两端特征参数、Zstd level 和 body 上限必须一致。
- 两端直连网口分别配置 IPv4，如前端 `192.168.100.1/24`、后端 `192.168.100.2/24`。绑定 IP 必须属于指定以太网接口。首版特征连接使用 IPv4，分析报文可包含 IPv4/IPv6。
- PCAP 必须是完整以太网帧，拒绝截断、RAW/SLL、超出接口 MTU 或快照长度的记录。含 FCS、GSO 大包等捕获结果需要先核对是否能作为真实线上帧回放。
- PCAP 的两个业务方向均从后端同一网口发出，前端被动分析，不与数据中的业务重新建立 TCP 会话。
- 默认回放 MAC 为 `02:00:00:90:00:01` 和 `02:00:00:90:00:02`，必须未占用且不同于两台真实网卡 MAC。前端混杂抓包只接受这对 MAC，排除特征连接的 ACK、控制和地址解析。派生文件只改 MAC，保留原时间戳、长度、IP/端口和载荷；原文件不修改，私有 `/tmp/ktag-tcp-replay-*` 在退出时清理。
- 确认 NIC 支持混杂接收，VLAN、GRO/LRO、VLAN stripping 等卸载没有改变逐包捕获。记录驱动、MTU、速率和卸载设置，程序不自动修改网卡配置。
- 抓包及原始发包需要系统权限，下面使用 sudo；也可按现场权限策略配置。

前端使用实际捕获时间戳，原 PCAP 时间戳只用于回放节奏，所以包间隔等时间特征可能不同于离线提取。**无损要求针对前端实际提取特征与后端恢复特征逐位一致。**

## 启动

两端创建输出目录；后端可先检查 PCAP，无需网络和模型：

```bash
mkdir -p /data/result
output/ktag_tcp_bench --role backend --prepare-only \
  --config tools/tcp_feature_bench/config.yaml \
  -p /data/class0.pcap -p /data/class1.pcap
```

加 `--replay-interface` 可在预检时核对现场 MTU。

后端先启动；接口、IP、模型和数据路径按现场填写：

```bash
sudo output/ktag_tcp_bench --role backend \
  --config tools/tcp_feature_bench/config.yaml \
  --bind-ip 192.168.100.2 --port 19090 \
  --replay-interface enp1s0f0 --model /data/legacy_model_package \
  -p /data/class0.pcap -p /data/class1.pcap \
  --result /data/result/tcp-zstd.csv --stats /data/result/backend.json
```

前端：

```bash
sudo output/ktag_tcp_bench --role frontend \
  --config tools/tcp_feature_bench/config.yaml \
  --local-ip 192.168.100.1 --connect 192.168.100.2:19090 \
  --capture-interface enp1s0f0 --stats /data/result/frontend.json
```

默认按 PCAP 原时间间隔回放。后端添加 `--replay-mbps 1000` 可指定 1000 Mbps，实际速率如实记录，不保证打满 100G；添加 `--replay-cpus 8` 可给打流子进程指定 CPU，主程序可通过 taskset 启动。CPU 编号按现场拓扑和 cpuset 配置。

双方自动协调打流，无需手动启动 tcpreplay：

1. 后端准备数据及模型，握手核对特征和压缩契约。
2. 每文件前端创建 Context、打开抓包并就绪后，后端开始打流。
3. 打流、前端提取/回传、后端推理并行。前端 1 采集 + 4 特征 + 1 输出工作线程，主线程接收控制；后端主线程接收/解压/推理，另有回放协调线程和子进程。
4. 后端宣布回放退出，前端继续等待尾包；核对数量、排空捕获缓冲及静默期后，排空队列、flush 特征、发尾批。后端完成该文件推理才允许下一文件。
5. 最终 END/DONE 核对所有计数，前端结束核心计时；后端随后输出 CSV，双方输出摘要及可选 JSON。

包队列容量是每个特征 worker 的容量，默认每队列 65536；特征输出队列默认 4096。队列满时阻塞，但原始回放没有 TCP 背压，过快打流仍可能导致 NIC/libpcap 丢包。缺包、多包、捕获丢包、压缩损坏、序号错误、断线及超时返回非零退出码，JSON 状态为 `failed`，不输出成功吞吐。

`progress_timeout_s` 用于建连、控制等待及通信进度，长间隔 PCAP 按需调大。`drain_timeout_s` 是宣布回放结束后的尾包等待上限；`quiet_period_ms` 默认 100 毫秒，计入主耗时，只是辅助收尾条件。SIGINT/SIGTERM 会结束会话、唤醒队列并停止/回收打流子进程。

双方至少每秒发送一次轻量 PROGRESS 心跳（小超时配置时更频繁），避免长时间回放或 Legacy UDP 只在文件结尾输出特征导致误判断线。前端心跳包含实收包数；后端心跳只表示协调器/接收循环仍存活，不宣称实时发送数量。心跳不表示输入无丢包，其应用字节和开销计入统计；单次推理调用若长于超时，仍需调大超时参数。

后端预测暂存内存，上限 1,000,000 条，超过明确失败。CSV 的 `src_ip/dst_ip` 是规范流键 A/B 地址；`source_index` 对应后端 `-p` 文件顺序，不自动解释为类别标签。输出目录需预先存在。

## 性能与正确性

前端 JSON 是核心输入吞吐主结果：

```text
core_ns = 首次特征 process 调用之前 → 前端收到并验证最终 DONE
input_gbit_s = 前端实收 captured bytes × 8 / core seconds / 10^9
records_s = 后端提交推理特征数 / core seconds
compression_ratio = 完整内部特征帧字节 / Zstd body 字节
compression_ratio_socket = 内部特征帧字节 / 双向 TCP 应用消息字节
```

主区间包含输入等待、组批、压缩、传输、背压、解压、推理、文件切换和收尾；模型初始化与最终 CSV 落盘另记。后端 `core_ns` 是首批特征接收完到最后一次推理完成，与前端不同。各阶段调用累计墙钟耗时可重叠，不能相加当作总时间。计时无效或运行失败时吞吐字段为 `null`。

统计还包括实发/实收包数、提取/解码/推理/过滤/预测数、阶段耗时、队列等待、进程及工作线程 CPU、回放子进程 CPU、RSS，以及整个运行的 NIC 收发/丢包/错误差值。NIC 计数包含该接口所有流量，读取不到时为 `null`。`replay_actual_mbps` 包含子进程启动和读文件等成本。主进程 CPU 包含初始化及输出，与回放子进程分开报告。

压缩比包含帧头及元数据，另报 `pure_float32_bytes`，不能据此宣称纯特征矩阵本身达到 10:1。暂未提供逐 TCP 连接重传统计；模型文件校验值、NUMA/CPU 亲和性及卸载设置可随现场记录保存。

独立正确性运行，两端分别添加 `--feature-audit /data/result/frontend.frames` 和 `--feature-audit /data/result/backend.frames`。把两文件复制到同一台机器核对：

```bash
cmp frontend.frames backend.frames
sha256sum frontend.frames backend.frames
```

两个文件是同一有序特征流压缩前/解压后的原始字节，应该完全相同。记录 CRC 和 Zstd checksum 默认始终校验。audit 写入增加开销，JSON 标记 `audit_enabled`，性能运行可关闭。

本地已完成所有新增 C++ 源码和测试源码的 Linux/ARM64 对象编译，以及真实 Zstd/特征帧、队列取消和 YAML 配置解析测试，覆盖 256 条记录、特殊 float 位模式、损坏数据、畸形消息和错误配置。尚未完成 Linux 完整链接构建；真实双机打流、现场模型推理及完整闭环仍需在目标 Linux 服务器验证。

测试集中还提供 Linux socketpair 收发/取消测试和 PCAP 预检测试；这两项需要目标 Linux 环境运行，本开发机未执行，不能计入已通过项目。
