# 自定义 Feature Provider 示例

`custom_feature_provider.c` 展示了客户只依赖公共 C ABI 注册二维特征的最小实现。

可独立检查示例能否由纯 C 编译器编译：

```bash
cc -std=c11 -Wall -Wextra -Werror -Iinclude \
  -c examples/feature_provider/custom_feature_provider.c \
  -o /tmp/custom_feature_provider.o
```

示例只负责底层 callbacks 注册，不负责动态库加载。后续阶段会由 `tools/demo`
在此机制之上增加 `.so` 生命周期管理。
