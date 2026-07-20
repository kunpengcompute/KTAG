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
    cmake -S "${LIB_ROOT_PATH}" -B "${OBJ_DIR}" \
        -DKDADK_APPID_TOOLCHAIN_INFO:STRING="${KDADK_APPID_C_TOOLCHAIN}" \
        -DKDADK_APPID_VERSION_INFO="${KDADK_APPID_INFO_VERSION}" \
        -DKDADK_APPID_COMPONENT_NAME:STRING="${KDADK_APPID_INFO_COMPONENT_NAME}" \
        -DKDADK_APPID_SOFTWARE_NAME:STRING="${KDADK_APPID_INFO_SOFTWARE_NAME}" \
        -DKDADK_APPID_PRODUCT_VERSION:STRING="${KDADK_APPID_INFO_PRODUCT_VERSION}" \
        -DKTAG_BUILD_TOOLS=ON \
        ${PERF_OPTION} \
        ${COVERAGE_OPTION}

    cmake --build "${OBJ_DIR}" --parallel "${BUILD_JOBS:-16}"

    if [ ! -f "${OUTPUT_DIR}/libKDADK_feature_extract.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libKDADK_feature_extract.so was not generated"
        exit 1
    fi
    if [ ! -f "${OUTPUT_DIR}/libktag_capture.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libktag_capture.so was not generated"
        exit 1
    fi
    if [ ! -f "${OUTPUT_DIR}/libktag_feature.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libktag_feature.so was not generated"
        exit 1
    fi
    if [ ! -f "${OUTPUT_DIR}/libKDADK_inference.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libKDADK_inference.so was not generated"
        exit 1
    fi
    if [ ! -f "${OUTPUT_DIR}/libktag_inference.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libktag_inference.so was not generated"
        exit 1
    fi
    if [[ "${COVERAGE_OPTION}" == *"-DENABLE_BUILD_LIB=ON"* ]] &&
       [ ! -f "${OUTPUT_DIR}/libkdadk_appidlib.so" ]; then
        echo "build failed: ${OUTPUT_DIR}/libkdadk_appidlib.so was not generated"
        exit 1
    fi
    if [ ! -x "${OUTPUT_DIR}/kdadk_demo" ]; then
        echo "build failed: ${OUTPUT_DIR}/kdadk_demo was not generated or is not executable"
        exit 1
    fi
    if [ ! -x "${OUTPUT_DIR}/ktag_three_layer_example" ]; then
        echo "build failed: three-layer customer example was not generated"
        exit 1
    fi
    if [ ! -f "${OUTPUT_DIR}/plugins/libktag_example_feature_plugin.so" ]; then
        echo "build failed: example Feature plugin was not generated"
        exit 1
    fi
    if [[ "${COVERAGE_OPTION}" == *"-DENABLE_BUILD_LIB=ON"* ]]; then
        for fixture in bad_abi create_fail no_entry; do
            if [ ! -f "${OUTPUT_DIR}/plugins/libktag_test_feature_plugin_${fixture}.so" ]; then
                echo "build failed: Feature plugin test fixture '${fixture}' was not generated"
                exit 1
            fi
        done
    fi

    if command -v ldd >/dev/null 2>&1; then
        for artifact in \
            libktag_capture.so \
            libktag_feature.so \
            libktag_inference.so \
            ktag_three_layer_example \
            kdadk_demo; do
            if ldd "${OUTPUT_DIR}/${artifact}" 2>&1 | grep -q "not found"; then
                echo "build failed: ${artifact} has unresolved runtime dependencies"
                ldd "${OUTPUT_DIR}/${artifact}"
                exit 1
            fi
        done
    fi

    if command -v readelf >/dev/null 2>&1; then
        if readelf -d "${OUTPUT_DIR}/libktag_feature.so" 2>/dev/null | \
           grep -q "libKDADK_feature_extract"; then
            echo "build failed: new Feature layer depends on its legacy adapter"
            exit 1
        fi
        if readelf -d "${OUTPUT_DIR}/libktag_inference.so" 2>/dev/null | \
           grep -qE "libKDADK_feature_extract|libKDADK_inference"; then
            echo "build failed: new Inference layer depends on a legacy adapter"
            exit 1
        fi
    fi

    echo "build success"
    echo "  library: ${OUTPUT_DIR}/libktag_capture.so"
    echo "  library: ${OUTPUT_DIR}/libktag_feature.so"
    echo "  library: ${OUTPUT_DIR}/libKDADK_feature_extract.so"
    echo "  library: ${OUTPUT_DIR}/libktag_inference.so"
    echo "  library: ${OUTPUT_DIR}/libKDADK_inference.so"
    echo "  demo:    ${OUTPUT_DIR}/kdadk_demo"
    echo "  example: ${OUTPUT_DIR}/ktag_three_layer_example"
    echo "  plugin:  ${OUTPUT_DIR}/plugins/libktag_example_feature_plugin.so"
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
