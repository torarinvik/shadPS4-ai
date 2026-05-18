// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {

struct ImageInfo;
struct Image;
class StreamBuffer;

class TileManager {
    struct TilingPipelineKey {
        AmdGpu::TileMode tile_mode{};
        AmdGpu::ArrayMode array_mode{};
        u32 num_bits{};
        u32 num_samples{};
        bool is_tiler{};

        bool operator==(const TilingPipelineKey&) const = default;
    };

    struct TilingPipelineKeyHash {
        size_t operator()(const TilingPipelineKey& key) const noexcept {
            size_t seed = std::hash<u32>{}(static_cast<u32>(key.tile_mode));
            seed ^= std::hash<u32>{}(static_cast<u32>(key.array_mode)) + 0x9e3779b9 + (seed << 6) +
                    (seed >> 2);
            seed ^= std::hash<u32>{}(key.num_bits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<u32>{}(key.num_samples) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<bool>{}(key.is_tiler) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

public:
    using ScratchBuffer = std::pair<vk::Buffer, VmaAllocation>;
    using Result = std::pair<vk::Buffer, u32>;

    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   Buffer& out_buffer, u32 out_offset, u32 copy_size);

    Result DetileImage(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info);

private:
    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler);
    ScratchBuffer GetScratchBuffer(u32 size);

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    StreamBuffer& stream_buffer;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::unordered_map<TilingPipelineKey, vk::UniquePipeline, TilingPipelineKeyHash>
        tiling_pipelines{};
};

} // namespace VideoCore
