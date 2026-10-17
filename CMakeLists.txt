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
file(GLOB IFSS_FFMPEG_DIRS LIST_DIRECTORIES true "${IFSS_DEPENDENCY_BUILD_ROOT}/externals/ffmpeg-*")
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
if(TARGET shadps4)
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
