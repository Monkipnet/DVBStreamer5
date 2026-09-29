add_executable(tvstreamer5_http_client_tests
    test_native_http_client.cpp
)

target_link_libraries(tvstreamer5_http_client_tests PRIVATE
    tvstreamer5_http_client
)
