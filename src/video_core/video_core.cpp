// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_null/renderer_null.h"
#ifdef HAS_OPENGL
#include "video_core/renderer_opengl/renderer_opengl.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/video_core.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

std::unique_ptr<VideoCore::RendererBase> CreateRenderer(Core::System& system, Core::Frontend::EmuWindow& emu_window, Tegra::GPU& gpu, std::unique_ptr<Core::Frontend::GraphicsContext> context) {
    [[maybe_unused]] auto& device_memory = system.Host1x().MemoryManager();
    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        return std::make_unique<OpenGL::RendererOpenGL>(emu_window, device_memory, gpu, std::move(context));
#endif
    case Settings::RendererBackend::Vulkan:
        return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context));
    case Settings::RendererBackend::Null:
        return std::make_unique<Null::RendererNull>(emu_window, gpu, std::move(context));
    default:
        return nullptr;
    }
}

} // Anonymous namespace

namespace VideoCore {

std::unique_ptr<Tegra::GPU> CreateGPU(Core::Frontend::EmuWindow& emu_window, Core::System& system) {
    Settings::UpdateRescalingInfo();

    const auto nvdec_value = Settings::values.nvdec_emulation.GetValue();
    const bool use_nvdec = nvdec_value != Settings::NvdecEmulation::Off;
    const bool use_async = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    auto gpu = std::make_unique<Tegra::GPU>(system, use_async, use_nvdec);
    auto context = emu_window.CreateSharedContext();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanInit", "stage=gpu_context_create_done");
#endif
    auto scope = context->Acquire();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "EdenVulkanInit", "stage=gpu_context_acquire_done");
#endif
    try {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "EdenVulkanInit", "stage=gpu_renderer_create_start");
#endif
        auto renderer = CreateRenderer(system, emu_window, *gpu, std::move(context));
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "EdenVulkanInit", "stage=gpu_renderer_create_done");
#endif
        gpu->BindRenderer(std::move(renderer));
        return gpu;
    } catch (const std::runtime_error& exception) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "EdenVulkanInit",
                            "stage=gpu_renderer_create_failed error=%s", exception.what());
#endif
        scope.Cancel();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: {}", exception.what());
        return nullptr;
    }
}

} // namespace VideoCore
