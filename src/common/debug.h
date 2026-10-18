// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef _MSC_VER
#define BREAKPOINT __debugbreak
#else
#define BREAKPOINT __builtin_trap
#endif
#define TRACY_GPU_ENABLED 0
#define EMULATOR_TRACE
#define RENDERER_TRACE
#define HLE_TRACE
#define TRACE_HINT(...)
#define TRACE_WARN(...)
#define TRACE_ERROR(...)
#define TRACE_CRIT(...)
#define TRACK_ALLOC(...)
#define TRACK_FREE(...)
#define FRAME_END
#define FIBER_ENTER(...)
#define FIBER_EXIT
