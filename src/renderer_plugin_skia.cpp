#include "gpu2d/d3d12_renderer.hpp"
#include "gpu2d/frame_planner.hpp"
#include "gpu2d/renderer_plugin_api.h"
#include "gpu2d/skia_rasterizer.hpp"

#include "renderer_plugin_common.hpp"

#include <d3d12.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace {

	struct SkiaRendererPlugin {
		explicit SkiaRendererPlugin(const Gpu2dRendererCreateInfo& info)
			: renderer(static_cast<HWND>(info.native_window), info.width, info.height,
				gpu2d::RendererOptions{ info.vsync != 0U, info.enable_debug_layer != 0U }),
			gpu_surface(renderer.nativeAdapter(), renderer.nativeDevice(), renderer.nativeQueue()),
			adapter_name(renderer.adapterName()) {
			std::cout << "Skia backend: Ganesh Direct3D 12 GPU\n";
		}

		~SkiaRendererPlugin() {
			gpu_surface.waitIdle();
			if (surface.valid()) renderer.releaseTexture(surface);
			renderer.waitIdle();
		}

		void resize(const std::uint32_t new_width, const std::uint32_t new_height) {
			renderer.resize(new_width, new_height);
		}

		gpu2d::D3D12Renderer renderer;
		gpu2d::FramePlanner planner;
		gpu2d::SkiaGpuCanvasSurface gpu_surface;
		gpu2d::TextureHandle surface;
		std::string adapter_name;
	};

	void* create(const Gpu2dRendererCreateInfo* info) {
		if (info == nullptr || info->struct_size < sizeof(Gpu2dRendererCreateInfo)) {
			gpu2d::plugin::last_error = "invalid create info";
			return nullptr;
		}
		try {
			return std::make_unique<SkiaRendererPlugin>(*info).release();
		}
		catch (const std::exception& error) {
			gpu2d::plugin::last_error = error.what();
			return nullptr;
		}
	}

	void destroy(void* instance) {
		try {
			delete static_cast<SkiaRendererPlugin*>(instance);
		}
		catch (...) {
			gpu2d::plugin::last_error = "renderer destroy failed";
		}
	}

	int32_t resize(void* instance, const uint32_t width, const uint32_t height) {
		return gpu2d::plugin::guarded(
			[&] { static_cast<SkiaRendererPlugin*>(instance)->resize(width, height); });
	}

	int32_t render(void* instance, const Gpu2dRendererFrame* frame,
		Gpu2dRendererTelemetry* telemetry) {
		if (instance == nullptr || frame == nullptr || telemetry == nullptr ||
			frame->struct_size < sizeof(Gpu2dRendererFrame) ||
			(frame->operation_count != 0U && frame->operations == nullptr)) {
			gpu2d::plugin::last_error = "invalid render arguments";
			return 0;
		}
		return gpu2d::plugin::guarded([&] {
			const auto plugin_start = std::chrono::steady_clock::now();
			auto& plugin = *static_cast<SkiaRendererPlugin*>(instance);
			const gpu2d::CanvasFrame canvas_frame = gpu2d::plugin::rebuildCanvasFrame(*frame);
			const auto paint_start = std::chrono::steady_clock::now();
			const gpu2d::SkiaGpuSurfaceInfo gpu_info = plugin.gpu_surface.apply(
				canvas_frame, frame->canvas_width, frame->canvas_height);
			const auto upload_start = std::chrono::steady_clock::now();
			if (gpu_info.recreated || !plugin.surface.valid()) {
				if (plugin.surface.valid()) plugin.renderer.releaseTexture(plugin.surface);
				plugin.surface = plugin.renderer.registerExternalTexture(
					gpu_info.resource, gpu_info.width, gpu_info.height, gpu_info.resource_state);
			} else {
				plugin.renderer.setExternalTextureState(plugin.surface, gpu_info.resource_state);
			}
			const auto compositor_start = std::chrono::steady_clock::now();
			const gpu2d::Rect viewport = gpu2d::Rect::fromXYWH(
				0.0F, 0.0F, static_cast<float>(frame->width), static_cast<float>(frame->height));
			gpu2d::Canvas compositor;
			compositor.drawImage(viewport, plugin.surface,
				gpu2d::Rect::fromXYWH(0.0F, 0.0F, 1.0F, 1.0F));
			const gpu2d::RenderPlan plan = plugin.planner.build(compositor.finish(), viewport);
			gpu2d::plugin::copyTelemetry(plugin.renderer.render(plan), *telemetry);
			plugin.gpu_surface.notifyExternalState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			const auto plugin_end = std::chrono::steady_clock::now();
			telemetry->paint_ms =
				std::chrono::duration<double, std::milli>(upload_start - paint_start).count();
			telemetry->texture_update_ms =
				std::chrono::duration<double, std::milli>(compositor_start - upload_start).count();
			telemetry->plugin_total_ms =
				std::chrono::duration<double, std::milli>(plugin_end - plugin_start).count();
			});
	}

	void waitIdle(void* instance) {
		try {
			if (instance != nullptr) {
				static_cast<SkiaRendererPlugin*>(instance)->renderer.waitIdle();
			}
		}
		catch (const std::exception& error) {
			gpu2d::plugin::last_error = error.what();
		}
		catch (...) {
			gpu2d::plugin::last_error = "renderer wait_idle failed";
		}
	}

	const char* adapterName(void* instance) {
		return instance != nullptr ? static_cast<SkiaRendererPlugin*>(instance)->adapter_name.c_str() : "";
	}

	const char* lastError() { return gpu2d::plugin::last_error.c_str(); }

	const Gpu2dRendererApi api{ sizeof(Gpu2dRendererApi), GPU2D_RENDERER_ABI_VERSION, "skia",
							   "Skia Ganesh D3D12 GPU",
							   gpu2d::CanvasCapabilityRectangles | gpu2d::CanvasCapabilityPaths |
							       gpu2d::CanvasCapabilityArcs | gpu2d::CanvasCapabilityShadows |
							       gpu2d::CanvasCapabilityLighterBlend | gpu2d::CanvasCapabilityPersistent |
							       gpu2d::CanvasCapabilityGradients,
							   create, destroy, resize, render,
							   waitIdle, adapterName, lastError };

}  // namespace

extern "C" __declspec(dllexport) const Gpu2dRendererApi* gpu2dGetRendererApi(
	const uint32_t requested_abi_version) {
	return requested_abi_version == GPU2D_RENDERER_ABI_VERSION ? &api : nullptr;
}