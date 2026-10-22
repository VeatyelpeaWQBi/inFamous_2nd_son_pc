# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.30)
project(inFamous_2nd_son_pc LANGUAGES NONE)
option(IFSS_BUILD_ENGINE "Build this project Windows runtime" ON)
if(NOT IFSS_BUILD_ENGINE)
    message(STATUS "Engine build explicitly disabled by IFSS_BUILD_ENGINE=OFF.")
    return()
endif()
if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "This project targets Windows x86-64 only.")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/project.json" IFSS_PROJECT_JSON)
string(JSON IFSS_EXTERNALS_ROOT GET "${IFSS_PROJECT_JSON}" dependencies_root)
string(JSON IFSS_DEPENDENCY_BUILD_ROOT GET "${IFSS_PROJECT_JSON}" dependency_build_root)
file(TO_CMAKE_PATH "${IFSS_EXTERNALS_ROOT}" IFSS_EXTERNALS_ROOT)
file(TO_CMAKE_PATH "${IFSS_DEPENDENCY_BUILD_ROOT}" IFSS_DEPENDENCY_BUILD_ROOT)
get_filename_component(IFSS_EXTERNALS_ROOT "${IFSS_EXTERNALS_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(IFSS_DEPENDENCY_BUILD_ROOT "${IFSS_DEPENDENCY_BUILD_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${IFSS_EXTERNALS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Existing dependencies are missing. Downloads are not permitted.")
endif()
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_FMT "${IFSS_EXTERNALS_ROOT}/fmt" CACHE PATH "" FORCE)
set(ENABLE_UPDATER OFF CACHE BOOL "" FORCE)
set(ENABLE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
# Reuse already-installed FFmpeg libraries; never run its download CMake path.
file(GLOB IFSS_FFMPEG_DIRS LIST_DIRECTORIES true "${IFSS_DEPENDENCY_BUILD_ROOT}/ffmpeg")
set(IFSS_FFMPEG_ROOT "")
foreach(candidate IN LISTS IFSS_FFMPEG_DIRS)
    if(EXISTS "${candidate}/lib/avcodec.lib")
        set(IFSS_FFMPEG_ROOT "${candidate}")
        break()
    endif()
endforeach()
if(NOT IFSS_FFMPEG_ROOT)
    message(FATAL_ERROR "Pre-existing FFmpeg libraries are unavailable; nothing is downloaded.")
endif()
add_library(ifss_ffmpeg INTERFACE)
add_library(FFmpeg::ffmpeg ALIAS ifss_ffmpeg)
target_include_directories(ifss_ffmpeg INTERFACE "${IFSS_EXTERNALS_ROOT}/ffmpeg-core/include")
foreach(name avformat avcodec swscale avutil avfilter swresample)
    if(NOT EXISTS "${IFSS_FFMPEG_ROOT}/lib/${name}.lib")
        message(FATAL_ERROR "An existing FFmpeg library is missing: ${name}")
    endif()
    target_link_libraries(ifss_ffmpeg INTERFACE "${IFSS_FFMPEG_ROOT}/lib/${name}.lib")
endforeach()
target_link_libraries(ifss_ffmpeg INTERFACE psapi strmiids uuid oleaut32 shlwapi ws2_32 ole32 user32 bcrypt)
add_compile_definitions(IFSS_PROJECT_LOCAL_USER_DATA=1)
include(cmake/EngineBuild.cmake)
# These dependencies intentionally use Clang's supported non-standard-layout offsetof
# extension (protobuf's generated parse tables and OpenAL's flexible-array allocator).
# Match their GCC policy for clang-cl, without disabling the diagnostic in our engine.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
    foreach(dependency IN ITEMS libprotobuf libprotobuf-lite libprotoc OpenAL)
        if(TARGET ${dependency})
            target_compile_options(${dependency} PRIVATE -Wno-invalid-offsetof)
        endif()
    endforeach()
    # BIO_callback_ctrl transports an SSL callback through its legacy BIO_info_cb
    # signature. The cast restores that callback's actual type; keep this exception
    # confined to the vendor adapter, never to TLS callers or the engine.
    if(TARGET ssl_obj)
        set_property(SOURCE "${IFSS_EXTERNALS_ROOT}/libressl/ssl/bio_ssl.c"
            TARGET_DIRECTORY ssl_obj APPEND PROPERTY COMPILE_OPTIONS
            -Wno-cast-function-type-mismatch)
    endif()
endif()
if(TARGET shadps4)
    target_sources(shadps4 PRIVATE
        src/core/second_son_controls.cpp src/core/second_son_controls.h
        src/core/second_son_chords.h src/core/second_son_prompts.cpp
        src/core/second_son_prompts.h src/core/second_son_touchpad_icons.h
        src/input/gamepad_type.h)
    set_target_properties(shadps4 PROPERTIES
        OUTPUT_NAME "inFAMOUS Second Son"
        PDB_NAME "inFAMOUS Second Son"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(IFSS_GAME_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/inFAMOUS Second Son.ico")
    if(NOT EXISTS "${IFSS_GAME_ICON_PATH}")
        message(FATAL_ERROR "The project game icon is missing.")
    endif()
    target_compile_definitions(shadps4 PRIVATE IFSS_GAME_ICON_PATH="${IFSS_GAME_ICON_PATH}")
    # Optional collection control from the installed VTune SDK; no runtime is copied.
    set(IFSS_ITT_SDK_ROOT "" CACHE PATH "Existing VTune SDK for diagnostic collection control")
    if(IFSS_ITT_SDK_ROOT)
        if(NOT EXISTS "${IFSS_ITT_SDK_ROOT}/include/ittnotify.h" OR
           NOT EXISTS "${IFSS_ITT_SDK_ROOT}/lib64/libittnotify.lib")
            message(FATAL_ERROR "The configured installed VTune SDK is incomplete.")
        endif()
        set_property(SOURCE src/core/benchmark_control.cpp APPEND PROPERTY
                     COMPILE_DEFINITIONS IFSS_HAVE_ITT=1)
        set_property(SOURCE src/core/benchmark_control.cpp APPEND PROPERTY
                     INCLUDE_DIRECTORIES "${IFSS_ITT_SDK_ROOT}/include")
        target_link_libraries(shadps4 PRIVATE "${IFSS_ITT_SDK_ROOT}/lib64/libittnotify.lib")
    endif()
    # Optional integration with the already installed Nsight SDK.
    file(GLOB IFSS_NSIGHT_SDK_CANDIDATES LIST_DIRECTORIES true
        "$ENV{ProgramFiles}/NVIDIA Corporation/Nsight Graphics */SDKs/NsightGraphicsSDK/*/include")
    list(SORT IFSS_NSIGHT_SDK_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    foreach(sdk IN LISTS IFSS_NSIGHT_SDK_CANDIDATES)
        if(EXISTS "${sdk}/NGFX_GPUTrace_Vulkan.h")
            set_property(SOURCE src/core/benchmark_control.cpp APPEND PROPERTY
                         COMPILE_DEFINITIONS IFSS_HAVE_NSIGHT_SDK=1)
            set_property(SOURCE src/core/benchmark_control.cpp APPEND PROPERTY
                         INCLUDE_DIRECTORIES "${sdk}")
            break()
        endif()
    endforeach()
endif()

# Small native regression using existing engine dependencies; no test framework download.
add_executable(ifss_log_sink_test EXCLUDE_FROM_ALL tests/test_log_file_sink.cpp)
target_compile_features(ifss_log_sink_test PRIVATE cxx_std_23)
target_include_directories(ifss_log_sink_test PRIVATE src)
target_link_libraries(ifss_log_sink_test PRIVATE spdlog::spdlog fmt::fmt)

# Pure state/dependency policy regressions, no Vulkan device or downloaded test framework.
add_executable(ifss_recording_workers_test EXCLUDE_FROM_ALL tests/test_recording_workers.cpp)
target_compile_features(ifss_recording_workers_test PRIVATE cxx_std_23)
target_include_directories(ifss_recording_workers_test PRIVATE src)

add_executable(ifss_copy_completion_test EXCLUDE_FROM_ALL tests/test_copy_completion.cpp)
target_compile_features(ifss_copy_completion_test PRIVATE cxx_std_23)
target_include_directories(ifss_copy_completion_test PRIVATE src)

add_executable(ifss_buffer_bind_cache_test EXCLUDE_FROM_ALL tests/test_buffer_bind_cache.cpp)
target_compile_features(ifss_buffer_bind_cache_test PRIVATE cxx_std_23)
target_include_directories(ifss_buffer_bind_cache_test PRIVATE src)
target_link_libraries(ifss_buffer_bind_cache_test PRIVATE Vulkan::Headers)

add_executable(ifss_readback_dependencies_test EXCLUDE_FROM_ALL tests/test_readback_dependencies.cpp)
target_compile_features(ifss_readback_dependencies_test PRIVATE cxx_std_23)
target_include_directories(ifss_readback_dependencies_test PRIVATE src)

# V2 scheduling/bounds policies: no device, framework or new dependency.
add_executable(ifss_optimization_v2_test EXCLUDE_FROM_ALL tests/test_optimization_v2.cpp)
target_compile_features(ifss_optimization_v2_test PRIVATE cxx_std_23)
target_include_directories(ifss_optimization_v2_test PRIVATE src)

# Diagnostic transport ordering/full/drop tests. Not part of the default engine build.
add_executable(ifss_path_trace_queue_test EXCLUDE_FROM_ALL tests/test_path_trace_queue.cpp)
target_compile_features(ifss_path_trace_queue_test PRIVATE cxx_std_23)
target_include_directories(ifss_path_trace_queue_test PRIVATE src)

add_executable(ifss_consumer_evidence_test EXCLUDE_FROM_ALL tests/test_consumer_evidence.cpp)
target_compile_features(ifss_consumer_evidence_test PRIVATE cxx_std_23)
target_include_directories(ifss_consumer_evidence_test PRIVATE src)

# V3 range provenance and batch coverage properties; no device/framework/dependency download.
add_executable(ifss_optimization_v3_test EXCLUDE_FROM_ALL tests/test_optimization_v3.cpp)
target_compile_features(ifss_optimization_v3_test PRIVATE cxx_std_23)
target_include_directories(ifss_optimization_v3_test PRIVATE src)

add_executable(ifss_sort_write_bounds_test EXCLUDE_FROM_ALL tests/test_ifss_sort_write_bounds.cpp)
target_compile_features(ifss_sort_write_bounds_test PRIVATE cxx_std_23)
target_include_directories(ifss_sort_write_bounds_test PRIVATE src)

# Exact layout identity / conservative fallback, no Vulkan device or loader.
add_executable(ifss_pipeline_layout_key_test EXCLUDE_FROM_ALL tests/test_pipeline_layout_key.cpp)
target_compile_features(ifss_pipeline_layout_key_test PRIVATE cxx_std_23)
target_include_directories(ifss_pipeline_layout_key_test PRIVATE src)
target_link_libraries(ifss_pipeline_layout_key_test PRIVATE Vulkan::Headers)

# R4 evidence semantics only; these do not execute during the normal engine build.
add_executable(ifss_feedback_write_history_test EXCLUDE_FROM_ALL tests/test_feedback_write_history.cpp)
target_compile_features(ifss_feedback_write_history_test PRIVATE cxx_std_23)
target_include_directories(ifss_feedback_write_history_test PRIVATE src)

add_executable(ifss_descriptor_evidence_test EXCLUDE_FROM_ALL tests/test_descriptor_evidence.cpp)
target_compile_features(ifss_descriptor_evidence_test PRIVATE cxx_std_23)
target_include_directories(ifss_descriptor_evidence_test PRIVATE src)
target_link_libraries(ifss_descriptor_evidence_test PRIVATE Vulkan::Headers)

# A1 state/differential equivalence regression; no Vulkan device/loader or game.
add_executable(ifss_descriptor_state_test EXCLUDE_FROM_ALL tests/test_descriptor_state.cpp)
target_compile_features(ifss_descriptor_state_test PRIVATE cxx_std_23)
target_include_directories(ifss_descriptor_state_test PRIVATE src)
target_link_libraries(ifss_descriptor_state_test PRIVATE Vulkan::Headers)

# C1 SPIR-V/data equivalence only; uses existing SPIR-V headers, no device or game.
add_executable(ifss_depth_reduction_test EXCLUDE_FROM_ALL
    tests/test_ifss_depth_reduction.cpp src/shader_recompiler/ifss_depth_reduction.cpp)
target_compile_features(ifss_depth_reduction_test PRIVATE cxx_std_23)
target_include_directories(ifss_depth_reduction_test PRIVATE src
    "${IFSS_EXTERNALS_ROOT}/sirit/externals/SPIRV-Headers/include")

# Execute the actual R7 IR pass without a Vulkan device or game startup.
add_executable(ifss_lighting_cse_test EXCLUDE_FROM_ALL
    tests/test_ifss_lighting_cse.cpp
    tests/test_ifss_lighting_vector.cpp
    src/shader_recompiler/ir/passes/ifss_lighting_cse.cpp
    src/shader_recompiler/ir/passes/ifss_lighting_vector.cpp
    src/shader_recompiler/ir/basic_block.cpp src/shader_recompiler/ir/microinstruction.cpp
    src/shader_recompiler/ir/value.cpp src/shader_recompiler/ir/type.cpp
    src/shader_recompiler/ir/opcodes.cpp src/shader_recompiler/ir/attribute.cpp
    src/shader_recompiler/ir/patch.cpp src/shader_recompiler/ir/dominance.cpp
    src/shader_recompiler/ir/post_order.cpp)
target_compile_features(ifss_lighting_cse_test PRIVATE cxx_std_23)
target_include_directories(ifss_lighting_cse_test PRIVATE src)
target_link_libraries(ifss_lighting_cse_test PRIVATE Boost::headers fmt::fmt magic_enum::magic_enum)

# Shared Xbox/touchpad policy checks; no device, game or default-build execution.
add_executable(ifss_second_son_controls_test EXCLUDE_FROM_ALL tests/test_second_son_controls.cpp)
target_compile_features(ifss_second_son_controls_test PRIVATE cxx_std_23)
target_include_directories(ifss_second_son_controls_test PRIVATE src)
target_link_libraries(ifss_second_son_controls_test PRIVATE SDL3::Headers)
