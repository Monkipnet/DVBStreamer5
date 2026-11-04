cmake_minimum_required(VERSION 3.16)
project(DVBStreamer5MediaCoreTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(media_core_tests
    ../test_media_core.cpp
    ../../src/media/CbrTsPacer.cpp
    ../../src/media/MpegTsRemapper.cpp
    ../../src/media/NativeMpegTsMux.cpp
    ../../src/media/TransportStream.cpp
    ../../src/media/RtpMpegTs.cpp
    ../../src/media/UdpSocket.cpp
    ../../src/media/LinuxDvbInput.cpp
    ../../src/media/NativePreviewHub.cpp
    ../../src/media/NativeUdpRelay.cpp
    ../../src/NativeHttpClient.cpp
)
target_include_directories(media_core_tests PRIVATE ../../src ../../third_party)
target_compile_definitions(media_core_tests PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(media_core_tests PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

if(MSVC)
    target_compile_options(media_core_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(media_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(media_core_tests PRIVATE ws2_32)
endif()

enable_testing()
add_test(NAME media_core_tests COMMAND media_core_tests)

# V330: deterministic NVENC A/B policy test, independent of CUDA/NVENC drivers.
add_executable(nvenc_quality_policy_test test_nvenc_quality_policy.cpp)
target_include_directories(nvenc_quality_policy_test PRIVATE ../../src)
add_test(NAME nvenc_quality_policy COMMAND nvenc_quality_policy_test)

# V332: GPU-free, tuner-free Unicable startup compatibility policy regression.
add_executable(unicable_tuning_policy_test test_unicable_tuning_policy.cpp)
target_include_directories(unicable_tuning_policy_test PRIVATE ../../src)
add_test(NAME unicable_tuning_policy COMMAND unicable_tuning_policy_test)


# V333: no-GPU, no-hardware test for single-stream HTTP/SRT CBR source clock.
add_executable(ip_cbr_source_clock_policy_test test_ip_cbr_source_clock_policy.cpp)
target_include_directories(ip_cbr_source_clock_policy_test PRIVATE ../../src)
add_test(NAME ip_cbr_source_clock_policy COMMAND ip_cbr_source_clock_policy_test)
