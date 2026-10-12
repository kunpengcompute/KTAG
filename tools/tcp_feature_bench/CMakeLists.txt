if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ktag_tcp_bench requires Linux (ordinary sockets, libpcap, tcpreplay)")
endif()
find_package(Threads REQUIRED)
find_path(KTAG_TCP_ZSTD_INCLUDE NAMES zstd.h REQUIRED)
find_library(KTAG_TCP_ZSTD_LIB NAMES zstd REQUIRED)
find_path(KTAG_TCP_PCAP_INCLUDE NAMES pcap/pcap.h REQUIRED)
find_library(KTAG_TCP_PCAP_LIB NAMES pcap REQUIRED)
get_filename_component(KTAG_TCP_YAML_LIBDIR "${YAML_CPP_LIB}" DIRECTORY)
get_filename_component(KTAG_TCP_ORT_LIBDIR "${ONNXRUNTIME_LIB}" DIRECTORY)
add_executable(ktag_tcp_bench
    main.cpp config.cpp protocol.cpp tcp_transport.cpp network_environment.cpp
    live_capture.cpp replay_controller.cpp frontend.cpp backend.cpp metrics.cpp
    ../demo/pipeline/ub_feature_wire.cpp
)
target_include_directories(ktag_tcp_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include ../demo/pipeline
    ${YAML_CPP_INCLUDE} ${KTAG_TCP_ZSTD_INCLUDE} ${KTAG_TCP_PCAP_INCLUDE}
)
target_link_libraries(ktag_tcp_bench PRIVATE
    ktag_capture ktag_feature ktag_inference Threads::Threads
    ${YAML_CPP_LIB} ${KTAG_TCP_ZSTD_LIB} ${KTAG_TCP_PCAP_LIB}
)
set_target_properties(ktag_tcp_bench PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${KTAG_TCP_YAML_LIBDIR};${KTAG_TCP_ORT_LIBDIR}"
    INSTALL_RPATH "$ORIGIN/../${KTAG_INSTALL_LIBDIR};${KTAG_TCP_YAML_LIBDIR};${KTAG_TCP_ORT_LIBDIR}"
)
install(TARGETS ktag_tcp_bench RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
option(KTAG_TCP_BENCH_BUILD_TESTS "Build TCP feature wire/codec tests" ON)
if(KTAG_TCP_BENCH_BUILD_TESTS)
    enable_testing()
    add_executable(ktag_tcp_wire_test tests/wire_test.cpp protocol.cpp ../demo/pipeline/ub_feature_wire.cpp)
    target_include_directories(ktag_tcp_wire_test PRIVATE
        . ${PROJECT_SOURCE_DIR}/include ../demo/pipeline ${KTAG_TCP_ZSTD_INCLUDE})
    target_link_libraries(ktag_tcp_wire_test PRIVATE ${KTAG_TCP_ZSTD_LIB} Threads::Threads)
    set_target_properties(ktag_tcp_wire_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ktag_tcp_wire COMMAND ktag_tcp_wire_test)
    add_executable(ktag_tcp_config_test tests/config_test.cpp config.cpp)
    target_include_directories(ktag_tcp_config_test PRIVATE
        . ${PROJECT_SOURCE_DIR}/include ../demo/pipeline ${YAML_CPP_INCLUDE} ${KTAG_TCP_ZSTD_INCLUDE})
    target_link_libraries(ktag_tcp_config_test PRIVATE ktag_feature ${YAML_CPP_LIB} ${KTAG_TCP_ZSTD_LIB})
    set_target_properties(ktag_tcp_config_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ktag_tcp_config COMMAND ktag_tcp_config_test)
    add_executable(ktag_tcp_transport_test tests/transport_test.cpp tcp_transport.cpp protocol.cpp)
    target_include_directories(ktag_tcp_transport_test PRIVATE
        . ${PROJECT_SOURCE_DIR}/include ../demo/pipeline ${KTAG_TCP_ZSTD_INCLUDE})
    target_link_libraries(ktag_tcp_transport_test PRIVATE ${KTAG_TCP_ZSTD_LIB} Threads::Threads)
    set_target_properties(ktag_tcp_transport_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ktag_tcp_transport COMMAND ktag_tcp_transport_test)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ktag_tcp_preflight
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_preflight.py
                    $<TARGET_FILE:ktag_tcp_bench>)
    endif()
endif()
