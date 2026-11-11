# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.30)
project(inFamous_2nd_son_pc LANGUAGES NONE)
option(IFSS_BUILD_ENGINE "Build this project Windows runtime" ON)
option(IFSS_RENDERER_INLINE_CANDIDATE "Experimental full inlining in three renderer units" OFF)
set(IFSS_RENDERER_PGO "Off" CACHE STRING "Renderer profile-guided optimization: Off, Train or Use")
set_property(CACHE IFSS_RENDERER_PGO PROPERTY STRINGS Off Train Use)
set(IFSS_RENDERER_PGO_DATA "" CACHE FILEPATH "Indexed LLVM profile for the renderer candidate")
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
        src/shader_recompiler/gds_counter_plan.cpp src/shader_recompiler/gds_counter_plan.h
        src/core/second_son_controls.cpp src/core/second_son_controls.h
        src/core/second_son_cheats.cpp src/core/second_son_cheats.h
        src/core/second_son_cheat_menu.cpp src/core/second_son_compatibility.h
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
    if(IFSS_RENDERER_INLINE_CANDIDATE)
        if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "The renderer inlining candidate requires the existing clang-cl toolchain")
        endif()
        # Keep debug information and all numeric/renderer policies. Only lift /Ob1's
        # explicit-inline restriction in the measured CPU preparation units.
        set_property(SOURCE
            src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
            src/video_core/renderer_vulkan/vk_rasterizer.cpp
            src/video_core/buffer_cache/buffer_cache.cpp
            TARGET_DIRECTORY shadps4 APPEND PROPERTY COMPILE_OPTIONS
            "$<$<CONFIG:RelWithDebInfo>:/Ob2>")
    endif()
    if(NOT IFSS_RENDERER_PGO STREQUAL "Off")
        include(cmake/RendererPgo.cmake)
    endif()
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

# Offline VTune report compatibility. Uses only Windows SDK libraries; no profiler injection
# into the game, downloaded dependency, or change to collected hardware events.
if(WIN32)
    add_executable(ifss_vtune_report_guard tests/bootstrap/vtune_report_probe.cpp)
    target_compile_features(ifss_vtune_report_guard PRIVATE cxx_std_20)
    target_link_libraries(ifss_vtune_report_guard PRIVATE dbghelp bcrypt)
endif()

# Small native regression using existing engine dependencies; no test framework download.
add_executable(ifss_log_sink_test EXCLUDE_FROM_ALL tests/test_log_file_sink.cpp)
target_compile_features(ifss_log_sink_test PRIVATE cxx_std_23)
target_include_directories(ifss_log_sink_test PRIVATE src)
target_link_libraries(ifss_log_sink_test PRIVATE spdlog::spdlog fmt::fmt)

# Pure state/dependency policy regressions, no Vulkan device or downloaded test framework.
add_executable(ifss_buffer_bind_cache_test EXCLUDE_FROM_ALL tests/test_buffer_bind_cache.cpp)
target_compile_features(ifss_buffer_bind_cache_test PRIVATE cxx_std_23)
target_include_directories(ifss_buffer_bind_cache_test PRIVATE src)
target_link_libraries(ifss_buffer_bind_cache_test PRIVATE Vulkan::Headers)

add_executable(ifss_readback_dependencies_test EXCLUDE_FROM_ALL tests/test_readback_dependencies.cpp)
target_compile_features(ifss_readback_dependencies_test PRIVATE cxx_std_23)
target_include_directories(ifss_readback_dependencies_test PRIVATE src)

add_executable(ifss_feedback_policy_test EXCLUDE_FROM_ALL tests/cache_runtime/test_feedback_policy.cpp)
target_compile_features(ifss_feedback_policy_test PRIVATE cxx_std_23)
target_include_directories(ifss_feedback_policy_test PRIVATE src)

add_executable(ifss_gds_counter_plan_test EXCLUDE_FROM_ALL
    tests/cache_runtime/test_gds_counter_plan.cpp src/shader_recompiler/gds_counter_plan.cpp)
target_compile_features(ifss_gds_counter_plan_test PRIVATE cxx_std_23)
target_include_directories(ifss_gds_counter_plan_test PRIVATE src
    "${IFSS_EXTERNALS_ROOT}/sirit/externals/SPIRV-Headers/include")

add_executable(ifss_gpu_full_word_summary_test EXCLUDE_FROM_ALL
    tests/cache_runtime/test_gpu_full_word_summary.cpp)
target_compile_features(ifss_gpu_full_word_summary_test PRIVATE cxx_std_23)
target_include_directories(ifss_gpu_full_word_summary_test PRIVATE src)

add_executable(ifss_writer_footprint_test EXCLUDE_FROM_ALL tests/test_writer_footprint.cpp)
target_compile_features(ifss_writer_footprint_test PRIVATE cxx_std_23)
target_include_directories(ifss_writer_footprint_test PRIVATE src)

# V2 scheduling/bounds policies: no device, framework or new dependency.
add_executable(ifss_access_union_test EXCLUDE_FROM_ALL tests/cache_runtime/test_access_union.cpp)
target_compile_features(ifss_access_union_test PRIVATE cxx_std_23)
target_include_directories(ifss_access_union_test PRIVATE src)
add_executable(ifss_buffer_access_test EXCLUDE_FROM_ALL tests/cache_runtime/test_buffer_access.cpp)
target_compile_features(ifss_buffer_access_test PRIVATE cxx_std_23)
target_include_directories(ifss_buffer_access_test PRIVATE src)

add_executable(ifss_optimization_v2_test EXCLUDE_FROM_ALL tests/test_optimization_v2.cpp)
target_compile_features(ifss_optimization_v2_test PRIVATE cxx_std_23)
target_include_directories(ifss_optimization_v2_test PRIVATE src)

# Diagnostic transport ordering/full/drop tests. Not part of the default engine build.
# Reuse the existing shared-readiness concurrency oracle; not part of the game build.
add_executable(ifss_compile_readiness_test EXCLUDE_FROM_ALL tests/cache_runtime/test_compile_readiness.cpp)
target_compile_features(ifss_compile_readiness_test PRIVATE cxx_std_23)
target_include_directories(ifss_compile_readiness_test PRIVATE src)

add_executable(ifss_path_trace_queue_test EXCLUDE_FROM_ALL tests/test_path_trace_queue.cpp)
target_compile_features(ifss_path_trace_queue_test PRIVATE cxx_std_23)
target_include_directories(ifss_path_trace_queue_test PRIVATE src)

# Shared Xbox/touchpad policy checks; no device, game or default-build execution.
add_executable(ifss_second_son_controls_test EXCLUDE_FROM_ALL tests/test_second_son_controls.cpp)
target_compile_features(ifss_second_son_controls_test PRIVATE cxx_std_23)
target_include_directories(ifss_second_son_controls_test PRIVATE src)
target_link_libraries(ifss_second_son_controls_test PRIVATE SDL3::Headers)

# Command-local descriptor reuse, real guest decode logic; no GPU/game.
add_executable(ifss_command_buffer_sharps_test EXCLUDE_FROM_ALL tests/cache_runtime/test_command_buffer_sharps.cpp)
target_compile_features(ifss_command_buffer_sharps_test PRIVATE cxx_std_23)
target_include_directories(ifss_command_buffer_sharps_test PRIVATE src)
target_link_libraries(ifss_command_buffer_sharps_test PRIVATE Boost::headers fmt::fmt magic_enum::magic_enum)

# Pure policy regression and optional headless GPU validation with the installed SDK loader.
add_executable(ifss_compute_barrier_policy_test EXCLUDE_FROM_ALL tests/cache_runtime/test_compute_barrier_policy.cpp)
target_compile_features(ifss_compute_barrier_policy_test PRIVATE cxx_std_23)
target_include_directories(ifss_compute_barrier_policy_test PRIVATE src)
target_link_libraries(ifss_compute_barrier_policy_test PRIVATE Vulkan::Headers fmt::fmt)
find_library(IFSS_INSTALLED_VULKAN_LOADER NAMES vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib")
if(IFSS_INSTALLED_VULKAN_LOADER)
    add_executable(ifss_compute_barrier_micro EXCLUDE_FROM_ALL tests/cache_runtime/compute_barrier_micro.cpp)
    target_compile_features(ifss_compute_barrier_micro PRIVATE cxx_std_23)
    target_include_directories(ifss_compute_barrier_micro PRIVATE src)
    target_link_libraries(ifss_compute_barrier_micro PRIVATE Vulkan::Headers "${IFSS_INSTALLED_VULKAN_LOADER}")
endif()
