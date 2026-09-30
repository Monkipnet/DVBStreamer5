add_executable(dvbstreamer5_vendor_tests
    test_vendored_dependencies.cpp
)

target_link_libraries(dvbstreamer5_vendor_tests PRIVATE
    dvbstreamer5_jsoncpp
    dvbstreamer5_dvbcsa
)
