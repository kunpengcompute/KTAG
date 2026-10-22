# 单机推理时延测试

复用 `ktag_demo`、公共 Capture/Feature/Inference API 与正式插件。测试入口默认关闭，
不影响普通 Demo 调度。本轮只覆盖离线 PCAP、`single_thread` 和 `full_parallel`；
不使用 UB、网口实时采集或 DPDK。输入必须是本机可读的完整 PCAP。

## 构建与运行

在服务器仓库根目录先执行：

```bash
sh build.sh
# 先检查普通推理已能使用同一份 YAML、模型包和 PCAP 正常输出。
# 建议先小规模验证命令，再运行正式轮次：
ROUNDS=1 ITERATIONS=100 WARMUP=10 bash test/latency/run_latency.sh \
  /absolute/path/task.yaml /absolute/path/sample.pcap /absolute/path/latency-smoke

ROUNDS=3 BATCH_SIZE=64 FEATURE_THREADS=4 INFERENCE_THREADS=2 \
  WARMUP=1000 ITERATIONS=10000 PACKET_DETAILS=0 \
  bash test/latency/run_latency.sh \
  /absolute/path/task.yaml /absolute/path/sample.pcap /absolute/path/latency-result
```

YAML 仍使用现有 `config.yaml` 或 `proxy_config.yaml`，插件、模型包和训练时的 Schema、
生命周期必须匹配；`backpressure.policy` 必须为 `block`。相对路径按 YAML 所在目录解析。
`pipeline.inference.batch_timeout_us` 控制组批等待时间，未填写时使用现有默认值 1000 us。

脚本顺序执行，避免多组测试相互抢占 CPU。每轮共 8 组：纯推理和端到端各测 batch=1、
batch=N，再分别覆盖单线程与多线程。ONNX 内部 intra/inter-op 线程固定为 1，比较的是
KTAG Worker 并行；实际配置记录在 `.run.csv`。固定服务器、模型、PCAP、CPU/NUMA
配置和外部负载，多轮结果分别保留，不把不同轮次的 P99 求平均后当作总体 P99。
性能构建不要开启 ASan、coverage；CPU/内存/线程配置可随结果一起记录。

单独执行一组（不需要 `-c/-j`，也不支持与它们同时使用）：

```bash
output/ktag_demo -r /absolute/path/task.yaml -p /absolute/path/sample.pcap \
  --pipeline-mode single_thread -n 1 --inference-threads 1 \
  --latency-mode e2e --latency-batch-size 1 \
  --latency-output /absolute/path/results/single-e2e-b1 \
  --latency-packet-details
```

多线程改为 `--pipeline-mode full_parallel -n 4 --inference-threads 2`；组批改为
`--latency-batch-size 64`；纯推理改为 `--latency-mode inference`，并去掉逐包明细开关。
已有报表前缀不会被覆盖；再次运行请换目录或前缀。未成功退出的运行不作为有效结果。

## 计时口径

- 纯模型：只测同步 `Ort::Session::Run`，不含预处理、张量构造和结果转换。
  通过新增的 `ktag_inference_predict_with_timing` 获取，原 `predict` 签名与行为不变。
- API 调用：计时包住整个公共推理调用，包含预处理、模型执行和结果转换；
  调用前向 Feature Batch 复制输入不在该计时内。
- per flow：分别统计第一包到预测、最后一包到预测，以及特征就绪到预测。
- per packet：该报文从 Capture 成功返回，到所属流的 Prediction Batch 可用。
  包含组流观察、特征计算、队列/组批等待和推理，不包括该包返回前的文件读取等待、
  模型初始化、文件打开或预测结果落盘；不是网卡线速到达时延。
- PCAP 按处理能力快速读取，不按历史时间戳回放。时延使用同进程 `steady_clock`，
  因此这些结果表示离线处理表现，不能直接宣称线上真实到达负载下的时延。

纯推理先提取真实特征并按模型包规则筛选有效样本，再让每个推理 Worker 建立独立
Context、预热并同步开始测量。预热和特征提取不计入纯模型分布；样本不足一批时
循环使用语料构造完整批次，`ITERATIONS` 是每个 Worker 的调用次数。
纯推理的 per packet 是摊销成本，不是逐包响应时延。

端到端不做额外预热，包含每个 Context 的首次模型调用；首次调用在模型分布中
单独列出。所有 Context、模型包与 Capture 在读包前创建。多线程测量阶段包括
Worker 启动与调度；单线程在主线程串行处理。每个 PCAP 独立创建上下文并测量，
多文件不会并发读取，也不跨文件组流或组批。

## 组批、尾批与记录关联

测试路径统一使用“达到流数上限、等待超时、或输入结束”三种提交条件。
组批等待从推理端接收本批第一条特征开始；排队等待另包含在端到端时延里。
单线程在处理报文/特征的边界检查超时，不能在同线程执行中的调用内抢占；
多线程在队列上按截止时间等待。实际批大小和提交原因必须一起比较。
batch=1 时包括 EOF/flush 输出在内，每次严格只向模型接口输入一条流。

尾部单独统计 Capture EOF、最后一个捕获报文、最后一个产生预测的特征就绪后，
到最后预测结果可用的时间；它们不是所有流的最大响应时延。
`.summary.csv` 中各分布的 `max` 才是本轮观察到的最大值。

关联键包含输入文件、分片 Worker、规范化 Flow Key 和记录首末包序号范围，
不把重复使用同一五元组的不同流代际混在一起。
被模型过滤的流在明细中标为 `filtered`，不伪造预测时延。
被解码拒绝、没有输出流或不在输出记录序号范围内的报文计入 `unassociated_packets`；
已关联但所属流被过滤的报文也不进入成功分类时延分布。
两种线程配置的输出流数、过滤数和关联报文数应一起核对。

## 结果文件

| 文件 | 内容 |
|---|---|
| `.summary.csv` | 各指标 count、mean、P50、P95、P99、max，单位 us；精确最近秩分位数 |
| `.batches.csv` | 实际输入/通过过滤流数、关联包数、纯模型/API 耗时 ns、Worker、提交原因、首次调用标记 |
| `.flows.csv` | 端到端逐流状态、标签/置信度、时间点和三种流级时延，单位 ns |
| `.packets.csv` | 端到端逐包时延明细，仅在 `--latency-packet-details` 开启时生成，单位 ns |
| `.sources.csv` | 各 PCAP 计数、处理总耗时和尾部清空时间，单位 ns |
| `.run.csv` | 模型/Schema、线程、批次、预热、计数及计时边界 |
| `.log` | 脚本保存的控制台输出 |

`model_batch` 是每次模型执行的耗时；`model_flow_batch_response` 按本批有效流数
加权，每条流共享整批模型执行时间。`model_amortized_per_flow` 和
`model_amortized_per_packet` 是计算成本摊销，不能解释成该流/报文真正只等待了这么久。
纯推理不生成逐流和逐包端到端文件，其批次可混合重放多个 PCAP 的特征，
`.batches.csv` 的 `source_index` 统一标记为 -1。所有报表在测量结束后写出。

精确逐包分位数需要保存包时间记录，内存随数据量增长；关闭逐包明细只减少磁盘输出，
不关闭采样。建议先使用小 PCAP，再逐步扩大，并对比开关关闭时的普通 Demo 吞吐量，
评估计时、队列和记录采集的开销。报告吞吐量是带统计的测试路径吞吐量，不能替代
未插桩产品路径的性能结果。
