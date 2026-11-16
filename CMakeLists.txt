cmake_minimum_required(VERSION 3.16.3)
project(KTAG_APPID LANGUAGES C CXX ASM)

# ----------------------------------------------------------------------------
# inclue required cmake files:
# ----------------------------------------------------------------------------
include(cmake/KTAG_APPIDUtil.cmake)
include(cmake/KTAG_APPIDModule.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

string(REGEX MATCHALL "([0-9]+)" KTAG_APPID_VERSION ${KTAG_APPID_VERSION_INFO})

list(GET KTAG_APPID_VERSION 0 KTAG_APPID_VERSION_MAJOR) # Add a feature or a new library. For example, increase the value of this parameter when adding the OMP.
list(GET KTAG_APPID_VERSION 1 KTAG_APPID_VERSION_MINOR) # Increases when an interface is added.
list(GET KTAG_APPID_VERSION 2 KTAG_APPID_VERSION_PATCH) # Increases when bugs are fixed

string(TIMESTAMP BUILDDATE "%Y.%m.%d")
set(KTAG_APPID_VERSION_BUILDDATE \"${BUILDDATE}\")  # Last build time
set(KTAG_APPID_LIBVERSION "${KTAG_APPID_VERSION_INFO}")

# 为库文件添加覆盖率编译选项
if(ENABLE_TEST_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

configure_file(
        "${PROJECT_SOURCE_DIR}/ktag_appidver.h.in"
        "${PROJECT_SOURCE_DIR}/ktag_appidver.h"
)

# ----------------------------------------------------------------------------
# config the output path of the library:
# ----------------------------------------------------------------------------
set(LIBRARY_OUTPUT_PATH    "${PROJECT_SOURCE_DIR}/output")
set(CMAKE_INSTALL_PREFIX    "${PROJECT_SOURCE_DIR}/output")
message("-- The output path of lib file is        ${LIBRARY_OUTPUT_PATH}")

include(config.cmake.in)

# ----------------------------------------------------------------------------
# add_modules
# ----------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    SET(KTAG_APPID_MODULE_TYPE SHARED)
else()
    SET(KTAG_APPID_MODULE_TYPE STATIC)
endif()

if(OPTIMIZE)
    add_definitions(-DHS_OPTIMIZE)
endif()

set(EXECUTABLE_OUTPUT_PATH "${PROJECT_SOURCE_DIR}/output")
option(KTAG_BUILD_TOOLS "Build KTAG command-line tools and demos" ON)
option(KTAG_BUILD_TCP_PCAP_BENCH
       "Build the independent Linux local-PCAP TCP/Zstd demo" OFF)
option(KTAG_BUILD_EXAMPLES "Build public three-layer integration examples" ON)
option(KTAG_BUILD_FEATURE_PLUGINS
       "Build the official Feature Provider plugins"
       ON)
option(KTAG_BUILD_PROXY_FEATURES
       "Build the official proxy protocol Feature Provider plugin"
       ON)
option(KTAG_ENABLE_DPDK_UB
       "Enable the optional DPDK UB transport in ktag_demo"
       OFF)
option(KTAG_ENABLE_DPDK
       "Build the optional DPDK poll-mode live capture backend" OFF)

if((KTAG_BUILD_TOOLS OR KTAG_BUILD_TCP_PCAP_BENCH) AND NOT KTAG_BUILD_FEATURE_PLUGINS)
    message(FATAL_ERROR "KTAG tools require KTAG_BUILD_FEATURE_PLUGINS")
endif()

# Install under the unified KTAG_APPID layout and expose modern CMake imported
# targets to customers.
set(KTAG_INSTALL_LIBDIR "lib/KTAG_APPID")
set(KTAG_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/KTAG_APPID")
set(KTAG_INSTALL_CMAKEDIR "lib/cmake/KTAG")

ADD_SUBDIRECTORY(src)
if(KTAG_BUILD_FEATURE_PLUGINS)
    ADD_SUBDIRECTORY(plugins)
endif()
if(KTAG_BUILD_EXAMPLES)
    ADD_SUBDIRECTORY(examples/three_layer)
endif()
if(KTAG_BUILD_TOOLS)
    ADD_SUBDIRECTORY(tools)
else()
    if(KTAG_BUILD_TCP_PCAP_BENCH)
        ADD_SUBDIRECTORY(tools/tcp_pcap_bench)
    endif()
endif()

set(KTAG_PUBLIC_HEADERS
    include/ktag_export.h
    include/ktag_types.h
    include/ktag_capture.h
    include/ktag_capture_dpdk.h
    include/ktag_feature.h
    include/ktag_feature_plugin.h
    include/ktag_traffic_features.h
    include/ktag_inference.h
    include/ktag_core.h
    include/ktag_type.h
    include/ktag_legacy_types.h
)
install(FILES ${KTAG_PUBLIC_HEADERS}
        DESTINATION "${KTAG_INSTALL_INCLUDEDIR}")


configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/KTAGConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/KTAGConfig.cmake"
    INSTALL_DESTINATION "${KTAG_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/KTAGConfigVersion.cmake"
    VERSION "${KTAG_APPID_LIBVERSION}"
    COMPATIBILITY SameMajorVersion
)
install(EXPORT KTAGTargets
        FILE KTAGTargets.cmake
        NAMESPACE KTAG::
        DESTINATION "${KTAG_INSTALL_CMAKEDIR}")
install(FILES
        "${PROJECT_BINARY_DIR}/KTAGConfig.cmake"
        "${PROJECT_BINARY_DIR}/KTAGConfigVersion.cmake"
        DESTINATION "${KTAG_INSTALL_CMAKEDIR}")
#show KTAG_APPID modules information
string(REPLACE "KTAG_APPID_" "" KTAG_APPID_MODULES_BUILD_ST          "${KTAG_APPID_MODULES_BUILD}")
string(REPLACE "KTAG_APPID_" "" KTAG_APPID_MODULES_DISABLED_USER_ST  "${KTAG_APPID_MODULES_DISABLED_USER}")
string(REPLACE "KTAG_APPID_" "" KTAG_APPID_MODULES_DISABLED_FORCE_ST "${pORTOCL_MODULES_DISABLED_FORCE}")

set(KTAG_APPID_MODULES_DISABLED_AUTO_ST "")
foreach(m ${KTAG_APPID_MODULES_DISABLED_AUTO})
    set(__mdeps "")
    foreach(d ${KTAG_APPID_MODULE_${m}_DEPS})
        if(d MATCHES "^KTAG_APPID_" AND NOT HAVE_${d})
            list(APPEND __mdeps ${d})
        endif()
    endforeach()
    if(__mdeps)
        list(APPEND KTAG_APPID_MODULES_DISABLED_AUTO_ST "${m}(deps: ${__mdeps})")
    else()
        list(APPEND KTAG_APPID_MODULES_DISABLED_AUTO_ST "${m}")
    endif()
endforeach()

string(REPLACE "KTAG_APPID_" "" KTAG_APPID_MODULES_DISABLED_AUTO_ST  "${KTAG_APPID_MODULES_DISABLED_AUTO_ST}")

message("===================The modules build information as follwos:")
message("    Public modules:         ${KTAG_APPID_MODULES_PUBLIC}"        )
message("    To be built:            ${KTAG_APPID_MODULES_BUILD}"         )
