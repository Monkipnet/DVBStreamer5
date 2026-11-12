add_executable(dvbstreamer5_native_transcoder_tests test_native_transcoder.cpp)
target_include_directories(dvbstreamer5_native_transcoder_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dvbstreamer5_native_transcoder_tests PRIVATE dvbstreamer5_media_core Threads::Threads dl)
add_test(NAME native_transcoder COMMAND dvbstreamer5_native_transcoder_tests)

# V493: isolated real-MPEG-TS CPU/VAAPI/NVENC downscale comparisons.
# This standalone executable is intentionally excluded from the portable
# production bundle and never modifies live tiles or service settings.
add_executable(dvbstreamer5_downscale_benchmark benchmark_downscale.cpp)
target_include_directories(dvbstreamer5_downscale_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dvbstreamer5_downscale_benchmark PRIVATE dvbstreamer5_media_core Threads::Threads dl)

# Script syntax must remain valid on Linux build hosts before benchmarking.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME downscale_benchmark_python_syntax
        COMMAND "${Python3_EXECUTABLE}" -m py_compile
                "${CMAKE_SOURCE_DIR}/scripts/benchmark_downscale.py")
endif()
