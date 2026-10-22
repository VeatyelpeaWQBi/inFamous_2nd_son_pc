# SPDX-License-Identifier: GPL-2.0-or-later
# Release compatibility patches must never modify the pinned source archive.
# Keep the derived source copy in this build tree, reusing it on later builds.
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/DependencyCompatibility.cmake" IFSS_COMPATIBILITY_SHA256)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" IFSS_OVERLAY_IMPLEMENTATION_SHA256)
string(SHA256 IFSS_OVERLAY_ID "${IFSS_INVENTORY_EXPECTED};${IFSS_COMPATIBILITY_SHA256};${IFSS_OVERLAY_IMPLEMENTATION_SHA256}")
string(SUBSTRING "${IFSS_OVERLAY_ID}" 0 16 IFSS_OVERLAY_SHORT_ID)
file(RELATIVE_PATH IFSS_OVERLAY_RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources/${IFSS_OVERLAY_SHORT_ID}")
ifss_project_local_path(IFSS_DEPENDENCY_OVERLAY "${CMAKE_CURRENT_SOURCE_DIR}" "${IFSS_OVERLAY_RELATIVE}")
set(IFSS_ARCHIVE_EXTERNALS_ROOT "${IFSS_EXTERNALS_ROOT}")
cmake_path(IS_PREFIX IFSS_ARCHIVE_EXTERNALS_ROOT "${IFSS_DEPENDENCY_OVERLAY}" NORMALIZE IFSS_OVERLAY_INSIDE_ARCHIVE)
if(IFSS_OVERLAY_INSIDE_ARCHIVE)
    message(FATAL_ERROR "The derived dependency source copy must be outside the pinned archive.")
endif()
set(IFSS_OVERLAY_STAMP "${IFSS_DEPENDENCY_OVERLAY}/.archive-id")
set(IFSS_OVERLAY_READY FALSE)
if(EXISTS "${IFSS_OVERLAY_STAMP}")
    file(READ "${IFSS_OVERLAY_STAMP}" IFSS_SAVED_OVERLAY_ID)
    if(NOT IFSS_SAVED_OVERLAY_ID STREQUAL IFSS_OVERLAY_ID)
        message(FATAL_ERROR "Dependency source copy identity mismatch; review '${IFSS_DEPENDENCY_OVERLAY}'.")
    endif()
    set(IFSS_OVERLAY_READY TRUE)
endif()
if(NOT IFSS_OVERLAY_READY)
    message(STATUS "Preparing build-local dependency source copy (approximately 0.85 GiB): ${IFSS_DEPENDENCY_OVERLAY}")
    file(MAKE_DIRECTORY "${IFSS_DEPENDENCY_OVERLAY}")
    file(COPY "${IFSS_ARCHIVE_EXTERNALS_ROOT}/" DESTINATION "${IFSS_DEPENDENCY_OVERLAY}"
        PATTERN ".archives" EXCLUDE PATTERN "prebuilt" EXCLUDE
        PATTERN "dependency-inventory.tsv" EXCLUDE PATTERN ".git" EXCLUDE)
endif()
set(IFSS_EXTERNALS_ROOT "${IFSS_DEPENDENCY_OVERLAY}")
include("${CMAKE_CURRENT_LIST_DIR}/DependencyCompatibility.cmake")
file(WRITE "${IFSS_OVERLAY_STAMP}" "${IFSS_OVERLAY_ID}")
