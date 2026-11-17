find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(KTAG_RX_DPDK REQUIRED IMPORTED_TARGET libdpdk>=24.11)
add_executable(ktag_rx_bench main.cpp)
target_link_libraries(ktag_rx_bench PRIVATE
    ktag_capture_dpdk ktag_capture Threads::Threads PkgConfig::KTAG_RX_DPDK)
set_target_properties(ktag_rx_bench PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN;${KTAG_RX_DPDK_LIBRARY_DIRS}")
