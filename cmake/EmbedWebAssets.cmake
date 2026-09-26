function(dvbstreamer5_generate_embedded_web_assets asset_dir output_dir output_var)
    set(asset_names
        mpegts.min.js
        hls.min.js
        LICENSE.mpegts.js
        LICENSE.hls.js
    )
    file(MAKE_DIRECTORY "${output_dir}")
    set(generated_source
        "#include \"EmbeddedWebAssets.h\"\n\nnamespace tvs::web {\nnamespace {\n"
    )

    foreach(asset_name IN LISTS asset_names)
        set(asset_path "${asset_dir}/${asset_name}")
        if(NOT EXISTS "${asset_path}")
            message(FATAL_ERROR "Required embedded web asset is missing: ${asset_path}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asset_path}")

        string(MAKE_C_IDENTIFIER "${asset_name}" symbol)
        string(TOUPPER "${symbol}" symbol)
        file(READ "${asset_path}" asset_hex HEX)
        string(LENGTH "${asset_hex}" asset_hex_length)
        set(asset_bytes "")
        set(offset 0)
        while(offset LESS asset_hex_length)
            math(EXPR chunk_length "${asset_hex_length} - ${offset}")
            if(chunk_length GREATER 512)
                set(chunk_length 512)
            endif()
            string(SUBSTRING "${asset_hex}" ${offset} ${chunk_length} asset_chunk)
            string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," asset_chunk_bytes "${asset_chunk}")
            string(APPEND asset_bytes "    ${asset_chunk_bytes}\n")
            math(EXPR offset "${offset} + ${chunk_length}")
        endwhile()
        string(APPEND generated_source
            "constexpr unsigned char ${symbol}[] = {\n${asset_bytes}    0x00\n};\n"
        )
    endforeach()

    string(APPEND generated_source
        "}\n\nstd::string_view embeddedWebAsset(std::string_view path) {\n"
    )
    string(APPEND generated_source
        "    if (path == \"/preview/mpegts.min.js\") return {reinterpret_cast<const char*>(MPEGTS_MIN_JS), sizeof(MPEGTS_MIN_JS) - 1};\n"
        "    if (path == \"/preview/hls.min.js\") return {reinterpret_cast<const char*>(HLS_MIN_JS), sizeof(HLS_MIN_JS) - 1};\n"
        "    if (path == \"/licenses/mpegts.js.txt\") return {reinterpret_cast<const char*>(LICENSE_MPEGTS_JS), sizeof(LICENSE_MPEGTS_JS) - 1};\n"
        "    if (path == \"/licenses/hls.js.txt\") return {reinterpret_cast<const char*>(LICENSE_HLS_JS), sizeof(LICENSE_HLS_JS) - 1};\n"
        "    return {};\n}\n}\n"
    )

    set(generated_path "${output_dir}/EmbeddedWebAssets.cpp")
    file(WRITE "${generated_path}" "${generated_source}")
    set(${output_var} "${generated_path}" PARENT_SCOPE)
endfunction()
