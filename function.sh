#!/bin/bash
##############################################################
## Copyright: Copyright (c) Huawei Technologies Co., Ltd. 2020-2021. All rights reserved.
## Build kdadk_appid lib
##############################################################

set -e

function kdadk_appid_util_conf()
{
    mkdir -p ${OBJ_DIR}
    mkdir -p ${OUTPUT_DIR}
}

function kdadk_appid_build()
{
    kdadk_appid_util_conf
    UTIL_DIR="./util"
    if [ ! -d "$UTIL_DIR" ]; then
        git clone --branch tag_Huawei_Secure_C_V100R001C01SPC017B001_00001 https://codehub-dg-y.huawei.com/hwsecurec_group/huawei_secure_c.git  ./util/huawei_secure_c
    if [ $? -ne 0 ]; then
        echo "huawei_secure_c download failed"
        exit 1
    else
        echo "huawei_secure_c download sucess"
    fi
    else
        echo "huawei_secure_c already exist"
    fi
    if ! rpm -q libpcap &>/dev/null || ! rpm -q libpcap-devel &>/dev/null; then
        yum install -y libpcap libpcap-devel
        if [ $? -ne 0 ]; then
            echo "libpcap install failed"
            exit 1
        else
            echo "libpcap install success"
        fi
    else
        echo "libpcap already exist"
    fi
    cd ${OBJ_DIR};\
    cmake ${LIB_ROOT_PATH} -DKDADK_APPID_TOOLCHAIN_INFO:STRING=${KDADK_APPID_C_TOOLCHAIN} -DKDADK_APPID_VERSION_INFO=${KDADK_APPID_INFO_VERSION}\
        -DKDADK_APPID_COMPONENT_NAME:STRING=${KDADK_APPID_INFO_COMPONENT_NAME} -DKDADK_APPID_SOFTWARE_NAME:STRING=${KDADK_APPID_INFO_SOFTWARE_NAME}\
        -DKDADK_APPID_PRODUCT_VERSION:STRING=${KDADK_APPID_INFO_PRODUCT_VERSION} ${COVERAGE_OPTION};\
    make -C ${OBJ_DIR} -j16
    echo "build success"
}

function deb_pack()
{
    rm -rf ${DEBPACK_ROOT_PATH}/KDADK_APPID
    mkdir -p ${DEBPACK_INCLUDE_PATH}
    mkdir -p ${DEBPACK_LIB_PATH}

    cp -rf ${LIB_ROOT_PATH}/include/* ${DEBPACK_INCLUDE_PATH}
    cp -rf ${OUTPUT_DIR}/* ${DEBPACK_LIB_PATH}

    sed -i "5c Standards-Version:${KDADK_APPID_INFO_VERSION}" ${DEBPACK_ROOT_PATH}/DEBIAN/control
    sed -i "6c Version:${KDADK_APPID_INFO_VERSION}" ${DEBPACK_ROOT_PATH}/DEBIAN/control
    sed -i "7c Package:${KDADK_APPID_INFO_SOFTWARE_NAME}" ${DEBPACK_ROOT_PATH}/DEBIAN/control
    dpkg-deb -b ${LIB_ROOT_PATH}/package/debpack/\
        ${LIB_ROOT_PATH}/package/output/${KDADK_APPID_INFO_SOFTWARE_NAME}-${KDADK_APPID_INFO_VERSION}.aarch64.deb
}