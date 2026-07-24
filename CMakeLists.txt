cmake_minimum_required(VERSION 3.16.3)
project(KDADK_APPID LANGUAGES C CXX ASM)

# ----------------------------------------------------------------------------
# inclue required cmake files:
# ----------------------------------------------------------------------------
include(cmake/KDADK_APPIDUtil.cmake)
include(cmake/KDADK_APPIDModule.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

string(REGEX MATCHALL "([0-9]+)" KDADK_APPID_VERSION ${KDADK_APPID_VERSION_INFO})

list(GET KDADK_APPID_VERSION 0 KDADK_APPID_VERSION_MAJOR) # Add a feature or a new library. For example, increase the value of this parameter when adding the OMP.
list(GET KDADK_APPID_VERSION 1 KDADK_APPID_VERSION_MINOR) # Increases when an interface is added.
list(GET KDADK_APPID_VERSION 2 KDADK_APPID_VERSION_PATCH) # Increases when bugs are fixed

string(TIMESTAMP BUILDDATE "%Y.%m.%d")
set(KDADK_APPID_VERSION_BUILDDATE \"${BUILDDATE}\")  # Last build time
set(KDADK_APPID_LIBVERSION "${KDADK_APPID_VERSION_INFO}")

# 为库文件添加覆盖率编译选项
if(ENABLE_TEST_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

configure_file(
        "${PROJECT_SOURCE_DIR}/kdadk_appidver.h.in"
        "${PROJECT_SOURCE_DIR}/kdadk_appidver.h"
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
    SET(KDADK_APPID_MODULE_TYPE SHARED)
else()
    SET(KDADK_APPID_MODULE_TYPE STATIC)
endif()

if(OPTIMIZE)
    add_definitions(-DHS_OPTIMIZE)
endif()

set(EXECUTABLE_OUTPUT_PATH "${PROJECT_SOURCE_DIR}/output")
option(KTAG_BUILD_TOOLS "Build KTAG command-line tools and demos" ON)
option(KTAG_BUILD_EXAMPLES "Build public three-layer integration examples" ON)
option(KTAG_INSTALL_LEGACY_COMPAT
       "Install the deprecated fixed-vector compatibility libraries and headers"
       ON)

# Keep the historical KDADK_APPID installation prefix layout while exposing
# modern CMake imported targets to customers.
set(KTAG_INSTALL_LIBDIR "lib/KDADK_APPID")
set(KTAG_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/KDADK_APPID")
set(KTAG_INSTALL_CMAKEDIR "lib/cmake/KTAG")

ADD_SUBDIRECTORY(src)
if(KTAG_BUILD_EXAMPLES)
    ADD_SUBDIRECTORY(examples/three_layer)
endif()
if(KTAG_BUILD_TOOLS)
    ADD_SUBDIRECTORY(tools)
endif()

set(KTAG_PUBLIC_HEADERS
    include/ktag_export.h
    include/ktag_types.h
    include/ktag_capture.h
    include/ktag_feature.h
    include/ktag_feature_plugin.h
    include/ktag_inference.h
)
install(FILES ${KTAG_PUBLIC_HEADERS}
        DESTINATION "${KTAG_INSTALL_INCLUDEDIR}")

# The compatibility headers expose historical implementation types and have a
# wider include dependency closure. Preserve that closure while the adapter is
# retained; new integrations should use only the six ktag_*.h headers above.
if(KTAG_INSTALL_LEGACY_COMPAT)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
            DESTINATION "${KTAG_INSTALL_INCLUDEDIR}"
            FILES_MATCHING PATTERN "*.h")
endif()

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/KTAGConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/KTAGConfig.cmake"
    INSTALL_DESTINATION "${KTAG_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/KTAGConfigVersion.cmake"
    VERSION "${KDADK_APPID_LIBVERSION}"
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
#show KDADK_APPID modules information
string(REPLACE "KDADK_APPID_" "" KDADK_APPID_MODULES_BUILD_ST          "${KDADK_APPID_MODULES_BUILD}")
string(REPLACE "KDADK_APPID_" "" KDADK_APPID_MODULES_DISABLED_USER_ST  "${KDADK_APPID_MODULES_DISABLED_USER}")
string(REPLACE "KDADK_APPID_" "" KDADK_APPID_MODULES_DISABLED_FORCE_ST "${pORTOCL_MODULES_DISABLED_FORCE}")

set(KDADK_APPID_MODULES_DISABLED_AUTO_ST "")
foreach(m ${KDADK_APPID_MODULES_DISABLED_AUTO})
    set(__mdeps "")
    foreach(d ${KDADK_APPID_MODULE_${m}_DEPS})
        if(d MATCHES "^KDADK_APPID_" AND NOT HAVE_${d})
            list(APPEND __mdeps ${d})
        endif()
    endforeach()
    if(__mdeps)
        list(APPEND KDADK_APPID_MODULES_DISABLED_AUTO_ST "${m}(deps: ${__mdeps})")
    else()
        list(APPEND KDADK_APPID_MODULES_DISABLED_AUTO_ST "${m}")
    endif()
endforeach()

string(REPLACE "KDADK_APPID_" "" KDADK_APPID_MODULES_DISABLED_AUTO_ST  "${KDADK_APPID_MODULES_DISABLED_AUTO_ST}")

message("===================The modules build information as follwos:")
message("    Public modules:         ${KDADK_APPID_MODULES_PUBLIC}"        )
message("    To be built:            ${KDADK_APPID_MODULES_BUILD}"         )
