cmake_minimum_required(VERSION 3.16.3)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KTAGUbBytesExample LANGUAGES CXX)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The UB example requires Linux and the deployed DPDK/UB SDK")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(KTAG_UB_EXAMPLE_DPDK REQUIRED IMPORTED_TARGET libdpdk>=24.11)
find_path(KTAG_UB_EXAMPLE_INCLUDE NAMES rte_ub_ring.h
    HINTS ${KTAG_UB_EXAMPLE_DPDK_INCLUDE_DIRS})
if(NOT KTAG_UB_EXAMPLE_INCLUDE)
    message(FATAL_ERROR "rte_ub_ring.h not found: use the deployed DPDK/UB SDK; upstream DPDK alone is insufficient")
endif()
get_filename_component(KTAG_UB_ADAPTER_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/KFlowLens/pipeline" ABSOLUTE)

add_executable(ktag_ub_bytes_example main.cpp
    "${KTAG_UB_ADAPTER_DIR}/ub_ring_transport.cpp")
target_include_directories(ktag_ub_bytes_example PRIVATE
    "${KTAG_UB_ADAPTER_DIR}" "${KTAG_UB_EXAMPLE_INCLUDE}")
target_link_libraries(ktag_ub_bytes_example PRIVATE
    PkgConfig::KTAG_UB_EXAMPLE_DPDK Threads::Threads)
set_target_properties(ktag_ub_bytes_example PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "${KTAG_UB_EXAMPLE_DPDK_LIBRARY_DIRS}")
