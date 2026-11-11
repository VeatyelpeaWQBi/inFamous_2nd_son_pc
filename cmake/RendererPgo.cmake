# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR IFSS_RENDERER_INLINE_CANDIDATE)
    message(FATAL_ERROR "Renderer PGO requires the ordinary RelWithDebInfo clang-cl configuration")
endif()

set(renderer_pgo_sources
    src/video_core/amdgpu/liverpool.cpp
    src/video_core/buffer_cache/buffer_cache.cpp
    src/video_core/page_manager.cpp
    src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
    src/video_core/renderer_vulkan/vk_rasterizer.cpp
    src/video_core/renderer_vulkan/vk_runtime.cpp)

if(IFSS_RENDERER_PGO STREQUAL "Train")
    if(IFSS_RENDERER_PGO_DATA)
        message(FATAL_ERROR "Renderer PGO training must start without indexed profile data")
    endif()
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(profile_runtime "${resource_dir}/lib/windows/clang_rt.profile-x86_64.lib")
    if(NOT EXISTS "${profile_runtime}")
        message(FATAL_ERROR "The installed clang-cl profile runtime is missing: ${profile_runtime}")
    endif()
    target_link_libraries(shadps4 PRIVATE "${profile_runtime}")
    set(profile_options /clang:-fprofile-generate /clang:-fprofile-update=atomic)
    set_property(SOURCE src/emulator.cpp TARGET_DIRECTORY shadps4
        APPEND PROPERTY COMPILE_DEFINITIONS IFSS_RENDERER_PGO_TRAINING=1)
elseif(IFSS_RENDERER_PGO STREQUAL "Use")
    if(NOT IS_ABSOLUTE "${IFSS_RENDERER_PGO_DATA}" OR NOT EXISTS "${IFSS_RENDERER_PGO_DATA}")
        message(FATAL_ERROR "Renderer PGO requires an existing absolute indexed profile path")
    endif()
    set(profile_options "/clang:-fprofile-use=${IFSS_RENDERER_PGO_DATA}"
        /clang:-Werror=backend-plugin)
    set_property(SOURCE ${renderer_pgo_sources} TARGET_DIRECTORY shadps4
        APPEND PROPERTY OBJECT_DEPENDS "${IFSS_RENDERER_PGO_DATA}")
else()
    message(FATAL_ERROR "Invalid renderer PGO mode: ${IFSS_RENDERER_PGO}")
endif()

set_property(SOURCE ${renderer_pgo_sources} TARGET_DIRECTORY shadps4
    APPEND PROPERTY COMPILE_OPTIONS ${profile_options})
