add_library(tvstreamer5_dvbcsa STATIC
    src/dvbcsa_algo.c
    src/dvbcsa_block.c
    src/dvbcsa_bs_algo.c
    src/dvbcsa_bs_block.c
    src/dvbcsa_bs_key.c
    src/dvbcsa_bs_stream.c
    src/dvbcsa_bs_transpose.c
    src/dvbcsa_bs_transpose128.c
    src/dvbcsa_key.c
    src/dvbcsa_stream.c
)

target_include_directories(tvstreamer5_dvbcsa PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(tvstreamer5_dvbcsa PRIVATE c_std_99)
target_compile_definitions(tvstreamer5_dvbcsa PRIVATE _XOPEN_SOURCE=600)
set_target_properties(tvstreamer5_dvbcsa PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tvstreamer5_dvbcsa PRIVATE -msse2)
endif()
