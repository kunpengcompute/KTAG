if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ktag_tcp_pcap_bench requires Linux")
endif()
find_package(Threads REQUIRED)
find_path(KTAG_PCAP_BENCH_ZSTD_INCLUDE NAMES zstd.h REQUIRED)
find_library(KTAG_PCAP_BENCH_ZSTD_LIB NAMES zstd REQUIRED)
get_filename_component(KTAG_PCAP_BENCH_YAML_LIBDIR "${YAML_CPP_LIB}" DIRECTORY)
get_filename_component(KTAG_PCAP_BENCH_ORT_LIBDIR "${ONNXRUNTIME_LIB}" DIRECTORY)
add_executable(ktag_tcp_pcap_bench
    main.cpp config.cpp protocol.cpp tcp_transport.cpp network_environment.cpp feature_plugins.cpp
    offline_input.cpp frontend.cpp backend.cpp online_pipeline.cpp metrics.cpp
    ../KFlowLens/pipeline/ub_feature_wire.cpp)
target_include_directories(ktag_tcp_pcap_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include ../KFlowLens/pipeline ${YAML_CPP_INCLUDE} ${KTAG_PCAP_BENCH_ZSTD_INCLUDE})
target_link_libraries(ktag_tcp_pcap_bench PRIVATE
    ktag_capture ktag_feature ktag_inference Threads::Threads ${YAML_CPP_LIB} ${KTAG_PCAP_BENCH_ZSTD_LIB} ${CMAKE_DL_LIBS})
add_dependencies(ktag_tcp_pcap_bench ktag_traffic_features)
if(KTAG_ENABLE_DPDK)
    target_compile_definitions(ktag_tcp_pcap_bench PRIVATE KTAG_TCP_BENCH_HAVE_DPDK=1)
    target_link_libraries(ktag_tcp_pcap_bench PRIVATE ktag_capture_dpdk)
endif()
set_target_properties(ktag_tcp_pcap_bench PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${KTAG_PCAP_BENCH_YAML_LIBDIR};${KTAG_PCAP_BENCH_ORT_LIBDIR}"
    INSTALL_RPATH "$ORIGIN/../${KTAG_INSTALL_LIBDIR};${KTAG_PCAP_BENCH_YAML_LIBDIR};${KTAG_PCAP_BENCH_ORT_LIBDIR}")
install(TARGETS ktag_tcp_pcap_bench RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
option(KTAG_TCP_PCAP_BENCH_BUILD_TESTS "Build independent local-PCAP functional tests" ON)
if(KTAG_TCP_PCAP_BENCH_BUILD_TESTS)
    enable_testing()
    add_executable(ktag_tcp_pcap_wire_test tests/wire_test.cpp protocol.cpp ../KFlowLens/pipeline/ub_feature_wire.cpp)
    add_executable(ktag_tcp_pcap_transport_test tests/transport_test.cpp tcp_transport.cpp protocol.cpp)
    add_executable(ktag_tcp_pcap_config_test tests/config_test.cpp config.cpp)
    add_executable(ktag_tcp_pcap_input_test tests/input_test.cpp offline_input.cpp)
    foreach(test_name wire transport config input)
        target_include_directories(ktag_tcp_pcap_${test_name}_test PRIVATE
            . ${PROJECT_SOURCE_DIR}/include ../KFlowLens/pipeline ${KTAG_PCAP_BENCH_ZSTD_INCLUDE} ${YAML_CPP_INCLUDE})
        set_target_properties(ktag_tcp_pcap_${test_name}_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        add_test(NAME ktag_tcp_pcap_${test_name} COMMAND ktag_tcp_pcap_${test_name}_test)
    endforeach()
    target_link_libraries(ktag_tcp_pcap_wire_test PRIVATE ${KTAG_PCAP_BENCH_ZSTD_LIB} Threads::Threads)
    target_link_libraries(ktag_tcp_pcap_transport_test PRIVATE ${KTAG_PCAP_BENCH_ZSTD_LIB} Threads::Threads)
    target_link_libraries(ktag_tcp_pcap_config_test PRIVATE ktag_feature ${YAML_CPP_LIB} ${KTAG_PCAP_BENCH_ZSTD_LIB})
    target_link_libraries(ktag_tcp_pcap_input_test PRIVATE ktag_capture)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ktag_tcp_pcap_frontend_protocol
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend.py $<TARGET_FILE:ktag_tcp_pcap_bench>)
        set(KTAG_TCP_PCAP_TEST_MODEL "" CACHE PATH "Optional Legacy V1 model package for real loopback test")
        if(KTAG_TCP_PCAP_TEST_MODEL)
            add_test(NAME ktag_tcp_pcap_model_loopback
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_loopback.py
                        $<TARGET_FILE:ktag_tcp_pcap_bench> ${KTAG_TCP_PCAP_TEST_MODEL})
        endif()
    endif()
endif()
