#include "gpu2d/d3d12_renderer.hpp"
#include "gpu2d/frame_planner.hpp"
#include "gpu2d/renderer_plugin_api.h"

#include "renderer_plugin_common.hpp"

#include <memory>
#include <string>

namespace {

	struct DirectRendererPlugin {
		explicit DirectRendererPlugin(const Gpu2dRendererCreateInfo& info)
			: renderer(static_cast<HWND>(info.native_window), info.width, info.height,
				gpu2d::RendererOptions{ info.vsync != 0U, info.enable_debug_layer != 0U }) {
		}

		~DirectRendererPlugin() { renderer.waitIdle(); }

		gpu2d::D3D12Renderer renderer;
		gpu2d::FramePlanner planner;
		gpu2d::DisplayList last_display_list;
		std::uint64_t page_revision{};
		std::string adapter_name;
	};

	[[nodiscard]] gpu2d::DisplayList rectangleDisplayList(const gpu2d::CanvasFrame& frame,
		const std::uint32_t canvas_width,
		const std::uint32_t canvas_height,
		const std::uint32_t viewport_width,
		const std::uint32_t viewport_height) {
		gpu2d::Canvas canvas;
		canvas.concat(gpu2d::Mat3x2::scale(
			static_cast<float>(viewport_width) / static_cast<float>(canvas_width),
			static_cast<float>(viewport_height) / static_cast<float>(canvas_height)));
		for (const gpu2d::CanvasFrameOperation& operation : frame.operations) {
			if (operation.kind == gpu2d::CanvasOperationKind::ClearRect) {
				continue;
			}
			if (operation.kind != gpu2d::CanvasOperationKind::FillRect ||
				operation.paint.kind != gpu2d::CanvasPaintKind::Solid ||
				operation.paint.shadow_blur != 0.0F || operation.paint.shadow_color.a != 0.0F ||
				operation.paint.composite != gpu2d::CanvasCompositeMode::SourceOver) {
				throw std::runtime_error(
					"Direct3D 12 Canvas adapter supports solid source-over fillRect operations only");
			}
			canvas.save();
			canvas.concat(operation.transform);
			canvas.drawRect(operation.rectangle, operation.paint.color);
			canvas.restore();
		}
		return canvas.finish();
	}

	void* create(const Gpu2dRendererCreateInfo* info) {
		if (info == nullptr || info->struct_size < sizeof(Gpu2dRendererCreateInfo)) {
			gpu2d::plugin::last_error = "invalid create info";
			return nullptr;
		}
		try {
			auto instance = std::make_unique<DirectRendererPlugin>(*info);
			instance->adapter_name = instance->renderer.adapterName();
			return instance.release();
		}
		catch (const std::exception& error) {
			gpu2d::plugin::last_error = error.what();
			return nullptr;
		}
	}

	void destroy(void* instance) {
		try {
			delete static_cast<DirectRendererPlugin*>(instance);
		}
		catch (...) {
			gpu2d::plugin::last_error = "renderer destroy failed";
		}
	}

	int32_t resize(void* instance, const uint32_t width, const uint32_t height) {
		return gpu2d::plugin::guarded([&] {
			static_cast<DirectRendererPlugin*>(instance)->renderer.resize(width, height);
			});
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
			auto& plugin = *static_cast<DirectRendererPlugin*>(instance);
			const gpu2d::CanvasFrame canvas_frame = gpu2d::plugin::rebuildCanvasFrame(*frame);
			if (plugin.page_revision != canvas_frame.page_revision) {
				plugin.page_revision = canvas_frame.page_revision;
				plugin.last_display_list = gpu2d::DisplayList{};
			}
			if (!canvas_frame.operations.empty()) {
				plugin.last_display_list = rectangleDisplayList(
					canvas_frame, frame->canvas_width, frame->canvas_height, frame->width, frame->height);
			}
			const gpu2d::Rect viewport = gpu2d::Rect::fromXYWH(
				0.0F, 0.0F, static_cast<float>(frame->width), static_cast<float>(frame->height));
			const gpu2d::RenderPlan plan = plugin.planner.build(plugin.last_display_list, viewport);
			gpu2d::plugin::copyTelemetry(plugin.renderer.render(plan), *telemetry);
			});
	}

	void waitIdle(void* instance) {
		try {
			if (instance != nullptr) {
				static_cast<DirectRendererPlugin*>(instance)->renderer.waitIdle();
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
		return instance != nullptr ? static_cast<DirectRendererPlugin*>(instance)->adapter_name.c_str() : "";
	}

	const char* lastError() { return gpu2d::plugin::last_error.c_str(); }

	const Gpu2dRendererApi api{ sizeof(Gpu2dRendererApi), GPU2D_RENDERER_ABI_VERSION, "d3d12",
							   "Direct3D 12 (fillRect subset)",
							   gpu2d::CanvasCapabilityRectangles, create, destroy, resize, render, waitIdle,
							   adapterName, lastError };

}  // namespace

extern "C" __declspec(dllexport) const Gpu2dRendererApi* gpu2dGetRendererApi(
	const uint32_t requested_abi_version) {
	return requested_abi_version == GPU2D_RENDERER_ABI_VERSION ? &api : nullptr;
}