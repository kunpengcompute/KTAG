<a id="编译指南"></a>

# KTAG安装指南

本文介绍KTAG在Linux/AArch64环境下的源码编译安装方法。

## 安装前准备

确认环境满足[检查安装环境](#检查安装环境)中的要求。

## 安装KTAG

1. 按照[准备源码](#准备源码)获取项目代码。
2. 按照[安装依赖](#安装依赖)准备yaml-cpp、ONNX Runtime和Python环境。
3. 按照[执行源码编译](#执行源码编译)完成构建。
4. 按照[安装后配置与验证](#安装后配置与验证)核对构建产物，并查看运行指导。

# 安装介绍

## 概述

KTAG用于对网络流量组流、提取特征并执行模型分类，功能范围见《[用户指南](user_guide.md#ktag-简介)》。

本文包含环境准备、依赖安装、源码编译、构建产物说明和升级操作。下文以 `/opt/KTAG` 为项目目录。

## 安装方式选择

| 安装方式 | 适用人群 | 核心优势 | 选择理由 |
| :--- | :--- | :--- | :--- |
| 源码编译安装 | 需要构建或集成KTAG的用户 | 可调整插件和可选后端 | 生成公共库、ktag_demo、示例和插件 |

本指南提供源码编译流程。已有部署产物时，可直接按《[快速入门](quick_start.md)》检查并运行；软件包和镜像的交付方式不在本流程中展开。

# 源码编译安装

先检查环境并获取源码，再准备依赖和执行构建。

## 检查安装环境

版本配套表列出openEuler 22.03 LTS SP4、openEuler 24.03 LTS SP2、鲲鹏920新型号处理器及GCC 10.3.1，见《[版本说明书](release_notes.md#ZH-CN_TOPIC_0000002518245458)》。本文不据此声明其他发行版或更低编译器版本已通过验证。

| 前置项 | 要求及用途 | 检查命令 |
| :--- | :--- | :--- |
| Linux/AArch64、Bash | 在配套平台构建；脚本使用Bash语法 | uname -m、bash --version |
| Git | 获取源码时必需 | git --version |
| GCC/G++、Make | 编译器需支持C++17；配套表为GCC 10.3.1 | gcc --version、g++ --version、make --version |
| CMake | 3.16.3或更高版本 | cmake --version |
| xmllint | 读取仓库根目录的 dependence.xml | xmllint --version |
| RPM/Yum、libpcap开发包 | 脚本用RPM检查，缺失时调用Yum安装 | rpm -q libpcap libpcap-devel |
| Python 3及pip | 训练、评估时必需；依赖以 tools/demo/python/packages.txt 为准 | python3 --version、python3 -m pip --version |

先补齐缺失项再构建。可用 `yum provides '*/xmllint'` 查询本机软件源中提供该命令的软件包。若libpcap包缺失，需由管理员安装 `libpcap libpcap-devel`；系统目录写入需要root权限或sudo。代码目录需要当前用户可写。工具缺失会导致命令失败，CMake或编译器不满足要求会导致配置或编译失败。

## 准备源码

克隆KTAG代码仓库到 `/opt` 目录；请先确保当前用户可以写入该目录，也可选择其他可写目录并替换后续路径。

```bash
cd /opt
git clone https://gitcode.com/boostkit/KTAG.git
cd /opt/KTAG
```

## 安装依赖

<a id="安装-yaml-cpp"></a>

### 安装yaml-cpp

1. 下载[yaml-cpp 0.8.0源码包](https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz)，版本说明见[官方发布页](https://github.com/jbeder/yaml-cpp/releases/tag/0.8.0)。

2. 将压缩包保存为 `yaml-cpp-0.8.0.tar.gz`，放在 `/opt/KTAG/util/` 目录下，然后执行以下命令。

   ```bash
   cd /opt/KTAG/util
   tar -xzf yaml-cpp-0.8.0.tar.gz
   cd yaml-cpp-0.8.0
   mkdir -p build
   cd build
   cmake .. -DYAML_BUILD_SHARED_LIBS=ON -DYAML_CPP_BUILD_TESTS=OFF
   make -j32
   sudo make install
   ```

   `make install` 写入 `/usr/local`；上例使用sudo，root用户可省略sudo。出现下述回显且安装命令退出码为0即为成功。

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

<a id="安装-onnx-runtime"></a>

### 安装ONNX Runtime

1. 下载Linux/AArch64预编译包 [onnxruntime](https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-aarch64-1.22.0.tgz)。

2. 将下载好的预编译包放在 `/opt/KTAG/util/` 目录下，然后执行以下命令。

   ```bash
   cd /opt/KTAG/util
   tar -xzf onnxruntime-linux-aarch64-1.22.0.tgz
   export ONNXRUNTIME_HOME=/opt/KTAG/util/onnxruntime-linux-aarch64-1.22.0
   export LD_LIBRARY_PATH=$ONNXRUNTIME_HOME/lib:$LD_LIBRARY_PATH
   ```

3. 检查解压产物和环境变量。

   ```bash
   test -f "$ONNXRUNTIME_HOME/include/onnxruntime_cxx_api.h"
   ls -l "$ONNXRUNTIME_HOME"/lib/libonnxruntime.so*
   printf '%s\n' "$ONNXRUNTIME_HOME"
   ```

   预期头文件检查退出码为0，库文件存在，输出路径为 `/opt/KTAG/util/onnxruntime-linux-aarch64-1.22.0`。

`ONNXRUNTIME_HOME` 指向解压后的ONNX Runtime根目录。未设置时，构建默认查找仓库的 `util/onnxruntime-linux-aarch64-1.22.0`；安装在其他位置时必须设置。`LD_LIBRARY_PATH` 供动态链接器定位运行库。

上面的export立即作用于当前Shell及其子进程，新终端需重新执行。若需持久化，可将同样的两条export写入 `~/.bashrc`，执行 `source ~/.bashrc` 后用 `printf` 和 `ldd output/ktag_demo` 验证。版本1.22.0的获取入口见[官方发布页](https://github.com/microsoft/onnxruntime/releases/tag/v1.22.0)；本指南未声明相近版本的替换兼容性。

<a id="配置-python-环境"></a>

### 配置Python环境

离线训练和评估使用的Python依赖记录在 `/opt/KTAG/tools/demo/python/packages.txt`，请先确认Python 3和pip可用，再执行。

```bash
python3 -m pip install -r /opt/KTAG/tools/demo/python/packages.txt
```

## 执行源码编译

执行下述命令完成构建。

```bash
bash /opt/KTAG/build.sh
```

命令退出码为0且最后出现 `build success` 表示构建成功。脚本会检查主要产物及可解析的运行依赖；失败时根据终端首个错误排查，必要时保存完整输出日志。

`build.sh` 的单个可选参数如下；不传参数执行默认构建，脚本未实现 `--help`。

| 参数 | 作用 |
| :--- | :--- |
| 不传参数 | 默认构建 |
| -lib | 额外构建版本查询库及测试插件，关闭隐藏符号 |
| -sve256 | 启用SVE256构建选项，需使用匹配的硬件与工具链 |
| -coverage | 启用覆盖率选项 |
| -perf | 保留性能分析所需符号与帧指针 |
| dpdk / -dpdk | 启用DPDK收包后端 |
| clean | 删除仓库的 build/、output/ |
| install / uninstall | 安装到系统目录或卸载，需要系统目录写权限 |
| deb | 清理、构建并调用仓库的deb打包流程 |
| util | 创建构建、输出目录 |

### 自定义构建选项

如需调整构建选项，先完成上面的脚本构建，再复用同一构建目录（保留脚本从 `/opt/KTAG/dependence.xml` 传入的版本等元数据）。

```bash
cmake -S /opt/KTAG -B /opt/KTAG/build \
  -DKTAG_BUILD_TOOLS=ON \
  -DKTAG_BUILD_FEATURE_PLUGINS=ON \
  -DKTAG_BUILD_PROXY_FEATURES=ON \
  -DENABLE_BUILD_LIB=ON
cmake --build /opt/KTAG/build
```

以下CMake布尔选项以 `ON` / `OFF` 设置，默认值指新构建目录；复用目录时以缓存及本次传入值为准。

| 选项 | 默认值 | ON / OFF 的作用 |
| :--- | :--- | :--- |
| KTAG_BUILD_TOOLS | ON | 构建 / 不构建ktag_demo与示例插件；开启时依赖正式插件构建 |
| KTAG_BUILD_FEATURE_PLUGINS | ON | 构建 / 不构建正式Feature Provider插件，包含Traffic插件 |
| KTAG_BUILD_PROXY_FEATURES | ON | 在正式插件构建开启时构建 / 不构建Proxy插件；代理分类需同时加载Traffic与Proxy插件 |
| ENABLE_BUILD_LIB | 未设置，等同 OFF | 额外构建 / 不构建版本查询库 libktag_appidlib.so；bash build.sh -lib 启用 |
| KTAG_ENABLE_DPDK | OFF | 构建 / 不构建DPDK收包后端；脚本通过 dpdk 参数启用 |
| KTAG_ENABLE_DPDK_UB | OFF | 启用 / 不启用UB分布式传输；与DPDK收包开关独立 |

Traffic插件负责通用流量特征，Proxy插件增加代理特征。三层公共库分别承担采集、特征提取和推理，见《[用户指南](user_guide.md#ktag-简介)》。UB启用方式见《[DPDK/UB使用指导](ub_user_guide.md)》。

# 安装后配置与验证

## 基础验证

构建完成后，核对 `/opt/KTAG/output/` 目录下的产物。

默认构建会同时生成三层公共库、Demo、三层集成示例和Feature Plugin；Traffic与Proxy插件均默认构建。主要产物如下。

```text
/opt/KTAG/output/libktag_capture.so
/opt/KTAG/output/libktag_feature.so
/opt/KTAG/output/libktag_inference.so
/opt/KTAG/output/ktag_demo
/opt/KTAG/output/ktag_three_layer_example
/opt/KTAG/output/plugins/libktag_example_feature_plugin.so
/opt/KTAG/output/plugins/libktag_traffic_features.so
/opt/KTAG/output/plugins/libktag_proxy_features.so
```

| 产物 | 类型与用途 |
| :--- | :--- |
| libktag_capture.so / libktag_feature.so / libktag_inference.so | 三层公共动态库，供示例与客户程序链接 |
| ktag_demo | 命令行工具，运行入口为 output/ktag_demo -h |
| ktag_three_layer_example | 三层API集成示例，运行方法见[示例说明](../../examples/README.md) |
| plugins/libktag_example_feature_plugin.so | 自定义插件示例 |
| plugins/libktag_traffic_features.so / plugins/libktag_proxy_features.so | 正式特征插件，由配置指定并动态加载 |

可执行文件、公共库和插件应配套部署并保留相对布局；运行时依赖检查使用 `ldd /opt/KTAG/output/ktag_demo`，预期无 `not found`。

## 配置与运行

应用与代理分类的配置和运行命令参见《[用户指南](user_guide.md)》。

# 升级

## 升级步骤

清理会删除 `build/` 和 `output/`，其中手工放入的模型、配置或日志需先另行备份；运行中的旧任务应先停止。

从含兼容库的旧构建升级时，先执行 `bash /opt/KTAG/build.sh clean` 清理原构建目录和产物，再重新构建。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布。 |
