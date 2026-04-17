# 编译指南

## 获取项目代码

克隆KDADK-APPID代码仓库到`/opt`目录下（这里以`/opt`目录为例，后续操作均在`/opt`目录下进行，或者可以选择其他自定义目录）：

```bash
# 进入到/opt目录下，可修改为自定义目录
cd /opt

# 克隆KDADK-APPID代码仓库
git clone https://gitcode.com/boostkit/KDADK-APPID.git

# 进入到KDADK-APPID目录下
cd KDADK-APPID
```

## 依赖准备

### 1. 安装yaml-cpp

下载源码包[yaml-cpp](https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz)。

将下载好的源码放在`KDADK-APPID/util/`文件夹下，进到`KDADK-APPID/util/yaml-cpp`。

```bash
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

将下载好的源码放在`KDADK-APPID/util/`文件夹下。

```bash
# 解压
tar -xzf onnxruntime-linux-aarch64-1.22.0.tgz

# 设置环境变量
export ONNXRUNTIME_HOME=/opt/KDADK-APPID/util/onnxruntime-linux-aarch64-1.22.0
export LD_LIBRARY_PATH=$ONNXRUNTIME_HOME/lib:$LD_LIBRARY_PATH
```

## 编译指导
在此目录下`/opt/KDADK-APPID/`执行下述命令

```
sh build.sh
```
编译得到可执行的二进制文件`libKDADK_feature_extract.so`和`libKDADK_inference.so`
回显如下：
```
[root@ceph1 output]# ls
libKDADK_feature_extract.so  libKDADK_inference.so
```