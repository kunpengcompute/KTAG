if(NOT KDADK_APPID_MODULES_PATH)
set(KDADK_APPID_MODULES_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
endif(NOT KDADK_APPID_MODULES_PATH)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============= Capture layer =============
find_package(Threads REQUIRED)

add_library(ktag_capture SHARED
    ${PROJECT_SOURCE_DIR}/src/packet_decode_internal.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/capture.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/packet_buffer.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/flow_key.cpp
)
add_library(KTAG::capture ALIAS ktag_capture)

target_compile_definitions(ktag_capture PRIVATE KTAG_BUILDING_DLL)
target_link_libraries(ktag_capture PRIVATE pcap Threads::Threads)
target_include_directories(ktag_capture
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/capture
)
set_target_properties(ktag_capture PROPERTIES
    EXPORT_NAME capture
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# ============= Optional DPDK live capture backend =============
if(KTAG_ENABLE_DPDK)
    find_package(PkgConfig REQUIRED)
    # 24.11 is the version this backend targets, and the same one the DPDK/UB
    # distributed transport requires; a process can only link one libdpdk.
    # Checked without REQUIRED so a too-old but installed libdpdk reports its
    # actual version instead of pkg-config's generic "not found".
    pkg_check_modules(DPDK libdpdk>=24.11)
    if(NOT DPDK_FOUND)
        execute_process(
            COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion libdpdk
            OUTPUT_VARIABLE KTAG_DPDK_DETECTED_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT KTAG_DPDK_DETECTED_VERSION)
            set(KTAG_DPDK_DETECTED_VERSION "none")
        endif()
        message(FATAL_ERROR
            "ktag_capture_dpdk requires DPDK 24.11 or later, but pkg-config "
            "reports libdpdk ${KTAG_DPDK_DETECTED_VERSION}.\n"
            "Point PKG_CONFIG_PATH at a 24.11+ installation "
            "(the DPDK/UB transport needs the same version), or configure "
            "with -DKTAG_ENABLE_DPDK=OFF.")
    endif()

    add_library(ktag_capture_dpdk SHARED
        ${PROJECT_SOURCE_DIR}/src/capture_dpdk/dpdk_capture.cpp
    )
    add_library(KTAG::capture_dpdk ALIAS ktag_capture_dpdk)

    target_compile_definitions(ktag_capture_dpdk PRIVATE KTAG_BUILDING_DLL)

    target_link_libraries(ktag_capture_dpdk PRIVATE
        ktag_capture
        Threads::Threads
    )

    target_include_directories(ktag_capture_dpdk
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${PROJECT_SOURCE_DIR}/src/capture_dpdk
        ${DPDK_INCLUDE_DIRS}
    )

    # DPDK requires -include rte_config.h and an arch-specific -march; these
    # are applied only to this target's translation units so the rest of the
    # project keeps its portable compiler flags.
    target_compile_options(ktag_capture_dpdk PRIVATE ${DPDK_CFLAGS_OTHER})
    target_link_directories(ktag_capture_dpdk PRIVATE ${DPDK_LIBRARY_DIRS})
    target_link_libraries(ktag_capture_dpdk PRIVATE ${DPDK_LIBRARIES})

    set_target_properties(ktag_capture_dpdk PROPERTIES
        EXPORT_NAME capture_dpdk
        BUILD_RPATH "$ORIGIN;${DPDK_LIBRARY_DIRS}"
        INSTALL_RPATH "$ORIGIN;${DPDK_LIBRARY_DIRS}"
    )

    install(TARGETS ktag_capture_dpdk
            EXPORT KTAGTargets
            LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}"
            ARCHIVE DESTINATION "${KTAG_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${KTAG_INSTALL_LIBDIR}")
endif()

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
add_library(ktag_feature SHARED
    ${PROJECT_SOURCE_DIR}/src/packet_decode_internal.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/dns_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/feature_schema.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/feature_batch.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/traffic_schema.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/provider_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/feature_view.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/feature_pipeline.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/domain_store.cpp
    ${PROJECT_SOURCE_DIR}/src/feature/feature_context.cpp
)
add_library(KTAG::feature ALIAS ktag_feature)

target_compile_definitions(ktag_feature PRIVATE
    KTAG_BUILDING_DLL
    KTAG_FEATURE_INTERNAL_BUILD
)

target_link_libraries(ktag_feature PRIVATE
    ktag_capture
    pcap
    Threads::Threads
)

target_include_directories(ktag_feature
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(ktag_feature PROPERTIES
    EXPORT_NAME feature
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Established fixed-vector public API. Its extraction entry points execute the
# same Traffic Providers as the Schema API. The old low-level symbols remain
# in this target temporarily so their independent removal can be reviewed and
# tested without changing the demo-facing API at the same time.
set(KTAG_FIXED_VECTOR_FEATURE_SOURCES
    ${PROJECT_SOURCE_DIR}/src/capture/dns_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/decode.cpp
    ${PROJECT_SOURCE_DIR}/src/flow_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/feature_extract.cpp
    ${PROJECT_SOURCE_DIR}/src/domain_name_hash.cpp
    ${PROJECT_SOURCE_DIR}/src/legacy/process_feature.cpp
    ${PROJECT_SOURCE_DIR}/src/kdadk_feature_extract.cpp
)
add_library(KDADK_feature_extract SHARED
    ${KTAG_FIXED_VECTOR_FEATURE_SOURCES}
)

target_compile_definitions(KDADK_feature_extract PRIVATE KDADK_BUILDING_DLL)

target_link_libraries(KDADK_feature_extract PRIVATE
    ktag_feature
    pcap
    Threads::Threads
)

target_include_directories(KDADK_feature_extract
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/
)
set_target_properties(KDADK_feature_extract PROPERTIES
    EXPORT_NAME feature_compat
    BUILD_RPATH "$ORIGIN;$ORIGIN/plugins"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/plugins"
)

# ============= Schema-aware inference layer =============
add_library(ktag_inference SHARED
    ${PROJECT_SOURCE_DIR}/src/inference/model_contract.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/prediction_batch.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/inference_context.cpp
)
add_library(KTAG::inference ALIAS ktag_inference)

target_compile_definitions(ktag_inference PRIVATE KTAG_BUILDING_DLL)

target_link_libraries(ktag_inference PRIVATE
    ktag_feature
    ${ONNXRUNTIME_LIB}
    ${YAML_CPP_LIB}
    Threads::Threads
)

target_include_directories(ktag_inference
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
    ${YAML_CPP_INCLUDE}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

set_target_properties(ktag_inference PROPERTIES
    EXPORT_NAME inference
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${YAML_CPP_LIB_DIR};${ONNXRUNTIME_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN;${YAML_CPP_LIB_DIR};${ONNXRUNTIME_LIB_DIR}"
)

# ============= Deprecated fixed-vector inference adapter =============
set(KTAG_LEGACY_INFERENCE_ADAPTER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/legacy/utils.cpp
    ${PROJECT_SOURCE_DIR}/src/legacy/kdadk_inference.cpp
)
add_library(KDADK_inference SHARED
    ${KTAG_LEGACY_INFERENCE_ADAPTER_SOURCES}
)

target_compile_definitions(KDADK_inference PRIVATE KDADK_BUILDING_DLL)

# 直接使用库文件路径，不使用 -l 链接
target_link_libraries(KDADK_inference PRIVATE 
    ${ONNXRUNTIME_LIB}
    ${YAML_CPP_LIB}
)

target_include_directories(KDADK_inference
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${KTAG_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${PROJECT_SOURCE_DIR}/
    ${YAML_CPP_INCLUDE}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

set_target_properties(KDADK_inference PROPERTIES
    EXPORT_NAME inference_compat
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "${YAML_CPP_LIB_DIR};${ONNXRUNTIME_LIB_DIR}"
    INSTALL_RPATH "${YAML_CPP_LIB_DIR};${ONNXRUNTIME_LIB_DIR}"
)

# ============= 生成动态库(可选) =============
if(ENABLE_BUILD_LIB)
    add_library(kdadk_appidlib SHARED
        ${PROJECT_SOURCE_DIR}/src/legacy/getProductVersion.c
    )

    target_compile_definitions(kdadk_appidlib PRIVATE KDADK_BUILDING_DLL)

    target_link_libraries(kdadk_appidlib PRIVATE
        pcap
        ktag_feature
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
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
    if(KTAG_INSTALL_LEGACY_COMPAT)
        install(TARGETS kdadk_appidlib
                LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}"
                ARCHIVE DESTINATION "${KTAG_INSTALL_LIBDIR}"
                RUNTIME DESTINATION "${KTAG_INSTALL_LIBDIR}")
    endif()
endif()

install(TARGETS ktag_capture ktag_feature ktag_inference
        EXPORT KTAGTargets
        LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${KTAG_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${KTAG_INSTALL_LIBDIR}")

if(KTAG_INSTALL_LEGACY_COMPAT)
    install(TARGETS KDADK_feature_extract KDADK_inference
            EXPORT KTAGTargets
            LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}"
            ARCHIVE DESTINATION "${KTAG_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${KTAG_INSTALL_LIBDIR}")
endif()

KDADK_APPID_glob_modules(${KDADK_APPID_MODULES_PATH})
