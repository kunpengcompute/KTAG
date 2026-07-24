# 三层公共 API 最小示例

该示例只包含以下公共头文件，并且只链接三个新库：

- `ktag_capture.h` / `libktag_capture.so`
- `ktag_feature.h` / `libktag_feature.so`
- `ktag_inference.h` / `libktag_inference.so`

构建仓库后执行：

```bash
cd output
./ktag_three_layer_example \
  ../result/legacy_model_package \
  /path/to/input.pcap
```

示例采用 `KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT`，因此可以直接使用从旧
351 维模型转换出的模型包。若改用 `MODERN` 生命周期，应使用同样生命周期
重新提取的数据训练模型。

客户应用可以在示例的同步调用之上自行增加线程和队列。三个库不会创建
Pipeline Worker，也不依赖 demo 的队列实现。
