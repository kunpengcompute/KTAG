[TOC]

# KDADK-APPID
## 1. 项目介绍
KDADK-APPID是鲲鹏数据分析开发套件的网络流分类库，包含流汇聚、协议检测、特征计算、AI引擎和训练工具包，针对鲲鹏特性进行极致亲和优化，在具备对明文流量分析能力的同时，亦能对加密流量进行实时分析，弥补传统基于规则检测的DPI方案无法分析加密流量的短板，为解决业界普遍难题提供端到端参考方案。

## 2. 版本说明
**表1** 版本说明
| KDADK-APPID  | 特性  |
| ------------ | ------------ |
| 1.1.0  | 鲲鹏数据分析开发套件的网络流分类库 |

## 3. 环境部署
KDADK-APPID当前适配的处理器和操作系统为鲲鹏920系列处理器，openEuler 22.03操作系统，若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

**表2** KDADK-APPID已验证环境
| 操作系统  | CPU类型  |
| ------------ | ------------ |
| openEuler 22.03 LTS SP4  | 鲲鹏920系列处理器  |

**表3** 软件要求
| 软件名称| 版本 |
| --- | --- |
| GCC | 10.3.1 |
| CMake | 3.22.0 |
| yaml-cpp | 0.8.0 |
| onnxruntime| 1.22.0|

### 3.1 依赖准备
#### 3.1.1 安装yaml-cpp
[这里](https://github.com/jbeder/yaml-cpp/releases/tag/0.8.0)下载源码包

将下载好的源码放在`KDADK-APPID/util/`文件夹下，进到`KDADK-APPID/util/yaml-cpp`

```
# 进到yaml-cpp目录下
cd yaml-cpp

# 创建build文件夹
mkdir -p build

# 进入build
cd build

# cmake构建
cmake .. -DYAML_BUILD_SHARED_LIBS=ON

# make编译
make -j32

# install
make install
```
出现下述回显即为成功：

```
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

#### 3.1.2 安装onnxruntime
[这里](https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-aarch64-1.22.0.tgz)下载源码包

将下载好的源码放在`KDADK-APPID/util/`文件夹下

```
# 解压
tar -xzf onnxruntime-linux-aarch64-1.22.0.tgz

# 设置环境变量
export ONNXRUNTIME_HOME=/path/to/your/onnxruntime
export LD_LIBRARY_PATH=$ONNXRUNTIME_HOME/lib:$LD_LIBRARY_PATH
```

### 3.2 编译指导
在根目录下KDADK-APPID/执行下述命令

```
sh build.sh
```
编译得到可执行的二进制文件`libKDADK_feature_extract.so`和`libKDADK_inference.so`
回显如下：
```
[root@ceph1 output]# ls
libKDADK_feature_extract.so  libKDADK_inference.so
```

## 4. 测试框架

### 4.1 功能测试

本项目采用Google Test框架对KDADK-APPID中实现的方法函数进行测试，测试用例的数据来自于采集到的pcap流量文件。在进行测试前，需要将pcap流量文件放在KDADK-APPID项目的./test/traces文件夹下供测试时使用。

测试流程：

1. 首先运行KDADK-APPID项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码

```
make
```

4. 运行功能测试

```
make rfunc
```

### 4.2 性能测试

测试流程：

1. 首先运行KDADK-APPID项目下的build.sh生成动态链接库.so文件供测试使用。

```
sh build.sh -lib
```

2. 进入test文件夹下的build目录。

```
cd test/build
```

3. 编译功能测试代码

```
make
```

4. 运行功能测试

```
make rperf
```

## 5. 其他事项
如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。