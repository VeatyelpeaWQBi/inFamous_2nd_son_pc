# SPDX-License-Identifier: GPL-2.0-or-later
# Project inputs must stay inside this checkout, including through symbolic links.
function(ifss_project_local_path output root relative)
    string(REPLACE "\\" "/" relative "${relative}")
    if("${relative}" STREQUAL "" OR IS_ABSOLUTE "${relative}" OR
       "${relative}" MATCHES ":|(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Project dependency paths must be local relative paths: '${relative}'")
    endif()
    file(REAL_PATH "${root}" local_root)
    file(REAL_PATH "${relative}" local_path BASE_DIRECTORY "${local_root}")
    cmake_path(IS_PREFIX local_root "${local_path}" NORMALIZE inside)
    if(NOT inside OR "${local_path}" STREQUAL "${local_root}")
        message(FATAL_ERROR "Project dependency path escapes the project: '${relative}'")
    endif()
    set(${output} "${local_path}" PARENT_SCOPE)
endfunction()
