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

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_library(CRYPT_LIBRARY crypt)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 gstreamer-app-1.0 gstreamer-mpegts-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(DVBCSA QUIET libdvbcsa)
if(NOT DVBCSA_FOUND)
    pkg_check_modules(DVBCSA QUIET dvbcsa)
endif()
if(NOT DVBCSA_FOUND)
    find_path(DVBCSA_INCLUDE_DIRS dvbcsa/dvbcsa.h)
    find_library(DVBCSA_LIBRARIES dvbcsa)
    if(NOT DVBCSA_INCLUDE_DIRS OR NOT DVBCSA_LIBRARIES)
        message(FATAL_ERROR "libdvbcsa development files were not found")
    endif()
endif()

find_package(Boost REQUIRED COMPONENTS thread)

add_subdirectory(third_party/twolame)

add_library(dvbstreamer5_media_core
    src/media/CbrTsPacer.cpp
    src/media/MpegTsRemapper.cpp
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
    PRIVATE ${LIBCURL_INCLUDE_DIRS}
)
target_link_libraries(dvbstreamer5_media_core PUBLIC
    Threads::Threads
    ${LIBCURL_LIBRARIES}
)
target_link_libraries(dvbstreamer5_media_core PRIVATE dvbstreamer5_twolame)
if(WIN32)
    target_link_libraries(dvbstreamer5_media_core PUBLIC ws2_32)
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${GSTREAMER_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${DVBCSA_INCLUDE_DIRS}
)

link_directories(
    ${GSTREAMER_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
    ${JSONCPP_LIBRARY_DIRS}
    ${Boost_LIBRARY_DIRS}
    ${DVBCSA_LIBRARY_DIRS}
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
    src/UdpInput.cpp
    src/StableUdpOutput.cpp
    src/NetupOutputPacer.cpp
    src/NetworkTsInput.cpp
    src/HlsSegmentScheduler.cpp
    src/mpts/MptsOutputManager.cpp
    src/StreamManager.cpp
    src/TranscoderModule.cpp
    src/media/GstMp2Encoder.cpp
    src/GstTranscoderProcess.cpp
    src/protocols/GstProtocolTypes.cpp
    src/protocols/GstInputProtocols.cpp
    src/protocols/GstOutputProtocols.cpp
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
    src/protocols/inputs/GstHttpInputProtocol.cpp
    src/protocols/inputs/GstHlsInputProtocol.cpp
    src/protocols/inputs/GstUdpInputProtocol.cpp
    src/protocols/inputs/GstRtpInputProtocol.cpp
    src/protocols/inputs/GstSrtInputProtocol.cpp
    src/protocols/inputs/GstRtspInputProtocol.cpp
    src/protocols/inputs/GstRtmpInputProtocol.cpp
    src/protocols/outputs/GstOutputProtocolUtils.cpp
    src/protocols/outputs/GstFifoOutputProtocol.cpp
    src/protocols/outputs/GstUdpOutputProtocol.cpp
    src/protocols/outputs/GstRtpOutputProtocol.cpp
    src/protocols/outputs/GstSrtOutputProtocol.cpp
    src/protocols/outputs/GstHttpOutputProtocol.cpp
    src/protocols/outputs/GstHlsOutputProtocol.cpp
    src/protocols/outputs/GstRtspOutputProtocol.cpp
    src/protocols/outputs/GstRtmpOutputProtocol.cpp
    src/HttpServer.cpp
    ${EMBEDDED_WEB_ASSET_SOURCE}
    src/OscamMiniManager.cpp
)

target_link_libraries(DVBStreamer5
    dvbstreamer5_media_core
    ${GSTREAMER_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${DVBCSA_LIBRARIES}
    ${Boost_LIBRARIES}
    OpenSSL::Crypto
    pthread
    dl
    $<$<BOOL:${CRYPT_LIBRARY}>:${CRYPT_LIBRARY}>
    glib-2.0
    gio-2.0
)

if(DEFINED TVSTREAMMERSAT5_BUILD_MEDIA_CORE_TESTS AND NOT DEFINED DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS)
    set(DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS "${TVSTREAMMERSAT5_BUILD_MEDIA_CORE_TESTS}" CACHE BOOL
        "Build standalone native MPEG-TS/RTP tests")
endif()
option(DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS "Build standalone native MPEG-TS/RTP tests" OFF)
if(DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS)
    add_subdirectory(tests/media_core)
endif()

option(DVBSTREAMER5_BUILD_MP2_ENCODER_TESTS "Build standalone PCM-to-MP2 encoder tests" OFF)
if(DVBSTREAMER5_BUILD_MP2_ENCODER_TESTS)
    add_subdirectory(tests/mp2_encoder)
endif()

if(DEFINED TVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE AND NOT DEFINED DVBSTREAMER5_BUILD_CA_PLUGIN_EXAMPLE)
    set(DVBSTREAMER5_BUILD_CA_PLUGIN_EXAMPLE "${TVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE}" CACHE BOOL
        "Build non-decoding CaBackend ABI example plugin")
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

install(TARGETS DVBStreamer5 RUNTIME DESTINATION bin)

include(${CMAKE_SOURCE_DIR}/cmake/OscamMini.cmake)
