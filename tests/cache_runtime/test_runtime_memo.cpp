// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <iostream>
#include "video_core/runtime_memo.h"

static void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void InputChanges() {
    std::array<std::uint32_t, 12> shader{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto bytes = std::as_bytes(std::span{shader});
    VideoCore::RuntimeSnapshot<64> metadata;
    Check(!metadata.Matches(bytes), "uninitialized metadata never reused");
    Check(metadata.Capture(bytes) && metadata.Matches(bytes), "unchanged scan prefix reused");
    // A newer header earlier in the searched range must invalidate the old selection.
    shader[2] = 0x5362724f;
    shader[3] = 0x00726468;
    Check(!metadata.Matches(bytes), "earlier header at the same address forces a scan");
    shader[2] = 3;
    shader[3] = 4;
    shader[11] ^= 1;
    Check(!metadata.Matches(bytes), "metadata/hash mutation forces a scan");
    shader[11] ^= 1;
    Check(metadata.Matches(bytes), "exact restoration is safe");
    Check(!metadata.Matches(bytes.first(8)), "shorter reused allocation cannot match");
    metadata.Invalidate();
    Check(!metadata.Matches(bytes), "explicit invalidation defeats old content");
}

static void BoundedInputs() {
    VideoCore::RuntimeSnapshot<16> inputs;
    std::array<std::byte, 16> small{};
    std::array<std::byte, 17> large{};
    Check(inputs.Capture(small), "snapshot accepts its exact limit");
    Check(!inputs.Capture(large), "oversized resource table falls back");
    Check(!inputs.Matches(small), "failed capture cannot expose the previous selection");
    Check(inputs.Capture({}) && inputs.Matches({}), "empty input is valid after capture");
    small[0] = std::byte{1};
    Check(inputs.Capture(small), "capture copies input ownership");
    small[0] = std::byte{2};
    Check(!inputs.Matches(small), "snapshot does not alias mutable source storage");
}

static void UploadLifetime() {
    VideoCore::RuntimeUploadMemo<16> upload;
    std::array<std::byte, 8> contents{};
    int graphics_stream{}, compute_stream{};
    upload.Capture(contents, &graphics_stream, 12, 3, 256);
    Check(upload.Matches(contents, &graphics_stream, 12, 3) && upload.Offset() == 256,
          "read-only table reuses the same live upload");
    Check(!upload.Matches(contents, &compute_stream, 12, 3), "queue owner must match");
    Check(!upload.Matches(contents, &graphics_stream, 13, 3), "submission lifetime must match");
    Check(!upload.Matches(contents, &graphics_stream, 12, 4),
          "ring wrap invalidates an offset even in the same submission");
    contents[3] = std::byte{42};
    Check(!upload.Matches(contents, &graphics_stream, 12, 3), "table contents must match");
    std::array<std::byte, 17> large{};
    upload.Capture(large, &graphics_stream, 12, 3, 0);
    Check(!upload.Matches(contents, &graphics_stream, 12, 3), "oversized upload falls back");
}

int main() {
    InputChanges();
    BoundedInputs();
    UploadLifetime();
    std::cout << "PASS: runtime input changes, bounded ownership, upload submission/queue/wrap\n";
}
