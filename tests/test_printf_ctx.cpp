// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <gtest/gtest.h>
#include "core/libraries/libc_internal/printf.h"

namespace {
struct GuestArgs {
    Common::VaRegSave registers{};
    std::array<u64, 2> overflow{};
    Common::VaList args{0, 48, overflow.data(), &registers};
};

TEST(GuestPrintf, TruncationPreservesGuardsAndReportsRequiredLength) {
    GuestArgs guest;
    const std::string text(1024, 'a');
    guest.registers.gp[0] = reinterpret_cast<u64>(text.c_str());
    std::array<char, 18> output;
    output.fill('#');
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(
                  output.data() + 1, 16, "%s", &guest.args), 1024);
    EXPECT_EQ(output.front(), '#');
    EXPECT_EQ(output.back(), '#');
    EXPECT_EQ(output[16], '\0');
    EXPECT_EQ(std::string(output.data() + 1), std::string(15, 'a'));
}

TEST(GuestPrintf, ZeroSizeCountsWithoutWriting) {
    GuestArgs guest;
    guest.registers.gp[0] = 42;
    char guard = '#';
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(&guard, 0, "value=%d", &guest.args), 8);
    EXPECT_EQ(guard, '#');
    GuestArgs measure;
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(nullptr, 0, "hello", &measure.args), 5);
}

TEST(GuestPrintf, OneByteBufferOnlyReceivesTerminator) {
    GuestArgs guest;
    std::array<char, 3> output{'#', '#', '#'};
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(output.data() + 1, 1, "abc", &guest.args), 3);
    EXPECT_EQ(output[0], '#');
    EXPECT_EQ(output[1], '\0');
    EXPECT_EQ(output[2], '#');
}

TEST(GuestPrintf, UsesGuestIntegerPointerAndFloatingRegisters) {
    GuestArgs guest;
    guest.registers.gp[0] = 7;
    guest.registers.gp[1] = reinterpret_cast<u64>("alpha");
    const double value = 1.5;
    std::memcpy(&guest.registers.fp[0], &value, sizeof(value));
    std::array<char, 32> output{};
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(
                  output.data(), output.size(), "%d/%s/%.1f", &guest.args), 11);
    EXPECT_STREQ(output.data(), "7/alpha/1.5");
}

TEST(GuestPrintf, GuestLongIs64BitOnWindowsAndSignedMinDoesNotOverflow) {
    GuestArgs guest;
    guest.registers.gp[0] = 0x100000000ULL;
    guest.registers.gp[1] = static_cast<u64>(std::numeric_limits<s64>::min());
    std::array<char, 64> output{};
    EXPECT_EQ(Libraries::LibcInternal::vsnprintf_ctx(
                  output.data(), output.size(), "%lu/%ld", &guest.args), 31);
    EXPECT_STREQ(output.data(), "4294967296/-9223372036854775808");
}
} // namespace
