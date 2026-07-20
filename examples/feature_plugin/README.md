# Feature Provider `.so` 示例

`custom_feature_plugin.c` 只通过公开 C ABI 注册 callbacks。demo 负责
`dlopen`、ABI 校验和卸载顺序，插件不创建流水线线程。

构建仓库后会生成：

```text
output/plugins/libktag_example_feature_plugin.so
```

配置示例：

```yaml
pipeline:
  enabled: true
  plugins:
    - ../../output/plugins/libktag_example_feature_plugin.so
  schema:
    add_features:
      - customer.packet_counts
```

插件必须导出 `ktag_feature_plugin_query`。`register_providers` 应当以事务
方式工作：返回失败前自行撤销本次调用已注册的 Provider。所有 Feature
Context 和 Registry 销毁后，demo 才会调用 `plugin_destroy` 并卸载 `.so`。
