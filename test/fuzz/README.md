# KDADK-APPID Fuzz 测试指南

## 1. 测试概述

KDADK-APPID Fuzz 测试是基于 LibFuzzer 框架实现的模糊测试，用于发现库中的潜在崩溃和安全漏洞。通过向库的核心 API 注入随机数据，测试系统在各种输入情况下的行为，确保库的健壮性和安全性。

## 2. 测试覆盖范围

Fuzz 测试覆盖了以下核心功能模块：

### 2.1 特征提取器模块
- `extractor_init` - 初始化特征提取器
- `process_packet` - 处理单个数据包，提取特征向量
- `process_packet_with_rawbow` - 处理单个数据包，提取特征向量和 rawbow
- `extractor_get_statistics` - 获取统计信息
- `extractor_reset_statistics` - 重置统计信息
- `extractor_finalize` - 处理所有剩余的流，提取特征向量
- `extractor_finalize_with_rawbow` - 处理所有剩余的流，提取特征向量和 rawbow
- `extractor_destroy` - 销毁特征提取器

### 2.2 推理引擎模块
- `inference_init` - 初始化推理引擎
- `inference_predict` - 批量推理
- `inference_destroy` - 销毁推理引擎

### 2.3 边界情况测试
- 空指针输入测试
- 随机链接类型测试
- 不同大小的输入数据测试

## 3. 技术实现

### 3.1 测试框架
- **测试框架**：LibFuzzer
- **编译器**：GCC 10.3.1+
- **构建系统**：CMake 3.10+

### 3.2 核心测试函数

测试的核心是 `LLVMFuzzerTestOneInput` 函数，它接收 LibFuzzer 生成的随机数据，并使用这些数据测试库的各种 API：

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // 测试代码...
    return 0;
}
```

### 3.3 测试策略

1. **随机数据注入**：使用 LibFuzzer 生成的随机数据作为输入
2. **API 调用序列**：按照合理的顺序调用相关 API
3. **资源管理**：确保所有分配的资源都被正确释放
4. **错误处理**：测试 API 在各种输入情况下的行为
5. **边界情况**：测试空指针、随机输入等边界情况

## 4. 构建指南

### 4.1 前提条件

1. **构建 KDADK 库**：
   - 首先需要构建 KDADK-APPID 库，生成 `KDADK_feature_extract` 和 `KDADK_inference` 库文件

2. **依赖项**：
   - GCC 10.3.1+
   - CMake 3.10+
   - LibFuzzer (随 LLVM 一起提供)
   - yaml-cpp 0.8.0
   - ONNX Runtime 1.22.0

### 4.2 构建步骤

1. **构建前准备**：

   需要确保config.yaml文件中，model_path_onnx和scaler_path_json路径为真实训练模型路径。

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

## 5. 运行指南

### 5.1 基本运行

使用 LibFuzzer 运行测试：

```bash
./kdadk_fuzzer -runs=30000000 -timeout=12600
```

### 5.2 运行选项

LibFuzzer 支持多种运行选项，常用的选项包括：

| 选项 | 描述 | 默认值 |
|------|------|--------|
| `-max_len` | 最大输入长度 | 4096 |
| `-timeout` | 单个测试用例的超时时间（秒） | 10 |
| `-runs` | 运行的测试用例数量 | 无限 |

