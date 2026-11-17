# KTAG 纯收包测试

`ktag_rx_bench` 复用 `libktag_capture_dpdk` 的 MBUF 接口，每个 RX 线程
独占一个队列，取包、线程本地计数后立即释放 mbuf。不执行流哈希分发、
Feature handoff、特征提取、UB 传输或推理，也不修改现有 Capture 实现。

## 构建

使用现有已成功配置的 Linux 构建目录，并使用与 testpmd 相同的 DPDK/PMD：

```bash
pkg-config --modversion libdpdk
cmake -S . -B build -DKTAG_ENABLE_DPDK=ON
cmake --build build --target ktag_rx_bench --parallel 8
```

需要 DPDK 24.11+，`KTAG_BUILD_TOOLS` 保持开启。找不到 `libdpdk.pc` 时，
先将对应安装目录的 pkgconfig 路径加入 `PKG_CONFIG_PATH`。工具目标只引用
Capture、DPDK 和线程库；仓库整体配置仍沿用已有的依赖和构建选项。
产物为 `output/ktag_rx_bench`。

## computer02 的四队列对照测试

先退出占用同一网口的 testpmd/KFlowLens。下面按现有 testpmd 的映射：
主线程 CPU192，Queue0..3 分别由 CPU193、194、195、196 操作；它们均在 NUMA2。
EAL 只启用主 lcore，RX 使用注册过的工作线程，避免再启动一套 EAL RX 线程。

```bash
./output/ktag_rx_bench \
  --device 0000:a5:00.0 \
  --rx-cpus 193,194,195,196 \
  --eal-args '--lcores=0@192 --main-lcore=0 -n 4 -a 0000:a5:00.0 --file-prefix=ktag_rx_bench' \
  --rxd 1024 --mbufs 65536 --burst 64
```

以上 rxd/mbufs/burst 是工具默认值，不能假定等于 testpmd 的默认值。
对照前在 testpmd 中用 `show config rxtx`、`show rxq info 0 0` 核对队列及
burst 参数，必要时显式调整两端。保持同一发包配置，并检查各 RX 队列分布；
Capture 沿用原有 RSS 配置。mlx5 网卡保持绑定 `mlx5_core`。

参数仅有 `--device`、`--rx-cpus`、`--eal-args`（必填），以及
`--rxd`、`--mbufs`、`--burst`。CPU 用逗号分隔，不接受重复值；CPU 数量就是
RX 队列/线程数。单 CPU 时使用 Capture 的单 reader MBUF 接口。
`--eal-args` 不包含程序名，可附加现场已验证的驱动加载、大页、内存参数。

## 运行与验收

1. 等待所有 RX 线程完成绑核和注册，工具打印 `READY`。
2. 启动 pktgen，保持稳定流量至少 60 秒。观察连续多个完整秒的速率。
3. 先停止 pktgen，等待约 2 秒排空，再向工具发送 Ctrl+C（也支持 SIGTERM）。
4. 保存每秒速率和最终汇总，与同轮 pktgen 实际 TX 包数对照。

每秒的 Gbps/Mpps 来自 DPDK 端口计数差值和实际单调时钟间隔，字节口径
与 testpmd 端口统计一致，不补算线上前导码、IFG、FCS。missed/errors/nombuf
为相对 READY 前快照的累计增量；网卡不支持的字段可能始终为零。
约 56.90Gbps / 14.00Mpps 是本次 testpmd 对照值，不是自动通过阈值。

最终汇总包含每队列应用取得并释放的包数/字节数，以及 Capture 的
received/delivered/pending。Capture 汇总只在所有 RX 线程停止后读取。
`pending` 是已从 PMD 取到但尚未返回给调用方的 burst 缓存；停止时若仍有流量，
这些包会由 Capture 关闭时释放，不计入应用已处理数。

整个进程的累计包数包括启动和排空阶段，不要用包含等待发包的总时长均值
对比稳态 57G。达到 testpmd 速率说明达到纯收包基线；要证明全部收齐，仍需
对齐发包/收包窗口并比较 TX/RX 包数，排除背景流量。工具不自动宣称零丢包。
退出码 0 表示正常停止；参数、绑核、初始化、统计或收包异常返回非零。

本测试的 mbuf 由收包线程自己释放，不覆盖跨核交接、报文复制或下游背压。
