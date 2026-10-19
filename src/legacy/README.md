# 旧版本兼容实现

本目录中的实现文件仅为保留 KTAG 固定向量兼容边界而存在：

- `../ktag_feature_extract.cpp`、`../flow_hash.cpp`、`../rawbow_token_dfa.cpp`
  和 `../feature_extract.cpp` 实现
  `libktag_feature_compat.so`；
- `ktag_legacy_inference.cpp` 和 `utils.cpp` 实现
  `libktag_inference_compat.so`；
- `getProductVersion.c` 属于历史聚合库。

新的 Capture、Feature 和 Inference 代码不得引用这些实现文件，也不得链接上述任一
兼容库。命名迁移保留固定向量的数据布局和处理行为，调用方需要更新名称并重新
编译；适配层本身的移除仍需项目所有者明确授权。
