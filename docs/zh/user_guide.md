# 用户指南
## 1. 测试框架

### 1.1 功能测试

本项目采用Google Test框架对KTAG中实现的方法函数进行测试，测试用例的数据来自于采集到的pcap流量文件。在进行测试前，需要将pcap流量文件放在KTAG项目的./test/traces文件夹下供测试时使用。

测试流程：

1. 首先运行KTAG项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码。

```
make
```

4. 运行功能测试。

```
make rfunc
```

### 1.2 性能测试

测试流程：

1. 首先运行KTAG项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码。

```
make
```

4. 运行功能测试。

```
make rperf
```

### 1.3 模糊测试（Fuzz Testing）

KTAG Fuzz 测试是基于 LibFuzzer 框架实现的模糊测试，用于发现库中的潜在崩溃和安全漏洞。通过向库的核心 API 注入随机数据，测试系统在各种输入情况下的行为，确保库的健壮性和安全性。

#### 1.3.1 测试覆盖范围

Fuzz 测试覆盖了以下核心功能模块：

- **特征提取器模块**：
  - `extractor_init` - 初始化特征提取器
  - `process_packet` - 处理单个数据包，提取特征向量
  - `process_packet_with_rawbow` - 处理单个数据包，提取特征向量和 rawbow
  - `extractor_get_statistics` - 获取统计信息
  - `extractor_reset_statistics` - 重置统计信息
  - `extractor_finalize` - 处理所有剩余的流，提取特征向量
  - `extractor_finalize_with_rawbow` - 处理所有剩余的流，提取特征向量和 rawbow
  - `extractor_destroy` - 销毁特征提取器

- **推理引擎模块**：
  - `inference_init` - 初始化推理引擎
  - `inference_predict` - 批量推理
  - `inference_destroy` - 销毁推理引擎

- **边界情况测试**：
  - 空指针输入测试。
  - 随机链接类型测试。
  - 不同大小的输入数据测试。

#### 1.3.2 构建指南

##### 1.3.2.1 前提条件

1. **构建 KDADK 库**：
   - 首先需要构建 KTAG 库，生成 `KDADK_feature_extract` 和 `KDADK_inference` 库文件。

2. **依赖项**：
   - GCC 10.3.1+
   - CMake 3.10+
   - LibFuzzer (随 LLVM 一起提供)
   - yaml-cpp 0.8.0
   - ONNX Runtime 1.22.0

##### 1.3.2.2 构建步骤

1. **构建前准备**：
   - 需要确保config.yaml文件中，model_path_onnx和scaler_path_json路径为真实训练模型路径。

2. **进入 fuzz 测试目录**：
   ```bash
   cd test/fuzz
   ```

3. **创建构建目录**：
   ```bash
   mkdir build && cd build
   ```

4. **配置 CMake**：
   ```bash
   cmake .. \
   -DCMAKE_C_COMPILER=clang \
   -DCMAKE_CXX_COMPILER=clang++ \
   -DCMAKE_CXX_FLAGS="-isystem /usr/include/c++/12 -isystem /usr/include/c++/12/aarch64-openEuler-linux" \
   -DCMAKE_EXE_LINKER_FLAGS="-L/usr/lib/gcc/aarch64-openEuler-linux/12 -B/usr/lib/gcc/aarch64-openEuler-linux/12"
   ```

5. **编译测试**：
   ```bash
   make
   ```
   编译成功后，将生成 `kdadk_fuzzer` 可执行文件。

#### 1.3.3 运行指南

使用 LibFuzzer 运行测试：

```bash
./kdadk_fuzzer -runs=30000000 -max_total_time=12600
```

LibFuzzer 支持多种运行选项，常用的选项包括：

| 选项 | 描述 | 默认值 |
|------|------|--------|
| `-max_len` | 最大输入长度 | 4096 |
| `-max_total_time` | 单个测试用例的超时时间（秒） | 10 |
| `-runs` | 运行的测试用例数量 | 无限 |

#### 1.3.4 查看覆盖率

由于 Fuzz 测试使用 Clang 编译，而主库使用 GCC 编译，两者生成的覆盖率数据格式不同，需要进行合并才能得到完整的覆盖率报告。

1. **生成 Clang 覆盖率数据**：
   ```bash
   ./kdadk_fuzzer -runs=10000
   llvm-profdata merge -sparse default.profraw -o kdadk.profdata
   llvm-cov export -format=lcov ./kdadk_fuzzer --instr-profile=kdadk.profdata > clang_coverage.info
   ```

2. **生成 GCC 覆盖率数据**：
   ```bash
   cd KTAG/build
   lcov --capture --directory . --output-file gcc_coverage.info
   ```

3. **合并覆盖率数据**：
   ```bash
   cp ../test/fuzz/build/clang_coverage.info ./
   lcov -a gcc_coverage.info -a clang_coverage.info -o combined_coverage.info
   ```

4. **生成 HTML 覆盖率报告**：
   ```bash
   genhtml combined_coverage.info --output-dir=coverage_report --show-details
   ```

5. **查看覆盖率报告**：
   打开 `coverage_report/index.html` 文件，在浏览器中查看详细的覆盖率报告。

## 2. 测试框架通用功能

### 2.1 测试工具和辅助函数

测试框架提供了一系列辅助函数和工具，用于简化测试过程：

- **GetExecutableDir()**：获取可执行文件所在目录
- **GetFirstPcapFile()**：从 test/traces 目录获取第一个 pcap 文件
- **calculate_time_ms()**：计算执行时间（毫秒）
- **process_error_test()**：处理测试过程中的错误
- **get_packet()**：从 pcap 文件中获取数据包用于测试

### 2.2 性能测试工具

性能测试框架提供了以下功能：

- **PerformanceTimer**：性能计时器，用于测量代码执行时间
- **PerformanceMetrics**：性能指标结构，包含以下字段：
  - `total_time_ms`：总耗时（毫秒）
  - `avg_time_ms`：平均耗时
  - `min_time_ms`：最小耗时
  - `max_time_ms`：最大耗时
  - `total_operations`：总操作数
  - `throughput`：吞吐量（操作/秒）
  - `memory_usage_mb`：内存使用（MB）

### 2.3 测试数据管理

- **测试数据路径**：测试数据应放在 `./test/traces` 目录下
- **支持的文件格式**：仅支持 `.pcap` 格式的流量文件
- **自动数据检测**：测试框架会自动检测并使用 `./test/traces` 目录中的第一个 pcap 文件

## 3. 故障排除

### 3.1 常见问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| Clang 编译错误 | 缺少 LibFuzzer 支持 | 确保 Clang 版本支持 LibFuzzer（Clang 6+） |
| 覆盖率数据合并失败 | 缺少 lcov 工具 | 安装 lcov：`sudo apt install lcov` |
| 报告生成失败 | 路径问题 | 确保所有路径正确，文件存在 |
| 内存分配失败 | 内存不足 | 检查系统内存使用情况，减少测试规模 |

### 3.2 调试技巧

1. **检查编译器版本**：确保 Clang 和 GCC 版本支持所需的功能。
2. **验证覆盖率选项**：确保编译时添加了正确的覆盖率选项。
3. **检查文件路径**：确保所有文件路径正确，特别是模型和配置文件的路径。
4. **查看错误信息**：仔细分析编译和运行时的错误信息，定位问题所在。
5. **使用调试模式**：在编译时添加 `-g` 选项，使用调试器分析问题。