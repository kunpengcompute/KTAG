cmake_minimum_required(VERSION 3.16.3)
project(kdadk_fuzz_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fuzzing deliberately exercises the retained compatibility boundary. The
# target is not meaningful without the product libraries, so fail during
# configuration instead of creating an executable that later fails to link.
find_library(KDADK_FEATURE_LIBRARY NAMES KDADK_feature_extract PATHS
    ${CMAKE_SOURCE_DIR}/../../output
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(KDADK_INFERENCE_LIBRARY NAMES KDADK_inference PATHS
    ${CMAKE_SOURCE_DIR}/../../output
    NO_DEFAULT_PATH
    REQUIRED
)

message(STATUS "Found KDADK_feature_extract: ${KDADK_FEATURE_LIBRARY}")
message(STATUS "Found KDADK_inference: ${KDADK_INFERENCE_LIBRARY}")

# 查找ONNX Runtime库
find_library(ONNX_RUNTIME_LIBRARY NAMES onnxruntime PATHS
    ${CMAKE_SOURCE_DIR}/../../util/onnxruntime-linux-aarch64-1.22.0/lib
    NO_DEFAULT_PATH
    REQUIRED
)
get_filename_component(
    ONNXRUNTIME_LIB_DIR ${ONNX_RUNTIME_LIBRARY} DIRECTORY
)

find_library(YAML_CPP_LIB
    NAMES yaml-cpp
    PATHS /usr/local/lib64 /usr/lib /usr/lib64
    REQUIRED
)

get_filename_component(YAML_CPP_LIB_DIR ${YAML_CPP_LIB} DIRECTORY)
message(STATUS "Found yaml-cpp library: ${YAML_CPP_LIB}")
message(STATUS "yaml-cpp library directory: ${YAML_CPP_LIB_DIR}")

message(STATUS "Found ONNX Runtime: ${ONNX_RUNTIME_LIBRARY}")

# 构建LibFuzzer目标
add_executable(kdadk_fuzzer kdadk_fuzzer.cpp)
target_include_directories(kdadk_fuzzer PRIVATE
    ${CMAKE_SOURCE_DIR}/../../include
)
target_compile_options(kdadk_fuzzer PRIVATE
    -fsanitize=fuzzer,address
    -fprofile-instr-generate
    -fcoverage-mapping
    -g
)
target_link_options(kdadk_fuzzer PRIVATE
    -fsanitize=fuzzer,address
    -fprofile-instr-generate
    -fcoverage-mapping
)
target_link_libraries(kdadk_fuzzer PRIVATE
    ${KDADK_FEATURE_LIBRARY}
    ${KDADK_INFERENCE_LIBRARY}
    ${ONNX_RUNTIME_LIBRARY}
    ${YAML_CPP_LIB}
)
get_filename_component(KDADK_OUTPUT_DIR ${KDADK_FEATURE_LIBRARY} DIRECTORY)
set_target_properties(kdadk_fuzzer PROPERTIES
    BUILD_RPATH "${KDADK_OUTPUT_DIR};${ONNX_RUNTIME_LIB_DIR};${YAML_CPP_LIB_DIR}"
)
message(STATUS "Building LibFuzzer test")
