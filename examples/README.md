# KTAG 示例

本目录包含四个示例，按从简单到复杂的顺序编排。建议按以下顺序阅读：

| 顺序 | 示例 | 说明 |
|:---:|------|------|
| 1 | [three_layer](#1-three_layer--三层架构最小示例) | 三层架构最小示例（新手从这里开始） |
| 2 | [feature_provider](#2-feature_provider--自定义-feature-provider) | 怎么写单个 Feature Provider |
| 3 | [feature_plugin](#3-feature_plugin--动态加载插件) | 怎么打包成动态库插件并加载 |
| 4 | [external_consumer](#4-external_consumer--仓外-cmake-集成) | 怎么用 CMake 在 KTAG 源码树之外集成 |

---

## 1. three_layer — 三层架构最小示例

只用三个公共头文件，只链接三个公共库：

- `ktag_capture.h` / `libktag_capture.so`
- `ktag_feature.h` / `libktag_feature.so`
- `ktag_inference.h` / `libktag_inference.so`

### 构建

```bash
cd KTAG
sh build.sh
```

### 运行

```bash
cd output
./ktag_three_layer_example MODEL_PACKAGE_DIR INPUT.pcap
```

其中 `MODEL_PACKAGE_DIR` 是一个 Schema-bound 模型包目录，由训练脚本或模型升级工具生成，内含 `manifest.json`、`scaler.json` 和 `model.onnx`。如果你还没有模型包，可以先通过 `tools/model_package/upgrade_legacy_model.py` 将旧模型转为新格式，或通过 `tools/demo/python/training.py` 训练一个新模型。

### 代码要点

- 通过 `ktag_feature_schema_create_legacy_v1()` 创建 Legacy Schema（354 维）
- 通过 `ktag_feature_registry_register_legacy_builtins()` 注册 4 个内置 Provider
- 通过 `ktag_domain_store_create()` 创建 Domain Store（DNS 反查缓存）
- 使用 `KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT` 生命周期模式，兼容旧 351 维模型
- 主循环：`ktag_capture_next` → `ktag_feature_process_packet` → `ktag_inference_predict`
- 文件读完后调用 `ktag_feature_flush` 输出所有残留流的预测
- 客户可在此同步调用之上自行增加线程和队列

---

## 2. feature_provider — 自定义 Feature Provider

`custom_feature_provider.c` 展示了客户只依赖公共 C ABI 注册二维特征的最小实现。

### 独立编译验证

```bash
cc -std=c11 -Wall -Wextra -Werror -Iinclude \
  -c examples/feature_provider/custom_feature_provider.c \
  -o /tmp/custom_feature_provider.o
```

### 代码要点

- 实现 `flow_finish` 回调：流结束时通过 `ktag_feature_output_set` 写入特征值
- 通过 `ktag_flow_view_get_packet_count` 等 Query API 读取方向、包数、包长等流信息
- `ktag_feature_descriptor` 声明特征 ID、维度、值类型
- `ktag_feature_provider_registration` 注册 Provider 和 feature 列表
- 示例只负责底层 callbacks 注册，不负责动态库加载。动态库生命周期管理由 `tools/demo` 在此机制之上实现

---

## 3. feature_plugin — 动态加载插件

`custom_feature_plugin.c` 是 Feature Provider 的动态库版本。Demo 负责加载 `.so` 文件（通过 `dlopen` 系统调用，类似 Windows 的 `LoadLibrary`）、ABI 校验和卸载顺序，插件不创建流水线线程。

### 构建

插件随 Demo 一起构建，产物位于：

```text
output/plugins/libktag_example_feature_plugin.so
```

### 配置示例

```yaml
pipeline:
  enabled: true
  plugins:
    - ../../output/plugins/libktag_example_feature_plugin.so
  schema:
    add_features:
      - customer.packet_counts
```

### 代码要点

- 插件必须通过 `KTAG_FEATURE_PLUGIN_EXPORT` 导出 `ktag_feature_plugin_query`
- `ktag_feature_plugin_query` 中校验 `host_abi_version` 与 `KTAG_FEATURE_PLUGIN_ABI_VERSION` 一致
- 填写 `ktag_feature_plugin_descriptor`：plugin_id、版本号、`register_providers` 回调
- `register_providers` 应以事务方式工作：返回失败前自行撤销本次调用已注册的 Provider
- 所有 Feature Context 和 Registry 销毁后，Demo 才会调用 `plugin_destroy` 并卸载 `.so`

---

## 4. external_consumer — 仓外 CMake 集成

这个最小工程模拟客户在 KTAG 源码树之外使用安装包。它只执行 `find_package(KTAG CONFIG REQUIRED)`，只链接以下三个公共目标：

- `KTAG::capture`
- `KTAG::feature`
- `KTAG::inference`

它复用 `examples/three_layer/three_layer_example.c`，但不会读取仓库的 include 目录，也不会链接构建树中的 target。

### 构建

```bash
# 先把 KTAG 安装到临时前缀
cmake --install build --prefix /tmp/ktag-stage

# 构建外部消费工程
cmake -S examples/external_consumer \
  -B /tmp/ktag-external-consumer-build \
  -DCMAKE_PREFIX_PATH=/tmp/ktag-stage
cmake --build /tmp/ktag-external-consumer-build --parallel 4
```

### 运行

```bash
LD_LIBRARY_PATH=/tmp/ktag-stage/lib/KDADK_APPID:$LD_LIBRARY_PATH \
  /tmp/ktag-external-consumer-build/ktag_external_consumer \
  /path/to/model_package \
  /path/to/input.pcap
```

配置、编译和运行全部成功，表示公共头文件、三个共享库和 CMake package targets 可以脱离 KTAG 构建树使用。