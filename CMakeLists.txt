cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ddhub VERSION 0.2.0 LANGUAGES C)
endif()

add_library(ddhub STATIC)
target_compile_features(ddhub PRIVATE c_std_99)

# An explicit CMake setting overrides the default in the shared configuration header.
if(DEFINED DDHUB_ASYNC_PROCESS_ENABLE)
    target_compile_definitions(ddhub PUBLIC DDHUB_ASYNC_PROCESS_ENABLE=$<BOOL:${DDHUB_ASYNC_PROCESS_ENABLE}>)
endif()
target_include_directories(ddhub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_subdirectory(src)
