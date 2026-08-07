if(NOT TARGET ktag_feature)
    message(FATAL_ERROR
        "ktag_proxy_features must be built as part of the KTAG source tree")
endif()

add_library(ktag_proxy_features SHARED
    proxy_feature_plugin.cpp
    proxy_carrier_parser.cpp
    proxy_math.cpp
    proxy_provider_common.cpp
    proxy_quic_parser.cpp
    proxy_sequence_provider.cpp
    proxy_tcp_signature_provider.cpp
    proxy_tls_parser.cpp
    proxy_quic_provider.cpp
)

target_include_directories(ktag_proxy_features PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ktag_proxy_features PRIVATE
    ktag_feature
)

set_target_properties(ktag_proxy_features PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/output/plugins"
    BUILD_RPATH "$ORIGIN/.."
    INSTALL_RPATH "$ORIGIN/.."
)

install(TARGETS ktag_proxy_features
        LIBRARY DESTINATION "${KTAG_INSTALL_LIBDIR}/plugins")
