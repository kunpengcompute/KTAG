# 暂时保留的旧推理与版本查询

本目录只包含：

- `ktag_legacy_inference.cpp` 和 `utils.cpp`：固定向量旧模型推理；
- `getProductVersion.c`：产品版本查询。

旧特征提取接口与独立特征兼容库已移除。冻结特征算法位于
`plugins/traffic_features/legacy/`，统一编译到 `libktag_traffic_features.so`。
Capture、公共 Feature 和公共 Inference 层不得依赖本目录的旧推理实现。
旧推理仅通过 `ktag_legacy_types.h` 使用数据类型，不再引用旧提取器头文件。
