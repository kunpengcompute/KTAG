#!/bin/bash
# KTAG build 作业脚本：在工作区根目录执行（工作区即 PR 预合入代码）
set -e

echo 'hello'

ls

mkdir util
cd util
wget --no-check-certificate https://buildtools.obs.cn-north-4.myhuaweicloud.com/yaml-cpp-0.8.0.zip
unzip yaml-cpp-0.8.0.zip
cd yaml-cpp-0.8.0
mkdir -p build
cd build
cmake .. -DYAML_BUILD_SHARED_LIBS=ON
make -j$(nproc)
make install


cd ..
cd ..
wget --no-check-certificate https://buildtools.obs.cn-north-4.myhuaweicloud.com/onnxruntime-linux-aarch64-1.22.0.tgz
tar -xzf onnxruntime-linux-aarch64-1.22.0.tgz
export ONNXRUNTIME_HOME=$(pwd)/onnxruntime-linux-aarch64-1.22.0
echo $ONNXRUNTIME_HOME


ls
cd ..
ls
if [ -f buildtools/build.sh ]; then
    sh buildtools/build.sh
else
    sh build.sh
fi
# zip -r KDADK-APPID_1.0.zip output/
# find / -type f -name "KDADK-APPID_1.0.zip"
# ls
