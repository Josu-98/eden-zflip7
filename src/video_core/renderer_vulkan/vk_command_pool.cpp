// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>

#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Vulkan {

constexpr size_t COMMAND_BUFFER_POOL_SIZE = 4;

namespace {

void MarkCommandPoolInitStage(const char* stage) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanScheduler", "command_pool stage=%s", stage);
#else
    (void)stage;
#endif
}

} // Anonymous namespace

struct CommandPool::Pool {
    vk::CommandPool handle;
    vk::CommandBuffers cmdbufs;
};

CommandPool::CommandPool(MasterSemaphore& master_semaphore_, const Device& device_)
    : ResourcePool(master_semaphore_, COMMAND_BUFFER_POOL_SIZE), device{device_} {}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(size_t begin, size_t end) {
    // Command buffers are going to be committed, recorded, executed every single usage cycle.
    // They are also going to be reset when committed.
    Pool& pool = pools.emplace_back();
    MarkCommandPoolInitStage("create_command_pool");
    pool.handle = device.GetLogical().CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    MarkCommandPoolInitStage("create_command_pool_succeeded");
    MarkCommandPoolInitStage("allocate_command_buffers");
    pool.cmdbufs = pool.handle.Allocate(COMMAND_BUFFER_POOL_SIZE);
    MarkCommandPoolInitStage("allocate_command_buffers_succeeded");
}

VkCommandBuffer CommandPool::Commit() {
    const size_t index = CommitResource();
    const auto pool_index = index / COMMAND_BUFFER_POOL_SIZE;
    const auto sub_index = index % COMMAND_BUFFER_POOL_SIZE;
    return pools[pool_index].cmdbufs[sub_index];
}

} // namespace Vulkan
