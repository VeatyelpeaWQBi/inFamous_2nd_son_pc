# SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

project(ImGui_Resources)

set(IFSS_FONT_EMBED_SOURCE "${IFSS_EXTERNALS_ROOT}/imgui/misc/fonts/binary_to_compressed_c.cpp")
if(NOT EXISTS "${IFSS_FONT_EMBED_SOURCE}")
    message(FATAL_ERROR "Configured ImGui font embedding source is missing: ${IFSS_FONT_EMBED_SOURCE}")
endif()
add_executable(Dear_ImGui_FontEmbed "${IFSS_FONT_EMBED_SOURCE}")

set(FONT_LIST
    NotoSansArabic-Regular.ttf
    NotoSansCJK-Regular.ttc
    NotoSans-Regular.ttf
    NotoSansSymbols2-Regular.ttf
    NotoSansThai-Regular.ttf
    ProggyVector-Regular.ttf
)

set(OutputList "")
FOREACH (FONT_FILE ${FONT_LIST})
    string(REGEX REPLACE "-" "_" fontname ${FONT_FILE})
    string(TOLOWER ${fontname} fontname)
    string(REGEX REPLACE "\\.(ttf|otf|ttc)$" "" fontname_cpp ${fontname})
    set(fontname_cpp "imgui_font_${fontname_cpp}")

    MESSAGE(STATUS "Embedding font ${FONT_FILE}")
    set(OUTPUT "generated_fonts/imgui_fonts/${fontname}")
    add_custom_command(
        OUTPUT "${OUTPUT}.g.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "generated_fonts/imgui_fonts"
        COMMAND $<TARGET_FILE:Dear_ImGui_FontEmbed> -u32 -nostatic "${CMAKE_CURRENT_SOURCE_DIR}/fonts/${FONT_FILE}" ${fontname_cpp} > "${OUTPUT}.g.cpp"
        DEPENDS Dear_ImGui_FontEmbed "fonts/${FONT_FILE}"
        USES_TERMINAL
    )
    list(APPEND OutputList "${OUTPUT}.g.cpp")
ENDFOREACH ()

# Windows x86-64 is little-endian. Word arrays preserve compressed bytes
# with 1/4 as many initializer expressions, avoiding huge Clang ASTs.
add_library(ImGui_Resources STATIC ${OutputList})
set(IMGUI_RESOURCES_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated_fonts PARENT_SCOPE)
