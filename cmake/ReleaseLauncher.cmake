# SPDX-License-Identifier: GPL-2.0-or-later
# Existing Windows .NET Framework only; no SDK, NuGet, interpreter or download.
file(TO_CMAKE_PATH "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319/csc.exe" IFSS_CSC)
if(NOT EXISTS "${IFSS_CSC}")
    message(FATAL_ERROR "Existing x64 .NET Framework C# compiler is missing: ${IFSS_CSC}")
endif()
get_filename_component(IFSS_MENU_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../scripts/native" ABSOLUTE)
set(IFSS_MENU_SOURCES)
set(IFSS_MENU_NATIVE_SOURCES)
foreach(name ProjectPaths LauncherSettings GameIdentity LaunchMenu ReleaseEntry)
    set(source "${IFSS_MENU_SOURCE_DIR}/${name}.cs")
    list(APPEND IFSS_MENU_SOURCES "${source}")
    # Framework csc misreads forward-slash source paths; only its command-line
    # arguments need native spelling. CMake dependency paths stay normalized.
    cmake_path(NATIVE_PATH source NORMALIZE native_source)
    list(APPEND IFSS_MENU_NATIVE_SOURCES "${native_source}")
endforeach()
set(IFSS_MENU_EXE "${CMAKE_BINARY_DIR}/support/Ifss.Launcher.exe")
cmake_path(NATIVE_PATH IFSS_MENU_EXE NORMALIZE IFSS_MENU_NATIVE_EXE)
cmake_path(NATIVE_PATH IFSS_GAME_ICON_PATH NORMALIZE IFSS_MENU_NATIVE_ICON)
add_custom_command(OUTPUT "${IFSS_MENU_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/support"
    COMMAND "${IFSS_CSC}" /nologo /utf8output /target:winexe /platform:x64 /optimize+ /warn:4 /warnaserror+
        /define:IFSS_RELEASE /main:Ifss.Native.ReleaseEntry
        /reference:System.dll /reference:System.Core.dll /reference:System.Web.Extensions.dll
        /reference:System.Drawing.dll /reference:System.Windows.Forms.dll
        "/win32icon:${IFSS_MENU_NATIVE_ICON}" "/out:${IFSS_MENU_NATIVE_EXE}" ${IFSS_MENU_NATIVE_SOURCES}
    DEPENDS ${IFSS_MENU_SOURCES} "${IFSS_GAME_ICON_PATH}"
    COMMENT "Building the native Windows launch menu"
    VERBATIM)
add_custom_target(ifss_launcher DEPENDS "${IFSS_MENU_EXE}")
add_dependencies(shadps4 ifss_launcher)
