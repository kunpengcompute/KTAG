cmake_minimum_required(VERSION 3.22.0)
project(kdadk_multi_thread)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# 源文件
set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kdadk_multi_thread.cpp
)

# 生成动态库
add_library(kdadk_multi_thread SHARED ${SOURCES})

# 链接依赖
target_link_libraries(kdadk_multi_thread
    pthread
    KDADK_feature_extract
    KDADK_inference
)

# 安装库文件
install(TARGETS kdadk_multi_thread
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# 安装头文件
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/kdadk_multi_thread.h
    DESTINATION include
)