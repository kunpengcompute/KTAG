# KTAG 测试说明

本文介绍 KTAG 的测试框架和使用方法，覆盖功能测试、性能测试、ASan 内存检测、
Fuzz 测试和 Python 工具链测试。测试统一入口为 `/opt/KTAG/test/build/`，KTAG
源码根目录以 `/opt/KTAG` 为例。

## 1. 目录结构

```text
/opt/KTAG/test/
├── build/            # 测试统一构建入口（Makefile）
├── common/
│   ├── fixtures/     # 用于校验插件加载失败路径的非法插件夹具
│   ├── functest/     # 功能测试源码（GoogleTest）
│   ├── include/      # 测试辅助头文件
│   └── perftest/     # 性能测试源码（GoogleTest）
├── fuzz/             # LibFuzzer 目标（详见 fuzz/README.md）
├── gtest-download/   # GoogleTest 1.8.0 下载脚本
├── gtest-parallel/   # gtest 并行执行工具
├── python/           # Python 工具链单元测试
└── kp_coverage.py    # lcov 覆盖率报告生成脚本
```

## 2. 环境准备

1. 按《[编译指南](../docs/zh/installation_guide.md)》完成依赖安装；
2. 以测试选项构建产品库（功能测试需要链接 `libktag_appidlib.so`）：

```bash
sh /opt/KTAG/build.sh -lib
```

3. 首次构建测试会自动下载 GoogleTest 1.8.0（需要外网）。离线环境请提前将
   `googletest-release-1.8.0` 目录放置在 `/opt/KTAG/test/` 下；
4. Python 工具链测试需要安装训练依赖：

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

## 3. 功能测试

### 3.1 覆盖范围

| 测试源文件 | 覆盖内容 |
|---|---|
| `func_capture.cpp` | Capture 层：规范化 Flow Key、正反流量归入同一流、IPv6 扩展头与分片边界、包缓冲和设备接口 |
| `func_domain_store.cpp` | Domain Store/Cache：并发写入、TTL、增量同步和版本失效 |
| `func_feature_context.cpp` | Feature Context：流生命周期、UDP 报文上限、DNS AAAA 域名关联、线程所有权、与旧固定向量输出一致性 |
| `func_feature_extract.cpp` | 插件冻结统计算法 |
| `func_feature_provider.cpp` | Provider 回调和 Fill Zero 策略（需 `internal=y`，使用内部头文件） |
| `func_feature_schema.cpp` | Schema 组合与维度展开 |
| `func_inference.cpp` | 公共 Inference 配置校验、线程数边界和 Prediction Batch 生命周期 |
| `func_inference_contract.cpp` | 模型包与 Schema 契约校验 |
| `func_latency.cpp` | 逐包关联、流代际隔离、精确分位数、定时队列和计时参数校验 |
| `func_latency_pipeline.cpp` | 真实 ONNX/PCAP 的单/多线程、逐流/逐包、尾批、过滤及失败清理 |
| `func_demo_inference_session.cpp` | 历史 Demo 调度的公共推理桥接：真实 ONNX 推理、过滤后源记录对齐、标准化及错误处理 |
| `func_proxy_features.cpp` | 代理特征插件：Sequence/Burst、TCP Signature、QUIC |
| `func_demo_pipeline.cpp` | demo Pipeline：配置解析、队列容量与背压、Schema CSV/预测输出契约、插件加载与卸载、冻结 Schema 保护 |
| `compile_ktag_public_headers.c` | 6 个公共头文件可独立编译 |
| `GetProductVer.cpp` | 产品版本查询接口 |

### 3.2 构建与运行

```bash
# 构建功能测试（生成 ktag_appid_functest）
cd /opt/KTAG/test/build
make func

# 运行全部功能测试
make rfunc

# 运行指定测试套（按 TEST 套名过滤）
make rfunc func=KTAGCaptureFlowKey
make rfunc func=KTAGDemoPipelineConfig
```

测试通过 gtest-parallel 并行执行，运行前会自动设置 `LD_LIBRARY_PATH`。

### 3.3 Provider 内部测试

`func_feature_provider.cpp` 使用内部私有头文件，默认不参与构建。需要验证
Provider 生命周期时显式启用：

```bash
cd /opt/KTAG/test/build
make func internal=y
make rfunc func=KTAGFeatureProvider
```

## 4. 性能测试

原性能测试 `perf_calculate.cpp` 与 `perf_e2e.cpp` 测量统计函数微基准，后者并未执行完整推理链路。
单机纯推理、逐流/逐包端到端时延及单/多线程批次对比，使用《[时延测试说明](latency/README.md)》中的 Demo 测试入口与脚本。

```bash
# 构建性能测试（生成 ktag_appid_perftest）
cd /opt/KTAG/test/build
make perf

# 运行全部性能测试
make rperf

# 运行指定性能测试
make rperf func=KTAG_PERFTEST_E2E
```

性能对比测试应固定 CPU 频率、线程数和数据规模，多轮取值。

## 5. ASan 内存检测

以 AddressSanitizer 选项构建产品库和功能测试并运行：

```bash
cd /opt/KTAG/test/build
make asan    # ASan 构建
make rasan   # 运行 ASan 功能测试
```

## 6. Fuzz 测试

Fuzz 测试使用 LibFuzzer，构建和运行命令：

```bash
cd /opt/KTAG/test/build
make fuzz
make rfuzz fuzzcount=10000 fuzzargs="-max_total_time=60"
```

覆盖范围、参数说明和覆盖率生成方法参见《[Fuzz 测试说明](fuzz/README.md)》。

## 7. Python 工具链测试

| 测试文件 | 覆盖内容 |
|---|---|
| `test_dataset_preflight.py` | 数据预检工具：Schema 契约、列头校验、训练/评估重叠检测 |
| `test_schema_training.py` | Schema 训练与评估：模型包生成、manifest 契约、代理 619 维特征目录 |

```bash
cd /opt/KTAG
python3 -m unittest discover -s /opt/KTAG/test/python -p "test_*.py" -v
```

## 8. 覆盖率

以 GCC coverage 选项构建并运行功能测试后，生成 lcov/HTML 覆盖率报告：

```bash
cd /opt/KTAG/test/build
make func cover=y
make rfunc
python3 /opt/KTAG/test/kp_coverage.py
```

报告输出在构建目录的 `all/` 下。

## 9. 命令速查

| 目标 | 命令（在 /opt/KTAG/test/build 下执行） |
|---|---|
| 构建功能/性能测试 | `make func` / `make perf` |
| 运行功能/性能测试 | `make rfunc` / `make rperf` |
| 运行单个测试套 | `make rfunc func=套名` / `make rperf func=套名` |
| 构建并运行全部 | `make` 后执行 `make run` |
| ASan | `make asan` 后执行 `make rasan` |
| Fuzz | `make fuzz` 后执行 `make rfuzz fuzzcount=10000` |
| 清理 | `make clean` |

`make help` 可查看全部目标和参数。
