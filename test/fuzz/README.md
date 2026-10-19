# KTAG Fuzz 测试说明

## 1. 覆盖范围

Fuzz 测试提供一个 LibFuzzer 目标：

```text
ktag_fuzzer
```

它通过公共 Feature API 覆盖插件特征提取：

- Feature Context 的创建、报文处理、Flush 和销毁；
- Traffic Feature Plugin 的加载、Provider 回调和 RawBoW 注解；
- 可选的旧 `inference_init/inference_predict`；
- Ethernet/Linux SLL 的随机报文和截断输入。

输入按 Ethernet 或 Linux SLL 链路解释。

## 2. 环境要求

- Clang/Clang++，并包含 LibFuzzer；
- CMake 3.16.3 或更高版本；
- 已构建公共 Feature 库、`plugins/libktag_traffic_features.so` 和旧推理库；
- yaml-cpp 0.8.0；
- ONNX Runtime 1.22.0。

## 3. 构建

从统一测试入口构建：

```bash
cd /opt/KTAG/test/build
make fuzz
```

该命令先以 ASan 选项构建产品库，再使用 Clang 配置并编译
`/opt/KTAG/test/fuzz/build/ktag_fuzzer`。

## 4. 运行

```bash
cd /opt/KTAG/test/build
make rfuzz fuzzcount=10000 fuzzargs="-max_total_time=60"
```

`rfuzz` 会设置运行所需的 `LD_LIBRARY_PATH`，并把
`/opt/KTAG/tools/demo/config.yaml` 作为可选旧推理配置传入。配置中的 ONNX 与
Scaler 路径必须有效；不需要覆盖旧推理时，可直接运行二进制且不设置
`KTAG_LEGACY_CONFIG`：

```bash
/opt/KTAG/test/fuzz/build/ktag_fuzzer -runs=10000 -max_total_time=60
```

常用 LibFuzzer 参数：

| 参数 | 说明 |
|---|---|
| `-runs=N` | 最多执行 N 次输入 |
| `-max_total_time=N` | 最长运行 N 秒 |
| `-max_len=N` | 限制单个输入长度 |

发现崩溃后，保存 LibFuzzer 生成的复现样本和完整 Sanitizer 回显，按项目信息
安全流程归档。

## 5. 覆盖率

Fuzz 目标使用 Clang 的 profile/coverage 选项。运行后生成覆盖率数据：

```bash
cd /opt/KTAG/test/fuzz/build
llvm-profdata merge -sparse default.profraw -o ktag.profdata
llvm-cov export \
  -format=lcov \
  ./ktag_fuzzer \
  --instr-profile=ktag.profdata \
  > clang_coverage.info
```

主库如使用 GCC coverage 构建，二者数据格式不同，需要通过相应工具分别生成后再合并。
