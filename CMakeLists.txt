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

enable_testing()

# Stage 8: build SRT 1.5.7 directly from vendored upstream source with OpenSSL EVP.
# No system libsrt development/runtime package, binary SRT payload, memfd loader,
# GnuTLS or Nettle compatibility layer is used anymore.
set(DVBSTREAMER5_SRT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/srt" CACHE PATH
    "Vendored Haivision SRT 1.5.7 source directory")
if(NOT EXISTS "${DVBSTREAMER5_SRT_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Vendored SRT 1.5.7 source not found at ${DVBSTREAMER5_SRT_SOURCE_DIR}. "
        "Run ./scripts/vendor_srt_source.sh (or ./install_deps.sh) before configuring.")
endif()
file(STRINGS "${DVBSTREAMER5_SRT_SOURCE_DIR}/CMakeLists.txt" _dvbstreamer5_srt_version_line
     REGEX "set[ \\t]*\\([ \\t]*SRT_VERSION[ \\t]+1\\.5\\.7[ \\t]*\\)" LIMIT_COUNT 1)
if(NOT _dvbstreamer5_srt_version_line)
    message(FATAL_ERROR "third_party/srt is not the locked SRT 1.5.7 source tree")
endif()

# SRT supports namespaced parent options specifically for add_subdirectory use.
set(LIBSRT_ENABLE_IMPORT_VARIABLES 1)
set(LIBSRT_ENABLE_SHARED OFF)
set(LIBSRT_ENABLE_STATIC ON)
set(LIBSRT_ENABLE_APPS OFF)
set(LIBSRT_ENABLE_EXAMPLES OFF)
set(LIBSRT_ENABLE_TESTING OFF)
set(LIBSRT_ENABLE_UNITTESTS OFF)
set(LIBSRT_ENABLE_BONDING OFF)
set(LIBSRT_ENABLE_ENCRYPTION ON)
set(LIBSRT_ENABLE_STDCXX_SYNC ON)
set(LIBSRT_ENABLE_LOGGING ON)
set(LIBSRT_ENABLE_HEAVY_LOGGING OFF)
set(LIBSRT_ENABLE_HAICRYPT_LOGGING OFF)
set(LIBSRT_ENABLE_CXX_DEPS OFF)
set(LIBSRT_USE_ENCLIB "openssl-evp")
set(LIBSRT_USE_OPENSSL_PC OFF)
set(LIBSRT_USE_CXX_STD "17")
set(LIBSRT_SRT_USE_OPENSSL_STATIC_LIBS OFF)
set(LIBSRT_ENABLE_SHOW_PROJECT_CONFIG OFF)
add_subdirectory("${DVBSTREAMER5_SRT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third_party/srt" EXCLUDE_FROM_ALL)
if(NOT TARGET srt_static)
    message(FATAL_ERROR "Vendored SRT 1.5.7 did not create the expected srt_static target")
endif()

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
    src/media/NativeHlsInput.cpp
    src/media/NativeHlsSegmenter.cpp
    src/media/NativeSrtTransport.cpp
    src/media/NativeUdpRelay.cpp
)
target_include_directories(dvbstreamer5_media_core
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dvbstreamer5_media_core PUBLIC
    Threads::Threads
    dvbstreamer5_http_client
    srt_static
    OpenSSL::Crypto
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
    add_subdirectory(tests/native_hls)
    add_subdirectory(tests/native_srt)
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

install(TARGETS DVBStreamer5 RUNTIME DESTINATION bin)

include(${CMAKE_SOURCE_DIR}/cmake/OscamMini.cmake)
