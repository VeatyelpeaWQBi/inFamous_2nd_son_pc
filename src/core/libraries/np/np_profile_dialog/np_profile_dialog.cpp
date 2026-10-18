// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "np_profile_dialog.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/commondialog.h"
#include "np_profile_dialog_ui.h"
#include <cstring>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

namespace Libraries::Np::NpProfileDialog {

static auto g_status = Libraries::CommonDialog::Status::NONE;
static NpProfileDialogState g_state{};
static OrbisNpProfileDialogResult g_result{};
static NpProfileDialogUi g_profile_dialog_ui;

Libraries::CommonDialog::Error sceNpProfileDialogOpen(OrbisNpProfileDialogParam* param) {
    if (g_status != Libraries::CommonDialog::Status::INITIALIZED &&
        g_status != Libraries::CommonDialog::Status::FINISHED) {
        LOG_ERROR(Lib_NpProfileDialog, "called without initialize");
        return Libraries::CommonDialog::Error::INVALID_STATE;
    }
    if (param == nullptr) {
        LOG_ERROR(Lib_NpProfileDialog, "param is nullptr");
        return Libraries::CommonDialog::Error::ARG_NULL;
    }

    ;

    NpProfileDialogState state{};
    state.onlineId = std::string(param->targetOnlineId.data);
    state.hasAccountId = false;
    state.mode = param->mode;
    state.userId = param->userId;
    g_state = state;
    g_result = {};
    g_result.userData = param->userData;
    g_status = Libraries::CommonDialog::Status::RUNNING;


    g_profile_dialog_ui = NpProfileDialogUi(&g_state, &g_status, &g_result);

    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Error PS4_SYSV_ABI sceNpProfileDialogClose() {
    if (g_status == Libraries::CommonDialog::Status::NONE) {
        return Libraries::CommonDialog::Error::NOT_INITIALIZED;
    }
    if (g_status != Libraries::CommonDialog::Status::RUNNING) {
        return Libraries::CommonDialog::Error::NOT_RUNNING;
    }
    g_profile_dialog_ui.Finish(Libraries::CommonDialog::Result::OK);
    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Error PS4_SYSV_ABI
sceNpProfileDialogGetResult(OrbisNpProfileDialogResult* result) {

    if (g_status == Libraries::CommonDialog::Status::NONE) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: not initialized (g_status=NONE)");
        return Libraries::CommonDialog::Error::NOT_INITIALIZED;
    }
    if (result == nullptr) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: result pointer is nullptr");
        return Libraries::CommonDialog::Error::ARG_NULL;
    }
    if (g_status != Libraries::CommonDialog::Status::FINISHED) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: dialog not finished (g_status={})",
                  static_cast<int>(g_status));
        return Libraries::CommonDialog::Error::NOT_FINISHED;
    }

    *result = g_result;

    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceNpProfileDialogGetStatus() {
    return g_status;
}

Libraries::CommonDialog::Error PS4_SYSV_ABI sceNpProfileDialogInitialize() {
    if (!CommonDialog::g_isInitialized) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: system not initialized");
        return Libraries::CommonDialog::Error::NOT_SYSTEM_INITIALIZED;
    }
    if (g_status != Libraries::CommonDialog::Status::NONE) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: already initialized (g_status={})",
                  static_cast<int>(g_status));
        return Libraries::CommonDialog::Error::ALREADY_INITIALIZED;
    }
    if (CommonDialog::g_isUsed) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: dialog system busy (g_isUsed=true)");
        return Libraries::CommonDialog::Error::BUSY;
    }

    g_status = Libraries::CommonDialog::Status::INITIALIZED;
    CommonDialog::g_isUsed = true;
    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Error PS4_SYSV_ABI
sceNpProfileDialogOpenA(OrbisNpProfileDialogParamA* param) {
    if (g_status != Libraries::CommonDialog::Status::INITIALIZED &&
        g_status != Libraries::CommonDialog::Status::FINISHED) {
        LOG_ERROR(Lib_NpProfileDialog, "called without initialize");
        return Libraries::CommonDialog::Error::INVALID_STATE;
    }
    if (param == nullptr) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: param is nullptr");
        return Libraries::CommonDialog::Error::ARG_NULL;
    }

    ;
    LOG_ERROR(Lib_NpProfileDialog, "(STUBBED) called");

    NpProfileDialogState state{};
    state.accountId = param->targetAccountId;
    state.hasAccountId = true;
    state.mode = param->mode;
    state.userId = param->userId;
    g_state = state;
    g_result = {};
    g_result.userData = param->userData;
    g_status = Libraries::CommonDialog::Status::RUNNING;


    g_profile_dialog_ui = NpProfileDialogUi(&g_state, &g_status, &g_result);

    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Error PS4_SYSV_ABI sceNpProfileDialogTerminate() {

    if (g_status == Libraries::CommonDialog::Status::RUNNING) {
        LOG_ERROR(Lib_NpProfileDialog, "dialog is running, closing it first");
        sceNpProfileDialogClose();
    }
    if (g_status == Libraries::CommonDialog::Status::NONE) {
        LOG_ERROR(Lib_NpProfileDialog, "failed: not initialized (g_status=NONE)");
        return Libraries::CommonDialog::Error::NOT_INITIALIZED;
    }

    g_status = Libraries::CommonDialog::Status::NONE;
    CommonDialog::g_isUsed = false;

    return Libraries::CommonDialog::Error::OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceNpProfileDialogUpdateStatus() {
    return g_status;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("uj9Cz7Tk0cc", "libSceNpProfileDialogCompat", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogOpen);
    LIB_FUNCTION("wkwjz0Xdo2A", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogClose);
    LIB_FUNCTION("8rhLl1-0W-o", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogGetResult);
    LIB_FUNCTION("3BqoiFOjSsk", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogGetStatus);
    LIB_FUNCTION("Lg+NCE6pTwQ", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogInitialize);
    LIB_FUNCTION("uj9Cz7Tk0cc", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogOpen);
    LIB_FUNCTION("nrQRlLKzdwE", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogOpenA);
    LIB_FUNCTION("0Sp9vJcB1-w", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogTerminate);
    LIB_FUNCTION("haVZE9FgKqE", "libSceNpProfileDialog", 1, "libSceNpProfileDialog",
                 sceNpProfileDialogUpdateStatus);
};

} // namespace Libraries::Np::NpProfileDialog
