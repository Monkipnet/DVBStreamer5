add_executable(dvbstreamer5_native_protocol_tests
    test_native_protocols.cpp
)
target_include_directories(dvbstreamer5_native_protocol_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dvbstreamer5_native_protocol_tests PRIVATE dvbstreamer5_media_core OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
add_test(NAME native_protocols COMMAND dvbstreamer5_native_protocol_tests)
