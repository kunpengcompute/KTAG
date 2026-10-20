# 编译指南

## 获取项目代码

克隆KTAG代码仓库到`/opt`目录下（这里以`/opt`目录为例，后续操作均在`/opt`目录下进行，或者可以选择其他自定义目录）：

```bash
cd /opt
git clone https://gitcode.com/boostkit/KTAG.git
cd /opt/KTAG
```

## 依赖准备

以下命令面向 Linux/AArch64 环境，需要 GCC/G++（支持 C++17）、CMake 3.16.3 或更高版本、Make、`xmllint`、libpcap 及其开发头文件。`build.sh` 使用 Bash 语法，并通过 RPM/Yum 检查和安装 libpcap；请先准备相应工具和安装权限。

### 1. 安装yaml-cpp

下载源码包[yaml-cpp](https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz)。

将压缩包保存为 `yaml-cpp-0.8.0.tar.gz`，放在 `/opt/KTAG/util/` 目录下，然后执行以下命令：

```bash
cd /opt/KTAG/util
tar -xzf yaml-cpp-0.8.0.tar.gz
cd yaml-cpp-0.8.0
mkdir -p build
cd build
cmake .. -DYAML_BUILD_SHARED_LIBS=ON -DYAML_CPP_BUILD_TESTS=OFF
make -j32
make install
```

出现下述回显即为成功：

```bash
...
-- Installing: /usr/local/include/yaml-cpp/binary.h
-- Installing: /usr/local/include/yaml-cpp/anchor.h
-- Installing: /usr/local/include/yaml-cpp/eventhandler.h
-- Installing: /usr/local/include/yaml-cpp/emitterstyle.h
-- Up-to-date: /usr/local/include/yaml-cpp/contrib
-- Installing: /usr/local/include/yaml-cpp/contrib/graphbuilder.h
-- Installing: /usr/local/include/yaml-cpp/contrib/anchordict.h
-- Installing: /usr/local/include/yaml-cpp/stlemitter.h
-- Installing: /usr/local/include/yaml-cpp/ostream_wrapper.h
-- Installing: /usr/local/lib64/cmake/yaml-cpp/yaml-cpp-targets.cmake
-- Installing: /usr/local/lib64/cmake/yaml-cpp/yaml-cpp-targets-noconfig.cmake
-- Installing: /usr/local/lib64/cmake/yaml-cpp/yaml-cpp-config.cmake
-- Installing: /usr/local/lib64/cmake/yaml-cpp/yaml-cpp-config-version.cmake
-- Installing: /usr/local/lib64/pkgconfig/yaml-cpp.pc
```

### 2. 安装onnxruntime

下载 Linux/AArch64 预编译包 [onnxruntime](https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-aarch64-1.22.0.tgz)。

将下载好的源码包放在`/opt/KTAG/util/`目录下，然后执行以下命令：

```bash
cd /opt/KTAG/util
tar -xzf onnxruntime-linux-aarch64-1.22.0.tgz
export ONNXRUNTIME_HOME=/opt/KTAG/util/onnxruntime-linux-aarch64-1.22.0
export LD_LIBRARY_PATH=$ONNXRUNTIME_HOME/lib:$LD_LIBRARY_PATH
```

### 3. 配置python环境

在`/opt/KTAG/tools/demo/python/packages.txt`文件中记录了离线训练需要的python环境依赖，可以执行下述命令安装python环境依赖。

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

## 编译指导

执行下述命令完成构建。

```bash
sh /opt/KTAG/build.sh
```

默认构建会同时生成三层公共库、Demo、三层集成示例和 Feature Plugin；Traffic 与 Proxy 插件均默认构建。主要产物如下：

```
/opt/KTAG/output/libktag_capture.so
/opt/KTAG/output/libktag_feature.so
/opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/ktag_demo
/opt/KTAG/output/ktag_three_layer_example
/opt/KTAG/output/plugins/libktag_example_feature_plugin.so
/opt/KTAG/output/plugins/libktag_traffic_features.so
/opt/KTAG/output/plugins/libktag_proxy_features.so
```

从含兼容库的旧构建升级时，先执行 `sh /opt/KTAG/build.sh clean` 清理原构建目录和产物，再重新构建。

如需调整构建选项，先完成上面的脚本构建，再复用同一构建目录（保留脚本从 `dependence.xml` 传入的版本等元数据）：

```bash
cmake -S /opt/KTAG -B /opt/KTAG/build \
  -DKTAG_BUILD_TOOLS=ON \
  -DKTAG_BUILD_FEATURE_PLUGINS=ON \
  -DKTAG_BUILD_PROXY_FEATURES=ON \
  -DENABLE_BUILD_LIB=ON
cmake --build /opt/KTAG/build
```

- `KTAG_BUILD_TOOLS`：构建 `ktag_demo` 和示例插件，依赖 `KTAG_BUILD_FEATURE_PLUGINS=ON`。
- `KTAG_BUILD_FEATURE_PLUGINS`：构建正式 Feature Plugin，默认开启；Traffic 插件由此选项控制。
- `KTAG_BUILD_PROXY_FEATURES`：在启用正式插件构建时额外构建 Proxy 插件，默认开启；代理分类运行时需要同时加载 Traffic 与 Proxy 插件。
- `ENABLE_BUILD_LIB`：额外构建并安装版本查询库 `libktag_appidlib.so`；`sh build.sh -lib` 使用该选项，功能测试需要此库。
- `KTAG_ENABLE_DPDK`：启用 DPDK 收包后端，默认关闭，对应 `sh build.sh dpdk`。`KTAG_ENABLE_DPDK_UB` 独立控制 UB 传输，启用方式见《[DPDK/UB 使用指导](ub_user_guide.md)》。

应用与代理分类的配置和运行命令参见《[用户指南](user_guide.md)》。
