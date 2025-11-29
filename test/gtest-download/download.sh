#!/bin/bash
##############################################################
## Copyright: Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
## @Filename: download.sh
## @Usage:    sh download.sh download googletest source code
##############################################################
set -e

curd=$(pwd)
kdadk_appid_test=$curd/..
root_path=$curd/../..

if [ -d "${kdadk_appid_test}/googletest-release-1.8.0" ]
then
    echo -e "\033[32m The googletest-release-1.8.0 directory already exists and is valid. \033[0m"
    exit 0
fi
# 下载googletest-release-1.8.0.zip
curl -L -k https://github.com/google/googletest/archive/refs/tags/release-1.8.0.zip -o googletest-release-1.8.0.zip

# 解压下载的ZIP包
unzip -d ${kdadk_appid_test} googletest-release-1.8.0.zip

# 删除下载的ZIP包
rm googletest-release-1.8.0.zip

echo -e "\033[32m googletest-release-1.8.0 has been successfully downloaded and extracted. \033[0m"