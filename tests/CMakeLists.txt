cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ddhub_host_tests C)
    include(CTest)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

foreach(suite IN ITEMS behavior concurrency)
    add_executable(ddhub_${suite}_test
        "${suite}_test.c"
        test_lock.c
        "${CMAKE_CURRENT_LIST_DIR}/../src/ddhub.c")
    target_compile_features(ddhub_${suite}_test PRIVATE c_std_99)
    target_link_libraries(ddhub_${suite}_test PRIVATE Threads::Threads)
    target_include_directories(ddhub_${suite}_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_definitions(ddhub_${suite}_test PRIVATE
        DDHUB_CONFIG_HEADER="ddhub_test_config.h")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ddhub_${suite}_test PRIVATE -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    endif()
    add_test(NAME ddhub_${suite}_test COMMAND ddhub_${suite}_test)
    set_tests_properties(ddhub_${suite}_test PROPERTIES TIMEOUT 30)
endforeach()
