cmake_minimum_required(VERSION 3.16)
project(DVBStreamer5MediaCoreTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED libcurl)

add_executable(media_core_tests
    ../test_media_core.cpp
    ../../src/media/CbrTsPacer.cpp
    ../../src/media/MpegTsRemapper.cpp
    ../../src/media/TransportStream.cpp
    ../../src/media/RtpMpegTs.cpp
    ../../src/media/UdpSocket.cpp
    ../../src/media/LinuxDvbInput.cpp
    ../../src/media/NativePreviewHub.cpp
    ../../src/media/NativeUdpRelay.cpp
)
target_include_directories(media_core_tests PRIVATE ../../src)
target_include_directories(media_core_tests PRIVATE ${LIBCURL_INCLUDE_DIRS})
target_link_directories(media_core_tests PRIVATE ${LIBCURL_LIBRARY_DIRS})
target_link_libraries(media_core_tests PRIVATE Threads::Threads ${LIBCURL_LIBRARIES})
target_link_options(media_core_tests PRIVATE ${LIBCURL_LDFLAGS_OTHER})

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
