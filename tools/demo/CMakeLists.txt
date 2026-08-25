if(NOT TARGET ktag_capture OR NOT TARGET ktag_feature OR
   NOT TARGET KDADK_feature_extract OR
   NOT TARGET ktag_inference OR NOT TARGET KDADK_inference)
    message(FATAL_ERROR "kdadk_demo must be built as part of the KTAG source tree")
endif()

find_package(Threads REQUIRED)

# src/ discovers yaml-cpp first, but keep the demo independently diagnosable
# when it is embedded by another build.
if(NOT YAML_CPP_INCLUDE)
    find_path(YAML_CPP_INCLUDE NAMES yaml-cpp/yaml.h REQUIRED)
endif()
if(NOT YAML_CPP_LIB)
    find_library(YAML_CPP_LIB NAMES yaml-cpp REQUIRED)
endif()
get_filename_component(
    KTAG_DEMO_YAML_CPP_LIB_DIR "${YAML_CPP_LIB}" DIRECTORY
)

# 可执行文件
add_executable(
    kdadk_demo
    main.cpp
    kdadk_file_writer.cpp
    online_inference.cpp
    utils.cpp
    pipeline/pipeline_config.cpp
    pipeline/pipeline_runner.cpp
    pipeline/schema_feature_writer.cpp
    pipeline/ub_feature_wire.cpp
    pipeline/demo_capture_source.cpp
    pipeline/demo_capture_dpdk_source.cpp
    plugin/feature_plugin_loader.cpp
)

# 链接库
target_include_directories(kdadk_demo PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${YAML_CPP_INCLUDE}
)

target_link_libraries(kdadk_demo PRIVATE
    ktag_capture
    ktag_feature
    KDADK_feature_extract
    ktag_inference
    KDADK_inference
    ${YAML_CPP_LIB}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(KTAG_ENABLE_DPDK_UB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
    target_sources(kdadk_demo PRIVATE
        pipeline/ub_ring_transport.cpp
    )
    target_compile_definitions(kdadk_demo PRIVATE
        KTAG_ENABLE_DPDK_UB=1
    )
    target_link_libraries(kdadk_demo PRIVATE
        PkgConfig::DPDK
    )
else()
    target_compile_definitions(kdadk_demo PRIVATE
        KTAG_ENABLE_DPDK_UB=0
    )
endif()

# Optional DPDK poll-mode capture backend.
if(KTAG_ENABLE_DPDK)
    target_compile_definitions(kdadk_demo PRIVATE KTAG_DEMO_HAVE_DPDK=1)
    target_link_libraries(kdadk_demo PRIVATE ktag_capture_dpdk)
endif()

# The demo intentionally exercises both the new three-layer API and the
# retained legacy command path; customer builds still see deprecation warnings.
target_compile_definitions(kdadk_demo PRIVATE
    KTAG_SUPPRESS_DEPRECATED_WARNINGS
)

# The loader consumes this example exactly like a customer-provided .so. It is
# deliberately not linked into kdadk_demo.
add_library(ktag_example_feature_plugin SHARED
    ${PROJECT_SOURCE_DIR}/examples/feature_plugin/custom_feature_plugin.c
)
target_include_directories(ktag_example_feature_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(ktag_example_feature_plugin PRIVATE
    ktag_feature
)
set_target_properties(ktag_example_feature_plugin PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/output/plugins"
    BUILD_RPATH "$ORIGIN/.."
    INSTALL_RPATH "$ORIGIN/.."
)
add_dependencies(kdadk_demo ktag_example_feature_plugin)
if(TARGET ktag_proxy_features)
    add_dependencies(kdadk_demo ktag_proxy_features)
endif()

if(ENABLE_BUILD_LIB)
    function(ktag_add_plugin_fixture target source)
        add_library(${target} SHARED ${source})
        target_include_directories(${target} PRIVATE
            ${PROJECT_SOURCE_DIR}/include
        )
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/output/plugins"
            BUILD_RPATH "$ORIGIN/.."
            INSTALL_RPATH "$ORIGIN/.."
        )
    endfunction()

    ktag_add_plugin_fixture(
        ktag_test_feature_plugin_bad_abi
        ${PROJECT_SOURCE_DIR}/test/common/fixtures/feature_plugin_bad_abi.c
    )
    ktag_add_plugin_fixture(
        ktag_test_feature_plugin_create_fail
        ${PROJECT_SOURCE_DIR}/test/common/fixtures/feature_plugin_create_fail.c
    )
    ktag_add_plugin_fixture(
        ktag_test_feature_plugin_no_entry
        ${PROJECT_SOURCE_DIR}/test/common/fixtures/feature_plugin_no_entry.c
    )
endif()

# Keep the Python training/evaluation helpers beside the executable so the
# demo no longer depends on the old KDADK-TOOLS repository layout.
add_custom_command(TARGET kdadk_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/python"
            "$<TARGET_FILE_DIR:kdadk_demo>/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/tools/model_package/create_legacy_package.py"
            "$<TARGET_FILE_DIR:kdadk_demo>/python/create_legacy_package.py"
    COMMENT "Copying kdadk_demo Python helpers"
)

set_target_properties(kdadk_demo PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${KTAG_DEMO_YAML_CPP_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN/../${KTAG_INSTALL_LIBDIR};${KTAG_DEMO_YAML_CPP_LIB_DIR}"
)

install(TARGETS ktag_example_feature_plugin
        LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}/plugins")
if(KTAG_INSTALL_LEGACY_COMPAT)
    # The demo intentionally retains --pipeline-mode legacy, so installing it
    # also requires the compatibility libraries.
    install(TARGETS kdadk_demo
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/KTAG/python")
    install(FILES
            "${PROJECT_SOURCE_DIR}/tools/model_package/create_legacy_package.py"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/KTAG/python")
endif()
