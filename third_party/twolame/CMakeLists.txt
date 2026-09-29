cmake_minimum_required(VERSION 3.10)

set(_twolame_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/libtwolame")
set(_twolame_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_twolame_generated_dir}")
configure_file(config.h.in "${_twolame_generated_dir}/config.h" COPYONLY)

add_library(tvstreamer5_twolame STATIC
    "${_twolame_lib_dir}/ath.c"
    "${_twolame_lib_dir}/availbits.c"
    "${_twolame_lib_dir}/bitbuffer.c"
    "${_twolame_lib_dir}/crc.c"
    "${_twolame_lib_dir}/dab.c"
    "${_twolame_lib_dir}/encode.c"
    "${_twolame_lib_dir}/energy.c"
    "${_twolame_lib_dir}/fft.c"
    "${_twolame_lib_dir}/get_set.c"
    "${_twolame_lib_dir}/mem.c"
    "${_twolame_lib_dir}/psycho_0.c"
    "${_twolame_lib_dir}/psycho_1.c"
    "${_twolame_lib_dir}/psycho_2.c"
    "${_twolame_lib_dir}/psycho_3.c"
    "${_twolame_lib_dir}/psycho_4.c"
    "${_twolame_lib_dir}/psycho_n1.c"
    "${_twolame_lib_dir}/subband.c"
    "${_twolame_lib_dir}/twolame.c"
    "${_twolame_lib_dir}/util.c"
)

target_include_directories(tvstreamer5_twolame
    PUBLIC
        "${_twolame_lib_dir}"
    PRIVATE
        "${_twolame_generated_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/win32"
)
set_target_properties(tvstreamer5_twolame PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
)
if(UNIX)
    target_link_libraries(tvstreamer5_twolame PUBLIC m)
endif()
