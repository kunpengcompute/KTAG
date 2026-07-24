#! /bin/bash
##############################################################
## Copyright: Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
## @Filename: build.sh
## @Usage: sh build.sh, sh build.sh -perf, sh build.sh clean, sh build.sh install or sh build.sh uninstall
##############################################################

set -e

LIB_ROOT_PATH=$(cd "$(dirname "${0}")"; pwd -P)
source ${LIB_ROOT_PATH}/function.sh
OBJ_DIR=${LIB_ROOT_PATH}/build
OUTPUT_DIR=${LIB_ROOT_PATH}/output
INSTALL_LIB_PATH=/usr/local/lib/KDADK_APPID/
INSTALL_INCLUDE_PATH=/usr/local/include/KDADK_APPID/
DEBPACK_ROOT_PATH=${LIB_ROOT_PATH}/package/debpack
DEBPACK_INCLUDE_PATH=${DEBPACK_ROOT_PATH}/KDADK_APPID/include
DEBPACK_LIB_PATH=${DEBPACK_ROOT_PATH}/KDADK_APPID/lib

KDADK_APPID_INFO_VERSION=$(xmllint --xpath "//KDADK_APPID_INFO_VERSION/text()" ${LIB_ROOT_PATH}/dependence.xml)
KDADK_APPID_INFO_COMPONENT_NAME=$(xmllint --xpath "//KDADK_APPID_INFO_COMPONENT_NAME/text()" ${LIB_ROOT_PATH}/dependence.xml)
KDADK_APPID_INFO_SOFTWARE_NAME=$(xmllint --xpath "//KDADK_APPID_INFO_SOFTWARE_NAME/text()" ${LIB_ROOT_PATH}/dependence.xml)
KDADK_APPID_INFO_PRODUCT_VERSION=$(xmllint --xpath "//KDADK_APPID_INFO_PRODUCT_VERSION/text()" ${LIB_ROOT_PATH}/dependence.xml)
cpulimit=$(xmllint --xpath "//KDADK_APPID_CPU_CHECK_ENABLE/text()" ${LIB_ROOT_PATH}/dependence.xml)
KDADK_APPID_C_TOOLCHAIN=$(xmllint --xpath "//KDADK_APPID_C_TOOLCHAIN/text()" ${LIB_ROOT_PATH}/dependence.xml)
export CC=${KDADK_APPID_C_TOOLCHAIN}
export KDADK_APPID_CPU_CHECK_ENABLE=${cpulimit}

function kdadk_appid_clean()
{
    if [ -d "${OBJ_DIR}" ]; then
        rm -rf ${OBJ_DIR}
    fi
    if [ -d "${OUTPUT_DIR}" ]; then
        rm -rf ${OUTPUT_DIR}
    fi
}

function kdadk_appid_install()
{
    # 检查是否以超级用户权限运行
    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root"
        exit 1
    fi
    if [ ! -f "${OBJ_DIR}/CMakeCache.txt" ]; then
        echo "build directory is missing; run 'sh build.sh' first"
        exit 1
    fi
    if [ ! -d "${INSTALL_LIB_PATH}" ]; then
        mkdir -p ${INSTALL_LIB_PATH}
    fi
    if [ ! -d "${INSTALL_INCLUDE_PATH}" ]; then
        mkdir -p ${INSTALL_INCLUDE_PATH}
    fi
    cmake --install "${OBJ_DIR}" --prefix /usr/local

    MARK="# Added by KDADK_APPID install script"
    if ! grep -q "$MARK" /etc/profile; then
    {
        echo "$MARK"
        echo "export LD_LIBRARY_PATH=\$LD_LIBRARY_PATH:${INSTALL_LIB_PATH}"
        echo "export C_INCLUDE_PATH=\$C_INCLUDE_PATH:${INSTALL_INCLUDE_PATH}"
        echo "export CPLUS_INCLUDE_PATH=\$CPLUS_INCLUDE_PATH:${INSTALL_INCLUDE_PATH}"
    } >> /etc/profile
        echo "Added environment variables to /etc/profile."
    else
        echo "The marker already exists in /etc/profile."
    fi
    # 使修改立即生效
    source /etc/profile
}

function kdadk_appid_uninstall()
{
    kdadk_appid_clean
    rm -rf \
        "${INSTALL_INCLUDE_PATH}" \
        "${INSTALL_LIB_PATH}" \
        /usr/local/lib/cmake/KTAG \
        /usr/local/share/KTAG
    rm -f /usr/local/bin/kdadk_demo /usr/local/bin/ktag_three_layer_example
    # 删除安装脚本写入的环境变量。
    while grep -q "# Added by KDADK_APPID install script" /etc/profile; do
        sed -i "/# Added by KDADK_APPID install script/{N;N;N;d}" /etc/profile
    done
}

PERF_OPTION="-DKTAG_ENABLE_PERF_PROFILING=OFF"

if [ "$1" == "" ] ;then
    COVERAGE_OPTION=""
    kdadk_appid_build
elif [ "$1" == "-lib" ]; then
    COVERAGE_OPTION="-DENABLE_BUILD_LIB=ON -DKDADK_ENABLE_HIDDEN_VISIBILITY=OFF"
    kdadk_appid_build
elif [ "$1" == "-sve256" ]; then
    COVERAGE_OPTION="-DENABLE_SVE_256=ON"
    kdadk_appid_build
elif [ "$1" == "-coverage" ]; then
    COVERAGE_OPTION="-DENABLE_TEST_COVERAGE=ON"
    kdadk_appid_build
elif [ "$1" == "-perf" ]; then
    COVERAGE_OPTION=""
    PERF_OPTION="-DKTAG_ENABLE_PERF_PROFILING=ON"
    kdadk_appid_build
elif [ "$1" == "clean" ] ;then
    kdadk_appid_clean
elif [ "$1" == "install" ] ;then
    kdadk_appid_install
elif [ "$1" == "uninstall" ] ;then
    kdadk_appid_uninstall
elif [ "$1" == "deb" ] ;then
    kdadk_appid_clean
    kdadk_appid_build
    deb_pack
elif [ "$1" == "util" ] ;then
    kdadk_appid_util_conf
fi
