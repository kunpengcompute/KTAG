# 编译指南

## 获取项目代码

克隆KTAG代码仓库到`/opt`目录下（这里以`/opt`目录为例，后续操作均在`/opt`目录下进行，或者可以选择其他自定义目录）：

```bash
cd /opt
git clone https://gitcode.com/boostkit/KTAG.git
cd /opt/KTAG
```

## 依赖准备

### 1. 安装yaml-cpp

下载源码包[yaml-cpp](https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz)。

将下载好的源码包放在`/opt/KTAG/util/`目录下，然后执行以下命令：

```bash
cd /opt/KTAG/util
tar -xzf yaml-cpp-0.8.0.tar.gz
cd yaml-cpp-0.8.0
mkdir -p build
cd build
cmake .. -DYAML_BUILD_SHARED_LIBS=ON
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

下载源码包[onnxruntime](https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-aarch64-1.22.0.tgz)。

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

完整构建会同时生成三层公共库、旧接口兼容库、demo 和 Feature Plugin。主要产物如下：

```
/opt/KTAG/output/libktag_capture.so
/opt/KTAG/output/libktag_feature.so
/opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/libktag_feature_compat.so
/opt/KTAG/output/libktag_inference_compat.so
/opt/KTAG/output/ktag_demo
/opt/KTAG/output/plugins/libktag_example_feature_plugin.so
/opt/KTAG/output/plugins/libktag_proxy_features.so
```

`libktag_feature_compat.so` 和 `libktag_inference_compat.so` 用于兼容旧固定向量接口；新接入推荐使用 Capture、Feature、Inference 三层公共库。代理分类需要构建并加载 `libktag_proxy_features.so`。

相关构建选项：

```bash
cmake -S /opt/KTAG -B /opt/KTAG/build \
  -DKTAG_BUILD_TOOLS=ON \
  -DKTAG_BUILD_PROXY_FEATURES=ON \
  -DKTAG_INSTALL_LEGACY_COMPAT=ON
cmake --build /opt/KTAG/build
```

- `KTAG_BUILD_TOOLS`：构建 `ktag_demo` 和示例插件。
- `KTAG_BUILD_PROXY_FEATURES`：构建正式代理 Feature Plugin，默认开启。
- `KTAG_INSTALL_LEGACY_COMPAT`：安装固定向量兼容库、头文件和依赖兼容层的 demo，默认开启。

## 命名迁移与安装布局

项目中的头文件、接口、构建变量、组件名称和命令行程序已统一使用 KTAG 命名。
固定向量兼容层保留原有数据布局与处理行为，但不再提供早期名称的别名。
升级现有项目时，需要更新头文件引用、类型和版本查询函数、链接库、CMake 参数、
环境变量及运行脚本，并重新编译。Python 训练和评估入口分别为 `KTAG_Train` 和
`KTAG_Evaluation`。

使用 `sh /opt/KTAG/build.sh install` 安装时，主要路径为：

```text
/usr/local/bin/ktag_demo
/usr/local/lib/KTAG_APPID/
/usr/local/include/KTAG_APPID/
/usr/local/lib/cmake/KTAG/
/usr/local/share/KTAG/python/
```

仓外 CMake 工程继续通过 `find_package(KTAG CONFIG REQUIRED)` 和 `KTAG::capture`、
`KTAG::feature`、`KTAG::inference` 集成。固定向量兼容库对应
`KTAG::feature_compat` 和 `KTAG::inference_compat`。

新安装不会自动清理早期版本的安装目录及环境配置。已安装早期版本时，应在切换
源码前使用对应版本的卸载入口清理原安装，再构建并安装当前版本。

应用与代理分类的配置和运行命令参见《[用户指南](user_guide.md)》。
