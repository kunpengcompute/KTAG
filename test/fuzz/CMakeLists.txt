cmake_minimum_required(VERSION 3.16.3)
project(ktag_fuzz_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fuzzing exercises the public Feature API and the dynamically loaded Traffic plugin. The
# target is not meaningful without the product libraries, so fail during
# configuration instead of creating an executable that later fails to link.
find_library(KTAG_FEATURE_LIBRARY NAMES ktag_feature PATHS
    ${CMAKE_SOURCE_DIR}/../../output
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(KTAG_INFERENCE_LIBRARY NAMES ktag_inference_compat PATHS
    ${CMAKE_SOURCE_DIR}/../../output
    NO_DEFAULT_PATH
    REQUIRED
)

message(STATUS "Found ktag_feature: ${KTAG_FEATURE_LIBRARY}")
message(STATUS "Found ktag_inference_compat: ${KTAG_INFERENCE_LIBRARY}")

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
add_executable(ktag_fuzzer ktag_fuzzer.cpp)
target_compile_definitions(ktag_fuzzer PRIVATE
    KTAG_FUZZ_FEATURE_PLUGIN="${CMAKE_SOURCE_DIR}/../../output/plugins/libktag_traffic_features.so")
target_include_directories(ktag_fuzzer PRIVATE
    ${CMAKE_SOURCE_DIR}/../../include
)
target_compile_options(ktag_fuzzer PRIVATE
    -fsanitize=fuzzer,address
    -fprofile-instr-generate
    -fcoverage-mapping
    -g
)
target_link_options(ktag_fuzzer PRIVATE
    -fsanitize=fuzzer,address
    -fprofile-instr-generate
    -fcoverage-mapping
)
target_link_libraries(ktag_fuzzer PRIVATE
    ${KTAG_FEATURE_LIBRARY}
    ${KTAG_INFERENCE_LIBRARY}
    ${ONNX_RUNTIME_LIBRARY}
    ${YAML_CPP_LIB}
)
get_filename_component(KTAG_OUTPUT_DIR ${KTAG_FEATURE_LIBRARY} DIRECTORY)
set_target_properties(ktag_fuzzer PROPERTIES
    BUILD_RPATH "${KTAG_OUTPUT_DIR};${ONNX_RUNTIME_LIB_DIR};${YAML_CPP_LIB_DIR}"
)
message(STATUS "Building LibFuzzer test")
