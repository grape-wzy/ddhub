add_library(ddhub_example_core STATIC
    ddhub_example.c
    "${CMAKE_CURRENT_LIST_DIR}/../../src/ddhub.c")

target_compile_features(ddhub_example_core PUBLIC c_std_99)
set_target_properties(ddhub_example_core PROPERTIES C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(ddhub_example_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_definitions(ddhub_example_core PUBLIC
    DDHUB_CONFIG_HEADER="ddhub_example_config.h")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ddhub_example_core PRIVATE -Wall -Wextra -Werror -Wpedantic)
elseif(MSVC)
    target_compile_options(ddhub_example_core PRIVATE /W4)
endif()
