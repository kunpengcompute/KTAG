cmake_minimum_required(VERSION 3.16.3)
project(KDADK_APPID LANGUAGES C CXX ASM)

# ----------------------------------------------------------------------------
# inclue required cmake files:
# ----------------------------------------------------------------------------
include(cmake/KDADK_APPIDUtil.cmake)
include(cmake/KDADK_APPIDModule.cmake)

string(REGEX MATCHALL "([0-9]+)" KDADK_APPID_VERSION ${KDADK_APPID_VERSION_INFO})

list(GET KDADK_APPID_VERSION 0 KDADK_APPID_VERSION_MAJOR) # Add a feature or a new library. For example, increase the value of this parameter when adding the OMP.
list(GET KDADK_APPID_VERSION 1 KDADK_APPID_VERSION_MINOR) # Increases when an interface is added.
list(GET KDADK_APPID_VERSION 2 KDADK_APPID_VERSION_PATCH) # Increases when bugs are fixed

string(TIMESTAMP BUILDDATE "%Y.%m.%d")
set(KDADK_APPID_VERSION_BUILDDATE \"${BUILDDATE}\")  # Last build time
set(KDADK_APPID_LIBVERSION "${KDADK_APPID_VERSION_INFO}")

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
ADD_SUBDIRECTORY(src)
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