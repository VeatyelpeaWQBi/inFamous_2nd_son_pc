# SPDX-License-Identifier: GPL-2.0-or-later
# Build/compiler fixes for the derived project-local source copy.
# Library versions, algorithms, data layout and constant bytes are preserved.
# No dependencies are downloaded or copied. Reviewed ABI diagnostics are scoped
# separately in DependencyDiagnostics.cmake, never disabled globally.
# Accept only the known old/new text; stop rather than guess after an update.
# Honor the parent's static-library settings on the first configure as well
# as cached reruns (miniz declares policies only through CMake 3.12).
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

function(ifss_patch_dependency relative before after)
    if(NOT IFSS_EXTERNALS_ROOT STREQUAL IFSS_DEPENDENCY_OVERLAY)
        message(FATAL_ERROR "Dependency compatibility patches require the build-local source copy.")
    endif()
    set(path "${IFSS_EXTERNALS_ROOT}/${relative}")
    file(READ "${path}" source)
    string(FIND "${source}" "${before}" old_position)
    string(FIND "${source}" "${after}" new_position)
    if(NOT new_position EQUAL -1)
        return()
    endif()
    if(old_position EQUAL -1)
        message(FATAL_ERROR "Dependency compatibility patch needs review: ${relative}")
    endif()
    string(REPLACE "${before}" "${after}" updated "${source}")
    file(WRITE "${path}" "${updated}")
    message(STATUS "Updated dependency build compatibility: ${relative}")
endfunction()

ifss_patch_dependency(robin-map/CMakeLists.txt
    "cmake_minimum_required(VERSION 3.5)"
    "cmake_minimum_required(VERSION 3.10)")
ifss_patch_dependency(zydis/CMakeLists.txt
    "cmake_minimum_required(VERSION 3.9 FATAL_ERROR)"
    "cmake_minimum_required(VERSION 3.10 FATAL_ERROR)")
ifss_patch_dependency(zydis/dependencies/zycore/CMakeLists.txt
    "cmake_minimum_required(VERSION 3.9 FATAL_ERROR)"
    "cmake_minimum_required(VERSION 3.10 FATAL_ERROR)")
ifss_patch_dependency(toml11/CMakeLists.txt
    "cmake_policy(SET CMP0127 OLD) # syntax of condition changed in 3.22"
    "cmake_policy(SET CMP0127 NEW) # use full condition syntax")
ifss_patch_dependency(sirit/CMakeLists.txt
    "set(CMAKE_WARN_DEPRECATED ON)"
    "# Use CMake's default deprecation diagnostics; the legacy variable is obsolete.")
ifss_patch_dependency(protobuf/CMakeLists.txt
    "# Revert to old behavior for MSVC debug symbols."
    "# Use the parent project's MSVC debug information abstraction.")
ifss_patch_dependency(protobuf/CMakeLists.txt
    "cmake_policy(SET CMP0141 OLD)"
    "cmake_policy(SET CMP0141 NEW)")
ifss_patch_dependency(CMakeLists.txt
    [=[set(ALSOFT_CONFIG OFF CACHE BOOL "" FORCE)]=]
    [=[unset(ALSOFT_CONFIG CACHE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)]=])

# clang-cl implements the MSVC ABI, but not cl.exe's /MP scheduling or legacy
# preprocessor switches. Ninja already schedules compilation.
ifss_patch_dependency(protobuf/cmake/protobuf-configure-target.cmake
    [=[target_compile_options("${target}" PRIVATE /MP)]=]
    [=[if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options("${target}" PRIVATE /MP)
        endif()]=])
ifss_patch_dependency(protobuf/cmake/libupb.cmake
    [=[if (MSVC)
  target_compile_options(libupb PRIVATE]=]
    [=[if (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(libupb PRIVATE]=])
ifss_patch_dependency(libressl/CMakeLists.txt
    [=[set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -MP -W4 ${MSVC_DISABLED_WARNINGS_STR}")]=]
    [=[set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W4 ${MSVC_DISABLED_WARNINGS_STR}")
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        string(APPEND CMAKE_C_FLAGS " /MP")
    endif()]=])

# These are exactly 16 binary bytes, not NUL-terminated text.
ifss_patch_dependency(libressl/crypto/compat/chacha_private.h
    [=[static const char sigma[16] = "expand 32-byte k";]=]
    [=[static const char sigma[16] = {'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k'};]=])
ifss_patch_dependency(libressl/crypto/compat/chacha_private.h
    [=[static const char tau[16] = "expand 16-byte k";]=]
    [=[static const char tau[16] = {'e','x','p','a','n','d',' ','1','6','-','b','y','t','e',' ','k'};]=])

# Use the libraries' existing Clang-compatible unused declarations.
ifss_patch_dependency(zstd/lib/common/compiler.h
    [=[#if defined(__GNUC__) || defined(__IAR_SYSTEMS_ICC__)]=]
    [=[#if defined(__GNUC__) || defined(__clang__) || defined(__IAR_SYSTEMS_ICC__)]=])
ifss_patch_dependency(zstd/lib/common/compiler.h
    [=[#if defined(__GNUC__)
#  define MEM_STATIC]=]
    [=[#if defined(__GNUC__) || defined(__clang__)
#  define MEM_STATIC]=])
ifss_patch_dependency(libusb/libusb/libusbi.h
    [=[#if defined(UNREFERENCED_PARAMETER) && !defined(__GNUC__)]=]
    [=[#if defined(UNREFERENCED_PARAMETER) && !defined(__GNUC__) && !defined(__clang__)]=])

# The private library build never uses the optional unprefixed zlib aliases.
# Leave consumers' definitions and all mz_* compression/ZIP APIs unchanged.
ifss_patch_dependency(miniz/CMakeLists.txt
    [=[add_library(${PROJECT_NAME} ${miniz_SOURCE})]=]
    [=[add_library(${PROJECT_NAME} ${miniz_SOURCE})
  target_compile_definitions(${PROJECT_NAME} PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)]=])

# Express the same packing in this header rather than across SDK includes.
ifss_patch_dependency(libusb/libusb/os/windows_common.h
    [=[#include <pshpack1.h>]=] [=[#pragma pack(push, 1)]=])
ifss_patch_dependency(libusb/libusb/os/windows_common.h
    [=[#include <poppack.h>]=] [=[#pragma pack(pop)]=])

# Windows NT version query replaces deprecated, manifest-dependent GetVersionEx.
# https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_osversioninfoexw
ifss_patch_dependency(libusb/libusb/os/windows_common.c
    [=[OSVERSIONINFOEXA vi, vi2;]=]
    [=[OSVERSIONINFOEXW vi;
    OSVERSIONINFOEXA vi2;
    typedef LONG (WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    rtl_get_version_fn rtl_get_version = ntdll ?
        (rtl_get_version_fn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;]=])
ifss_patch_dependency(libusb/libusb/os/windows_common.c
    [=[if (!GetVersionExA((OSVERSIONINFOA *)&vi)) {
		memset(&vi, 0, sizeof(vi));
		vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
		if (!GetVersionExA((OSVERSIONINFOA *)&vi))
			return WINDOWS_UNDEFINED;
	}]=]
    [=[if (!rtl_get_version || rtl_get_version((OSVERSIONINFOW *)&vi) != 0)
        return WINDOWS_UNDEFINED;]=])

# Header-only optional directory functions are inline, with identical linkage.
ifss_patch_dependency(libressl/include/compat/dirent_msvc.h
    [=[static struct _wdirent*
_wreaddir]=]
    [=[static __inline struct _wdirent*
_wreaddir]=])
ifss_patch_dependency(libressl/include/compat/dirent_msvc.h
    [=[static void
rewinddir]=]
    [=[static __inline void
rewinddir]=])
# Include the owning enum definition instead of using a C enum forward extension.
ifss_patch_dependency(libressl/ssl/tls_internal.h
    [=[#include <openssl/evp.h>]=]
    [=[#include <openssl/evp.h>
#include <openssl/ssl.h>]=])
ifss_patch_dependency(libressl/ssl/tls_internal.h
    [=[enum ssl_encryption_level_t;]=]
    [=[/* ssl_encryption_level_t is defined by openssl/ssl.h above. */]=])

# Sirit has the same cl.exe-only scheduler flag in its local option list.
ifss_patch_dependency(sirit/CMakeLists.txt
    [=[        /MP
]=]
    [=[        $<$<CXX_COMPILER_ID:MSVC>:/MP>
]=])
