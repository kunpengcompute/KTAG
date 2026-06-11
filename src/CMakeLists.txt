if(NOT KDADK_APPID_MODULES_PATH)
set(KDADK_APPID_MODULES_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
endif(NOT KDADK_APPID_MODULES_PATH)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# ============= 生成特征提取动态链接库 =============
add_library(KDADK_feature_extract SHARED
    ${PROJECT_SOURCE_DIR}/src/decode.cpp
    ${PROJECT_SOURCE_DIR}/src/flow_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/feature_extract.cpp
    ${PROJECT_SOURCE_DIR}/src/domain_name_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/process_feature.cpp
    ${PROJECT_SOURCE_DIR}/src/kdadk_feature_extract.cpp
)

target_compile_definitions(KDADK_feature_extract PRIVATE KDADK_BUILDING_DLL)

target_link_libraries(KDADK_feature_extract PRIVATE pcap)

target_include_directories(KDADK_feature_extract PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
)

# ============= 生成 ONNX 推理动态链接库 =============
add_library(KDADK_inference SHARED
    ${PROJECT_SOURCE_DIR}/src/utils.cpp
    ${PROJECT_SOURCE_DIR}/src/kdadk_inference.cpp
)

target_compile_definitions(KDADK_inference PRIVATE KDADK_BUILDING_DLL)

# 直接使用库文件路径，不使用 -l 链接
target_link_libraries(KDADK_inference PRIVATE 
    ${ONNXRUNTIME_LIB}
    ${YAML_CPP_LIB}
)

target_include_directories(KDADK_inference PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
    ${YAML_CPP_INCLUDE}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

set_target_properties(KDADK_inference PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ============= 生成动态库(可选) =============
if(ENABLE_BUILD_LIB)
    add_library(kdadk_appidlib SHARED
        ${PROJECT_SOURCE_DIR}/src/getProductVersion.c
    )

    target_compile_definitions(kdadk_appidlib PRIVATE KDADK_BUILDING_DLL)

    target_link_libraries(kdadk_appidlib PRIVATE
        pcap
        KDADK_feature_extract
        KDADK_inference
        ${ONNXRUNTIME_LIB} 
    )

    target_include_directories(kdadk_appidlib PRIVATE
        ${PROJECT_SOURCE_DIR}/
        ${PROJECT_SOURCE_DIR}/include
        ${ONNXRUNTIME_INCLUDE_DIR}
    )
    
    set_target_properties(kdadk_appidlib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

KDADK_APPID_glob_modules(${KDADK_APPID_MODULES_PATH})
