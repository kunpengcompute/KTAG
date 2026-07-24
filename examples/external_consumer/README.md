# 仓外 CMake 消费验证

这个最小工程模拟客户在 KTAG 源码树之外使用安装包。它只执行
`find_package(KTAG CONFIG REQUIRED)`，并且只链接以下三个公共目标：

- `KTAG::capture`
- `KTAG::feature`
- `KTAG::inference`

它复用 `examples/three_layer/three_layer_example.c`，但不会读取仓库的 include
目录，也不会链接构建树中的 target。推荐先把产品安装到临时前缀，再构建：

```bash
cmake --install build --prefix /tmp/ktag-stage
cmake -S examples/external_consumer \
  -B /tmp/ktag-external-consumer-build \
  -DCMAKE_PREFIX_PATH=/tmp/ktag-stage
cmake --build /tmp/ktag-external-consumer-build --parallel 4
```

运行：

```bash
LD_LIBRARY_PATH=/tmp/ktag-stage/lib/KDADK_APPID:$LD_LIBRARY_PATH \
  /tmp/ktag-external-consumer-build/ktag_external_consumer \
  /path/to/model_package \
  /path/to/input.pcap
```

配置、编译和运行全部成功，表示公共头文件、三个共享库和 CMake package
targets 可以脱离 KTAG 构建树使用。
