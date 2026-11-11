// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include "shader_recompiler/resource.h"
#include "video_core/command_buffer_sharps.h"

static void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    struct Inputs {
        std::array<u32, 16> flattened_ud_buf{};
        u64 pgm_base{};
    } info;
    using Snapshot = VideoCore::CommandBufferSharps<AmdGpu::Buffer, Shader::NUM_BUFFERS>;
    Snapshot snapshot;
    Shader::BufferResource desc{};
    using Fetch = Shader::SharpFetch<AmdGpu::Buffer>;
    Check(!snapshot.Find(&info, 0, 1, 0), "uninitialized snapshot reused");
    AmdGpu::Buffer raw{};
    raw.base_address = 0x123400;
    raw.num_records = 4096;
    raw.stride = 4;
    std::memcpy(info.flattened_ud_buf.data(), &raw, sizeof(raw));
    desc.sharp_fetch.summary = Fetch::Summary::SingleLoad;
    desc.sharp_fetch.offsets[0] = 0;
    Check(desc.GetSharp(info) == raw, "single load must retain address, size and stride");
    info.pgm_base = 0x10000;
    desc.post_op = Shader::SharpFetchPostOp::OffsetByProgramBase;
    auto expected = raw;
    expected.base_address += info.pgm_base;
    Check(desc.GetSharp(info) == expected, "program-base post-op was lost");
    desc.post_op = Shader::SharpFetchPostOp::BitwiseOrDw1WithImm;
    desc.post_op_dw1_mask = 1u << 16;
    expected = raw;
    reinterpret_cast<u32*>(&expected)[1] |= desc.post_op_dw1_mask;
    Check(desc.GetSharp(info) == expected, "immediate post-op was lost");
    raw.type = 3;
    std::memcpy(info.flattened_ud_buf.data(), &raw, sizeof(raw));
    Check(desc.GetSharp(info) == AmdGpu::Buffer::Null(), "invalid type must normalize to null");
    u32 rng = 0x39e72a81;
    u64 epoch = 0;
    for (const auto summary :
         {Fetch::Summary::SingleLoad, Fetch::Summary::MultiLoad, Fetch::Summary::Invalid}) {
        for (const auto op :
             {Shader::SharpFetchPostOp::None, Shader::SharpFetchPostOp::BitwiseOrDw1WithImm,
              Shader::SharpFetchPostOp::OffsetByProgramBase}) {
            for (u32 trial = 0; trial < 1024; ++trial) {
                for (auto& word : info.flattened_ud_buf) {
                    rng ^= rng << 13;
                    rng ^= rng >> 17;
                    rng ^= rng << 5;
                    word = rng;
                }
                desc.sharp_fetch.summary = summary;
                desc.sharp_fetch.load_mask = trial & 15;
                desc.sharp_fetch.offsets = {0, 3, 7, 11};
                desc.sharp_fetch.immediates = {rng, rng ^ 17, rng ^ 555, rng ^ 1111};
                desc.post_op = op;
                desc.post_op_dw1_mask = trial << 16;
                info.pgm_base = static_cast<u64>(rng) << 4;
                snapshot.Begin(&info, ++epoch, 2);
                const auto decoded = desc.GetSharp(info);
                snapshot.Store(0, decoded);
                snapshot.Store(1, AmdGpu::Buffer::Null());
                Check(!snapshot.Find(&info, epoch, 2, 0), "cold/unpublished snapshot reused");
                snapshot.Publish();
                const auto* cached = snapshot.Find(&info, epoch, 2, 0);
                Check(cached && *cached == desc.GetSharp(info), "decoded descriptor differs");
                Check(snapshot.Find(&info, epoch, 2, 1) &&
                          *snapshot.Find(&info, epoch, 2, 1) == AmdGpu::Buffer::Null(),
                      "null descriptor was treated as a miss");
                Check(!snapshot.Find(&info, epoch + 1, 2, 0), "stale epoch reused");
                Check(!snapshot.Find(&desc, epoch, 2, 0), "different stage reused");
                Check(!snapshot.Find(&info, epoch, 1, 0), "changed count reused");
                Check(!snapshot.Find(&info, epoch, 2, 2), "out-of-range entry reused");
                Check(!snapshot.Find(&info, epoch, 2, SIZE_MAX), "wrapped index reused");
            }
        }
    }
    snapshot.Begin(&info, ++epoch, Shader::NUM_BUFFERS);
    snapshot.Store(Shader::NUM_BUFFERS - 1, AmdGpu::Buffer::Null());
    snapshot.Publish();
    Check(snapshot.Find(&info, epoch, Shader::NUM_BUFFERS, Shader::NUM_BUFFERS - 1),
          "last legal entry rejected");
    Check(!snapshot.Find(&info, epoch, Shader::NUM_BUFFERS, 0), "absent entry reused");
    snapshot.Begin(&info, ++epoch, Shader::NUM_BUFFERS + 1);
    snapshot.Store(0, AmdGpu::Buffer::Null());
    snapshot.Publish();
    Check(!snapshot.Find(&info, epoch, Shader::NUM_BUFFERS + 1, 0), "oversized snapshot reused");
    snapshot.Begin(nullptr, 0, 0);
    snapshot.Publish();
    Check(!snapshot.Find(nullptr, 0, 0, 0), "ineligible command reused");
    std::cout << "Command descriptor equivalence/lifetime checks passed (9216 decode cases).\n";
}
