# 前端本地 PCAP → Zstd → TCP → 后端推理

PCAP 放前端，模型包放后端；前端不执行推理，后端不读取输入 PCAP、不打流。设计说明见 [本地 PCAP TCP 压缩用例设计](../../docs/zh/tcp_pcap_bench_design.md)。

```text
前端本地 PCAP → owned 报文池 → 4 个按流分配的 Feature worker
             → 二进制特征帧组批 → Zstd → 普通 TCP
后端普通 TCP → 解压 / CRC / 序号校验 → KTAG 推理 → CSV / JSON
```

读取行为沿用原 UB 前端普通 PCAP 路径：尽快顺序读取，保留文件中秒 / 微秒时间戳；每文件独立 Context 和共享 Domain Store，EOF 后排空队列、flush 和发送尾批。后端完成该文件推理后，前端才处理下一个文件。默认包池 65536、每 worker 包队列 65536、输出队列 4096；池满重试、队列满阻塞，没有主动丢包策略。

所有 354 个 Legacy V1 float32 值及流元数据无损传输；后端模型按 manifest 选择自身输入维度。10:1 是目标，实际比例和性能如实报告，不设置吞吐达标门槛。

## 独立编译

两端需要 Linux、C++17、CMake、libpcap、yaml-cpp、libzstd（1.4.0+）、ONNX Runtime。直接复用已有 KTAG 依赖，不要求 tcpreplay、tcprewrite、DPDK、UB 库或服务。当前项目根构建配置按 aarch64；下面适用于现场的 ARM Linux 服务器。

本目标复用 KTAG 公共库和 Demo 的内部帧编码，不依赖打流工具。离线模式无需DPDK；在线模式可启用DPDK收包后端。`KTAG_BUILD_TCP_PCAP_BENCH` 默认关闭，需要时单独启用。

在 KTAG 根目录执行；ONNX 路径改为实际包含 `include/onnxruntime_c_api.h` 和 `lib/libonnxruntime.so` 的安装根目录：

```bash
export KTAG_ORT=/actual/onnxruntime
cmake -S . -B build/tcp-pcap-bench \
  -DKTAG_APPID_VERSION_INFO=1.0.1 \
  -DONNXRUNTIME_ROOT="$KTAG_ORT" \
  -DKTAG_BUILD_TOOLS=OFF \
  -DKTAG_BUILD_TCP_PCAP_BENCH=ON \
  -DKTAG_BUILD_EXAMPLES=OFF -DKTAG_BUILD_PROXY_FEATURES=OFF \
  -DKTAG_ENABLE_DPDK=OFF -DKTAG_ENABLE_DPDK_UB=OFF
cmake --build build/tcp-pcap-bench --target \
  ktag_tcp_pcap_bench ktag_tcp_pcap_wire_test ktag_tcp_pcap_config_test \
  ktag_tcp_pcap_transport_test ktag_tcp_pcap_input_test -j "$(nproc)"
(cd build/tcp-pcap-bench/tools/tcp_pcap_bench && ctest --output-on-failure)
ldd output/ktag_tcp_pcap_bench
output/ktag_tcp_pcap_bench --help
```

`KTAG_APPID_VERSION_INFO` 必须传入；上面的 `1.0.1` 对应当前 `dependence.xml` 中的 `KTAG_APPID_INFO_VERSION`。原 `build.sh` 会读取并传入此值，直接运行 CMake 时不会自动读取。项目版本变更时须同步使用 XML 中的版本号。

若非标准安装路径的动态库未找到，按实际位置设置运行库搜索路径；sudo 启动时需要显式传入，但本地模式通常不需要 sudo：

```bash
export LD_LIBRARY_PATH="$PWD/output:$KTAG_ORT/lib:/usr/local/lib:/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

两角色使用同一个新可执行文件，所以前端不需要模型包，但仍需要可加载 ONNX Runtime 动态库。无需先编译 `KFlowLens`。如果只构建主程序，测试目标需另外构建后才能执行 CTest；关闭测试可用 `-DKTAG_TCP_PCAP_BENCH_BUILD_TESTS=OFF`。

## 输入和配置

可使用本目录 `config.yaml`，也可使用现有 UB YAML 的有效提取 / Schema / 模型设置。Schema 固定为 `ktag_legacy_v1`，不支持增删值或插件；两端特征参数、Zstd level 和 body 上限必须一致。UB 的共享内存、EAL、节点和 replay 配置不参与连接或本地参数校验；配置指定 `capture.backend=dpdk` 会明确拒绝。

`pipeline.capture.packet_pool_size` 会实际应用，不改为逐包新建独立复制。公共 API 的 0-as-default flow capacity / idle / packet limit 保持旧语义，握手 hash 归一化后比较。配置模型相对路径以 YAML 所在目录解析；CLI 路径以当前工作目录解析。默认新配置不提供模型路径，后端通过 `--model` 指定。

多个 `-p` 按前端参数顺序处理，每文件 `source_index` 为 0、1、2……。如果旧 UB YAML 配置了 `ground_truth_labels`，本用例打印未应用该映射的 WARNING；当前不生成分类报告。CSV 中 source_index 是文件序号，src_ip/dst_ip 是规范流键 A/B 地址。

首轮使用 Ethernet PCAP；实际读取格式以公共 Capture / Flow Key API 为准，统计记录 link type。不改 MAC / IP / 端口，不做 BPF，不按网口 MTU 拒绝本地大包。正常返回的 caplen 小于 len 的报文沿用旧处理方式，用 captured bytes 计算特征和输入吞吐，另外记录 `short_captures` / `original_bytes`，不能声称这些输入都是完整线上帧。纳秒输入沿用现有 API 的微秒转换语义。

有效空 PCAP 成功结束并输出空结果；如果没有首次特征处理，核心吞吐为 `null`。至少需要一个 `-p`。原文件不开写；输出不能覆盖 PCAP、配置、模型包或其他输出路径。

## 启动

两端进入 KTAG 根目录，创建输出目录：

```bash
mkdir -p result/tcp-pcap-bench
```

后端先启动。模型参数是包含 `manifest.json` 等文件的模型包目录，后端不传 `-p`：

```bash
output/ktag_tcp_pcap_bench --role backend \
  --config tools/tcp_pcap_bench/config.yaml \
  --bind-ip 192.168.2.172 --port 19090 \
  --model /data/legacy_model_package \
  --result result/tcp-pcap-bench/predictions.csv \
  --stats result/tcp-pcap-bench/backend.json
```

看到 `listening` 后，在前端启动；PCAP 路径指前端机器上的文件：

```bash
output/ktag_tcp_pcap_bench --role frontend \
  --config tools/tcp_pcap_bench/config.yaml \
  --local-ip 192.168.2.171 --connect 192.168.2.172:19090 \
  -p /data/class0.pcap -p /data/class1.pcap \
  --stats result/tcp-pcap-bench/frontend.json
```

这里的 IP 为示例：后端日志已有 192.168.2.172，前端 192.168.2.171 尚需确认配置及未占用。绑定地址必须确实属于本机。默认使用系统路由，可先用 `ip route get 192.168.2.172 from 192.168.2.171` 核对前端出口；选配 `--network-interface enp65s0f0np0` 可检查 IP 是否属于该网口。这个参数不执行 `SO_BINDTODEVICE`，不需要 raw socket / 混杂抓包权限。普通用户使用可读输入、可写输出和 19090 等非特权端口即可运行。

程序每秒发送进度心跳（小超时配置时更频繁），避免 UDP 特征主要在 EOF flush 时产生导致误判断线。超时通过 YAML 的 `transport.progress_timeout_s` 配置；建连 / 文件初始化 / 单次模型调用超过该值时需要调大。SIGINT / SIGTERM 停止 socket 等待并唤醒有界队列；普通 IO 错误、编码损坏、CRC / 序号错误、断线和推理错误返回非零。

## 截断文件兼容

按用户要求沿用旧本地模式：已经读到至少一条完整记录后，若 Capture 返回 IO 错误且错误文本含 `truncated`，处理此前完整报文，flush 特征，保留部分预测、打印 WARNING 并成功退出。没有完整记录的截断、非法文件头和其他 IO 错误不会按此策略恢复。TCP / 压缩 / 推理错误也不会被当作文件截断。

因此完整输入验收需要同时检查：

```text
status == "complete"
input_complete == true
```

可恢复尾部截断为退出码 0、`status=complete`、`input_complete=false`，两端均记录 warnings 和每文件输入状态，吞吐明确对应实际读到的前缀。普通错误为 `status=failed`，不输出成功吞吐。caplen < len 是捕获快照缺失部分原始报文，不等于损坏的 PCAP 文件尾部，单独统计。

## 性能和无损验证

前端主指标 `core_ns` 从首次 Feature process 调用前，到收到并验证最终 DONE；`pipeline_ns` 另记读取线程启动到 DONE，覆盖首次 process 前的读取。`input_gbit_s` 是 captured bytes × 8 / core seconds / 10^9；`records_s` 是提交推理记录数 / core seconds。

最终回显在完整 JSON 之前打印中文性能统计和管线统计，展示格式参照 UB demo，但实现全部放在本目录，不调用或链接 UB 的统计实现。前端第一行是 `核心处理吞吐量（TCP 压缩全链路）`，后端第一行是 `推理有效吞吐量（后端接收至推理）`。后端阶段指标不能替代前端全链路指标。

为方便直接对照旧 UB 回显，人读吞吐首先使用 MiB/s 和 Gibit/s：分别为 `bytes / seconds / 1024²` 和 `bytes × 8 / seconds / 1024³`。UB 旧版虽写 MB/s、Gbps，实际也是上述二进制计算。下一行同时提供十进制 Gbit/s 换算。JSON 原有单位和字段保持兼容：第一个字段 `core_throughput_gbit_s` 和 `input_gbit_s` 仍使用十进制，`input_gibit_s_ub_legacy` 对应与 UB 直接比较的二进制值；后端 `core_throughput_gbit_s` 为 `null`。失败或没有有效核心计时区间时，首行显示 `N/A`；兼容尾部截断仍成功退出，并在首行注明部分 PCAP 输入。

统计展示相似不代表所有阶段指标完全一致。TCP 的 `特征计算累计耗时` 是所有工作线程 API 调用经过时间之和，不能与 UB 的 `特征计算耗时（最忙线程）` 直接比较；TCP 的特征 CPU 秒使用线程 CPU 时钟，而 UB 当前打印的特征 CPU 累计计算量实际来自 API 调用经过时间之和。TCP `端到端总耗时` 使用 `total_wall_ns`，范围为进程内 Metrics 创建到本次统计采样，包含初始化和结果输出；`pipeline_ns` 的读取开始到 DONE 耗时另外展示。两套用例严格比较应使用前端核心吞吐，并核对相同 PCAP、实际读取包数 / 字节数、Schema、提取参数、模型和 CPU 资源。

计时包含计时开始后的读文件、磁盘 / 页缓存等待、提取、组批、编码、压缩、传输、背压、解压、校验、推理、文件切换和收尾。包时间戳用于特征计算，不用于性能分母。不扣除读文件时间构造所谓纯计算吞吐；各并行阶段累计耗时不可相加当作总时间。

后端 core 为首批特征接收完到最后一次推理完成；其 pipeline 是首个 FILE_READY 到 DONE，不能与前端口径混用。模型 / 握手 / 首次上下文准备另记，DONE 表示推理完成，随后输出 CSV，落盘时间另记。CSV 写失败时后端非零退出；前端收到 DONE 不能证明后端文件已落盘。

统计提供实际输入、过滤 / 拒绝、提取 / 发送 / 解码 / 提交 / 过滤 / 预测计数、owned 池和队列等待、阶段纳秒、进程 / 工作线程 CPU、RSS、实际压缩比及整块 NIC 增量。抓包丢包、回放速率和子进程 CPU 不适用，值为 `null`。后端不实际读 PCAP，其输入计数来自 FILE_END；后端读取和提取计时未发生，不能把它们解释成后端成本。

`compression_ratio` 包含内部记录头和元数据；另报 `pure_float32_bytes`，不据此保证纯特征矩阵达到 10:1。`compression_ratio_socket` 分母含双向 TCP 应用消息；NIC 字节包含该接口其他流量，不等于 TCP 应用字节。暂未提供逐连接 TCP 重传统计。默认预测暂存上限 1,000,000 条，可通过 `inference.prediction_limit` 调整；超出预算明确失败，不丢弃预测。

正确性运行可分别增加：

```bash
# 前端参数
--feature-audit result/tcp-pcap-bench/frontend.frames
# 后端参数
--feature-audit result/tcp-pcap-bench/backend.frames
```

把两文件复制到同一目录后核对：

```bash
cmp frontend.frames backend.frames
sha256sum frontend.frames backend.frames
```

应逐字节一致，包括浮点特殊位模式。比较同一次运行的压缩前 / 解压后特征，不要求不同并行运行的输出顺序一致。audit 写入有开销，JSON 标记 `audit_enabled`；测性能时关闭。

## Linux 验证范围

CTest 提供四个 C++ 测试和一个 Python 前端协议测试：

- wire：真实 Zstd、256 条内部记录、特殊 float 位模式、CRC / framing、损坏数据、文件计数和旧打流 magic 拒绝。
- config：角色与 PCAP 归属、UB YAML、owned 池参数、0-as-default 归一化、打流参数隔离及输出路径保护。
- transport：Linux socketpair 大批次短读短写、分段 header、合并消息、断线及取消。
- input：真实 Capture API 的 owned 池耗尽 / 重试、原时间戳和顺序、大包 / caplen < len、空输入和截断兼容。
- frontend_protocol：实际前端和测试协议接收端完成多文件、空文件、EOF 尾批、损坏尾部、池容量 1、audit 和断线；它不测试真实模型推理。

若提供现有 Legacy V1 模型，可以先在任意一台服务器执行真实两角色 loopback 测试，无需现场 PCAP、网卡权限或打流工具：

```bash
python3 tools/tcp_pcap_bench/tests/test_loopback.py \
  output/ktag_tcp_pcap_bench /data/legacy_model_package
```

该测试生成少量 UDP PCAP（含空文件和截断尾部），启动真实前端、真实后端及 ONNX 模型，检查逐文件 / 全局计数、特殊收尾、CSV 数量和 audit。也可配置 `-DKTAG_TCP_PCAP_TEST_MODEL=/data/legacy_model_package` 把它加入 CTest；未设置模型时不会把未验证的推理测试标为通过。

当前 Windows 开发机已完成所有新增 C++ 源码 / 测试的 Linux/ARM64 对象编译、真实 Zstd / 特征帧测试和 yaml-cpp 配置解析测试；Python 脚本已做语法检查。尚未在本机完成完整 Linux 链接构建，socketpair、实际 Capture / frontend 和现场模型 loopback 需在 Linux 执行。双机吞吐及实际压缩率尚无实测数据。新模式验证结果不代表原打流模式已通过。

## 普通流量插件依赖

本工具保留独立TCP/Zstd在线及离线链路，特征计算已迁移到公共Feature API与
traffic Provider插件。Frontend默认从公共特征库旁的
`plugins/libktag_traffic_features.so` 加载插件，可用
`KTAG_TRAFFIC_FEATURE_PLUGIN` 指定路径，或在配置的 `pipeline.plugins`
（无pipeline段时为顶层 `plugins`）中显式列出插件路径；相对路径按配置文件目录解析。
Schema仍为冻结的 `ktag_legacy_v1`（354维）。Backend只接收特征并调用公共推理API。
构建该工具时须启用 `KTAG_BUILD_FEATURE_PLUGINS`，安装时部署配套公共库和插件。
