if(NOT TARGET ktag_feature)
    message(FATAL_ERROR
        "ktag_traffic_features must be built as part of the KTAG source tree")
endif()

add_library(ktag_traffic_features SHARED
    traffic_feature_plugin.cpp
    traffic_provider_common.cpp
    legacy/metadata_provider.cpp
    legacy/tuple_provider.cpp
    traffic_statistics_provider.cpp
    rawbow_provider.cpp
)
add_library(KTAG::traffic_features ALIAS ktag_traffic_features)

target_include_directories(ktag_traffic_features PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(ktag_traffic_features PRIVATE
    KTAG_TRAFFIC_FEATURES_BUILDING_DLL
)

target_link_libraries(ktag_traffic_features PRIVATE
    ktag_feature
)

# The compatibility library discovers this plugin at runtime through the
# public Plugin ABI. Keep a build-order dependency without creating a link-time
# dependency or a DT_NEEDED entry.
if(TARGET ktag_feature_compat)
    add_dependencies(ktag_feature_compat ktag_traffic_features)
endif()

set_target_properties(ktag_traffic_features PROPERTIES
    EXPORT_NAME traffic_features
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/output/plugins"
    BUILD_RPATH "$ORIGIN/.."
    INSTALL_RPATH "$ORIGIN/.."
)

install(TARGETS ktag_traffic_features
        EXPORT KTAGTargets
        LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}/plugins")
