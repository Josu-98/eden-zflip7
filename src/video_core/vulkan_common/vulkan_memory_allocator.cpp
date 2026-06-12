// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include <ranges>
#include "video_core/vulkan_common/vma.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/settings.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#endif

namespace Vulkan {
    namespace {

#ifdef __ANDROID__
constexpr VkDeviceSize kDirectProbeBufferSize = 8ull * 1024ull * 1024ull;
constexpr VkBufferUsageFlags kDirectProbeRequiredUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

void LogDirectProbeStage(const char* stage) {
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe", "stage=%s", stage);
}

void LogDirectProbeResult(const char* stage, VkResult result) {
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe", "stage=%s result=%d", stage, result);
}

bool ShouldRunDirectBufferProbe(const VkBufferCreateInfo& ci, MemoryUsage usage) {
    return ci.size == kDirectProbeBufferSize &&
           (ci.usage & kDirectProbeRequiredUsage) == kDirectProbeRequiredUsage &&
           usage == MemoryUsage::Stream;
}

void RunDirectBufferAllocationProbe(const Device& device,
                                    const VkBufferCreateInfo& ci,
                                    const VkPhysicalDeviceMemoryProperties& properties) {
    const VkDevice vk_device = *device.GetLogical();
    const auto& dld = device.GetDispatchLoader();

    if (!dld.vkCreateBuffer || !dld.vkGetBufferMemoryRequirements2 || !dld.vkAllocateMemory ||
        !dld.vkBindBufferMemory || !dld.vkMapMemory || !dld.vkUnmapMemory || !dld.vkFreeMemory ||
        !dld.vkDestroyBuffer) {
        LogDirectProbeStage("direct_probe_missing_proc");
        return;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool mapped_ok = false;

    const auto cleanup = [&]() {
        if (mapped_ok && memory != VK_NULL_HANDLE) {
            dld.vkUnmapMemory(vk_device, memory);
            mapped_ok = false;
            mapped = nullptr;
        }
        if (buffer != VK_NULL_HANDLE) {
            dld.vkDestroyBuffer(vk_device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            dld.vkFreeMemory(vk_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    };

    const VkResult create_result = dld.vkCreateBuffer(vk_device, &ci, nullptr, &buffer);
    LogDirectProbeResult("direct_probe_create_buffer_result", create_result);
    if (create_result != VK_SUCCESS) {
        cleanup();
        return;
    }

    const VkBufferMemoryRequirementsInfo2 req_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .buffer = buffer,
    };
    VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = nullptr,
        .memoryRequirements{},
    };
    dld.vkGetBufferMemoryRequirements2(vk_device, &req_info, &requirements);
    const VkMemoryRequirements& reqs = requirements.memoryRequirements;
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe",
                      "stage=direct_probe_requirements size=%llu alignment=%llu type_bits=0x%x",
                      static_cast<unsigned long long>(reqs.size),
                      static_cast<unsigned long long>(reqs.alignment), reqs.memoryTypeBits);

    bool any_bind_success = false;
    for (u32 type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
        if (((reqs.memoryTypeBits >> type_index) & 1u) == 0u) {
            continue;
        }
        const VkMemoryPropertyFlags flags = properties.memoryTypes[type_index].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            continue;
        }
        const u32 heap_index = properties.memoryTypes[type_index].heapIndex;
        __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe",
                            "stage=direct_probe_candidate type=%u flags=0x%x heap=%u",
                            type_index, flags, heap_index);

        VkMemoryAllocateInfo alloc_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = reqs.size,
            .memoryTypeIndex = type_index,
        };
        const VkResult allocate_result = dld.vkAllocateMemory(vk_device, &alloc_info, nullptr, &memory);
        LogDirectProbeResult("direct_probe_allocate_result", allocate_result);
        if (allocate_result != VK_SUCCESS) {
            continue;
        }

        __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe",
                            "stage=direct_probe_bind_details memory_type=%u heap=%u flags=0x%x allocation_size=%llu bind_proc=%p",
                            type_index, heap_index, flags, static_cast<unsigned long long>(reqs.size),
                            (void*)dld.vkBindBufferMemory);
        Dl_info bind_info{};
        const void* bind_proc = reinterpret_cast<const void*>(dld.vkBindBufferMemory);
        if (dladdr(bind_proc, &bind_info) != 0 && bind_info.dli_fbase != nullptr) {
            const auto bind_address = reinterpret_cast<uintptr_t>(bind_proc);
            const auto base_address = reinterpret_cast<uintptr_t>(bind_info.dli_fbase);
            __android_log_print(
                ANDROID_LOG_INFO, "EdenVulkanProbe",
                "stage=direct_probe_bind_owner object=%s base=%p symbol=%s symbol_address=%p relative_offset=0x%llx",
                bind_info.dli_fname != nullptr ? bind_info.dli_fname : "<unknown>",
                bind_info.dli_fbase, bind_info.dli_sname != nullptr ? bind_info.dli_sname : "<none>",
                bind_info.dli_saddr,
                static_cast<unsigned long long>(bind_address - base_address));
        } else {
            __android_log_print(ANDROID_LOG_INFO, "EdenVulkanProbe",
                                "stage=direct_probe_bind_owner unresolved bind_proc=%p", bind_proc);
        }
        const VkResult bind_result =
            dld.vkBindBufferMemory(vk_device, buffer, memory, 0);
        LogDirectProbeResult("direct_probe_bind_result", bind_result);
        if (bind_result != VK_SUCCESS) {
            cleanup();
            continue;
        }

        any_bind_success = true;
        const VkResult map_result = dld.vkMapMemory(vk_device, memory, 0, reqs.size, 0, &mapped);
        LogDirectProbeResult("direct_probe_map_result", map_result);
        if (map_result == VK_SUCCESS) {
            mapped_ok = true;
        }

        cleanup();
        break;
    }
    if (!any_bind_success) {
        LogDirectProbeStage("direct_probe_all_bind_failed");
    }

    cleanup();
}
#endif

// Helpers translating MemoryUsage to flags/usage

        [[maybe_unused]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
            switch (usage) {
                case MemoryUsage::DeviceLocal:
                    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                case MemoryUsage::Upload:
                    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                case MemoryUsage::Download:
                    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                case MemoryUsage::Stream:
                    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            }
            ASSERT_MSG(false, "Invalid memory usage={}", usage);
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }

        [[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
            if (usage == MemoryUsage::Download) {
                return VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            }
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
                    return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
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


// This avoids calling vkGetBufferMemoryRequirements* directly.
        template<typename T>
        static VkBuffer GetVkHandleFromBuffer(const T &buf) {
            if constexpr (requires { static_cast<VkBuffer>(buf); }) {
                return static_cast<VkBuffer>(buf);
            } else if constexpr (requires {{ buf.GetHandle() } -> std::convertible_to<VkBuffer>; }) {
                return buf.GetHandle();
            } else if constexpr (requires {{ buf.Handle() } -> std::convertible_to<VkBuffer>; }) {
                return buf.Handle();
            } else if constexpr (requires {{ buf.vk_handle() } -> std::convertible_to<VkBuffer>; }) {
                return buf.vk_handle();
            } else {
                static_assert(sizeof(T) == 0, "Cannot extract VkBuffer handle from vk::Buffer");
                return VK_NULL_HANDLE;
            }
        }

    } // namespace

//MemoryCommit is now VMA-backed
    MemoryCommit::MemoryCommit(VmaAllocator alloc, VmaAllocation a,
                               const VmaAllocationInfo &info) noexcept
            : allocator{alloc}, allocation{a}, memory{info.deviceMemory},
              offset{info.offset}, size{info.size}, mapped_ptr{info.pMappedData} {
        // Log GPU memory allocation
        if (Settings::values.gpu_logging_enabled.GetValue() &&
            Settings::values.gpu_log_memory_tracking.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
                reinterpret_cast<uintptr_t>(memory),
                static_cast<u64>(size),
                0  // Memory property flags (not easily available from VMA)
            );
        }
    }

    MemoryCommit::~MemoryCommit() { Release(); }

    MemoryCommit::MemoryCommit(MemoryCommit &&rhs) noexcept
            : allocator{std::exchange(rhs.allocator, nullptr)},
              allocation{std::exchange(rhs.allocation, nullptr)},
              memory{std::exchange(rhs.memory, VK_NULL_HANDLE)},
              offset{std::exchange(rhs.offset, 0)},
              size{std::exchange(rhs.size, 0)},
              mapped_ptr{std::exchange(rhs.mapped_ptr, nullptr)} {}

    MemoryCommit &MemoryCommit::operator=(MemoryCommit &&rhs) noexcept {
        if (this != &rhs) {
            Release();
            allocator = std::exchange(rhs.allocator, nullptr);
            allocation = std::exchange(rhs.allocation, nullptr);
            memory = std::exchange(rhs.memory, VK_NULL_HANDLE);
            offset = std::exchange(rhs.offset, 0);
            size = std::exchange(rhs.size, 0);
            mapped_ptr = std::exchange(rhs.mapped_ptr, nullptr);
        }
        return *this;
    }

    std::span<u8> MemoryCommit::Map()
    {
        if (!allocation) return {};
        if (!mapped_ptr) {
            if (vmaMapMemory(allocator, allocation, &mapped_ptr) != VK_SUCCESS) return {};
        }
        const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                    (std::numeric_limits<size_t>::max)()));
        return std::span<u8>{static_cast<u8 *>(mapped_ptr), n};
    }

    std::span<const u8> MemoryCommit::Map() const
    {
        if (!allocation) return {};
        if (!mapped_ptr) {
            void *p = nullptr;
            if (vmaMapMemory(allocator, allocation, &p) != VK_SUCCESS) return {};
            const_cast<MemoryCommit *>(this)->mapped_ptr = p;
        }
        const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                    (std::numeric_limits<size_t>::max)()));
        return std::span<const u8>{static_cast<const u8 *>(mapped_ptr), n};
    }

    void MemoryCommit::Unmap()
    {
        if (allocation && mapped_ptr) {
            vmaUnmapMemory(allocator, allocation);
            mapped_ptr = nullptr;
        }
    }

    void MemoryCommit::Release() {
        if (allocation && allocator) {
            // Log GPU memory deallocation
            if (Settings::values.gpu_logging_enabled.GetValue() &&
                Settings::values.gpu_log_memory_tracking.GetValue() &&
                memory != VK_NULL_HANDLE) {
                GPU::Logging::GPULogger::GetInstance().LogMemoryDeallocation(
                    reinterpret_cast<uintptr_t>(memory)
                );
            }

            if (mapped_ptr) {
                vmaUnmapMemory(allocator, allocation);
                mapped_ptr = nullptr;
            }
            vmaFreeMemory(allocator, allocation);
        }
        allocation = nullptr;
        allocator = nullptr;
        memory = VK_NULL_HANDLE;
        offset = 0;
        size = 0;
    }

    MemoryAllocator::MemoryAllocator(const Device &device_)
            : device{device_}, allocator{device.GetAllocator()},
              properties{device_.GetPhysical().GetMemoryProperties().memoryProperties},
              buffer_image_granularity{
                      device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {

        // Preserve the previous "RenderDoc small heap" trimming behavior that we had in original vma minus the heap bug
        if (device.HasDebuggingToolAttached())
        {
            using namespace Common::Literals;
            ForEachDeviceLocalHostVisibleHeap(device, [this](size_t heap_idx, VkMemoryHeap &heap) {
                if (heap.size <= 256_MiB) {
                    for (u32 t = 0; t < properties.memoryTypeCount; ++t) {
                        if (properties.memoryTypes[t].heapIndex == heap_idx) {
                            valid_memory_types &= ~(1u << t);
                        }
                    }
                }
            });
        }
    }

    MemoryAllocator::~MemoryAllocator() = default;

    vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo &ci) const
    {
        const VmaAllocationCreateInfo alloc_ci = {
                .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .requiredFlags = 0,
                .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                .memoryTypeBits = 0,
                .pool = VK_NULL_HANDLE,
                .pUserData = nullptr,
                .priority = 0.f,
        };

        VkImage handle{};
        VmaAllocation allocation{};
        VmaAllocationInfo alloc_info{};
        vk::Check(vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info));

        // Log GPU memory allocation for images
        if (Settings::values.gpu_logging_enabled.GetValue() &&
            Settings::values.gpu_log_memory_tracking.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
                reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
                static_cast<u64>(alloc_info.size),
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            );
        }

        return vk::Image(handle, ci.usage, *device.GetLogical(), allocator, allocation,
                         device.GetDispatchLoader());
    }

    vk::Buffer
    MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const
    {
#ifdef __ANDROID__
        if (ShouldRunDirectBufferProbe(ci, usage)) {
            RunDirectBufferAllocationProbe(device, ci, properties);
        }
#endif

        const VmaAllocationCreateInfo alloc_ci = {
                .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
                .usage = MemoryUsageVma(usage),
                .requiredFlags = 0,
                .preferredFlags = MemoryUsagePreferredVmaFlags(usage),
                .memoryTypeBits = valid_memory_types,
                .pool = VK_NULL_HANDLE,
                .pUserData = nullptr,
                .priority = 0.f,
        };

        VkBuffer handle{};
        VmaAllocationInfo alloc_info{};
        VmaAllocation allocation{};
        VkMemoryPropertyFlags property_flags{};

        const VkResult res =
            vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info);
#ifdef __ANDROID__
        if (ShouldRunDirectBufferProbe(ci, usage)) {
            LogDirectProbeResult("vma_create_buffer_result", res);
        }
#endif

        vk::Check(res);
        vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);

        // Log GPU memory allocation for buffers
        if (Settings::values.gpu_logging_enabled.GetValue() &&
            Settings::values.gpu_log_memory_tracking.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
                reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
                static_cast<u64>(alloc_info.size),
                property_flags
            );
        }

        u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
        const std::span<u8> mapped_data = data ? std::span<u8>{data, ci.size} : std::span<u8>{};
        const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data,
                          is_coherent,
                          device.GetDispatchLoader());
    }

    MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements &reqs, MemoryUsage usage)
    {
        const auto vma_usage = MemoryUsageVma(usage);
        VmaAllocationCreateInfo ci{};
        ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
        ci.usage = vma_usage;
        ci.memoryTypeBits = reqs.memoryTypeBits & valid_memory_types;
        ci.requiredFlags = 0;
        ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);

        VmaAllocation a{};
        VmaAllocationInfo info{};

        VkResult res = vmaAllocateMemory(allocator, &reqs, &ci, &a, &info);

        if (res != VK_SUCCESS) {
            // Relax 1: drop budget constraint
            auto ci2 = ci;
            ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            res = vmaAllocateMemory(allocator, &reqs, &ci2, &a, &info);

            // Relax 2: if we preferred DEVICE_LOCAL, drop that preference
            if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                auto ci3 = ci2;
                ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                res = vmaAllocateMemory(allocator, &reqs, &ci3, &a, &info);
            }
        }

        vk::Check(res);
        return MemoryCommit(allocator, a, info);
    }

    MemoryCommit MemoryAllocator::Commit(const vk::Buffer &buffer, MemoryUsage usage) {
        // Allocate memory appropriate for this buffer automatically
        const auto vma_usage = MemoryUsageVma(usage);

        VmaAllocationCreateInfo ci{};
        ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
        ci.usage = vma_usage;
        ci.requiredFlags = 0;
        ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);
        ci.pool = VK_NULL_HANDLE;
        ci.pUserData = nullptr;
        ci.priority = 0.0f;

        const VkBuffer raw = *buffer;

        VmaAllocation a{};
        VmaAllocationInfo info{};

        // Let VMA infer memory requirements from the buffer
        VkResult res = vmaAllocateMemoryForBuffer(allocator, raw, &ci, &a, &info);

        if (res != VK_SUCCESS) {
            auto ci2 = ci;
            ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            res = vmaAllocateMemoryForBuffer(allocator, raw, &ci2, &a, &info);

            if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                auto ci3 = ci2;
                ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                res = vmaAllocateMemoryForBuffer(allocator, raw, &ci3, &a, &info);
            }
        }

        vk::Check(res);
        vk::Check(vmaBindBufferMemory2(allocator, a, 0, raw, nullptr));
        return MemoryCommit(allocator, a, info);
    }



} // namespace Vulkan
