cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ddhub VERSION 0.1.0 LANGUAGES C)
endif()

add_library(ddhub STATIC)
target_compile_features(ddhub PRIVATE c_std_99)
target_include_directories(ddhub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_subdirectory(src)
