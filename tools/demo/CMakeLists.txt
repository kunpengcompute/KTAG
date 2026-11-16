if(NOT TARGET ktag_capture OR NOT TARGET ktag_feature OR NOT TARGET ktag_inference)
    message(FATAL_ERROR "ktag_demo must be built as part of the KTAG source tree")
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
    ktag_demo
    main.cpp
    feature_session.cpp
    inference_session.cpp
    ktag_file_writer.cpp
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
target_include_directories(ktag_demo PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${YAML_CPP_INCLUDE}
)

target_link_libraries(ktag_demo PRIVATE
    ktag_capture
    ktag_feature
    ktag_inference
    ${YAML_CPP_LIB}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(KTAG_ENABLE_DPDK_UB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
    target_sources(ktag_demo PRIVATE
        pipeline/ub_ring_transport.cpp
    )
    target_compile_definitions(ktag_demo PRIVATE
        KTAG_ENABLE_DPDK_UB=1
    )
    target_link_libraries(ktag_demo PRIVATE
        PkgConfig::DPDK
    )
else()
    target_compile_definitions(ktag_demo PRIVATE
        KTAG_ENABLE_DPDK_UB=0
    )
endif()

# Optional DPDK poll-mode capture backend.
if(KTAG_ENABLE_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK_RING REQUIRED IMPORTED_TARGET libdpdk)
    target_sources(ktag_demo PRIVATE
        pipeline/dpdk_ring_handoff.cpp
    )
    target_compile_definitions(ktag_demo PRIVATE KTAG_DEMO_HAVE_DPDK=1)
    target_link_libraries(ktag_demo PRIVATE ktag_capture_dpdk)
    target_link_directories(ktag_demo PRIVATE ${DPDK_RING_LIBRARY_DIRS})
    target_link_libraries(ktag_demo PRIVATE ${DPDK_RING_LIBRARIES})
    # DPDK's -include/arch flags apply only to the ring wrapper, never to the
    # rest of the demo, which keeps its portable compiler flags.
    set_source_files_properties(pipeline/dpdk_ring_handoff.cpp PROPERTIES
        COMPILE_OPTIONS "${DPDK_RING_CFLAGS}")
endif()

# The loader consumes this example exactly like a customer-provided .so. It is
# deliberately not linked into ktag_demo.
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
add_dependencies(ktag_demo ktag_example_feature_plugin)
add_dependencies(ktag_demo ktag_traffic_features)
if(TARGET ktag_proxy_features)
    add_dependencies(ktag_demo ktag_proxy_features)
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
# demo no longer depends on the original standalone tools repository layout.
add_custom_command(TARGET ktag_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/python"
            "$<TARGET_FILE_DIR:ktag_demo>/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/tools/model_package/create_legacy_package.py"
            "$<TARGET_FILE_DIR:ktag_demo>/python/create_legacy_package.py"
    COMMENT "Copying ktag_demo Python helpers"
)

set_target_properties(ktag_demo PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${KTAG_DEMO_YAML_CPP_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN/../${KTAG_INSTALL_LIBDIR};${KTAG_DEMO_YAML_CPP_LIB_DIR}"
)

install(TARGETS ktag_example_feature_plugin
        LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}/plugins")
install(TARGETS ktag_demo
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/KTAG/python")
install(FILES
        "${PROJECT_SOURCE_DIR}/tools/model_package/create_legacy_package.py"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/KTAG/python")
