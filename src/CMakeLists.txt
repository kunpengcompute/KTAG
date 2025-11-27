if(NOT KDADK_APPID_MODULES_PATH)
set(KDADK_APPID_MODULES_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
endif(NOT KDADK_APPID_MODULES_PATH)

# ============= ONNX Runtime 配置 =============
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_HOME}" CACHE PATH "Root directory of ONNX Runtime installation")
if(NOT ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "${PROJECT_SOURCE_DIR}/util/onnxruntime-linux-aarch64-1.22.0")
endif()

# 查找 ONNX Runtime
find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH
    REQUIRED
)

find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_c_api.h
    PATHS ${ONNXRUNTIME_ROOT}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
    REQUIRED
)

get_filename_component(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_LIB} DIRECTORY)
message(STATUS "Found ONNX Runtime library: ${ONNXRUNTIME_LIB}")
message(STATUS "ONNX Runtime library directory: ${ONNXRUNTIME_LIB_DIR}")

# ============= yaml-cpp 配置 =============
find_path(YAML_CPP_INCLUDE
    NAMES yaml-cpp/yaml.h
    PATHS /usr/local/include /usr/include
    REQUIRED
)

find_library(YAML_CPP_LIB
    NAMES yaml-cpp
    PATHS /usr/local/lib64 /usr/lib /usr/lib64
    REQUIRED
)

get_filename_component(YAML_CPP_LIB_DIR ${YAML_CPP_LIB} DIRECTORY)
message(STATUS "Found yaml-cpp library: ${YAML_CPP_LIB}")
message(STATUS "yaml-cpp library directory: ${YAML_CPP_LIB_DIR}")

# ============= huawei安全函数配置 =============
set(SECUREC_ROOT "$ENV{SECUREC_HOME}" CACHE PATH "Root directory of SECUREC installation")
if(NOT SECUREC_ROOT)
    set(SECUREC_ROOT "${PROJECT_SOURCE_DIR}/util/libboundscheck")
endif()
find_path(SECUREC_INCLUDE
    NAMES securec.h
    PATHS ${SECUREC_ROOT}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(SECUREC_LIB
    NAMES boundscheck
    PATHS ${SECUREC_ROOT}
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH
    REQUIRED
)

get_filename_component(SECUREC_LIB_DIR ${SECUREC_LIB} DIRECTORY)
message(STATUS "Found securec library: ${SECUREC_LIB}")
message(STATUS "securec library directory: ${SECUREC_LIB_DIR}")

# ============= 生成特征提取动态链接库 =============
add_library(featureExtract SHARED
    ${PROJECT_SOURCE_DIR}/src/decode.cpp
    ${PROJECT_SOURCE_DIR}/src/flow_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/feature_extract.cpp
    ${PROJECT_SOURCE_DIR}/src/domain_name_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/process_feature.cpp
)

target_link_libraries(featureExtract PRIVATE ${SECUREC_LIB} pcap)

target_include_directories(featureExtract PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/util/libboundscheck/include
)

# ============= 生成 ONNX 推理动态链接库 =============
add_library(onnxInference SHARED
    ${PROJECT_SOURCE_DIR}/src/onnx_inference.cpp
)

# 直接使用库文件路径，不使用 -l 链接
target_link_libraries(onnxInference PRIVATE 
    ${ONNXRUNTIME_LIB}
    ${YAML_CPP_LIB}
)

target_include_directories(onnxInference PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
    ${YAML_CPP_INCLUDE}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

set_target_properties(onnxInference PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    LINK_FLAGS "-Wl,-rpath,${ONNXRUNTIME_LIB_DIR}:${YAML_CPP_LIB_DIR}:/usr/local/lib64:/usr/lib64"
)

# ============= 生成可执行文件 =============
add_executable(kdadk_appid 
    ${PROJECT_SOURCE_DIR}/src/getProductVersion.c
    ${PROJECT_SOURCE_DIR}/src/main.cpp
)

target_link_libraries(kdadk_appid PRIVATE 
    ${SECUREC_LIB} 
    pcap 
    featureExtract
    onnxInference
)

target_include_directories(kdadk_appid PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/util/libboundscheck/include
)

set_target_properties(kdadk_appid PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    LINK_FLAGS "-Wl,-rpath,${ONNXRUNTIME_LIB_DIR}:${YAML_CPP_LIB_DIR}:/usr/local/lib64:/usr/lib64:\$ORIGIN"
)

# ============= 生成动态库(可选) =============
if(ENABLE_BUILD_LIB)
    add_library(kdadk_appidlib SHARED
        ${PROJECT_SOURCE_DIR}/src/getProductVersion.c
        ${PROJECT_SOURCE_DIR}/src/decode.cpp
        ${PROJECT_SOURCE_DIR}/src/flow_hash.cpp
        ${PROJECT_SOURCE_DIR}/src/feature_extract.cpp
        ${PROJECT_SOURCE_DIR}/src/domain_name_hash.cpp
        ${PROJECT_SOURCE_DIR}/src/process_feature.cpp
    )

    target_link_libraries(kdadk_appidlib PRIVATE 
        ${SECUREC_LIB} 
        pcap
        onnxInference
    )

    target_include_directories(kdadk_appidlib PRIVATE
        ${PROJECT_SOURCE_DIR}/
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/util/libboundscheck/include
    )
    
    set_target_properties(kdadk_appidlib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

KDADK_APPID_glob_modules(${KDADK_APPID_MODULES_PATH})
