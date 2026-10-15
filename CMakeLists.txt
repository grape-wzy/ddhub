add_library(ddhub STATIC)
target_compile_features(ddhub PRIVATE c_std_99)
target_include_directories(ddhub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ddhub PRIVATE zephyr_interface)

add_subdirectory(src)

target_link_libraries(app PRIVATE ddhub)
