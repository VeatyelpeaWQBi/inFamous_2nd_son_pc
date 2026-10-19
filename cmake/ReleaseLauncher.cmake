# SPDX-License-Identifier: GPL-2.0-or-later
# Existing Windows .NET Framework only; no SDK, NuGet, interpreter or download.
file(TO_CMAKE_PATH "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319/csc.exe" IFSS_CSC)
if(NOT EXISTS "${IFSS_CSC}")
    message(FATAL_ERROR "Existing x64 .NET Framework C# compiler is missing: ${IFSS_CSC}")
endif()
set(IFSS_MENU_SOURCES)
foreach(name ProjectPaths LauncherSettings GameIdentity LaunchMenu ReleaseEntry)
    list(APPEND IFSS_MENU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/scripts/native/${name}.cs")
endforeach()
set(IFSS_MENU_EXE "${CMAKE_BINARY_DIR}/support/Ifss.Launcher.exe")
add_custom_command(OUTPUT "${IFSS_MENU_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/support"
    COMMAND "${IFSS_CSC}" /nologo /target:winexe /platform:x64 /optimize+ /warn:4 /warnaserror+
        /define:IFSS_RELEASE /main:Ifss.Native.ReleaseEntry
        /reference:System.dll /reference:System.Core.dll /reference:System.Web.Extensions.dll
        /reference:System.Drawing.dll /reference:System.Windows.Forms.dll
        "/win32icon:${IFSS_GAME_ICON_PATH}" "/out:${IFSS_MENU_EXE}" ${IFSS_MENU_SOURCES}
    DEPENDS ${IFSS_MENU_SOURCES} "${IFSS_GAME_ICON_PATH}"
    COMMENT "Building the native Windows launch menu"
    VERBATIM)
add_custom_target(ifss_launcher DEPENDS "${IFSS_MENU_EXE}")
add_dependencies(shadps4 ifss_launcher)
