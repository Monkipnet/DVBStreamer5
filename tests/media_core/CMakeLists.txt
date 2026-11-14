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
    ../../src/media/PcrVbrUdpOutputWorker.cpp
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

# Byte-exact SIMD/scalar upscaling, no GPU or external media libraries.
add_executable(native_scaler_simd_test ../test_native_scaler_simd.cpp)
target_include_directories(native_scaler_simd_test PRIVATE ../../src)
add_test(NAME native_scaler_simd COMMAND native_scaler_simd_test)
# V367: validate exact embedded-browser PCR and CBR marker persistence.
find_program(DVBSTREAMER5_NODE_EXECUTABLE NAMES node nodejs)
if(DVBSTREAMER5_NODE_EXECUTABLE)
    add_test(NAME cbr_profile_ui
        COMMAND ${DVBSTREAMER5_NODE_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../web/test_cbr_profile_ui.js")
endif()

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

# V336: one CBR stuffing stage for live transcode; physical output shaper
# remains responsible for 7x188 NULL filling in normal and Source-PCR modes.
add_executable(transcode_cbr_single_shaper_test
    test_transcode_cbr_single_shaper.cpp
    ../../src/media/NativeMpegTsMux.cpp
    ../../src/media/CbrTsPacer.cpp
    ../../src/media/TransportStream.cpp
)
target_include_directories(transcode_cbr_single_shaper_test PRIVATE ../../src)
add_test(NAME transcode_cbr_single_shaper COMMAND transcode_cbr_single_shaper_test)

# V340: pure no-GPU/no-DVB regression for HLS OnDemand cold-start grace.
add_executable(ondemand_hls_startup_policy_test test_ondemand_hls_startup_policy.cpp)
target_include_directories(ondemand_hls_startup_policy_test PRIVATE ../../src)
add_test(NAME ondemand_hls_startup_policy COMMAND ondemand_hls_startup_policy_test)

# V341: HLS-only passthrough vs network CBR mixed-output gating.
add_executable(hls_only_cbr_routing_policy_test test_hls_only_cbr_routing_policy.cpp)
target_include_directories(hls_only_cbr_routing_policy_test PRIVATE ../../src)
add_test(NAME hls_only_cbr_routing_policy COMMAND hls_only_cbr_routing_policy_test)

# V342: replacement-file repeated TS must retain monotonic PCR and PID CC.
add_executable(native_file_loop_clock_test
    test_native_file_loop_clock.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_file_loop_clock_test PRIVATE ../../src)
add_test(NAME native_file_loop_clock COMMAND native_file_loop_clock_test)

# V344: native ISO-BMFF to MPEG-TS without FFmpeg, plus primary media recovery.
add_executable(native_mp4_replacement_test
    test_native_mp4_replacement.cpp
    ../../src/media/NativeMpegTsMux.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_mp4_replacement_test PRIVATE ../../src)
add_test(NAME native_mp4_replacement COMMAND native_mp4_replacement_test)

add_executable(native_failover_health_test test_native_failover_health.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_failover_health_test PRIVATE ../../src)
add_test(NAME native_failover_health COMMAND native_failover_health_test)

# V346: retain SRT MPEG-TS PCR/PTS/DTS and CC across both failover directions.
add_executable(native_srt_switch_clock_test
    test_native_srt_switch_clock.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_srt_switch_clock_test PRIVATE ../../src)
add_test(NAME native_srt_switch_clock COMMAND native_srt_switch_clock_test)

# V350: HLS source transitions must retain the live manifest and media sequence.
add_executable(native_hls_source_switch_test
    test_native_hls_source_switch.cpp
    ../../src/media/NativeHlsSegmenter.cpp
    ../../src/media/NativeSampleAes.cpp
    ../../src/media/NativeTsDemux.cpp
    ../../src/media/NativeMpegTsMux.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_hls_source_switch_test PRIVATE ../../src)
target_link_libraries(native_hls_source_switch_test PRIVATE OpenSSL::Crypto Threads::Threads)
add_test(NAME native_hls_source_switch COMMAND native_hls_source_switch_test)

# V354: a backup program named 'backup1' must keep the original channel
# SID, PMT PID, video/audio PIDs and service name for already-open players.
add_executable(native_failover_program_identity_test
    test_native_failover_program_identity.cpp
    ../../src/media/NativeMpegTsMux.cpp
    ../../src/media/MpegTsRemapper.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(native_failover_program_identity_test PRIVATE ../../src)
add_test(NAME native_failover_program_identity COMMAND native_failover_program_identity_test)

add_executable(source_pcr_packet_pacing_test
    test_source_pcr_packet_pacing.cpp
    ../../src/media/CbrTsPacer.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(source_pcr_packet_pacing_test PRIVATE ../../src)
add_test(NAME source_pcr_packet_pacing COMMAND source_pcr_packet_pacing_test)

# V515: physical CBR slot PCR / adaptation-only PCR packets, byte-exact PES,
# payload continuity, and large 900-packet P-frame. No GPU or network needed.
add_executable(cbr_output_pcr_smoothing_test
    test_cbr_output_pcr_smoothing.cpp
    ../../src/media/CbrTsPacer.cpp
    ../../src/media/TransportStream.cpp)
target_include_directories(cbr_output_pcr_smoothing_test PRIVATE ../../src)
add_test(NAME cbr_output_pcr_smoothing COMMAND cbr_output_pcr_smoothing_test)


# V425: no-hardware regression of pre-queue PID selection for shared DVB
# multiplexes, including CAT/PMT, EIT and PMT-generation changes.
add_executable(shared_dvb_pid_admission_test test_shared_dvb_pid_admission.cpp)
target_include_directories(shared_dvb_pid_admission_test PRIVATE ../../src)
add_test(NAME shared_dvb_pid_admission COMMAND shared_dvb_pid_admission_test)

# V491: byte-exact I420->NV12 SIMD upload for VAAPI, QSV and NVENC.
add_executable(native_i420_nv12_test test_native_i420_nv12.cpp)
target_include_directories(native_i420_nv12_test PRIVATE ../../src)
add_test(NAME native_i420_nv12 COMMAND native_i420_nv12_test)

# V491: repeated scaling must retain memory and reset frame metadata.
add_executable(native_video_frame_scratch_test
    test_native_video_frame_scratch.cpp
    ../../src/media/NativeVideoProcessing.cpp)
target_include_directories(native_video_frame_scratch_test PRIVATE ../../src)
add_test(NAME native_video_frame_scratch COMMAND native_video_frame_scratch_test)

# V492: launcher allocator default and override policy.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME portable_malloc_arena_policy
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_portable_malloc_arena_policy.py")
endif()

# V487: no-GPU test for decode-order DTS pacing and bounded waits.
add_executable(native_video_pes_pacer_test test_native_video_pes_pacer.cpp)
target_include_directories(native_video_pes_pacer_test PRIVATE ../../src)
add_test(NAME native_video_pes_pacer COMMAND native_video_pes_pacer_test)

# V510: pure policy test for online-only auto-recovery and bounded retry delays.
add_executable(auto_start_recovery_policy_test ../test_auto_start_recovery_policy.cpp)
target_include_directories(auto_start_recovery_policy_test PRIVATE ../../src)
add_test(NAME auto_start_recovery_policy COMMAND auto_start_recovery_policy_test)

# V526: source PES PTS must periodically re-anchor reassembled H.264 access
# units without breaking split-PES picture boundaries or MPEG PTS wrap.
add_executable(h264_pes_pts_recovery_test test_h264_pes_pts_recovery.cpp)
target_include_directories(h264_pes_pts_recovery_test PRIVATE ../../src)
add_test(NAME h264_pes_pts_recovery COMMAND h264_pes_pts_recovery_test)
