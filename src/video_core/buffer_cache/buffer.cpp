// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_diagnostics.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryUsage type) {
    switch (type) {
    case MemoryUsage::Upload:
        return "Upload";
    case MemoryUsage::Download:
        return "Download";
    case MemoryUsage::Stream:
        return "Stream";
    case MemoryUsage::DeviceLocal:
        return "DeviceLocal";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
    return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryUsage::Download:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryUsage::DeviceLocal:
        return {};
    }
    return {};
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
    case MemoryUsage::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::Upload:
    case MemoryUsage::Download:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

namespace {

struct BufferPoolKey {
    u64 size;
    u32 usage_flags;
    u32 sharing_mode;
    u32 memory_usage;
    u32 allocation_flags;
    u32 preferred_flags;

    bool operator==(const BufferPoolKey& other) const noexcept {
        return std::memcmp(this, &other, sizeof(BufferPoolKey)) == 0;
    }
};

struct BufferPoolKeyHash {
    std::size_t operator()(const BufferPoolKey& key) const noexcept {
        const auto* data = reinterpret_cast<const u8*>(&key);
        std::size_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < sizeof(key); ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }
};

VmaAllocationCreateInfo MakeBufferAllocationInfo(const vk::BufferCreateInfo& buffer_ci,
                                                 MemoryUsage usage) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    const VmaAllocationCreateFlags bda_flag =
        with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    return VmaAllocationCreateInfo{
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage),
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };
}

BufferPoolKey MakeBufferPoolKey(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage) {
    const auto alloc_ci = MakeBufferAllocationInfo(buffer_ci, usage);
    return BufferPoolKey{
        .size = buffer_ci.size,
        .usage_flags = static_cast<u32>(static_cast<VkBufferUsageFlags>(buffer_ci.usage)),
        .sharing_mode = static_cast<u32>(buffer_ci.sharingMode),
        .memory_usage = static_cast<u32>(alloc_ci.usage),
        .allocation_flags = static_cast<u32>(alloc_ci.flags),
        .preferred_flags = static_cast<u32>(alloc_ci.preferredFlags),
    };
}

class BufferReusePool {
public:
    static BufferReusePool& Get() {
        static BufferReusePool pool;
        return pool;
    }

    bool Acquire(const BufferPoolKey& key, VkBuffer& out_buffer, VmaAllocation& out_alloc) {
        if (!enabled) {
            return false;
        }
        std::scoped_lock lock{mutex};
        auto it = entries.find(key);
        if (it == entries.end() || it->second.empty()) {
            ++stat_misses;
            return false;
        }
        const Entry entry = it->second.back();
        it->second.pop_back();
        total_bytes -= key.size;
        --total_count;
        ++stat_hits;
        out_buffer = entry.buffer;
        out_alloc = entry.allocation;
        return true;
    }

    void Release(VmaAllocator allocator, const BufferPoolKey& key, VkBuffer buffer,
                 VmaAllocation allocation) {
        if (buffer == VK_NULL_HANDLE) {
            return;
        }
        if (!enabled) {
            vmaDestroyBuffer(allocator, buffer, allocation);
            return;
        }
        std::scoped_lock lock{mutex};
        entries[key].push_back(Entry{buffer, allocation});
        total_bytes += key.size;
        ++total_count;
        while (total_bytes > max_bytes || total_count > max_count) {
            if (!EvictOne(allocator)) {
                break;
            }
        }
    }

    void Drain(VmaAllocator allocator) {
        std::scoped_lock lock{mutex};
        const u64 total_acquire = stat_hits + stat_misses;
        const double hit_rate = total_acquire ? (100.0 * stat_hits / total_acquire) : 0.0;
        LOG_INFO(Render_Vulkan,
                 "BufferReusePool stats: hits={} misses={} hit_rate={:.1f}% evictions={} "
                 "residual_entries={} residual_bytes={}",
                 stat_hits, stat_misses, hit_rate, stat_evictions, total_count, total_bytes);
        enabled = false;
        for (auto& [key, bucket] : entries) {
            for (const Entry& entry : bucket) {
                vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
            }
        }
        entries.clear();
        total_bytes = 0;
        total_count = 0;
    }

private:
    struct Entry {
        VkBuffer buffer;
        VmaAllocation allocation;
    };

    BufferReusePool() {
        if (const char* value = std::getenv("SHADPS4_BUFFER_REUSE_POOL")) {
            enabled = std::strcmp(value, "0") != 0;
        }
        if (const char* value = std::getenv("SHADPS4_BUFFER_REUSE_POOL_MAX_MB")) {
            const unsigned long mb = std::strtoul(value, nullptr, 10);
            if (mb > 0) {
                max_bytes = static_cast<u64>(mb) * 1024 * 1024;
            }
        }
    }

    bool EvictOne(VmaAllocator allocator) {
        for (auto& [key, bucket] : entries) {
            if (!bucket.empty()) {
                const Entry entry = bucket.back();
                bucket.pop_back();
                vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
                total_bytes -= key.size;
                --total_count;
                ++stat_evictions;
                return true;
            }
        }
        return false;
    }

    bool enabled = true;
    u64 max_bytes = 128ull * 1024 * 1024;
    u32 max_count = 2048;
    u64 total_bytes = 0;
    u32 total_count = 0;
    u64 stat_hits = 0;
    u64 stat_misses = 0;
    u64 stat_evictions = 0;
    std::unordered_map<BufferPoolKey, std::vector<Entry>, BufferPoolKeyHash> entries;
    std::mutex mutex;
};

} // namespace

void DrainBufferReusePool(VmaAllocator allocator) {
    BufferReusePool::Get().Drain(allocator);
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (buffer) {
        BufferReusePool::Get().Release(allocator, MakeBufferPoolKey(buffer_ci, usage), buffer,
                                       allocation);
        buffer = vk::Buffer{};
        allocation = {};
        bda_addr = 0;
    }
}

void UniqueBuffer::Create(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage,
                          VmaAllocationInfo* out_alloc_info) {
    this->buffer_ci = buffer_ci;
    this->usage = usage;
    ASSERT(!buffer);
    {
        VkBuffer reused{};
        VmaAllocation reused_alloc{};
        if (BufferReusePool::Get().Acquire(MakeBufferPoolKey(buffer_ci, usage), reused,
                                           reused_alloc)) {
            buffer = vk::Buffer{reused};
            allocation = reused_alloc;
            if (out_alloc_info) {
                vmaGetAllocationInfo(allocator, allocation, out_alloc_info);
            }
            if (buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress) {
                vk::BufferDeviceAddressInfo bda_info{.buffer = buffer};
                bda_addr = device.getBufferAddress(bda_info);
                if (bda_addr == 0) {
                    Vulkan::RecordGpuCommandDiagnostic(
                        "buffer_reuse_bda_failed size=%llu usage=%s memory_usage=%s handle=0x%llx",
                        static_cast<unsigned long long>(buffer_ci.size),
                        vk::to_string(buffer_ci.usage).c_str(), BufferTypeName(usage).data(),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                            static_cast<VkBuffer>(buffer))));
                    Vulkan::DumpGpuCommandDiagnostics("buffer_reuse_bda_failed");
                }
                ASSERT_MSG(bda_addr != 0, "Failed to get buffer device address");
            }
            return;
        }
    }
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    const VmaAllocationCreateInfo alloc_ci = MakeBufferAllocationInfo(buffer_ci, usage);

    const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    VkBuffer unsafe_buffer{};
    VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                      &allocation, out_alloc_info);
    if (result != VK_SUCCESS) {
        Vulkan::RecordGpuCommandDiagnostic(
            "buffer_alloc_failed result=%s size=%llu usage=%s sharing=%s memory_usage=%s "
            "alloc_flags=0x%x preferred_flags=0x%x required_flags=0x%x",
            vk::to_string(vk::Result{result}).c_str(),
            static_cast<unsigned long long>(buffer_ci.size), vk::to_string(buffer_ci.usage).c_str(),
            vk::to_string(buffer_ci.sharingMode).c_str(), BufferTypeName(usage).data(),
            static_cast<u32>(alloc_ci.flags), static_cast<u32>(alloc_ci.preferredFlags),
            static_cast<u32>(alloc_ci.requiredFlags));
        Vulkan::DumpGpuCommandDiagnostics("buffer_alloc_failed");
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
               vk::to_string(vk::Result{result}));
    buffer = vk::Buffer{unsafe_buffer};

    if (with_bda) {
        vk::BufferDeviceAddressInfo bda_info{
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        if (bda_result == 0) {
            Vulkan::RecordGpuCommandDiagnostic(
                "buffer_bda_failed size=%llu usage=%s memory_usage=%s handle=0x%llx",
                static_cast<unsigned long long>(buffer_ci.size), vk::to_string(buffer_ci.usage).c_str(),
                BufferTypeName(usage).data(),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                    static_cast<VkBuffer>(buffer))));
            Vulkan::DumpGpuCommandDiagnostics("buffer_bda_failed");
        }
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_, MemoryUsage usage_,
               VAddr cpu_addr_, vk::BufferUsageFlags flags, u64 size_bytes_)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, instance{&instance_}, scheduler{&scheduler_},
      usage{usage_}, buffer{instance->GetDevice(), instance->GetAllocator()} {
    // Create buffer object.
    const vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = flags,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, usage, &alloc_info);

    const auto device = instance->GetDevice();
    Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);

    // Record buffer creation in the GPU-op ring (mirrors the FREE buffer entry) so a device-loss
    // dump reveals create/free churn pairs at the same address - the residual UFC 3 loss shows heavy
    // small-buffer traffic, and we otherwise only recorded frees.
    Vulkan::RecordGpuCommandDiagnostic("CREATE buffer addr=0x%llx size=%llu",
                                       static_cast<unsigned long long>(cpu_addr),
                                       static_cast<unsigned long long>(size_bytes));
    VideoCore::Diag::NoteBufferChurn(cpu_addr, size_bytes);

    // Map it if it is host visible.
    VkMemoryPropertyFlags property_flags{};
    vmaGetAllocationMemoryProperties(instance->GetAllocator(), buffer.allocation, &property_flags);
    if (alloc_info.pMappedData) {
        mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
    }
    is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

void Buffer::Invalidate(u64 offset, u64 size) const {
    if (!is_coherent && usage == MemoryUsage::Download) {
        vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset, size);
    }
}

void Buffer::Fill(u64 offset, u32 num_bytes, u32 value) {
    scheduler->EndRendering();
    ASSERT_MSG(offset % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer size must be a multiple of 4 bytes");
    VideoCore::Diag::CheckBufferRange("Buffer::Fill", SizeBytes(), offset, num_bytes);
    const auto cmdbuf = scheduler->CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x4000;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x1000;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           MemoryUsage usage, u64 size_bytes)
    : Buffer{instance, scheduler, usage, 0, AllFlags, size_bytes} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    const auto device = instance.GetDevice();
    Vulkan::SetObjectName(device, Handle(), "StreamBuffer({}):{:#x}", BufferTypeName(usage),
                          size_bytes);
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!is_coherent && usage == MemoryUsage::Stream) {
        size = Common::AlignUp(size, instance->NonCoherentAtomSize());
    }

    if (size > this->size_bytes) {
        return {nullptr, 0};
    }

    mapped_size = size;

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    const u64 mapped_upper_bound = offset + size;
    if (!WaitPendingOperations(mapped_upper_bound, allow_wait)) {
        return {nullptr, 0};
    }

    return {mapped_data.data() + offset, offset};
}

void StreamBuffer::Commit() {
    if (!is_coherent) {
        if (usage == MemoryUsage::Download) {
            vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset,
                                    mapped_size);
        } else {
            vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, mapped_size);
        }
    }

    offset += mapped_size;
    if (current_watch_cursor != 0 &&
        current_watches[current_watch_cursor].tick == scheduler->CurrentTick()) {
        current_watches[current_watch_cursor].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = scheduler->CurrentTick();
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler->IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler->Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
