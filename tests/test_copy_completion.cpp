// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <thread>
#include "video_core/buffer_cache/copy_coalesce.h"
#include "video_core/buffer_cache/readback_completion.h"
#include "common/auxiliary_evidence.h"

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
struct Region { std::uint64_t srcOffset, dstOffset, size; };
int main() {
    const std::array<std::uint32_t, 7> dma{0xc0055000, 0x04004000, 0, 0, 0, 0, 4};
    Require(Common::AuxiliaryEvidence::ValidWords(1, dma), "selected DMA packet has exact opcode and width");
    Require(!Common::AuxiliaryEvidence::ValidWords(2, dma), "release cannot be inferred from a DMA header");
    Require(!Common::AuxiliaryEvidence::ValidWords(1, std::span{dma}.first(6)), "truncated packet evidence rejected");
    Require(Common::AuxiliaryEvidence::Pack(0x89abcdef, 0xfedcba98) == 0xfedcba9889abcdefULL,
            "packet packing preserves all unsigned bits");
    std::mt19937 random{11256};
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        std::array<unsigned char, 1024> source{}, original{}, merged{};
        for (auto& byte : source) byte = static_cast<unsigned char>(random());
        std::array<Region, 32> regions{}, result{};
        std::uint64_t src{}, dst{};
        for (auto& region : regions) {
            src += random() % 2;
            dst += random() % 2;
            const auto size = 1 + random() % 8;
            region = {src, dst, size};
            src += size;
            dst += size;
            std::memcpy(original.data() + region.dstOffset, source.data() + region.srcOffset, region.size);
        }
        const auto count = VideoCore::CoalesceCopyRegions<Region>(regions, result);
        Require(count > 0 && count <= regions.size(), "valid bounded region plan");
        for (std::size_t i = 0; i < count; ++i) {
            const auto& region = result[i];
            std::memcpy(merged.data() + region.dstOffset, source.data() + region.srcOffset, region.size);
        }
        Require(merged == original, "byte results and untouched gaps match original copy operations");
    }
    constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    const std::array<Region, 2> wrapped{{{Max - 3, 8, 8}, {4, 16, 4}}};
    std::array<Region, 2> result{};
    Require(VideoCore::CoalesceCopyRegions<Region>(wrapped, result) == 2, "overflow cannot merge unrelated ranges");
    const std::array<Region, 3> contiguous{{{0, 16, 4}, {4, 20, 4}, {8, 24, 4}}};
    std::array<Region, 3> output{};
    Require(VideoCore::CoalesceCopyRegions<Region>(contiguous, output) == 1 && output[0].size == 12,
            "contiguous source and destination become one identical byte interval");
    Require(VideoCore::CoalesceCopyRegions<Region>(contiguous, result) == 0, "insufficient scratch space retains fallback");
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        VideoCore::ReadbackCompletion completion{0x1234, 8, iteration};
        std::array<unsigned, 256> backing{};
        Require(!completion.Ready(), "a new request is never pre-completed by reused storage");
        std::jthread producer([&] {
            backing.fill(iteration + 1);
            completion.Publish();
        });
        while (!completion.Ready()) std::this_thread::yield();
        for (auto value : backing) Require(value == iteration + 1, "release publication exposes all prior backing writes");
        Require(completion.address == 0x1234 && completion.size == 8 && completion.trace_id == iteration,
                "request identity remains exact across publication");
    }
    std::puts("Copy byte-equivalence, overflow and readback publication tests passed.");
}
