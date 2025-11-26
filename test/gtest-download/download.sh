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

if [ -d ${kdadk_appid_test}/googletest-release-1.8.0 ]
then
    echo -e "\033[32m The googletest-release-1.8.0 directory already exists and does not need to be downloaded. \033[0m"
    exit 0
fi

if [ -d ~/.ArtGet/conf/Setting.xml ]
then
    nohup artget config
fi

cmc_password=encryption:ETMsDgAAAY4S7gUEABFBRVMvR0NNL05vUGFkZGluZwCAABAAELcAVe9+GU/ik188ZHnG3fQAAAAuBknEnEdt8fAqjCslt31HDGdpmipHvAi+10X4PkHNVs6ojhO7fkXMFMEbyuY7KwAU/mcPNq9Ed+fdPzrUqwHtDWTtRfM= && sudo sed -i "/<password>/,/<\/password>/s#<password>.*</password>#<password>${cmc_password}</password>#g" ~/.ArtGet/conf/Setting.xml
sudo sed -i "/<userName>/,/<\/userName>/s#<userName>.*</userName>#<userName>pDriverCI</userName>#g" ~/.ArtGet/conf/Setting.xml
sudo sed -i "/<agentPath>/,/<\/agentPath>/s#<agentPath>.*</agentPath>#<agentPath>${kdadk_appid_test}</agentPath>#g" ~/.ArtGet/conf/Setting.xml

artget pull -d download.xml

unzip -d ${kdadk_appid_test} googletest-release-1.8.0.zip