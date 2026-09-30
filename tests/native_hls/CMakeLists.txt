add_executable(dvbstreamer5_native_hls_tests
    test_native_hls.cpp
)
target_include_directories(dvbstreamer5_native_hls_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib
)
target_link_libraries(dvbstreamer5_native_hls_tests PRIVATE
    dvbstreamer5_media_core
    Threads::Threads
)
add_test(NAME dvbstreamer5_native_hls COMMAND dvbstreamer5_native_hls_tests)
