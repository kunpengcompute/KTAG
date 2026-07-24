add_executable(ktag_three_layer_example three_layer_example.c)

target_link_libraries(ktag_three_layer_example PRIVATE
    ktag_capture
    ktag_feature
    ktag_inference
)

set_target_properties(ktag_three_layer_example PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN/../${KTAG_INSTALL_LIBDIR}"
)

install(TARGETS ktag_three_layer_example
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
