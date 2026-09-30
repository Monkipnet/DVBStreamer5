cmake_minimum_required(VERSION 3.10)
project(DVBStreamer5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake)
dvbstreamer5_generate_embedded_web_assets(
    ${CMAKE_SOURCE_DIR}/web/vendor
    ${CMAKE_BINARY_DIR}/generated
    EMBEDDED_WEB_ASSET_SOURCE
)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)


add_library(dvbstreamer5_boost_headers INTERFACE)
target_include_directories(dvbstreamer5_boost_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/boost
)
target_compile_definitions(dvbstreamer5_boost_headers INTERFACE
    BOOST_ERROR_CODE_HEADER_ONLY
)

add_library(dvbstreamer5_jsoncpp STATIC
    third_party/jsoncpp/jsoncpp.cpp
)
target_include_directories(dvbstreamer5_jsoncpp PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party
)

add_library(dvbstreamer5_http_client STATIC
    src/NativeHttpClient.cpp
)
target_include_directories(dvbstreamer5_http_client PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(dvbstreamer5_http_client PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(dvbstreamer5_http_client PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(WIN32)
    target_link_libraries(dvbstreamer5_http_client PUBLIC ws2_32)
endif()

add_subdirectory(third_party/twolame)
add_subdirectory(third_party/libdvbcsa)

add_library(dvbstreamer5_media_core
    src/media/CbrTsPacer.cpp
    src/media/MpegTsRemapper.cpp
    src/media/NativeMpegTsMux.cpp
    src/media/Mp2Encoder.cpp
    src/media/TransportStream.cpp
    src/media/RtpMpegTs.cpp
    src/media/UdpSocket.cpp
    src/media/LinuxDvbInput.cpp
    src/media/NativePreviewHub.cpp
    src/media/NativeUdpRelay.cpp
)
target_include_directories(dvbstreamer5_media_core
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dvbstreamer5_media_core PUBLIC
    Threads::Threads
    dvbstreamer5_http_client
)
target_link_libraries(dvbstreamer5_media_core PRIVATE dvbstreamer5_twolame)
if(WIN32)
    target_link_libraries(dvbstreamer5_media_core PUBLIC ws2_32)
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
)

add_executable(DVBStreamer5
    src/main.cpp
    src/utils.cpp
    src/ConfigManager.cpp
    src/DvbSatellite.cpp
    src/CardManager.cpp
    src/CaBackend.cpp
    src/ca/backends/newcamd/NewcamdPlugin.cpp
    src/ca/backends/newcamd/NewcamdClient.cpp
    src/TelegramNotifier.cpp
    src/StableUdpOutput.cpp
    src/mpts/MptsOutputManager.cpp
    src/StreamManager.cpp
    src/TranscoderModule.cpp
    src/protocols/stream/StreamInputProtocol.cpp
    src/protocols/stream/StreamOutputProtocol.cpp
    src/protocols/stream/inputs/StreamTestInputProtocol.cpp
    src/protocols/stream/inputs/StreamHttpInputProtocol.cpp
    src/protocols/stream/inputs/StreamHlsInputProtocol.cpp
    src/protocols/stream/inputs/StreamUdpInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtpInputProtocol.cpp
    src/protocols/stream/inputs/StreamSrtInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtspInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtmpInputProtocol.cpp
    src/protocols/stream/inputs/StreamFileInputProtocol.cpp
    src/protocols/stream/outputs/StreamUdpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamHttpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamHlsOutputProtocol.cpp
    src/protocols/stream/outputs/StreamSrtOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtspOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtmpOutputProtocol.cpp
    src/HttpServer.cpp
    ${EMBEDDED_WEB_ASSET_SOURCE}
    src/OscamMiniManager.cpp
)

target_link_libraries(DVBStreamer5
    dvbstreamer5_boost_headers
    dvbstreamer5_media_core
    dvbstreamer5_jsoncpp
    dvbstreamer5_dvbcsa
    OpenSSL::Crypto
    pthread
    dl
)
option(DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS "Build standalone native MPEG-TS/RTP tests" OFF)
if(DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS)
    add_subdirectory(tests/media_core)
endif()

option(DVBSTREAMER5_BUILD_MP2_ENCODER_TESTS "Build standalone PCM-to-MP2 encoder tests" OFF)
if(DVBSTREAMER5_BUILD_MP2_ENCODER_TESTS)
    add_subdirectory(tests/mp2_encoder)
endif()

option(DVBSTREAMER5_BUILD_VENDOR_TESTS "Build tests for vendored third-party libraries" OFF)
if(DVBSTREAMER5_BUILD_VENDOR_TESTS)
    add_subdirectory(tests/vendor)
    add_subdirectory(tests/http_client)
    add_subdirectory(tests/boost_headers)
endif()

option(DVBSTREAMER5_BUILD_CA_PLUGIN_EXAMPLE "Build non-decoding CaBackend ABI example plugin" OFF)
if(DVBSTREAMER5_BUILD_CA_PLUGIN_EXAMPLE)
    add_library(dvbstreamer5_ca_passthrough_example SHARED
        examples/ca_backend_passthrough_plugin.cpp
    )
    target_include_directories(dvbstreamer5_ca_passthrough_example PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(dvbstreamer5_ca_passthrough_example PROPERTIES
        PREFIX ""
        OUTPUT_NAME "dvbstreamer5-ca-passthrough-example"
    )
endif()

enable_testing()

install(TARGETS DVBStreamer5 RUNTIME DESTINATION bin)

include(${CMAKE_SOURCE_DIR}/cmake/OscamMini.cmake)
