// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "razor_cpu.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::RazorCpu {

s32 PS4_SYSV_ABI sceRazorCpuBeginLogicalFileAccess() {
    return ORBIS_OK;
}

void PS4_SYSV_ABI sceRazorCpuDisableFiberUserMarkers() { ; }

s32 PS4_SYSV_ABI sceRazorCpuEndLogicalFileAccess() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuFiberLogNameChange() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuFiberSwitch() {
    return ORBIS_OK;
}

bool PS4_SYSV_ABI sceRazorCpuFlushOccurred() {
    return false;
}

s32 PS4_SYSV_ABI sceRazorCpuGetDataTagStorageSize() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuGpuMarkerSync() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuInitDataTags() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuInitializeGpuMarkerContext() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuInitializeInternal() {
    return ORBIS_OK;
}

bool PS4_SYSV_ABI sceRazorCpuIsCapturing() {
    return false;
}

s32 PS4_SYSV_ABI sceRazorCpuJobManagerDispatch() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuJobManagerJob() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuJobManagerSequence() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuNamedSync() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuPlotValue() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuPopMarker() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuPushMarker() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuPushMarkerStatic() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuResizeTaggedBuffer() {
    return ORBIS_OK;
}

void PS4_SYSV_ABI sceRazorCpuSetPopMarkerCallback() { ; }

void PS4_SYSV_ABI sceRazorCpuSetPushMarkerCallback() { ; }

void PS4_SYSV_ABI sceRazorCpuSetPushMarkerStaticCallback() { ; }

s32 PS4_SYSV_ABI sceRazorCpuShutdownDataTags() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuStartCaptureInternal() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuStopCaptureInternal() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuSync() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuTagArray() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuTagBuffer() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuUnTagBuffer() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuWorkloadRunBegin() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuWorkloadRunEnd() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuWorkloadSubmit() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRazorCpuWriteBookmark() {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("JFzLJBlYIJE", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuBeginLogicalFileAccess);
    LIB_FUNCTION("SfRTRZ1Sh+U", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuDisableFiberUserMarkers);
    LIB_FUNCTION("gVioM9cbiDs", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuEndLogicalFileAccess);
    LIB_FUNCTION("G90IIOtgFQ0", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuFiberLogNameChange);
    LIB_FUNCTION("PAytDtFGpqY", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuFiberSwitch);
    LIB_FUNCTION("sPhrQD31ClM", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuFlushOccurred);
    LIB_FUNCTION("B782NptkGUc", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuGetDataTagStorageSize);
    LIB_FUNCTION("EH9Au2RlSrE", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuGpuMarkerSync);
    LIB_FUNCTION("A7oRMdaOJP8", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuInitDataTags);
    LIB_FUNCTION("NFwh-J-BrI0", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuInitializeGpuMarkerContext);
    LIB_FUNCTION("ElNyedXaa4o", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuInitializeInternal);
    LIB_FUNCTION("EboejOQvLL4", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuIsCapturing);
    LIB_FUNCTION("dnEdyY4+klQ", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuJobManagerDispatch);
    LIB_FUNCTION("KP+TBWGHlgs", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuJobManagerJob);
    LIB_FUNCTION("9FowWFMEIM8", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuJobManagerSequence);
    LIB_FUNCTION("XCuZoBSVFG8", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuNamedSync);
    LIB_FUNCTION("njGikRrxkC0", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuPlotValue);
    LIB_FUNCTION("YpkGsMXP3ew", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuPopMarker);
    LIB_FUNCTION("zw+celG7zSI", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuPushMarker);
    LIB_FUNCTION("uZrOwuNJX-M", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuPushMarkerStatic);
    LIB_FUNCTION("D0yUjM33QqU", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuResizeTaggedBuffer);
    LIB_FUNCTION("jqYWaTfgZs0", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuSetPopMarkerCallback);
    LIB_FUNCTION("DJsHcEb94n0", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuSetPushMarkerCallback);
    LIB_FUNCTION("EZtqozPTS4M", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuSetPushMarkerStaticCallback);
    LIB_FUNCTION("emklx7RK-LY", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuShutdownDataTags);
    LIB_FUNCTION("TIytAjYeaik", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuStartCaptureInternal);
    LIB_FUNCTION("jWpkVWdMrsM", "libSceRazorCpu", 1, "libSceRazorCpu",
                 sceRazorCpuStopCaptureInternal);
    LIB_FUNCTION("Ax7NjOzctIM", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuSync);
    LIB_FUNCTION("we3oTKSPSTw", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuTagArray);
    LIB_FUNCTION("vyjdThnQfQQ", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuTagBuffer);
    LIB_FUNCTION("0yNHPIkVTmw", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuUnTagBuffer);
    LIB_FUNCTION("Crha9LvwvJM", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuWorkloadRunBegin);
    LIB_FUNCTION("q1GxBfGHO0s", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuWorkloadRunEnd);
    LIB_FUNCTION("6rUvx-6QmYc", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuWorkloadSubmit);
    LIB_FUNCTION("G3brhegfyNg", "libSceRazorCpu", 1, "libSceRazorCpu", sceRazorCpuWriteBookmark);
}

} // namespace Libraries::RazorCpu