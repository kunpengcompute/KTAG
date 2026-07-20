cmake_minimum_required(VERSION 3.16.3)
project(KTAGExternalConsumer LANGUAGES C)

find_package(KTAG CONFIG REQUIRED)

# Reuse the same source as the in-tree example, but resolve every dependency
# exclusively through the installed KTAG package. This target deliberately has
# no access to repository include directories or build-tree library targets.
add_executable(
    ktag_external_consumer
    "${CMAKE_CURRENT_LIST_DIR}/../three_layer/three_layer_example.c"
)

target_link_libraries(ktag_external_consumer PRIVATE
    KTAG::capture
    KTAG::feature
    KTAG::inference
)

set_target_properties(ktag_external_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
