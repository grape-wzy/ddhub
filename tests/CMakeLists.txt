cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ddhub_host_tests LANGUAGES C CXX)
    include(CTest)
else()
    enable_language(CXX)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

foreach(mode IN ITEMS async sync)
    if(mode STREQUAL "async")
        set(async_enabled 1)
    else()
        set(async_enabled 0)
    endif()

    add_library(ddhub_test_core_${mode} STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../src/ddhub.c" test_lock.c)
    target_compile_features(ddhub_test_core_${mode} PUBLIC c_std_99)
    target_link_libraries(ddhub_test_core_${mode} PUBLIC Threads::Threads)
    target_include_directories(ddhub_test_core_${mode} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_definitions(ddhub_test_core_${mode} PUBLIC
        DDHUB_CONFIG_HEADER="ddhub_test_config.h" DDHUB_ASYNC_PROCESS_ENABLE=${async_enabled})
    set(mode_targets ddhub_test_core_${mode})

    foreach(suite IN ITEMS behavior concurrency cpp_interop)
        if(suite STREQUAL "cpp_interop")
            set(extension cpp)
        else()
            set(extension c)
        endif()
        set(test_target ddhub_${suite}_${mode}_test)
        add_executable(${test_target} "${suite}_test.${extension}")
        target_link_libraries(${test_target} PRIVATE ddhub_test_core_${mode})
        target_compile_features(${test_target} PRIVATE cxx_std_11)
        list(APPEND mode_targets ${test_target})
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target} PROPERTIES TIMEOUT 30)
    endforeach()

    # This boundary test includes the implementation to exercise sequence exhaustion.
    set(test_target ddhub_generation_${mode}_test)
    add_executable(${test_target} generation_test.c test_lock.c)
    target_compile_features(${test_target} PRIVATE c_std_99)
    target_link_libraries(${test_target} PRIVATE Threads::Threads)
    target_include_directories(${test_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_definitions(${test_target} PRIVATE
        DDHUB_CONFIG_HEADER="ddhub_test_config.h" DDHUB_ASYNC_PROCESS_ENABLE=${async_enabled})
    list(APPEND mode_targets ${test_target})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 30)

    foreach(target IN LISTS mode_targets)
        set_target_properties(${target} PROPERTIES
            C_STANDARD_REQUIRED YES C_EXTENSIONS NO CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /W4 /UNDEBUG)
        endif()
    endforeach()
    if(CMAKE_NM)
        add_test(NAME ddhub_symbols_${mode}_test COMMAND "${CMAKE_COMMAND}"
            "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:ddhub_test_core_${mode}>"
            "-DASYNC_ENABLED=${async_enabled}" -P "${CMAKE_CURRENT_LIST_DIR}/verify_symbols.cmake")
    endif()
endforeach()

# Verify header visibility without relying on linking an undefined function.
include(CheckCSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
if(DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
    set(saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
endif()
set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/../include")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_REQUIRED_DEFINITIONS -DDDHUB_ASYNC_PROCESS_ENABLE=0)
check_c_source_compiles("#include <ddhub.h>\nint main(void) { ddhub_status_t (*fn)(void) = ddhub_process; return fn(); }"
    DDHUB_SYNC_EXPOSES_PROCESS)
if(DDHUB_SYNC_EXPOSES_PROCESS)
    message(FATAL_ERROR "Synchronous headers must not expose ddhub_process")
endif()
set(CMAKE_REQUIRED_DEFINITIONS -DDDHUB_ASYNC_PROCESS_ENABLE=1)
check_c_source_compiles("#include <ddhub.h>\nint main(void) { ddhub_status_t (*fn)(void) = ddhub_process; return fn(); }"
    DDHUB_ASYNC_EXPOSES_PROCESS)
if(NOT DDHUB_ASYNC_EXPOSES_PROCESS)
    message(FATAL_ERROR "Asynchronous headers must expose ddhub_process(void)")
endif()
if(DEFINED saved_try_compile_target_type)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${saved_try_compile_target_type}")
    unset(saved_try_compile_target_type)
else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
cmake_pop_check_state()
