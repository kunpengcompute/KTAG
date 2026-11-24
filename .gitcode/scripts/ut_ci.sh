#!/bin/bash
# KTAG ut 作业脚本：在工作区根目录执行（工作区即 PR 预合入代码）
set -e

echo 'hello'

yum install python -y

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
    sh buildtools/build.sh -lib
else
    sh build.sh -lib
fi
cd test/traces
wget --no-check-certificate https://boostkit-dpi.obs.cn-north-4.myhuaweicloud.com/1229/data.zip
unzip data.zip
cp -r output/*.pcap .
ls
cd ..
wget --no-check-certificate https://buildtools.obs.cn-north-4.myhuaweicloud.com/googletest-release-1.8.0.zip
unzip googletest-release-1.8.0.zip
cd build
make
make rfunc