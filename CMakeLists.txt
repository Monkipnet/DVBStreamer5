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

# Stage 8/9: production builds use the vendored SRT 1.5.7 source tree
# and statically link it with the OpenSSL EVP crypto backend.
set(DVBSTREAMER5_SRT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/srt" CACHE PATH
    "Vendored Haivision SRT 1.5.7 source directory")
if(NOT EXISTS "${DVBSTREAMER5_SRT_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Vendored SRT 1.5.7 source not found at ${DVBSTREAMER5_SRT_SOURCE_DIR}. "
        "Run ./scripts/vendor_srt_source.sh (or ./install_deps.sh) before configuring.")
endif()
file(STRINGS "${DVBSTREAMER5_SRT_SOURCE_DIR}/CMakeLists.txt" _dvbstreamer5_srt_version_line
     REGEX "SRT_VERSION[ \t]+1[.]5[.]7" LIMIT_COUNT 1)
if(NOT _dvbstreamer5_srt_version_line)
    message(FATAL_ERROR "third_party/srt is not the locked SRT 1.5.7 source tree")
endif()
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

# Stage 9: codec backends are project-local static libraries built from
# locked vendored source snapshots. There is intentionally no runtime codec
# dlopen fallback.
set(DVBSTREAMER5_CODEC_PREFIX "${CMAKE_SOURCE_DIR}/third_party/native-codecs-prefix" CACHE PATH
    "Static native codec prefix produced by scripts/build_native_codecs.sh")
set(_codec_headers
    "${DVBSTREAMER5_CODEC_PREFIX}/include/wels/codec_api.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/libde265/de265.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/kvazaar.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/fdk-aac/aacdecoder_lib.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/fdk-aac/aacenc_lib.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/ittiam-mpeg2/impeg2d.h"
    "${DVBSTREAMER5_CODEC_PREFIX}/include/pl_mpeg/pl_mpeg.h")
foreach(_h IN LISTS _codec_headers)
    if(NOT EXISTS "${_h}")
        message(FATAL_ERROR "Native codec header missing: ${_h}. Run ./scripts/vendor_native_codecs.sh && ./scripts/build_native_codecs.sh")
    endif()
endforeach()
function(dvbstreamer5_import_static_codec target filename)
    set(_path "${DVBSTREAMER5_CODEC_PREFIX}/lib/${filename}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Native codec static library missing: ${_path}. Run ./scripts/build_native_codecs.sh")
    endif()
    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_path}")
endfunction()
dvbstreamer5_import_static_codec(dvbstreamer5_openh264 libopenh264.a)
dvbstreamer5_import_static_codec(dvbstreamer5_de265 libde265.a)
dvbstreamer5_import_static_codec(dvbstreamer5_kvazaar libkvazaar.a)
dvbstreamer5_import_static_codec(dvbstreamer5_fdkaac libfdk-aac.a)
dvbstreamer5_import_static_codec(dvbstreamer5_mpeg2dec libmpeg2dec.a)
set(DVBSTREAMER5_CODEC_SOURCE src/media/NativeCodecVendored.cpp)
set(DVBSTREAMER5_CODEC_LIBRARIES
    dvbstreamer5_openh264 dvbstreamer5_de265 dvbstreamer5_kvazaar
    dvbstreamer5_fdkaac dvbstreamer5_mpeg2dec)

add_library(dvbstreamer5_media_core
    src/media/CbrTsPacer.cpp
    src/media/MpegTsRemapper.cpp
    src/media/NativeMpegTsMux.cpp
    ${DVBSTREAMER5_CODEC_SOURCE}
    src/media/NativeHardwareCodec.cpp
    src/media/NativeVideoProcessing.cpp
    src/media/NativeTranscoderPipeline.cpp
    src/media/NativeTestPatternSource.cpp
    src/media/Mp2Encoder.cpp
    src/media/TransportStream.cpp
    src/media/RtpMpegTs.cpp
    src/media/UdpSocket.cpp
    src/media/LinuxDvbInput.cpp
    src/media/SharedDvbInputPool.cpp
    src/media/NativePreviewHub.cpp
    src/media/NativeHlsInput.cpp
    src/media/NativeHlsSegmenter.cpp
    src/media/NativeSampleAes.cpp
    src/media/NativeTsDemux.cpp
    src/media/NativeCmaf.cpp
    src/media/NativeRtspTransport.cpp
    src/media/NativeRtmpTransport.cpp
    src/media/NativeSrtTransport.cpp
    src/media/NativeUdpRelay.cpp
)
target_include_directories(dvbstreamer5_media_core
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PRIVATE
        ${DVBSTREAMER5_CODEC_PREFIX}/include
        ${DVBSTREAMER5_CODEC_PREFIX}/include/ittiam-mpeg2
)
target_link_libraries(dvbstreamer5_media_core PUBLIC
    Threads::Threads
    dvbstreamer5_http_client
    srt_static
    OpenSSL::Crypto
)
target_link_libraries(dvbstreamer5_media_core PRIVATE
    dvbstreamer5_twolame
    ${DVBSTREAMER5_CODEC_LIBRARIES}
    ${CMAKE_DL_LIBS}
    m
)
if(WIN32)
    target_link_libraries(dvbstreamer5_media_core PUBLIC ws2_32)
endif()

# Hardware encoders are enabled by default. A backend is compiled when its
# native SDK headers/libraries are available on the build host.
option(DVBSTREAMER5_ENABLE_VAAPI "Enable native VAAPI hardware backend when available" ON)
option(DVBSTREAMER5_ENABLE_INTEL_QSV "Enable native Intel oneVPL/QSV backend when available" ON)
option(DVBSTREAMER5_ENABLE_NVENC "Enable native NVIDIA NVENC backend when available" ON)

# Optional direct hardware video backends. These are vendor/OS APIs, not media frameworks.
# CPU codecs remain fully self-contained when the SDK headers are absent.

# Direct libva backend for legacy Intel generations (for example Haswell/i965)
# where the modern oneVPL GPU runtime exposes no implementation.
if(DVBSTREAMER5_ENABLE_VAAPI)
    find_path(DVBSTREAMER5_VA_INCLUDE_DIR NAMES va/va.h)
    find_library(DVBSTREAMER5_VA_LIBRARY NAMES va)
    find_library(DVBSTREAMER5_VA_DRM_LIBRARY NAMES va-drm)
endif()
if(DVBSTREAMER5_ENABLE_VAAPI AND DVBSTREAMER5_VA_INCLUDE_DIR AND DVBSTREAMER5_VA_LIBRARY AND DVBSTREAMER5_VA_DRM_LIBRARY)
    target_compile_definitions(dvbstreamer5_media_core PRIVATE DVBSTREAMER5_HAVE_VAAPI=1)
    target_include_directories(dvbstreamer5_media_core PRIVATE ${DVBSTREAMER5_VA_INCLUDE_DIR})
    target_link_libraries(dvbstreamer5_media_core PRIVATE
        ${DVBSTREAMER5_VA_LIBRARY}
        ${DVBSTREAMER5_VA_DRM_LIBRARY}
    )
    message(STATUS "Native Intel VAAPI backend: enabled (direct libva)")
else()
    message(STATUS "Native Intel VAAPI backend: disabled (install libva-dev)")
endif()

if(DVBSTREAMER5_ENABLE_INTEL_QSV)
    find_path(DVBSTREAMER5_VPL_INCLUDE_DIR NAMES vpl/mfxdispatcher.h)
    find_library(DVBSTREAMER5_VPL_LIBRARY NAMES vpl)
endif()
if(DVBSTREAMER5_ENABLE_INTEL_QSV AND DVBSTREAMER5_VPL_INCLUDE_DIR AND DVBSTREAMER5_VPL_LIBRARY AND EXISTS "${DVBSTREAMER5_VPL_INCLUDE_DIR}/vpl/mfxvideo.h")
    target_compile_definitions(dvbstreamer5_media_core PRIVATE DVBSTREAMER5_HAVE_VPL=1)
    target_include_directories(dvbstreamer5_media_core PRIVATE ${DVBSTREAMER5_VPL_INCLUDE_DIR})
    target_link_libraries(dvbstreamer5_media_core PRIVATE ${DVBSTREAMER5_VPL_LIBRARY})
    message(STATUS "Native Intel QSV/VAAPI backend: enabled via oneVPL")
else()
    message(STATUS "Native Intel QSV/VAAPI backend: disabled (install libvpl-dev)")
endif()

if(DVBSTREAMER5_ENABLE_NVENC)
    find_path(DVBSTREAMER5_NVENC_INCLUDE_DIR NAMES nvEncodeAPI.h PATH_SUFFIXES ffnvcodec nvcodec)
endif()
if(DVBSTREAMER5_ENABLE_NVENC AND DVBSTREAMER5_NVENC_INCLUDE_DIR)
    target_compile_definitions(dvbstreamer5_media_core PRIVATE DVBSTREAMER5_HAVE_NVENC_HEADERS=1)
    target_include_directories(dvbstreamer5_media_core PRIVATE ${DVBSTREAMER5_NVENC_INCLUDE_DIR})
    message(STATUS "Native NVIDIA NVENC backend: enabled (runtime loaded dynamically)")
else()
    message(STATUS "Native NVIDIA NVENC backend: disabled (nvEncodeAPI.h not found)")
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
    add_subdirectory(tests/native_protocols)
    add_subdirectory(tests/native_transcoder)
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
