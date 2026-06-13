// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <variant>
#include <boost/container/static_vector.hpp>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

static void MarkRasterizerInitStage(const char* stage) {
    LOG_INFO(Render_Vulkan, "Rasterizer init stage: {}", stage);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanRasterizer", "stage=%s", stage);
#endif
}

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {
    MarkRasterizerInitStage("guest_descriptor_queue");
    payload_start = payload.data();
    payload_cursor = payload.data();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload.data() + frame_index * FRAME_PAYLOAD_SIZE;
    payload_cursor = payload_start;
}

void UpdateDescriptorQueue::Acquire() {
    // Minimum number of entries required.
    // This is the maximum number of entries a single draw call might use.
    static constexpr size_t MIN_ENTRIES = 0x400;

    if (std::distance(payload_start, payload_cursor) + MIN_ENTRIES >= FRAME_PAYLOAD_SIZE) {
        LOG_WARNING(Render_Vulkan, "Payload overflow, waiting for worker thread");
        scheduler.WaitWorker();
        payload_cursor = payload_start;
    }
    upload_start = payload_cursor;
}

} // namespace Vulkan
