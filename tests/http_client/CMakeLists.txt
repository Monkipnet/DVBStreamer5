add_executable(dvbstreamer5_http_client_tests
    test_native_http_client.cpp
)

target_link_libraries(dvbstreamer5_http_client_tests PRIVATE
    dvbstreamer5_http_client
)
