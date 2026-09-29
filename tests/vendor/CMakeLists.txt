add_executable(tvstreamer5_vendor_tests
    test_vendored_dependencies.cpp
)

target_link_libraries(tvstreamer5_vendor_tests PRIVATE
    tvstreamer5_jsoncpp
    tvstreamer5_dvbcsa
)
