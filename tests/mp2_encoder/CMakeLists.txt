cmake_minimum_required(VERSION 3.16)
project(DVBStreamer5Mp2EncoderTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_subdirectory(../../third_party/twolame "${CMAKE_CURRENT_BINARY_DIR}/twolame")

add_executable(mp2_encoder_tests
    ../test_mp2_encoder.cpp
    ../../src/media/Mp2Encoder.cpp
)
target_include_directories(mp2_encoder_tests PRIVATE ../../src)
target_link_libraries(mp2_encoder_tests PRIVATE dvbstreamer5_twolame)
if(MSVC)
    target_compile_options(mp2_encoder_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mp2_encoder_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME mp2_encoder_tests COMMAND mp2_encoder_tests)
