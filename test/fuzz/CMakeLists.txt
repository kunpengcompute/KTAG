cmake_minimum_required(VERSION 3.10)
project(kdadk_fuzz_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/../../util/onnxruntime-linux-aarch64-1.22.0/include
    ${CMAKE_SOURCE_DIR}/../common/include
    ${CMAKE_SOURCE_DIR}/../../include
)

# 查找KDADK库tim
find_library(KDADK_FEATURE_LIBRARY NAMES KDADK_feature_extract PATHS 
    ${CMAKE_SOURCE_DIR}/../../output
    ${CMAKE_SOURCE_DIR}/../../build/lib
    ${CMAKE_SOURCE_DIR}/../../lib
    ${CMAKE_SOURCE_DIR}/../..
)

find_library(KDADK_INFERENCE_LIBRARY NAMES KDADK_inference PATHS 
    ${CMAKE_SOURCE_DIR}/../../output
    ${CMAKE_SOURCE_DIR}/../../build/lib
    ${CMAKE_SOURCE_DIR}/../../lib
    ${CMAKE_SOURCE_DIR}/../..
)

if(NOT KDADK_FEATURE_LIBRARY OR NOT KDADK_INFERENCE_LIBRARY)
    message(WARNING "KDADK libraries not found, fuzz test will be built without KDADK")
    message(STATUS "Looking for KDADK_feature_extract in: ${CMAKE_SOURCE_DIR}/../../output")
    message(STATUS "Looking for KDADK_inference in: ${CMAKE_SOURCE_DIR}/../../output")
else()
    message(STATUS "Found KDADK_feature_extract: ${KDADK_FEATURE_LIBRARY}")
    message(STATUS "Found KDADK_inference: ${KDADK_INFERENCE_LIBRARY}")
endif()

# 强制使用LibFuzzer
set(HAS_LIBFUZZER TRUE)

# 查找ONNX Runtime库
find_library(ONNX_RUNTIME_LIBRARY NAMES onnxruntime PATHS 
    ${CMAKE_SOURCE_DIR}/../../util/onnxruntime-linux-aarch64-1.22.0/lib
    ${CMAKE_SOURCE_DIR}/../../lib
    ${CMAKE_SOURCE_DIR}/../..
)

find_library(YAML_CPP_LIB
    NAMES yaml-cpp
    PATHS /usr/local/lib64 /usr/lib /usr/lib64
    REQUIRED
)

get_filename_component(YAML_CPP_LIB_DIR ${YAML_CPP_LIB} DIRECTORY)
message(STATUS "Found yaml-cpp library: ${YAML_CPP_LIB}")
message(STATUS "yaml-cpp library directory: ${YAML_CPP_LIB_DIR}")

if(ONNX_RUNTIME_LIBRARY)
    message(STATUS "Found ONNX Runtime: ${ONNX_RUNTIME_LIBRARY}")
else()
    message(WARNING "ONNX Runtime library not found, fuzz test may fail to link")
endif()

# 构建LibFuzzer目标
add_executable(kdadk_fuzzer kdadk_fuzzer.cpp)
target_compile_options(kdadk_fuzzer PRIVATE -fsanitize=fuzzer,address -fprofile-arcs -ftest-coverage -g)
if(KDADK_FEATURE_LIBRARY AND KDADK_INFERENCE_LIBRARY)
    target_link_libraries(kdadk_fuzzer PRIVATE -fsanitize=fuzzer,address -fprofile-arcs -ftest-coverage -lgcov ${KDADK_FEATURE_LIBRARY} ${KDADK_INFERENCE_LIBRARY} ${YAML_CPP_LIB})
    if(ONNX_RUNTIME_LIBRARY)
        target_link_libraries(kdadk_fuzzer PRIVATE ${ONNX_RUNTIME_LIBRARY})
    endif()
else()
    target_link_libraries(kdadk_fuzzer PRIVATE -fsanitize=fuzzer,address -fprofile-arcs -ftest-coverage -lgcov)
endif()
message(STATUS "Building LibFuzzer test")
