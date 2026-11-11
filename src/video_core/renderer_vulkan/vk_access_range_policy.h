// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Vulkan {
struct AccessRangePolicyStatus { bool enabled; bool used; };
// Startup-only experiment receipts; no per-access atomic counters.
AccessRangePolicyStatus GetAccessRangePolicyStatus() noexcept;
}
