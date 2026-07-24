# 旧版本兼容实现

本目录中的实现文件仅为保留 KDADK 固定向量兼容边界而存在：

- `kdadk_feature_extract.cpp` 和 `process_feature.cpp` 实现
  `libKDADK_feature_extract.so`；
- `kdadk_inference.cpp` 和 `utils.cpp` 实现
  `libKDADK_inference.so`；
- `getProductVersion.c` 属于历史聚合库。

新的 Capture、Feature 和 Inference 代码不得引用这些实现文件，也不得链接上述任一
兼容库。只有在 `docs/zh/migration_guide.md` 中的兼容移除门槛获得批准后，才可移除
这些适配层。
