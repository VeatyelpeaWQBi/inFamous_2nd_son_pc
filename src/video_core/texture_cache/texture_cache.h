// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/perf_stats.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "common/multi_level_page_table.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t ADDRESS_SPACE_BITS = 40;
        static constexpr size_t L1_BITS = 10;
        static constexpr size_t PAGE_BITS = 20;
        static constexpr bool NULL_CHECK = true;
    };
    using PageTable = Common::MultiLevelPageTable<Traits>;
    static constexpr u64 NumImagePages = u64{1}
                                         << (Traits::ADDRESS_SPACE_BITS - Traits::PAGE_BITS);

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Frame x-ray: writes the image at the address (or overlapping it) to a raw file in the log
    /// folder, waiting for the GPU. Returns false when no image is there.
    /// Frame x-ray: writes the image at the address raw to the log folder (the one used most
    /// recently among those there, or the one of the given size when width is not 0).
    bool DumpImageToFile(VAddr address, std::string_view prefix, u32 width = 0, u32 height = 0);

    /// Generation of the cache: grows whenever an image is registered, unregistered or replaced
    /// (rebind needed), so that a binding found for a descriptor can be reused while the cache
    /// does not change (Rasterizer texture binding memo). Lock free.
    [[nodiscard]] u64 Generation() const noexcept {
        return generation.load(std::memory_order_acquire);
    }

    /// Generation of an image: grows whenever the guest (or a GPU copy) wrote its memory, so
    /// that the image needs a refresh check before its next binding. Lock free.
    [[nodiscard]] u32 ImageGeneration(ImageId image_id) const noexcept {
        return image_id.index < NumImageGens
                   ? image_gens[image_id.index].load(std::memory_order_acquire)
                   : 0;
    }

    /// Garbage collector tick, one per guest frame (command processor thread).
    [[nodiscard]] u64 GcTick() const noexcept {
        return gc_tick;
    }

    /// The use order of the garbage collector, as FindImage updates it for an image it finds
    /// (Rasterizer render target memo, once per frame and image).
    void TouchImageLocked(ImageId image_id) {
        std::scoped_lock lock{mutex};
        TouchImage(slot_images[image_id]);
    }

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        Image& image = slot_images[image_id];
        // Clean, and tracked over its whole range: nothing to do but touch it (bound every
        // draw as a render target or texture: the lock is skipped). A guest write arriving
        // this instant is seen by the next binding, as it would be with the lock.
        if (Common::Perf::GetTuning().tex_memo.load(std::memory_order_relaxed) &&
            False(image.flags & ImageFlagBits::Dirty) &&
            image.track_addr == image.info.guest_address &&
            image.track_addr_end == image.info.guest_address + image.info.guest_size) {
            TouchImage(image);
            return;
        }
        std::scoped_lock lk{mutex};
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /**
     * Render targets of games that do not give the size hint of the command stream are created
     * with the pitch and aligned height of their buffer. A texture descriptor of the same buffer
     * tells the real size (Tuning::rt_size_learn): the target is then shrunk to it (its content
     * kept), and targets bound later at this address take this size, so that sampling the
     * target with normalized coordinates covers exactly the rendered area, not the padding.
     */
    [[nodiscard]] AmdGpu::CbDbExtent LearnedTargetExtent(VAddr address, u32 pitch) const;
    [[nodiscard]] ImageId LearnTargetSize(ImageId image_id, const ImageInfo& tsharp_info);
    [[nodiscard]] ImageId ShrinkImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth);

    /// Grows whenever a sampler is deleted: a handle remembered outside (Rasterizer sampler
    /// memo) is valid while it does not change. Lock free.
    [[nodiscard]] u64 SamplersGeneration() const noexcept {
        return samplers_generation.load(std::memory_order_acquire);
    }

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Whether the slice of the metadata surface is cleared, and marks it used (one lookup
    /// for IsMetaCleared then TouchMeta(false), per target and draw).
    bool ConsumeMetaClear(VAddr address, u32 slice) {
        if (address == 0) {
            return false;
        }
        auto it = surface_metas.find(address);
        if (it == surface_metas.end()) {
            return false;
        }
        const bool cleared = it.value().clear_mask & (1u << slice);
        it.value().clear_mask &= ~(1u << slice);
        return cleared;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PAGE_BITS;
        for (u64 page = addr >> Traits::PAGE_BITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Whether an image is registered in the page_table pages of the range. Lock free.
    bool HasImagesInRange(VAddr addr, size_t size) const;

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    /// Images registered in each page_table page, changed under the lock and read without it by
    /// invalidations, which mostly hit pages without images (guest write faults).
    std::atomic<u64> generation{1};
    std::atomic<u64> samplers_generation{1};
    void BumpGeneration() noexcept {
        generation.fetch_add(1, std::memory_order_release);
    }
    static constexpr u32 NumImageGens = 1 << 16;
    std::unique_ptr<std::atomic<u32>[]> image_gens{std::make_unique<std::atomic<u32>[]>(NumImageGens)};
    void BumpImageGeneration(ImageId image_id) noexcept {
        if (image_id.index < NumImageGens) {
            image_gens[image_id.index].fetch_add(1, std::memory_order_release);
        } else {
            BumpGeneration();
        }
    }
    std::unique_ptr<std::atomic<u32>[]> page_image_counts{
        std::make_unique<std::atomic<u32>[]>(NumImagePages)};
    /// Registered images by guest start address, for exact address lookups.
    tsl::robin_map<VAddr, boost::container::small_vector<ImageId, 2>> image_address_index;
    struct LearnedExtent {
        u32 pitch;
        AmdGpu::CbDbExtent extent;
    };
    tsl::robin_map<VAddr, LearnedExtent> learned_extents; ///< See LearnTargetSize.
    std::mutex mutex;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
