# 暂时保留的旧推理兼容接口

本目录只包含：

- `ktag_legacy_inference.cpp` 和 `utils.cpp`：固定向量旧模型推理。

产品与组件版本查询实现已移至 `src/core/version.c`，继续由现有的
`libktag_appidlib.so` 交付，接口名称保持不变。

旧特征提取接口与独立特征兼容库已移除。冻结特征算法位于
`plugins/traffic_features/legacy/`，统一编译到 `libktag_traffic_features.so`。
Capture、公共 Feature 和公共 Inference 层不得依赖本目录的旧推理实现。
旧推理仅通过 `ktag_legacy_types.h` 使用数据类型，不再引用旧提取器头文件。

Demo 的全部推理入口已改用 `src/inference` 的公共 Inference API，不再链接
`libktag_inference_compat.so`。本目录的旧推理实现只供仍调用旧 C 接口的外部程序使用；
关闭 `KTAG_BUILD_LEGACY_COMPAT` 和 `KTAG_INSTALL_LEGACY_COMPAT` 后仍可构建并安装 Demo。
