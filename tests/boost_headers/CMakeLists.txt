add_executable(tvstreamer5_boost_headers_tests
    test_boost_headers.cpp
)

target_link_libraries(tvstreamer5_boost_headers_tests PRIVATE
    tvstreamer5_boost_headers
    Threads::Threads
)
if(WIN32)
    target_link_libraries(tvstreamer5_boost_headers_tests PRIVATE ws2_32 mswsock)
endif()
